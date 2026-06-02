// test_ffi_version.cpp — Fase A do sistema de versão (ggql_ffi_version).
//
// Cobre o símbolo handle-free ggql_ffi_version:
//   - caminho feliz: rc==0, abi_version == GGQL_FFI_ABI_VERSION (macro compilada),
//     struct_size ecoado de volta, strings não-vazias.
//   - out NULL → -1.
//   - struct_size insuficiente → -2 (padrão C5 dos outros structs).
//
// Handle-free: NÃO requer .idx nem engine carregada — roda sem dataset.
// Ver docs/plano_versionamento_engine.md.

#include "ffi/ffi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FALHA: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    // 1. Caminho feliz.
    GgqlVersionInfo v;
    v.struct_size = sizeof(GgqlVersionInfo);
    const int32_t rc = ggql_ffi_version(&v);
    check(rc == 0, "rc deve ser 0");
    check(v.abi_version == GGQL_FFI_ABI_VERSION,
          "abi_version (runtime) deve bater com a macro compilada (compile-time)");
    check(v.struct_size == sizeof(GgqlVersionInfo),
          "struct_size deve ser ecoado de volta intacto");
    check(v.version_str != nullptr && v.version_str[0] != '\0', "version_str não-vazio");
    check(v.git_hash   != nullptr && v.git_hash[0]   != '\0', "git_hash não-vazio");
    check(v.build_date != nullptr && v.build_date[0] != '\0', "build_date não-vazio");

    std::printf("version=%s  abi=%u  git=%s  built=%s  (%u.%u.%u)\n",
                v.version_str, v.abi_version, v.git_hash, v.build_date,
                v.version_major, v.version_minor, v.version_patch);

    // 2. out NULL → -1.
    check(ggql_ffi_version(nullptr) == -1, "out NULL deve retornar -1");

    // 3. struct_size insuficiente → -2 (caller compilado contra header antigo).
    GgqlVersionInfo small;
    small.struct_size = sizeof(size_t);  // menor que sizeof(GgqlVersionInfo)
    check(ggql_ffi_version(&small) == -2, "struct_size insuficiente deve retornar -2");

    std::printf("test_ffi_version OK\n");
    return 0;
}
