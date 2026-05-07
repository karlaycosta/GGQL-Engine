# CMakeLists.txt — wrapper minimo para jltsiren/gbwt
#
# Compila os fontes de third_party/gbwt como biblioteca estatica `gbwt`,
# substituindo o build via Makefile do upstream (que usa autotools-style).
#
# Lista de fontes derivada do `LIBOBJS` do Makefile original (apenas os .cpp
# que compoem libgbwt.a — exclui main programs como build_gbwt, merge_gbwt etc).
#
# Dependencias:
#   - sdsl   (target do third_party/sdsl-cmake — fork vgteam com simple_sds.hpp)
#   - libzstd (sistema, via pkg-config)
#   - OpenMP

cmake_minimum_required(VERSION 3.16)
project(gbwt_jltsiren_wrapper LANGUAGES CXX)

set(GBWT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gbwt")

# ── Fontes da biblioteca (derivadas de LIBOBJS no Makefile upstream) ─────────
set(_GBWT_SOURCES
    "${GBWT_DIR}/src/algorithms.cpp"
    "${GBWT_DIR}/src/bwtmerge.cpp"
    "${GBWT_DIR}/src/cached_gbwt.cpp"
    "${GBWT_DIR}/src/dynamic_gbwt.cpp"
    "${GBWT_DIR}/src/fast_locate.cpp"
    "${GBWT_DIR}/src/files.cpp"
    "${GBWT_DIR}/src/gbwt.cpp"
    "${GBWT_DIR}/src/internal.cpp"
    "${GBWT_DIR}/src/metadata.cpp"
    "${GBWT_DIR}/src/support.cpp"
    "${GBWT_DIR}/src/test.cpp"
    "${GBWT_DIR}/src/utils.cpp"
    "${GBWT_DIR}/src/variants.cpp"
)

add_library(gbwt STATIC ${_GBWT_SOURCES})

# Headers publicos do gbwt (include/gbwt/*.h)
target_include_directories(gbwt PUBLIC
    $<BUILD_INTERFACE:${GBWT_DIR}/include>
)

# ── Dependencias ─────────────────────────────────────────────────────────────

# sdsl: target do wrapper sdsl-cmake (fornece headers vgteam fork)
target_link_libraries(gbwt PUBLIC sdsl)

# libzstd: usado por gbwt para serializacao incremental
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED libzstd)
target_include_directories(gbwt PRIVATE ${ZSTD_INCLUDE_DIRS})
target_link_libraries(gbwt PUBLIC ${ZSTD_LIBRARIES})

# OpenMP: parallel build/merge interno do GBWT
find_package(OpenMP REQUIRED)
target_link_libraries(gbwt PUBLIC OpenMP::OpenMP_CXX)

set_target_properties(gbwt PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_compile_options(gbwt PRIVATE
    -O3 -march=native -fno-strict-aliasing
    -Wno-attributes -Wno-unused-parameter -Wno-deprecated-declarations
)
