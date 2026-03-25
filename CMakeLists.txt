cmake_minimum_required(VERSION 3.16)
project(ggql_engine LANGUAGES CXX)

# Requer GCC 14+ para suporte completo a C++23.
# Passe o compilador na linha de comando se necessário:
#   cmake -B build -DCMAKE_CXX_COMPILER=g++-14
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Gera build/compile_commands.json — usado pelo IntelliSense do VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependências ─────────────────────────────────────────
# PEGTL (header-only, git submodule em third_party/pegtl)
# Clone com: git submodule update --init
add_library(pegtl INTERFACE)
target_include_directories(pegtl INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/pegtl/include
)

# BooPHF agora está em include/mphf/ — coberto pelo include_directories global abaixo.
# third_party/bbhash não é mais necessário.

# xxHash agora está em include/mphf/xxh64.hpp — coberto pelo include_directories global.
# third_party/xxhash não é mais necessário.

# ── Includes do próprio projeto ───────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ── bench_ingest ─────────────────────────────────────────
add_executable(bench_ingest src/ingest/bench_ingest.cpp)
target_compile_options(bench_ingest PRIVATE -O3 -march=native -fopenmp -flto=auto -fno-strict-aliasing)
target_link_options(bench_ingest PRIVATE -flto=auto)
target_link_libraries(bench_ingest PRIVATE gomp pthread sqlite3 sdsl tbb)

# ── ggql_parser_dev ──────────────────────────────────────
# Driver de desenvolvimento/teste da gramática GGQL (PEGTL).
# Executar: ./build/ggql_parser_dev
add_executable(ggql_parser_dev src/parser/ggql_parser_dev.cpp)
target_compile_options(ggql_parser_dev PRIVATE -O2 -march=native)
target_link_libraries(ggql_parser_dev PRIVATE pegtl)

# ── ggql_engine_dev ──────────────────────────────────────
# Driver de integração: parser + QueryPlanner + topologia CSR + SQLite.
# Executar: ./build/ggql_engine_dev
add_executable(ggql_engine_dev src/ggql_engine_dev.cpp)
target_compile_options(ggql_engine_dev PRIVATE -O2 -march=native -fopenmp)
target_link_libraries(ggql_engine_dev PRIVATE pegtl sdsl sqlite3 gomp pthread)

# ── ggql_repl ────────────────────────────────────────────
# REPL interativo: parse GGQL + QueryPlanner + ingest GFA.
# Uso: ./build/ggql_repl [--gfa <arquivo.gfa>] [--db <sqlite>] [--threads N]
add_executable(ggql_repl src/repl/ggql_repl.cpp)
target_compile_options(ggql_repl PRIVATE -O2 -march=native -fopenmp)
target_link_libraries(ggql_repl PRIVATE pegtl sdsl sqlite3 gomp pthread tbb)

# ── bench_bubbles ─────────────────────────────────────────
# Benchmark isolado de find_tips / find_repeats / find_bubbles.
# Uso: ./build/bench_bubbles <arquivo.gfa> [threads] [max_depth] [runs]
add_executable(bench_bubbles src/storage/bench_bubbles.cpp)
target_compile_options(bench_bubbles PRIVATE -O3 -march=native -fopenmp -flto=auto)
target_link_options(bench_bubbles PRIVATE -flto=auto)
target_link_libraries(bench_bubbles PRIVATE gomp pthread sdsl sqlite3 tbb)

# ── Testes ────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
