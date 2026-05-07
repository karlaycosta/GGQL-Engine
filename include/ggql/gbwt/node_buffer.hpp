#pragma once

#include <cstdint>

#include <sdsl/int_vector_buffer.hpp>

namespace ggql {

// NodeBuffer — buffer em disco de valores codificados de nós com bit-packing
// de largura variável (`int_width` definido no construtor).
//
// É um alias direto sobre `sdsl::int_vector_buffer<0>`. O typedef
// `gbwt::text_buffer_type` no third_party é exatamente esse tipo, então o
// formato on-disk fica binary-compat com caches gerados antes da migração.
// O alias permite que o ingestor pare de incluir headers de gbwt sem mudar
// o layout dos arquivos temporários.
//
// API mínima usada pelo projeto (vide ingestor.cpp / path_index.cpp):
//   NodeBuffer(path, mode, buf_bytes, width)
//   push_back(value)
//   close()
//   range-for sobre os elementos (yields uint64_t)
using NodeBuffer = sdsl::int_vector_buffer<0>;

// Sentinel que separa sequências dentro de um NodeBuffer. Idêntico a
// gbwt::ENDMARKER (0). Mantido em 0 para preservar binary compat com caches
// existentes; gbwt_encode_node garante que valores reais ≥ 2.
inline constexpr std::uint64_t kEndmarker = 0;

}  // namespace ggql
