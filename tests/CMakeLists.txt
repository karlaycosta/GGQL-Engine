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
    test_path_storage.cpp
    test_topology_pstore_build.cpp
    test_path_backend_coexistence.cpp
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

# ── Testes do query planner (dependem de ggql_query) ─────────────────────────
add_executable(test_query_planner test_query_planner.cpp)
target_compile_options(test_query_planner PRIVATE
    -std=c++23 -march=native
    $<$<CONFIG:Debug>:-O0 -g>
    $<$<NOT:$<CONFIG:Debug>>:-O2>
)
target_link_libraries(test_query_planner PRIVATE ggql_query)
add_test(NAME test_query_planner COMMAND test_query_planner)

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
    ENVIRONMENT "GGQL_TEST_IDX=${CMAKE_SOURCE_DIR}/data/chm13_subset_sm.gfa.idx"
)
