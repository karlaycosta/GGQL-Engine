// entity_columns.hpp — Schema das entidades GGQL (fonte única de verdade).
//
// Antes (2026-05-20) as listas viviam em `src/query/query_planner.cpp` no
// namespace `detail`, e o FFI (`src/ffi/ffi.cpp` Round 2.E) duplicava as
// strings. Risco de drift: adicionar campo no planner sem atualizar o FFI
// daria schema incompleto via `ggql_engine_describe_entity`.
//
// Esta header é a fonte única; query_planner inclui e usa direto;
// FFI inclui e converte para `const char* const[]` ao expor.
//
// `std::string_view` literal `"..."sv` aponta para um `const char*` com
// NUL-terminator preservado (literais terminam em '\0' no .rodata), então
// `.data()` é seguro para C ABI.

#ifndef GGQL_QUERY_ENTITY_COLUMNS_HPP
#define GGQL_QUERY_ENTITY_COLUMNS_HPP

#include <array>
#include <string_view>

namespace ggql::query {

// Colunas compartilhadas por nós de grau (FIND TIP e FIND REPEAT).
inline constexpr std::array<std::string_view, 3> kNodeColumns = {
    "id", "in_degree", "out_degree"};

inline constexpr std::array<std::string_view, 5> kBubbleColumns = {
    "source", "sink", "branch_count", "internal_node_count", "has_cycles"};

inline constexpr std::array<std::string_view, 5> kSegmentColumns = {
    "id", "name", "length", "read_count", "depth"};

inline constexpr std::array<std::string_view, 3> kEdgeColumns = {
    "from_id", "to_id", "to_orient"};

inline constexpr std::array<std::string_view, 4> kPathColumns = {
    "id", "name", "node_count", "length"};

} // namespace ggql::query

#endif // GGQL_QUERY_ENTITY_COLUMNS_HPP
