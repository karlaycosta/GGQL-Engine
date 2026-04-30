// snarl_decomposition_vgstyle.cpp — Implementação fiel ao vg
// (vgteam/vg, src/integrated_snarl_finder.cpp).
//
// Diferenças com snarl_decomposition.cpp (Fases 1-6):
//   - Snarls representados como pares de EDGES (sides), não nodes
//   - Cactus tree explícito via Tsin 3-EC
//   - cycles_in_cactus(): DFS detectando back-edges → next_along_cycle map
//   - bridge forest construído via merge de cycles
//   - Decomposição recursiva via stack de SnarlChainFrame
//
// API: find_all_snarls_vgstyle(topo) → std::vector<SnarlRecord>
//      Retorna SnarlRecord com source/sink mapeados de volta para nodes
//      para compatibilidade com a API existente.
//
// Roadmap de implementação (em chunks):
//   ✓ Estrutura básica do arquivo + assinatura
//   ⏳ compute_next_along_cycle() — DFS detectando back-edges
//   ⏳ compute_bridge_forest() — merge cycles, identifica tip-to-tip
//   ⏳ decompose_via_stack() — emit snarls/chains recursivamente
//   ⏳ Mapeamento back de edges → SnarlRecord nodes
//   ⏳ Tests + comparação contra vg

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"
#include "ggql/storage/detail/tsin_3ecc.hpp"
#include "ggql/storage/detail/union_find.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace ggql::snarl_decomposition {

namespace {

// ── Side encoding (2 sides por nó GFA) ──────────────────────────────────
// (Reusado de snarl_decomposition.cpp — ver descrição lá.)
// side_id = (node_id << 1) | side_bit; side_bit=0=L, 1=R

// ── compute_next_along_cycle ──────────────────────────────────────────────
//
// Adaptação direta de cycles_in_cactus() do vg
// (vg/src/integrated_snarl_finder.cpp ~linha 533).
//
// DFS sobre o BIEDGED graph (sides) restrito a componentes 3-EC. Detecta
// back-edges (vizinho já na stack) e para cada uma, registra a sequência
// de edges no ciclo via next_along_cycle[prev] = next.
//
// Inputs:
//   bg:          biedged graph (já construído)
//   side_to_comp: mapa side → componente 3-EC (de Tsin)
//
// Output:
//   next_along_cycle: mapa side → side seguinte no ciclo
//   longest_cycle_per_comp: comp_id → (length, representative_side)
struct CycleData {
    std::unordered_map<uint64_t, uint64_t> next_along_cycle;
    std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>> longest_per_comp;
    // ↑ comp_id → (cycle_length_in_sides, representative_side)
};

// (placeholder: implementação real virá em chunks futuros)
[[maybe_unused]]
static CycleData compute_next_along_cycle(
    const std::vector<std::vector<uint64_t>>& bg_adj,
    const std::vector<uint32_t>& side_to_comp) {
    CycleData out;
    const size_t N2 = bg_adj.size();
    if (N2 == 0) { return out; }

    // DFS frame: side + edge_idx (próximo vizinho a explorar)
    // visited_stack_pos: side → posição no stack DFS atual (UINT32_MAX se não na stack)
    struct Frame { uint64_t here; uint32_t edge_idx; };
    std::vector<Frame> stk;
    stk.reserve(N2);
    std::vector<uint32_t> stack_pos(N2, UINT32_MAX);
    std::vector<uint8_t>  fully_visited(N2, 0);

    for (uint64_t root = 0; root < N2; ++root) {
        if (fully_visited[root] || stack_pos[root] != UINT32_MAX) { continue; }

        stk.push_back({root, 0u});
        stack_pos[root] = 0;

        while (!stk.empty()) {
            auto& fr = stk.back();
            const auto& neigh = bg_adj[static_cast<size_t>(fr.here)];

            if (fr.edge_idx < neigh.size()) {
                const uint64_t to = neigh[fr.edge_idx++];

                // Restringe ao mesmo componente 3-EC
                if (side_to_comp[to] != side_to_comp[fr.here]) { continue; }

                if (stack_pos[to] == UINT32_MAX && !fully_visited[to]) {
                    // Tree edge: novo frame
                    const uint32_t new_pos = static_cast<uint32_t>(stk.size());
                    stack_pos[to] = new_pos;
                    stk.push_back({to, 0u});
                } else if (stack_pos[to] != UINT32_MAX
                           && stack_pos[to] < stack_pos[fr.here]) {
                    // Back edge: fecha um ciclo
                    // Cycle path: stk[stack_pos[to]+1 .. fr.stack_pos] + back to 'to'
                    const uint32_t target_pos = stack_pos[to];
                    const uint32_t curr_pos   = stack_pos[fr.here];

                    if (curr_pos > target_pos) {
                        // Constroi cycle: edges são os "here" de cada frame entre
                        // target_pos+1 até curr_pos, fechando em 'to'.
                        uint64_t prev = stk[target_pos].here;
                        size_t cycle_len = 1; // contagem em sides
                        for (uint32_t i = target_pos + 1; i <= curr_pos; ++i) {
                            const uint64_t cur = stk[i].here;
                            out.next_along_cycle[prev] = cur;
                            prev = cur;
                            ++cycle_len;
                        }
                        // Fecha o ciclo
                        out.next_along_cycle[prev] = stk[target_pos].here;

                        // Atualiza longest_per_comp
                        const uint32_t cid = side_to_comp[fr.here];
                        auto& rec = out.longest_per_comp[cid];
                        if (cycle_len > rec.first) {
                            rec.first = cycle_len;
                            rec.second = stk[target_pos].here;
                        }
                    }
                }
            } else {
                // Pop frame
                stack_pos[fr.here] = UINT32_MAX;
                fully_visited[fr.here] = 1;
                stk.pop_back();
            }
        }
    }
    return out;
}

} // namespace

// API pública placeholder — substituirá find_all_snarls quando o
// refactoring estiver completo. Por ora, expõe a estrutura para testes
// incrementais.
std::vector<SnarlRecord> find_all_snarls_vgstyle(const GfaCsrTopology& /*topo*/,
                                                  int /*num_threads*/) {
    // Stub: retorna vazio até a implementação completa estar pronta.
    return {};
}

} // namespace ggql::snarl_decomposition
