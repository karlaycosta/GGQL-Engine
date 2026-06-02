cmake_minimum_required(VERSION 3.20)

enable_testing()

# ── Testes do ingestor (dependem de ggql_ingest) ──────────────────────────────
set(INGEST_TEST_SRCS
    test_ingestor_tokenize.cpp
    test_ingestor_remap.cpp
    test_ingestor_binpack.cpp
    test_ingestor_manifest.cpp
    test_ingestor_parse.cpp
    test_ingestor_profiles.cpp
)

foreach(src ${INGEST_TEST_SRCS})
    get_filename_component(tname ${src} NAME_WE)
    add_executable(${tname} ${src})
    target_compile_options(${tname} PRIVATE
        -std=c++23 -march=native -fopenmp -fno-strict-aliasing
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<NOT:$<CONFIG:Debug>>:-O2>
    )
    target_link_libraries(${tname} PRIVATE ggql_ingest)
    add_test(NAME ${tname} COMMAND ${tname})
endforeach()

# ── Testes de storage (dependem apenas de ggql_storage) ──────────────────────
set(STORAGE_TEST_SRCS
    test_storage_csr.cpp
    test_storage_fm_index.cpp
)

foreach(src ${STORAGE_TEST_SRCS})
    get_filename_component(tname ${src} NAME_WE)
    add_executable(${tname} ${src})
    target_compile_options(${tname} PRIVATE
        -std=c++23 -march=native -fopenmp -fno-strict-aliasing
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<NOT:$<CONFIG:Debug>>:-O2>
    )
    target_link_libraries(${tname} PRIVATE ggql_storage)
    add_test(NAME ${tname} COMMAND ${tname})
endforeach()

# ── Teste do BooPHF (header-only, só precisa de OpenMP) ──────────────────────
add_executable(test_boophf test_boophf.cpp)
target_compile_options(test_boophf PRIVATE
    -std=c++23 -march=native -fopenmp -fno-strict-aliasing
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_options(test_boophf PRIVATE -fopenmp)
add_test(NAME test_boophf COMMAND test_boophf)

# Bench standalone (não roda em ctest — gera CSV em stdout).
add_executable(bench_boophf bench_boophf.cpp)
target_compile_options(bench_boophf PRIVATE
    -std=c++23 -O3 -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_boophf PRIVATE -fopenmp)

# ── Teste de gramática (parser puro — sem topology, instantâneo) ────────────
add_executable(test_grammar test_grammar.cpp)
target_compile_options(test_grammar PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_grammar PRIVATE ggql_query)
add_test(NAME test_grammar COMMAND test_grammar)

# ── Testes do query planner (dependem de ggql_query) ─────────────────────────
add_executable(test_query_planner test_query_planner.cpp)
target_compile_options(test_query_planner PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_query_planner PRIVATE ggql_query)
add_test(NAME test_query_planner COMMAND test_query_planner)

# ── Teste de crosses() — carrega .idx real, valida path-index lookup ─────────
add_executable(test_query_crosses test_query_crosses.cpp)
target_compile_options(test_query_crosses PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_query_crosses PRIVATE ggql_query ggql_storage ggql_ingest)
add_test(
    NAME test_query_crosses
    COMMAND test_query_crosses
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_query_crosses PROPERTIES
    ENVIRONMENT "GGQL_TEST_IDX=${CMAKE_SOURCE_DIR}/data/chm13_subset_100w.gfa.idx"
)

# ── Smoke tests do planner em índice real (B-4) ──────────────────────────────
add_executable(test_query_planner_idx test_query_planner_idx.cpp)
target_compile_options(test_query_planner_idx PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_query_planner_idx PRIVATE ggql_query ggql_storage ggql_ingest)
add_test(
    NAME test_query_planner_idx
    COMMAND test_query_planner_idx
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_query_planner_idx PROPERTIES
    ENVIRONMENT "GGQL_TEST_IDX=${CMAKE_SOURCE_DIR}/data/chm13_subset_100w.gfa.idx"
)

# ── Utilitário de validação Tier A — não roda em ctest, executado por scripts ─
add_executable(run_queries validation/run_queries.cpp)
target_compile_options(run_queries PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(run_queries PRIVATE ggql_query ggql_storage ggql_ingest)

# ── Bisect B-V2: verifica simetria E_fwd↔E_bwd em um .idx carregado ──────────
add_executable(check_csr_symmetry validation/check_csr_symmetry.cpp)
target_compile_options(check_csr_symmetry PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(check_csr_symmetry PRIVATE ggql_storage)

# ── Testes de integração FFI de query (dependem de ggql_ffi) ─────────────────
add_executable(test_ffi_query test_ffi_query.cpp)
target_compile_options(test_ffi_query PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_ffi_query PRIVATE ggql_ffi)
add_test(
    NAME test_ffi_query
    COMMAND test_ffi_query
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_ffi_query PROPERTIES
    ENVIRONMENT "GGQL_TEST_IDX=${CMAKE_SOURCE_DIR}/data/chm13_subset_100w.gfa.idx"
)

# ── Teste de cancelamento do ingest (C2 / P10) ───────────────────────────────
add_executable(test_ffi_ingest_cancel test_ffi_ingest_cancel.cpp)
target_compile_options(test_ffi_ingest_cancel PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_ffi_ingest_cancel PRIVATE ggql_ffi)
add_test(
    NAME test_ffi_ingest_cancel
    COMMAND test_ffi_ingest_cancel
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_ffi_ingest_cancel PROPERTIES
    ENVIRONMENT "GGQL_TEST_GFA=${CMAKE_SOURCE_DIR}/data/chm13_subset_10w.gfa"
)

# ── Teste do cursor stateful (C7.1) ──────────────────────────────────────────
add_executable(test_ffi_cursor test_ffi_cursor.cpp)
target_compile_options(test_ffi_cursor PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_ffi_cursor PRIVATE ggql_ffi)
add_test(
    NAME test_ffi_cursor
    COMMAND test_ffi_cursor
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_ffi_cursor PROPERTIES
    ENVIRONMENT "GGQL_TEST_IDX=${CMAKE_SOURCE_DIR}/data/chm13_subset_sm.gfa.idx"
)

# ── Teste do subgrafo induzido p/ o GraphView (C12) ──────────────────────────
add_executable(test_ffi_subgraph test_ffi_subgraph.cpp)
target_compile_options(test_ffi_subgraph PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_ffi_subgraph PRIVATE ggql_ffi)
add_test(
    NAME test_ffi_subgraph
    COMMAND test_ffi_subgraph
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_ffi_subgraph PROPERTIES
    ENVIRONMENT "GGQL_TEST_IDX=${CMAKE_SOURCE_DIR}/data/chm13_subset_sm.gfa.idx"
)

# ── Teste do contrato de versão (Fase A) — handle-free, sem dataset ──────────
add_executable(test_ffi_version test_ffi_version.cpp)
target_compile_options(test_ffi_version PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_ffi_version PRIVATE ggql_ffi)
add_test(
    NAME test_ffi_version
    COMMAND test_ffi_version
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
