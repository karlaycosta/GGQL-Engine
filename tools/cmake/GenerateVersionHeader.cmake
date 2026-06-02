# GenerateVersionHeader.cmake
#
# Gera ${OUT} (ggql/version.h) a partir de ${TEMPLATE} (version.h.in), injetando
# a identidade de build (git short hash + flag dirty + data). Invocado de duas
# formas em CMakeLists.txt:
#   1. uma vez no configure (execute_process) — garante o header para IntelliSense;
#   2. a cada build via `cmake -P` (custom target ggql_version) — o hash nunca
#      envelhece entre commits.
#
# Vars esperadas (-D): GGQL_VERSION_STR, GGQL_VERSION_MAJOR/MINOR/PATCH,
#                      SRC_DIR, TEMPLATE, OUT.

find_package(Git QUIET)

set(GGQL_GIT_HASH "unknown")
set(GGQL_GIT_DIRTY 0)

if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${SRC_DIR}"
        OUTPUT_VARIABLE _hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND _hash)
        set(GGQL_GIT_HASH "${_hash}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${SRC_DIR}"
            OUTPUT_VARIABLE _status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_status)
            set(GGQL_GIT_DIRTY 1)
            set(GGQL_GIT_HASH "${GGQL_GIT_HASH}-dirty")
        endif()
    endif()
endif()

# Granularidade de DIA (não segundo): mantém o header estável dentro do mesmo dia
# para o mesmo commit, evitando recompilar ffi.cpp a cada build incremental.
string(TIMESTAMP GGQL_BUILD_DATE "%Y-%m-%d" UTC)

# Escreve num temporário e só substitui se mudou — copy_if_different preserva o
# mtime quando nada relevante mudou, então quem inclui o header não recompila à toa.
configure_file("${TEMPLATE}" "${OUT}.tmp" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
