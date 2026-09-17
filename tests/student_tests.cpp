#include "doctest/doctest.h"

#include "binary_io.hpp"
#include "catalog.hpp"
#include "catalog_codec.hpp"
#include "crc32.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::stringstream make_stream(const std::string& bytes) {
    return std::stringstream(bytes, std::ios::binary | std::ios::in | std::ios::out);
}

std::string encode_raw_record(std::uint32_t magic, std::uint16_t version,
                               std::uint32_t declared_length,
                               const std::vector<std::byte>& payload,
                               bool append_valid_crc) {
    std::ostringstream output(std::ios::binary | std::ios::out);
    lab2::write_u32_le(output, magic);
    lab2::write_u16_le(output, version);
    lab2::write_u32_le(output, declared_length);
    lab2::write_exact(output, payload);
    if (append_valid_crc) {
        lab2::write_u32_le(output, lab2::crc32(payload));
    }
    return output.str();
}

} // namespace

TEST_CASE("P01 - archivo vacio produce indice primario vacio y offset 0 es invalido") {
    const std::string empty_bytes;

    auto input_build = make_stream(empty_bytes);
    const lab2::PrimaryBuildResult result = lab2::build_primary_index(input_build);
    CHECK(result.ok());
    CHECK(result.entries.empty());

    auto input_read = make_stream(empty_bytes);
    CHECK(lab2::read_record_at(input_read, 0).status == lab2::ReadStatus::InvalidOffset);
}

TEST_CASE("P02 - read_record_at rechaza una version no soportada") {
    const std::vector<std::byte> payload =
        lab2::encode_payload({"X1", "COMPOSER", "TITLE"});
    const std::string bytes = encode_raw_record(
        lab2::RECORD_MAGIC, /*version=*/2,
        static_cast<std::uint32_t>(payload.size()), payload,
        /*append_valid_crc=*/true);

    auto input = make_stream(bytes);
    CHECK(lab2::read_record_at(input, 0).status == lab2::ReadStatus::UnsupportedVersion);
}

TEST_CASE("P03 - read_record_at rechaza longitud de payload cero y longitud excesiva") {
    SUBCASE("longitud declarada cero") {
        const std::string bytes =
            encode_raw_record(lab2::RECORD_MAGIC, lab2::RECORD_VERSION, 0, {}, false);
        auto input = make_stream(bytes);
        CHECK(lab2::read_record_at(input, 0).status == lab2::ReadStatus::InvalidLength);
    }

    SUBCASE("longitud declarada mayor a MAX_PAYLOAD_SIZE") {
        const std::string bytes = encode_raw_record(
            lab2::RECORD_MAGIC, lab2::RECORD_VERSION, lab2::MAX_PAYLOAD_SIZE + 1, {}, false);
        auto input = make_stream(bytes);
        CHECK(lab2::read_record_at(input, 0).status == lab2::ReadStatus::InvalidLength);
    }
}

TEST_CASE("P04 - read_record_at distingue header truncado de payload truncado") {
    const std::vector<std::byte> payload =
        lab2::encode_payload({"X1", "COMPOSER", "TITLE"});
    const std::string full = encode_raw_record(
        lab2::RECORD_MAGIC, lab2::RECORD_VERSION,
        static_cast<std::uint32_t>(payload.size()), payload, /*append_valid_crc=*/true);

    SUBCASE("header truncado (menos de 10 bytes)") {
        const std::string truncated = full.substr(0, 7);
        auto input = make_stream(truncated);
        CHECK(lab2::read_record_at(input, 0).status == lab2::ReadStatus::TruncatedHeader);
    }

    SUBCASE("payload truncado (header completo, payload a la mitad)") {
        const std::string truncated = full.substr(0, 10 + payload.size() / 2);
        auto input = make_stream(truncated);
        CHECK(lab2::read_record_at(input, 0).status == lab2::ReadStatus::TruncatedPayload);
    }
}

TEST_CASE("P05 - un offset dentro de otro registro no produce una lectura valida") {
    const lab2::Record first{"AAA1", "BACH", "TOCCATA"};
    const lab2::Record second{"BBB2", "HANDEL", "MESSIAH"};

    std::ostringstream output(std::ios::binary | std::ios::out);
    std::uint64_t offset_first = 0;
    std::uint64_t offset_second = 0;
    std::string error;
    REQUIRE(lab2::write_record(output, first, &offset_first, error));
    REQUIRE(lab2::write_record(output, second, &offset_second, error));
    const std::string bytes = output.str();

    auto input = make_stream(bytes);
    const lab2::ReadResult result = lab2::read_record_at(input, offset_first + 4);

    CHECK_FALSE(result.ok());
}

TEST_CASE("P06 - intersect_sorted intersecta listas ordenadas sin duplicados") {
    const std::vector<std::string> left{"ANG3795", "COL31809", "DG18807", "ZZZ"};
    const std::vector<std::string> right{"COL31809", "DG18807", "DG18807", "WAR23699"};

    const auto result = lab2::intersect_sorted(left, right);

    CHECK(result == std::vector<std::string>{"COL31809", "DG18807"});
}