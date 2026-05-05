#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <gbwt/utils.h>

namespace ggql {

// GbwtGroup — agrupa sequências de um job de construção do gBWT.
//
// em RAM:  in_ram_seqs não vazio → tmp_path ignorado.
// em disco: tmp_path aponta para um text_buffer_type temporário.
// sigma (vg-equivalent): max encoded value + 1 (vide gbwt_encode_node).
//   0 = desconhecido → usa num_nodes_ como fallback.
struct GbwtGroup {
    std::string tmp_path;
    std::vector<std::string> path_names;
    std::vector<gbwt::vector_type> in_ram_seqs;
    uint64_t sigma = 0;
};

// ── Encoding GBWT-bound: shift +1 para reservar valor 0 a ENDMARKER ─────────
//
// O encoding ingênuo `(node_id << 1) | orient` colide com gbwt::ENDMARKER == 0
// quando node_id == 0 e orient == 0 (forward). Em pangenomas, o nó interno 0
// é traversado por vários paths em forward → cada um seria silenciosamente
// dividido pelo iterator do gbwt::text_buffer_type, dessincronizando count
// de paths vs path_names_ (vide docs/validation_test_01_chm13_internal.md §4.1).
//
// Solução: shift de +1 em todos os encodings GBWT-bound. Valor 0 fica
// reservado a ENDMARKER. Smallest GBWT-stored value passa de 0 para 2.
//
// Convenção:
//   encode: ((internal_id + 1) << 1) | orient_bit  → mínimo = 2 (≠ ENDMARKER)
//   decode: (encoded >> 1) - 1
//
// Usar EXCLUSIVAMENTE em valores que vão para gbwt::text_buffer_type ou
// gbwt::DynamicGBWT. NÃO aplicar a E_fwd/E_bwd/CSR (não têm conceito de
// ENDMARKER e usam o encoding bruto).

// Codifica node_id + orient_bit em um único uint64_t para gBWT.
// Pré-condição: internal_id < UINT64_MAX/2 (overflow do shift wrap a
// 0 = ENDMARKER). Garantido na prática: nenhum dataset realístico
// (HPRC ~10⁹ nós) chega perto.
[[nodiscard]] inline constexpr uint64_t gbwt_encode_node(uint64_t internal_id,
                                                         uint64_t orient_bit) noexcept {
    return ((internal_id + 1ull) << 1) | (orient_bit & 1ull);
}

// Decodifica node_id de um valor encoded.
// Pré-condição: encoded > 0 (encoded != gbwt::ENDMARKER). Caller deve
// filtrar ENDMARKER antes — decode(0) retorna UINT64_MAX (wrap), sem
// significado semântico. Decisão deliberada: evita branch em hot path
// chamado ~10⁹ vezes em HPRC.
[[nodiscard]] inline constexpr uint64_t gbwt_decode_node(uint64_t encoded) noexcept {
    return (encoded >> 1) - 1ull;
}

// Extrai o bit de orientação (0 = forward, 1 = reverse). Sem pré-condição
// — válido para qualquer encoded, incluindo ENDMARKER.
[[nodiscard]] inline constexpr uint64_t gbwt_decode_orient(uint64_t encoded) noexcept {
    return encoded & 1ull;
}

} // namespace ggql
