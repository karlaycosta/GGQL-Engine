cmake_minimum_required(VERSION 3.16)
project(ggql_engine LANGUAGES CXX)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Requer GCC 14+ para suporte completo a C++23.
# Passe o compilador na linha de comando se necessário:
#   cmake -B build -DCMAKE_CXX_COMPILER=g++-14
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Gera build/compile_commands.json — usado pelo IntelliSense do VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

# {fmt} — Biblioteca de formatação moderna (estilo Python/C++23) com suporte a cores.
add_subdirectory(third_party/fmt)

# ── Dependências ─────────────────────────────────────────
# PEGTL (header-only, git submodule em third_party/pegtl)
# Clone com: git submodule update --init
add_library(pegtl INTERFACE)
target_include_directories(pegtl INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/pegtl/include
)

# vgteam/sdsl-lite (fork com simple_sds.hpp — exigido pelo libgbwt)
# Substitui o sdsl do sistema para todos os targets do projeto.
# Clone com: git submodule update --init third_party/sdsl-lite
add_subdirectory(third_party/sdsl-cmake)

# libgbwt (r-index / GBWT incremental — git submodule em third_party/gbwt)
# Substitui sdsl::csa_wt + construct_im na Camada 2 da GfaCsrTopology.
# Clone com: git submodule update --init third_party/gbwt
add_subdirectory(third_party/gbwt)
target_link_libraries(gbwt PRIVATE fmt::fmt)

# libsais (criação ultra-rápida de Suffix Arrays paralela)
set(LIBSAIS_USE_OPENMP ON CACHE BOOL "Use OpenMP for parallelization" FORCE)
set(LIBSAIS_BUILD_SHARED_LIB OFF CACHE BOOL "Build static" FORCE)
add_subdirectory(third_party/libsais)

# BooPHF agora está em include/mphf/ — coberto pelo include_directories global abaixo.
# third_party/bbhash não é mais necessário.

# xxHash agora está em include/mphf/xxh64.hpp — coberto pelo include_directories global.
# third_party/xxhash não é mais necessário.

# ── Includes do próprio projeto ───────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ═══════════════════════════════════════════════════════════════════════════════
# Arquitetura em camadas — cada camada só recompila quando seu próprio código
# ou dependências diretas mudam.
# ═══════════════════════════════════════════════════════════════════════════════

# ── Camada 0 — ggql_core (utilitários e CLI; sem deps externas pesadas) ───────
add_library(ggql_core STATIC
    src/utils.cpp
    src/cli/console.cpp
)
target_compile_options(ggql_core PRIVATE
    -O3 -flto=auto -march=native
)
target_link_libraries(ggql_core PUBLIC fmt::fmt)

# ── Camada 1 — ggql_storage (CSR + FM-Index + gBWT) ──────────────────────────
add_library(ggql_storage STATIC
    src/storage/csr_builder.cpp
    src/storage/graph_algorithms.cpp
    src/storage/path_index.cpp
    src/storage/fm_index.cpp
    src/storage/snarl_decomposition.cpp
    src/storage/snarl_decomposition_vgstyle.cpp
)
target_compile_options(ggql_storage PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_libraries(ggql_storage PUBLIC ggql_core sdsl gbwt libsais tbb gomp pthread)

# ── Camada 2 — ggql_ingest (parser GFA; depende de Storage) ──────────────────
add_library(ggql_ingest STATIC
    src/ingest/ingestor.cpp
)
target_compile_options(ggql_ingest PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_libraries(ggql_ingest PUBLIC ggql_storage)

# ── Camada 3 — ggql_query (planner + evaluador; depende de Ingest + PEGTL) ───
add_library(ggql_query STATIC
    src/query/query_planner.cpp
    src/query/query_evaluator.cpp
)
target_compile_options(ggql_query PRIVATE
    -O3 -flto=auto -march=native
)
target_link_libraries(ggql_query PUBLIC ggql_ingest pegtl)


# ── ggql_repl ────────────────────────────────────────────
# REPL interativo: parse GGQL + QueryPlanner + ingest GFA.
# Uso: ./build/ggql_repl [--gfa <arquivo.gfa>] [--idx <dir>] [--threads N]
add_executable(ggql_repl src/repl/ggql_repl.cpp)
target_compile_options(ggql_repl PRIVATE
    -O3 -flto=auto -march=native -fopenmp
)
target_link_options(ggql_repl PRIVATE -flto=auto)
target_link_libraries(ggql_repl PRIVATE ggql_query)

# ── bench_gbwt ─────────────────────────────────────────
# Benchmark vg-equivalent: remapeia IDs MPHF → sequenciais por componente,
# reduz sigma do DynamicGBWT de ~85M para ~M_i, habilita paralelismo efetivo.
# Uso: ./build/bench_gbwt <arquivo.gfa> [threads] [max_paths]
add_executable(bench_gbwt src/bench_gbwt.cpp)
target_compile_options(bench_gbwt PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_gbwt PRIVATE -flto=auto)
target_link_libraries(bench_gbwt PRIVATE ggql_ingest)

# ── bubble_vs_snarl (diagnóstico) ─────────────────────────
# Tool de diagnóstico que compara find_snarls_streaming (cactus) com
# find_bubbles (Onodera 2013) no mesmo .idx. Usado para validar
# discrepâncias contra ferramentas externas (vg snarls, BubbleGun).
add_executable(bubble_vs_snarl tools/bubble_vs_snarl.cpp)
target_compile_options(bubble_vs_snarl PRIVATE
    -O3 -march=native -fopenmp -fno-strict-aliasing
)
target_link_libraries(bubble_vs_snarl PRIVATE ggql_storage)



# ── ggql_ffi (Dart/Flutter Integration) ──────────────────
# Biblioteca compartilhada (.so/.dll/.dylib) para uso via Dart FFI.
# Encapsula instâncias C++ com extern "C" e ponteiros opacos.
add_library(ggql_ffi SHARED src/ffi/ffi.cpp)
target_compile_options(ggql_ffi PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fPIC
)
target_link_options(ggql_ffi PRIVATE -flto=auto)
target_link_libraries(ggql_ffi PRIVATE ggql_query)

# ── Testes unitários do ingestor (ARCH-5) ────────────────────────────────────
add_subdirectory(tests)
