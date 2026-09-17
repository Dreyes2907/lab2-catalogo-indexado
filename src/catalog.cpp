#include "catalog.hpp"

#include "binary_io.hpp"
#include "catalog_codec.hpp"
#include "crc32.hpp"

#include <algorithm>
#include <utility>

namespace lab2 {

const char* to_string(ReadStatus status) {
    switch (status) {
    case ReadStatus::Ok: return "Ok";
    case ReadStatus::NotFound: return "NotFound";
    case ReadStatus::IndexKeyMismatch: return "IndexKeyMismatch";
    case ReadStatus::InvalidOffset: return "InvalidOffset";
    case ReadStatus::TruncatedHeader: return "TruncatedHeader";
    case ReadStatus::BadMagic: return "BadMagic";
    case ReadStatus::UnsupportedVersion: return "UnsupportedVersion";
    case ReadStatus::InvalidLength: return "InvalidLength";
    case ReadStatus::TruncatedPayload: return "TruncatedPayload";
    case ReadStatus::MissingChecksum: return "MissingChecksum";
    case ReadStatus::ChecksumMismatch: return "ChecksumMismatch";
    case ReadStatus::MalformedPayload: return "MalformedPayload";
    }
    return "UnknownReadStatus";
}

const char* to_string(BuildStatus status) {
    switch (status) {
    case BuildStatus::Ok: return "Ok";
    case BuildStatus::ReadError: return "ReadError";
    case BuildStatus::DuplicateKey: return "DuplicateKey";
    }
    return "UnknownBuildStatus";
}

const char* to_string(VerificationIssueType type) {
    switch (type) {
    case VerificationIssueType::UnsortedIndex: return "UnsortedIndex";
    case VerificationIssueType::DuplicateKey: return "DuplicateKey";
    case VerificationIssueType::DuplicateOffset: return "DuplicateOffset";
    case VerificationIssueType::RecordReadError: return "RecordReadError";
    case VerificationIssueType::KeyMismatch: return "KeyMismatch";
    }
    return "UnknownVerificationIssue";
}

// TODO 1 — read_record_at
ReadResult read_record_at(std::istream& input, std::uint64_t offset) {
    ReadResult result{};
    result.offset = offset;
    result.next_offset = offset;

    // Paso 1-2: el offset debe caer estrictamente dentro del archivo.
    // offset == *size también se rechaza: no queda ni un byte después de ahí.
    const auto size = stream_size(input);
    if (!size.has_value() || offset >= *size) {
        result.status = ReadStatus::InvalidOffset;
        result.detail = "Offset fuera del archivo.";
        return result;
    }

    // Paso 3: posicionar el cursor de lectura.
    if (!seek_absolute(input, offset)) {
        result.status = ReadStatus::InvalidOffset;
        result.detail = "No se pudo posicionar en el offset indicado.";
        return result;
    }

    // Paso 4-5: magic. Se lee primero como bytes crudos; solo se interpreta
    // semánticamente (¿es MUS2?) después de confirmar que los 4 bytes existen.
    std::uint32_t magic{};
    if (!read_u32_le(input, magic)) {
        result.status = ReadStatus::TruncatedHeader;
        result.detail = "Header incompleto (magic).";
        return result;
    }
    if (magic != RECORD_MAGIC) {
        result.status = ReadStatus::BadMagic;
        result.detail = "Magic number inválido.";
        return result;
    }

    // Paso 6: versión.
    std::uint16_t version{};
    if (!read_u16_le(input, version)) {
        result.status = ReadStatus::TruncatedHeader;
        result.detail = "Header incompleto (version).";
        return result;
    }
    if (version != RECORD_VERSION) {
        result.status = ReadStatus::UnsupportedVersion;
        result.detail = "Versión de registro no soportada.";
        return result;
    }

    // Paso 7: payload_length. CRÍTICO que esto se valide ANTES de reservar
    // memoria para el payload (paso 8).
    std::uint32_t payload_length{};
    if (!read_u32_le(input, payload_length)) {
        result.status = ReadStatus::TruncatedHeader;
        result.detail = "Header incompleto (payload_length).";
        return result;
    }
    if (payload_length == 0 || payload_length > MAX_PAYLOAD_SIZE) {
        result.status = ReadStatus::InvalidLength;
        result.detail = "Longitud de payload fuera de rango.";
        return result;
    }

    // Paso 8: ahora sí es seguro reservar y leer el payload completo.
    std::vector<std::byte> payload(payload_length);
    if (!read_exact(input, payload)) {
        result.status = ReadStatus::TruncatedPayload;
        result.detail = "Payload incompleto.";
        return result;
    }

    // Paso 9: leer el CRC almacenado (todavía sin comparar).
    std::uint32_t stored_crc{};
    if (!read_u32_le(input, stored_crc)) {
        result.status = ReadStatus::MissingChecksum;
        result.detail = "CRC incompleto o ausente.";
        return result;
    }

    // Paso 10: recalcular CRC-32 SOLO sobre el payload (no sobre el header)
    // y compararlo. Este es el punto de integridad física: si no coincide,
    // el registro se rechaza aquí y NUNCA se llega a decodificar el payload.
    const std::uint32_t computed_crc = crc32(payload);
    if (computed_crc != stored_crc) {
        result.status = ReadStatus::ChecksumMismatch;
        result.detail = "El CRC calculado no coincide con el almacenado.";
        return result;
    }

    // Paso 11: solo tras pasar el CRC es seguro confiar en el contenido del
    // payload y decodificarlo. decode_payload ya viene provisto.
    PayloadDecodeResult decoded = decode_payload(payload);
    if (!decoded.record.has_value()) {
        result.status = ReadStatus::MalformedPayload;
        result.detail = decoded.detail;
        return result;
    }

    // Paso 12: éxito. next_offset le permite al llamador (build_primary_index)
    // avanzar sin tener que buscar el próximo magic byte a byte.
    result.status = ReadStatus::Ok;
    result.record = std::move(decoded.record);
    result.next_offset = offset + RECORD_HEADER_SIZE + payload_length + RECORD_CHECKSUM_SIZE;
    return result;
}

// TODO 2 — build_primary_index
PrimaryBuildResult build_primary_index(std::istream& input) {
    PrimaryBuildResult result{};

    const auto size = stream_size(input);
    if (!size.has_value()) {
        result.status = BuildStatus::ReadError;
        result.detail = "No se pudo determinar el tamaño del archivo.";
        return result;
    }

    std::uint64_t offset = 0;
    while (offset < *size) {
        ReadResult r = read_record_at(input, offset);
        if (!r.ok()) {
            result.status = BuildStatus::ReadError;
            result.error_offset = offset;
            result.detail = r.detail;
            return result;
        }
        result.entries.push_back(PrimaryEntry{r.record->label_id, offset});
        offset = r.next_offset; // avanzar por next_offset, nunca buscando magic
    }

    std::sort(result.entries.begin(), result.entries.end(),
              [](const PrimaryEntry& a, const PrimaryEntry& b) {
                  return a.label_id < b.label_id;
              });

    for (std::size_t i = 1; i < result.entries.size(); ++i) {
        if (result.entries[i].label_id == result.entries[i - 1].label_id) {
            result.status = BuildStatus::DuplicateKey;
            result.error_key = result.entries[i].label_id;
            result.detail = "Clave primaria duplicada.";
            return result;
        }
    }

    result.status = BuildStatus::Ok;
    return result;
}


// TODO 3 — find_offset (búsqueda binaria manual)
std::optional<std::uint64_t> find_offset(
    std::span<const PrimaryEntry> index,
    std::string_view label_id) {
    if (index.empty()) {
        return std::nullopt;
    }

    std::size_t low = 0;
    std::size_t high = index.size() - 1;

    while (low <= high) {
        const std::size_t mid = low + (high - low) / 2; // evita overflow de (low+high)/2
        const std::string_view mid_key = index[mid].label_id;

        if (mid_key == label_id) {
            return index[mid].offset;
        }
        if (mid_key < label_id) {
            low = mid + 1;
        } else {
            if (mid == 0) {
                break; // ya no hay nada a la izquierda; evita underflow
            }
            high = mid - 1;
        }
    }
    return std::nullopt;
}


ReadResult find_record(
    std::istream& input,
    std::span<const PrimaryEntry> index,
    std::string_view label_id) {
    const auto offset = find_offset(index, label_id);
    if (!offset.has_value()) {
        return {ReadStatus::NotFound, std::nullopt, 0, 0,
                "La clave no existe en el índice primario."};
    }
    ReadResult result = read_record_at(input, *offset);
    if (result.ok() && result.record->label_id != label_id) {
        result.status = ReadStatus::IndexKeyMismatch;
        result.record.reset();
        result.detail = "La clave del índice no coincide con la clave del registro.";
    }
    return result;
}


// TODO 4 — build_composer_index (índice invertido, sin contenedores
// asociativos de la STL)
ComposerBuildResult build_composer_index(
    std::istream& input,
    std::span<const PrimaryEntry> primary) {
    ComposerBuildResult result{};

    struct ComposerLabelPair {
        std::string composer;
        std::string label_id;
    };

    std::vector<ComposerLabelPair> pairs;
    pairs.reserve(primary.size());

    for (const auto& entry : primary) {
        ReadResult r = read_record_at(input, entry.offset);
        if (!r.ok()) {
            result.skipped.push_back(SkippedRecord{entry.label_id, entry.offset, r.status});
            continue;
        }
        if (r.record->label_id != entry.label_id) {
            result.skipped.push_back(
                SkippedRecord{entry.label_id, entry.offset, ReadStatus::IndexKeyMismatch});
            continue;
        }
        pairs.push_back(ComposerLabelPair{r.record->composer, r.record->label_id});
    }

    std::sort(pairs.begin(), pairs.end(),
              [](const ComposerLabelPair& a, const ComposerLabelPair& b) {
                  if (a.composer != b.composer) {
                      return a.composer < b.composer;
                  }
                  return a.label_id < b.label_id;
              });

    for (auto& pair : pairs) {
        // Nuevo grupo de compositor cuando cambia respecto al último agregado.
        if (result.entries.empty() || result.entries.back().composer != pair.composer) {
            result.entries.push_back(ComposerEntry{pair.composer, {}});
        }
        auto& label_ids = result.entries.back().label_ids;
        // Como pairs está ordenado por (composer, label_id), un duplicado
        // exacto siempre queda justo al final de label_ids.
        if (label_ids.empty() || label_ids.back() != pair.label_id) {
            label_ids.push_back(pair.label_id);
        }
    }

    return result;
}

// TODO 5 — find_by_composer (misma técnica de búsqueda binaria manual)
std::span<const std::string> find_by_composer(
    const ComposerIndex& index,
    std::string_view composer) {
    if (index.empty()) {
        return {};
    }

    std::size_t low = 0;
    std::size_t high = index.size() - 1;

    while (low <= high) {
        const std::size_t mid = low + (high - low) / 2;
        const std::string_view mid_key = index[mid].composer;

        if (mid_key == composer) {
            return index[mid].label_ids;
        }
        if (mid_key < composer) {
            low = mid + 1;
        } else {
            if (mid == 0) {
                break;
            }
            high = mid - 1;
        }
    }
    return {};
}

// TODO 6 — verify_primary_index
VerificationReport verify_primary_index(
    std::istream& input,
    std::span<const PrimaryEntry> index) {
    VerificationReport report{};
    report.entries_checked = index.size();

    // Verificaciones estructurales del índice 

    // UnsortedIndex: se compara en el orden FÍSICO en que llegan las entradas.
    for (std::size_t i = 1; i < index.size(); ++i) {
        if (index[i].label_id < index[i - 1].label_id) {
            report.issues.push_back(VerificationIssue{
                VerificationIssueType::UnsortedIndex,
                index[i].label_id, index[i].offset,
                ReadStatus::Ok,
                "La entrada rompe el orden ascendente respecto a la anterior."});
        }
    }

    // DuplicateKey: copia ordenada por label_id; los duplicados quedan
    // adyacentes sin importar el orden original.
    std::vector<PrimaryEntry> by_key(index.begin(), index.end());
    std::sort(by_key.begin(), by_key.end(),
              [](const PrimaryEntry& a, const PrimaryEntry& b) {
                  return a.label_id < b.label_id;
              });
    for (std::size_t i = 1; i < by_key.size(); ++i) {
        if (by_key[i].label_id == by_key[i - 1].label_id) {
            report.issues.push_back(VerificationIssue{
                VerificationIssueType::DuplicateKey,
                by_key[i].label_id, by_key[i].offset,
                ReadStatus::Ok, "Clave primaria repetida en el índice."});
        }
    }

    // DuplicateOffset: misma técnica, ordenando por offset.
    std::vector<PrimaryEntry> by_offset(index.begin(), index.end());
    std::sort(by_offset.begin(), by_offset.end(),
              [](const PrimaryEntry& a, const PrimaryEntry& b) {
                  return a.offset < b.offset;
              });
    for (std::size_t i = 1; i < by_offset.size(); ++i) {
        if (by_offset[i].offset == by_offset[i - 1].offset) {
            report.issues.push_back(VerificationIssue{
                VerificationIssueType::DuplicateOffset,
                by_offset[i].label_id, by_offset[i].offset,
                ReadStatus::Ok, "Dos claves apuntan al mismo offset."});
        }
    }

    // Verificaciones contra el archivo de datos 
    for (const auto& entry : index) {
        ReadResult r = read_record_at(input, entry.offset);
        if (!r.ok()) {
            report.issues.push_back(VerificationIssue{
                VerificationIssueType::RecordReadError,
                entry.label_id, entry.offset,
                r.status, r.detail});
            continue; // no cuenta como readable_matching_entries
        }
        if (r.record->label_id != entry.label_id) {
            report.issues.push_back(VerificationIssue{
                VerificationIssueType::KeyMismatch,
                entry.label_id, entry.offset,
                r.status, "La clave del registro no coincide con la del índice."});
            continue;
        }
        ++report.readable_matching_entries;
    }

    return report;
}

// BONO — intersect_sorted (dos punteros, O(n + m))
std::vector<std::string> intersect_sorted(
    std::span<const std::string> left,
    std::span<const std::string> right) {
    std::vector<std::string> result;
    std::size_t i = 0;
    std::size_t j = 0;

    while (i < left.size() && j < right.size()) {
        if (left[i] == right[j]) {
            if (result.empty() || result.back() != left[i]) {
                result.push_back(left[i]);
            }
            ++i;
            ++j;
        } else if (left[i] < right[j]) {
            ++i;
        } else {
            ++j;
        }
    }
    return result;
}

} // namespace lab2