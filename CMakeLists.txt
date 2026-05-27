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
# estática `gbwt`. Usado por GbwtBackend (Camada 2 da GfaCsrTopology) em
# include/ggql/storage/gbwt_backend.hpp.
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

# ── Allocator ────────────────────────────────────────────────────────────────
# Allocators sao escolhidos por target porque tem requisitos diferentes:
#
#   - Binarios standalone (ggql_repl, bench_*): jemalloc-system por
#     padrao. Linkado no main executable, faz interposition de malloc/
#     free do processo no startup. Maior throughput. Initial-Exec TLS
#     funciona porque o allocator entra no processo desde o startup.
#
#   - libggql_ffi.so: mimalloc por padrao. .so eh carregada via dlopen
#     pelo consumer (Dart NativeCallable, Python ctypes). Jemalloc nesse
#     cenario causa SIGABRT ("free(): invalid pointer") porque ja existem
#     ponteiros alocados pelo consumer via glibc malloc — e jemalloc, ao
#     fazer interposition tardia, recebe esses frees sem conseguir
#     reconhece-los. Mimalloc tem fallback automatico para libc free em
#     ponteiros nao-mimalloc; dlopen-safe sem LD_PRELOAD.
#
# Bench (chm13_subset_100w, 8 threads, bench_gbwt, mediana de 4 runs):
#   glibc:           5.60 s · 2.14 GB · 2.86 M nos/s  (baseline historico)
#   jemalloc-system: 2.70 s · 2.86 GB · 5.94 M nos/s  (-52% Fase 5, +108%)
#   jemalloc-vendor: 2.88 s · 2.86 GB · 5.57 M nos/s  (-2% vs system, TLS dinamico)
#   mimalloc:        2.82 s · 3.08 GB · 5.70 M nos/s  (-4% vs system, +7% RSS)
#
# Variaveis:
#   -DGGQL_ALLOCATOR     = jemalloc-system|jemalloc|mimalloc|glibc
#                          (binarios standalone; default: jemalloc-system)
#   -DGGQL_FFI_ALLOCATOR = jemalloc-system|mimalloc|glibc
#                          (libggql_ffi.so; default: mimalloc)
#                          ATENCAO: 'jemalloc' (vendor) NAO esta disponivel aqui
#                          porque causa SIGABRT em dlopen contexts.
#                          'jemalloc-system' exige LD_PRELOAD do consumer
#                          (fragil; evite).
#
# Sub-opcoes:
#   - jemalloc-system (default bins): libjemalloc_pic.a do apt (sudo apt
#     install libjemalloc-dev). Initial-Exec TLS. Funciona em bins porque
#     o allocator entra no processo desde o startup.
#   - jemalloc (vendor): fallback quando jemalloc-system nao esta
#     disponivel (p.ex. CI Docker minimal sem apt). Baixado em build-time
#     via ExternalProject_Add (jemalloc 5.3.0 do GitHub) com
#     --disable-initial-exec-tls. ~6% mais lento que system.
#   - mimalloc (default ffi): libmimalloc.so.2 (sudo apt install
#     libmimalloc2.0). TLS general-dynamic, dlopen-safe.
#   - glibc: sem allocator extra. Baseline.

set(GGQL_ALLOCATOR "jemalloc-system" CACHE STRING
    "Allocator for standalone binaries (jemalloc-system|jemalloc|mimalloc|glibc)")
set_property(CACHE GGQL_ALLOCATOR PROPERTY STRINGS
    jemalloc-system jemalloc mimalloc glibc)

set(GGQL_FFI_ALLOCATOR "mimalloc" CACHE STRING
    "Allocator for libggql_ffi.so (mimalloc recommended for dlopen-safety)")
set_property(CACHE GGQL_FFI_ALLOCATOR PROPERTY STRINGS
    jemalloc-system mimalloc glibc)

if(GGQL_FFI_ALLOCATOR STREQUAL "jemalloc")
    message(FATAL_ERROR
        "GGQL_FFI_ALLOCATOR=jemalloc (vendor) crasha em dlopen contexts "
        "(SIGABRT por interposicao de malloc). Use mimalloc (default), "
        "jemalloc-system (com LD_PRELOAD), ou glibc.")
endif()

# Macros que materializam cada allocator UMA UNICA VEZ, mesmo se bin e ffi
# escolherem o mesmo (guardas internos).

macro(_ggql_ensure_jemalloc_vendor)
    if(NOT TARGET jemalloc_vendor_lib)
        include(ExternalProject)
        set(_jvp ${CMAKE_BINARY_DIR}/_deps/jemalloc)
        set(_jvl ${_jvp}/lib/libjemalloc_pic.a)
        ExternalProject_Add(jemalloc_vendor
            URL https://github.com/jemalloc/jemalloc/releases/download/5.3.0/jemalloc-5.3.0.tar.bz2
            URL_HASH SHA256=2db82d1e7119df3e71b7640219b6dfe84789bc0537983c3b7ac4f7189aecfeaa
            PREFIX ${_jvp}
            CONFIGURE_COMMAND <SOURCE_DIR>/configure
                --prefix=<INSTALL_DIR>
                --disable-initial-exec-tls
                --with-jemalloc-prefix=
                --disable-shared
                --enable-static
                CC=${CMAKE_C_COMPILER}
                CXX=${CMAKE_CXX_COMPILER}
                CFLAGS=-fPIC
                CXXFLAGS=-fPIC
            BUILD_COMMAND make -j
            INSTALL_COMMAND make install
            BUILD_BYPRODUCTS ${_jvl}
            LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON
        )
        add_library(jemalloc_vendor_lib STATIC IMPORTED)
        set_target_properties(jemalloc_vendor_lib PROPERTIES
            IMPORTED_LOCATION ${_jvl})
        add_dependencies(jemalloc_vendor_lib jemalloc_vendor)
    endif()
endmacro()

macro(_ggql_ensure_jemalloc_system)
    if(NOT DEFINED _GGQL_JEMALLOC_SYS_LIB)
        find_library(_GGQL_JEMALLOC_SYS_LIB
            NAMES libjemalloc_pic.a jemalloc REQUIRED)
    endif()
endmacro()

macro(_ggql_ensure_mimalloc)
    if(NOT DEFINED _GGQL_MIMALLOC_LIB)
        # Debian/Ubuntu nao distribui mimalloc estatico — usar .so dinamica.
        # Aceita o nome canonico (com .so symlink do -dev) ou o versionado
        # (libmimalloc.so.2, presente sem o -dev).
        find_library(_GGQL_MIMALLOC_LIB
            NAMES mimalloc libmimalloc.so.2
            HINTS /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu
            REQUIRED)
    endif()
endmacro()

function(_ggql_resolve_allocator choice out_var)
    if(choice STREQUAL "jemalloc")
        _ggql_ensure_jemalloc_vendor()
        set(${out_var} jemalloc_vendor_lib PARENT_SCOPE)
    elseif(choice STREQUAL "jemalloc-system")
        _ggql_ensure_jemalloc_system()
        set(${out_var} ${_GGQL_JEMALLOC_SYS_LIB} PARENT_SCOPE)
    elseif(choice STREQUAL "mimalloc")
        _ggql_ensure_mimalloc()
        set(${out_var} ${_GGQL_MIMALLOC_LIB} PARENT_SCOPE)
    elseif(choice STREQUAL "glibc")
        set(${out_var} "" PARENT_SCOPE)
    else()
        message(FATAL_ERROR
            "Allocator '${choice}' invalido. "
            "Use jemalloc|jemalloc-system|mimalloc|glibc.")
    endif()
endfunction()

_ggql_resolve_allocator(${GGQL_ALLOCATOR} GGQL_BIN_ALLOCATOR_LIB)
_ggql_resolve_allocator(${GGQL_FFI_ALLOCATOR} GGQL_FFI_ALLOCATOR_LIB)
message(STATUS "Allocator (binaries): ${GGQL_ALLOCATOR}")
message(STATUS "Allocator (libggql_ffi.so): ${GGQL_FFI_ALLOCATOR}")

# jemalloc-system na FFI causa cannot-allocate-TLS / SIGABRT em dlopen
# tardio. Avise o desenvolvedor que precisa LD_PRELOAD no consumer.
if(GGQL_FFI_ALLOCATOR STREQUAL "jemalloc-system")
    message(WARNING
        "GGQL_FFI_ALLOCATOR=jemalloc-system usa Initial-Exec TLS. "
        "Consumers que dlopen libggql_ffi.so (Flutter, Python ctypes) "
        "precisam LD_PRELOAD=libjemalloc.so.2 OU vao crashar com "
        "'cannot allocate memory in static TLS block' / SIGABRT. "
        "Default 'mimalloc' resolve isso automaticamente.")
endif()

# ── Camada 1 — ggql_storage (CSR + FM-Index + PathStorage + GbwtBackend) ─────
add_library(ggql_storage STATIC
    src/storage/csr_builder.cpp
    src/storage/graph_algorithms.cpp
    src/storage/path_index.cpp
    src/storage/fm_index.cpp
    src/storage/snarl_decomposition.cpp
    src/storage/gbwt_backend.cpp
)
target_compile_options(ggql_storage PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
    -funroll-loops -ftree-vectorize -fomit-frame-pointer
)
# Allocator nao linkado aqui — cada consumer final (executavel ou .so)
# linka seu proprio via GGQL_BIN_ALLOCATOR_LIB / GGQL_FFI_ALLOCATOR_LIB.
target_link_libraries(ggql_storage PUBLIC ggql_core sdsl gbwt libsais tbb gomp pthread)

# ── Camada 2 — ggql_ingest (parser GFA; depende de Storage) ──────────────────
add_library(ggql_ingest STATIC
    src/ingest/ingestor.cpp
    src/ingest/survey.cpp
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
target_link_libraries(ggql_repl PRIVATE ggql_query ${GGQL_BIN_ALLOCATOR_LIB})

# ── bench_gbwt ─────────────────────────────────────────
# Benchmark vg-equivalent: remapeia IDs MPHF → sequenciais por componente,
# reduz sigma do DynamicGBWT de ~85M para ~M_i, habilita paralelismo efetivo.
# Uso: ./build/bench_gbwt <arquivo.gfa> [threads] [max_paths]
add_executable(bench_gbwt src/bench_gbwt.cpp)
target_compile_options(bench_gbwt PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_gbwt PRIVATE -flto=auto)
target_link_libraries(bench_gbwt PRIVATE ggql_ingest ${GGQL_BIN_ALLOCATOR_LIB})

# bench_suggest_profile — protótipo: compara baseline vs light survey
# vs prepare+reuse para decidir se o reuse cross-call vale a pena.
add_executable(bench_suggest_profile src/bench_suggest_profile.cpp)
target_compile_options(bench_suggest_profile PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_suggest_profile PRIVATE -flto=auto)
target_link_libraries(bench_suggest_profile PRIVATE ggql_ingest ${GGQL_BIN_ALLOCATOR_LIB})

# bench_survey — micro-bench isolado pra profile (perf record / callgrind).
# Mantém -O3 (medir caso real) + -g pra ter símbolos no perf report.
add_executable(bench_survey tools/bench_survey.cpp)
target_compile_options(bench_survey PRIVATE
    -O3 -g -fno-omit-frame-pointer -march=native -fopenmp -fno-strict-aliasing
)
target_link_libraries(bench_survey PRIVATE ggql_ingest ${GGQL_BIN_ALLOCATOR_LIB})

# bench_snarl — microbenchmark isolado do snarl_decomposition::find_all_snarls.
add_executable(bench_snarl tools/bench_snarl.cpp)
target_compile_options(bench_snarl PRIVATE
    -O3 -march=native -fopenmp -fno-strict-aliasing
    -funroll-loops -ftree-vectorize
)
target_link_options(bench_snarl PRIVATE -flto=auto)
target_link_libraries(bench_snarl PRIVATE ggql_storage ${GGQL_BIN_ALLOCATOR_LIB})

# bench_metadata — isola tempo de load() e queries de GfaMetadataStore.
add_executable(bench_metadata tools/bench_metadata.cpp)
target_compile_options(bench_metadata PRIVATE
    -O3 -flto=auto -march=native
)
target_link_options(bench_metadata PRIVATE -flto=auto)
target_link_libraries(bench_metadata PRIVATE ggql_ingest ${GGQL_BIN_ALLOCATOR_LIB})

# bench_topology — isola tempo de GfaCsrTopology::load() e save().
add_executable(bench_topology tools/bench_topology.cpp)
target_compile_options(bench_topology PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_topology PRIVATE -flto=auto)
target_link_libraries(bench_topology PRIVATE ggql_storage ${GGQL_BIN_ALLOCATOR_LIB})

# bench_graph_algorithms — mede build_path_stats, path_nodes,
# make_component_remap e find_bubbles em isolamento.
add_executable(bench_graph_algorithms tools/bench_graph_algorithms.cpp)
target_compile_options(bench_graph_algorithms PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_graph_algorithms PRIVATE -flto=auto)
target_link_libraries(bench_graph_algorithms PRIVATE ggql_ingest ${GGQL_BIN_ALLOCATOR_LIB})

# bench_fm_index — mede compute_payload_hash, search_kmer, load/save
# isolados de GfaSequenceIndex.
add_executable(bench_fm_index tools/bench_fm_index.cpp)
target_compile_options(bench_fm_index PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fno-strict-aliasing
)
target_link_options(bench_fm_index PRIVATE -flto=auto)
target_link_libraries(bench_fm_index PRIVATE ggql_storage ${GGQL_BIN_ALLOCATOR_LIB})



# ── ggql_ffi (Dart/Flutter Integration) ──────────────────
# Biblioteca compartilhada (.so/.dll/.dylib) para uso via Dart FFI.
# Encapsula instâncias C++ com extern "C" e ponteiros opacos.
add_library(ggql_ffi SHARED src/ffi/ffi.cpp)
target_compile_options(ggql_ffi PRIVATE
    -O3 -flto=auto -march=native -fopenmp -fPIC
)
target_link_options(ggql_ffi PRIVATE -flto=auto)
target_link_libraries(ggql_ffi PRIVATE ggql_query ${GGQL_FFI_ALLOCATOR_LIB})

# ── Testes unitários do ingestor (ARCH-5) ────────────────────────────────────
add_subdirectory(tests)

