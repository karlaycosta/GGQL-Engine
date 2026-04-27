# CMakeLists.txt — wrapper mínimo para vgteam/sdsl-lite
#
# Compila os fontes de third_party/sdsl-lite como biblioteca estática `sdsl`,
# substituindo o sdsl do sistema para todos os targets que linkam "sdsl".
# O vgteam fork adiciona simple_sds.hpp e int_vector/sd_vector::simple_sds_*,
# exigidos pelo libgbwt (third_party/gbwt).
#
# Dependências providas pelo sistema:
#   - divsufsort / divsufsort64 (libdivsufsort-dev)
#
# NÃO usa o CMakeLists.txt upstream do sdsl-lite (que exige googletest, faz
# configure de vários arquivos e força CMAKE_BUILD_TYPE=Release).

cmake_minimum_required(VERSION 3.16)
project(sdsl_vgteam_wrapper LANGUAGES CXX C)

set(SDSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdsl-lite")

# ── Gera structure_tree.cpp a partir do template (.cmake → .cpp) ──────────────
# O template usa @PROJECT_SOURCE_DIR@ apenas num literal de string para gerar
# HTML de debug (caminho do d3.min.js). Não afeta compilação nem linking.
set(PROJECT_SOURCE_DIR "${SDSL_DIR}")
configure_file(
    "${SDSL_DIR}/lib/structure_tree.cpp.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/structure_tree.cpp"
    @ONLY
)

# ── Fontes da biblioteca ───────────────────────────────────────────────────────
file(GLOB _SDSL_SOURCES
    "${SDSL_DIR}/lib/*.cpp"          # todos exceto *.cmake (extensão diferente)
)
# Adiciona o .cpp gerado
list(APPEND _SDSL_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/structure_tree.cpp")

# ── Target estático ───────────────────────────────────────────────────────────
add_library(sdsl STATIC ${_SDSL_SOURCES})

# Headers vgteam (inclui simple_sds.hpp, sd_vector corrigido, etc.)
target_include_directories(sdsl PUBLIC
    $<BUILD_INTERFACE:${SDSL_DIR}/include>
)

# divsufsort.h e divsufsort64.h — hdr do sistema (/usr/include/x86_64-linux-gnu)
# Estão no include path padrão; tornamos explícito por robustez.
target_include_directories(sdsl PRIVATE
    "/usr/include/x86_64-linux-gnu"
)

set_target_properties(sdsl PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_compile_options(sdsl PRIVATE
    -O3 -march=native -fno-strict-aliasing
    # silencia warnings dos fontes upstream
    -Wno-attributes -Wno-unused-parameter
)

# Link com divsufsort (construct_sa.cpp chama divsufsort() / divsufsort64())
find_library(DIVSUFSORT_LIB   divsufsort   REQUIRED)
find_library(DIVSUFSORT64_LIB divsufsort64 REQUIRED)
target_link_libraries(sdsl PUBLIC "${DIVSUFSORT_LIB}" "${DIVSUFSORT64_LIB}")
