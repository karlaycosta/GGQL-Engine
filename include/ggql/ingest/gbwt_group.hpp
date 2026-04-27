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
// sigma (vg-equivalent): max node_id visto neste grupo + 1.
//   0 = desconhecido → usa num_nodes_ como fallback.
struct GbwtGroup {
    std::string tmp_path;
    std::vector<std::string> path_names;
    std::vector<gbwt::vector_type> in_ram_seqs;
    uint64_t sigma = 0;
};

} // namespace ggql
