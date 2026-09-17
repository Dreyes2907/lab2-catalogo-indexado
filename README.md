# Laboratorio 2 — Catálogo musical indexado y confiable

**Estudiante:** David Reynerio Reyes Barahona
**Curso:** Estructura de Datos II
**Lenguaje:** C++20

## Compilación y ejecución

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

En Windows con MSVC, agrega la configuración al build y al ctest:

```powershell
cmake --build build --config Debug
ctest --test-dir build --output-on-failure -C Debug
```

### Uso del CLI

```bash
./build/catalog_generate data/catalog.psv data/catalog.bin
./build/lab2_catalog build data/catalog.bin data/catalog.idx
./build/lab2_catalog find data/catalog.bin data/catalog.idx DG18807
./build/lab2_catalog composer data/catalog.bin data/catalog.idx BEETHOVEN
./build/lab2_catalog verify data/catalog.bin data/catalog.idx
```

## Complejidades

| Operación | Complejidad | Justificación |
|---|---|---|
| `build_primary_index` | `O(n log n)` en tiempo, `O(n)` en memoria | Recorrido del archivo `O(n)` usando `next_offset` (nunca se busca el magic byte a byte) + `std::sort` de las `n` entradas `O(n log n)` + verificación de duplicados sobre entradas ya ordenadas `O(n)`. El término dominante es el ordenamiento. |
| `find_offset` / `find_record` | `O(log n)` en memoria + una operación de E/S | Búsqueda binaria manual sobre el arreglo ordenado de entradas (`O(log n)`), seguida de **un único** `seek` + lectura de un registro en disco. El costo de disco es independiente de `n`. |
| `build_composer_index` | `O(m log m)`, con `m` = número de entradas primarias | Resolver cada entrada primaria contra el archivo es `O(m)`; ordenar los pares `(compositor, label_id)` es `O(m log m)`; agrupar sobre pares ya ordenados es `O(m)`. |
| `find_by_composer` | `O(log k)`, con `k` = número de compositores distintos | Búsqueda binaria manual sobre `ComposerIndex`, ordenado por compositor. |
| `verify_primary_index` | `O(n log n)` | Las verificaciones de `DuplicateKey`/`DuplicateOffset` usan copias ordenadas del índice (`O(n log n)` cada una) en vez de comparar cada entrada contra todas las demás (`O(n²)`); la verificación contra el archivo es `O(n)` llamadas a `read_record_at`. |

## Integridad física vs. consistencia lógica

**Integridad física** responde a la pregunta: *¿los bytes leídos son los mismos que se escribieron?* Se detecta con verificaciones de bajo nivel que no requieren entender el significado de los datos: un header incompleto (`TruncatedHeader`), un payload incompleto (`TruncatedPayload`), o un CRC-32 que no coincide con el recalculado (`ChecksumMismatch`). Esta capa se valida completamente dentro de `read_record_at`, antes de intentar interpretar el contenido.

**Consistencia lógica** responde a una pregunta distinta: *¿las estructuras y referencias tienen sentido dentro del sistema?* Un registro puede tener un CRC perfectamente válido —los bytes son exactamente los que se escribieron— y aun así ser lógicamente inconsistente: un `magic` o `version` incorrectos, una longitud de payload fuera de rango, un índice con claves duplicadas o desordenado, dos entradas apuntando al mismo offset, o la clave del índice apuntando a un registro con una clave *distinta*. Esta segunda capa es la que audita `verify_primary_index`, y es la razón por la que encontrar una entrada en el índice nunca es, por sí solo, garantía de que el registro sea confiable: hay que seguir el enlace (índice → offset → registro) y validar ambas capas antes de aceptar el resultado.