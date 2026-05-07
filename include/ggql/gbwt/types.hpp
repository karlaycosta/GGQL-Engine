#pragma once

#include <cstdint>
#include <vector>

// Tipos do subsistema interno de paths/haplótipos. Substituem o que vinha
// de `<gbwt/utils.h>` quando o projeto ainda dependia da libgbwt externa.
//
// Convenção de tamanho: `node_type` é uint32_t (equivalente a
// `GBWT_SAVE_MEMORY` ativo no upstream). Suficiente para grafos com até
// ~2³² símbolos encoded — o encoding `gbwt_encode_node(id, orient) =
// ((id+1)<<1)|orient` consome 1 bit a mais e exige id < 2³¹ - 1.
// Datasets reais (HPRC ~10⁹ nós) ficam confortáveis nessa janela.

namespace ggql::gbwt {

using size_type   = std::uint64_t;
using node_type   = std::uint32_t;
using vector_type = std::vector<node_type>;

// Sentinela que separa sequências em buffers concatenados. Mantido em 0
// para preservar binary compat com caches existentes — `gbwt_encode_node`
// garante que valores reais ≥ 2.
inline constexpr node_type ENDMARKER = 0;

}  // namespace ggql::gbwt
