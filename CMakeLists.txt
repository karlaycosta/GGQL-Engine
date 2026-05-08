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

# vgteam/sdsl-lite (fork com simple_sds.hpp — usado por PathStorage / int_vector
# e int_vector_buffer). Substitui o sdsl do sistema para todos os targets.
# Clone com: git submodule update --init third_party/sdsl-lite
add_subdirectory(third_party/sdsl-cmake)

# libgbwt — wrapper local em third_party/gbwt-cmake/ (padrão idêntico ao
# sdsl-cmake). Compila os fontes do submódulo jltsiren/gbwt como biblioteca
# estática `gbwt`. Coexiste com PathStorage (include/ggql/gbwt/) — backend
# selecionável por flag em runtime/build (feat/path-backend-coexistence).
# Clone com: git submodule update --init third_party/gbwt
add_subdirectory(third_party/gbwt-cmake)

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

# ── Allocator: jemalloc ──────────────────────────────────────────────────────
# jemalloc substitui o malloc/free do glibc para o processo inteiro. Em
# workloads alloc-heavy como o gBWT build (DynamicGBWT cria/libera milhoes
# de small vectors em multiplas threads), jemalloc venceu glibc/mimalloc/
# tbbmalloc/tcmalloc no nosso bench:
#
#   chm13_subset_100w (8 threads):
#     glibc:    5.60 s · 2.14 GB · 2.86 M nos/s  (baseline)
#     jemalloc: 3.47 s · 1.38 GB · 4.62 M nos/s  (-38% wall, -36% RSS, +62% throughput)
#
# REQUIRED falha o build se libjemalloc nao estiver disponivel — instale
# com `sudo apt install libjemalloc-dev` (Ubuntu/Debian) ou
# `brew install jemalloc` (macOS). Em Windows o suporte e limitado;
# considere mimalloc como alternativa nesses ambientes.
find_library(JEMALLOC_LIB NAMES jemalloc REQUIRED)
message(STATUS "Found jemalloc: ${JEMALLOC_LIB}")

# ── Camada 1 — ggql_storage (CSR + FM-Index + PathStorage + GbwtBackend) ─────
add_library(ggql_storage STATIC
    src/storage/csr_builder.cpp
    src/storage/graph_algorithms.cpp
    src/storage/path_index.cpp
    src/storage/fm_index.cpp
    src/storage/snarl_decomposition.cpp
    src/gbwt/gbwt_backend.cpp
)
target_compile_options(ggql_storage PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
    -funroll-loops -ftree-vectorize -fomit-frame-pointer
)
target_link_libraries(ggql_storage PUBLIC ggql_core sdsl gbwt libsais tbb gomp pthread ${JEMALLOC_LIB})

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

# bench_snarl — microbenchmark isolado do snarl_decomposition::find_all_snarls.
add_executable(bench_snarl tools/bench_snarl.cpp)
target_compile_options(bench_snarl PRIVATE
    -O3 -march=native -fopenmp -fno-strict-aliasing
    -funroll-loops -ftree-vectorize
)
target_link_options(bench_snarl PRIVATE -flto=auto)
target_link_libraries(bench_snarl PRIVATE ggql_storage)

# bench_metadata — isola tempo de load() e queries de GfaMetadataStore.
add_executable(bench_metadata tools/bench_metadata.cpp)
target_compile_options(bench_metadata PRIVATE
    -O3 -flto=auto -march=native
)
target_link_options(bench_metadata PRIVATE -flto=auto)
target_link_libraries(bench_metadata PRIVATE ggql_ingest)

# bench_topology — isola tempo de GfaCsrTopology::load() e save().
add_executable(bench_topology tools/bench_topology.cpp)
target_compile_options(bench_topology PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_topology PRIVATE -flto=auto)
target_link_libraries(bench_topology PRIVATE ggql_storage)

# bench_graph_algorithms — mede build_path_stats, path_nodes,
# make_component_remap e find_bubbles em isolamento.
add_executable(bench_graph_algorithms tools/bench_graph_algorithms.cpp)
target_compile_options(bench_graph_algorithms PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_graph_algorithms PRIVATE -flto=auto)
target_link_libraries(bench_graph_algorithms PRIVATE ggql_ingest)

# bench_fm_index — mede compute_payload_hash, search_kmer, load/save
# isolados de GfaSequenceIndex.
add_executable(bench_fm_index tools/bench_fm_index.cpp)
target_compile_options(bench_fm_index PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_fm_index PRIVATE -flto=auto)
target_link_libraries(bench_fm_index PRIVATE ggql_storage)



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
