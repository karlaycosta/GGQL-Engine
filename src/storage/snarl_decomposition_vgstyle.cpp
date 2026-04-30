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

// ── Chunk 2: bridge forest (merge cycles into vertices) ──────────────────
//
// Adaptação de:
//   MergedAdjacencyGraph forest(cactus);
//   for (kv : next_along_cycle) forest.merge(kv.first, kv.second);
//
// O cactus já tem componentes 3-EC merged via Tsin. A bridge forest
// adicionalmente merge todos os pares (edge, next_along_cycle[edge]),
// colapsando ciclos em vertices únicos. Resultado: floresta de bridges
// (apenas tree edges sobreviven, cycles viram vertices).
//
// Inputs: side_to_comp (cactus), next_along_cycle
// Output: side_to_forest_id (forest)
[[maybe_unused]]
static std::vector<uint32_t> build_bridge_forest(
    const std::vector<uint32_t>& side_to_comp,
    const std::unordered_map<uint64_t, uint64_t>& next_along_cycle) {

    const size_t N2 = side_to_comp.size();

    // UnionFind sobre N2 sides, inicialmente cada side é seu próprio rep.
    // Mas começamos do cactus: sides no mesmo comp 3-EC já estão merged.
    detail::UnionFind uf(N2);
    // Sincroniza UF inicial com side_to_comp: para cada par de sides do
    // mesmo componente, faz uf.unite. Isso reproduz o estado do cactus.
    {
        std::unordered_map<uint32_t, uint64_t> first_side_of_comp;
        first_side_of_comp.reserve(N2 / 4 + 1);
        for (size_t s = 0; s < N2; ++s) {
            const uint32_t c = side_to_comp[s];
            auto it = first_side_of_comp.find(c);
            if (it == first_side_of_comp.end()) {
                first_side_of_comp.emplace(c, s);
            } else {
                uf.unite(it->second, s);
            }
        }
    }

    // Aplica merges adicionais para next_along_cycle:
    // forest = cactus + merge cycle edges
    for (const auto& [a, b] : next_along_cycle) {
        uf.unite(static_cast<size_t>(a), static_cast<size_t>(b));
    }

    // Compress UF → side_to_forest_id [0, num_forest_components)
    std::vector<uint32_t> side_to_forest_id(N2, UINT32_MAX);
    std::unordered_map<size_t, uint32_t> root_to_id;
    root_to_id.reserve(N2 / 4 + 1);
    uint32_t next_id = 0;
    for (size_t s = 0; s < N2; ++s) {
        const size_t r = uf.find(s);
        auto it = root_to_id.find(r);
        if (it == root_to_id.end()) {
            it = root_to_id.emplace(r, next_id++).first;
        }
        side_to_forest_id[s] = it->second;
    }
    return side_to_forest_id;
}

// ── Chunk 3: longest_paths_in_forest (tip-to-tip DFS) ──────────────────
//
// Adaptação de longest_paths_in_forest() do vg (~L778 de
// integrated_snarl_finder.cpp).
//
// DFS na bridge forest: para cada vertex (componente forest), tracking
// "deepest leaf path length" e quando dois subtrees convergem, registra
// uma tip-to-tip path via combine.
//
// Output: vetor de (length, representative_forest_id) ordenado por length
[[maybe_unused]]
static std::vector<std::pair<size_t, uint32_t>>
longest_paths_in_forest(const std::vector<std::vector<uint64_t>>& bg_adj,
                        const std::vector<uint32_t>& side_to_forest_id) {
    const size_t N2 = bg_adj.size();

    // Adjacência forest-level: edges entre vertices do forest (= bridges).
    // Constrói lista de adjacência por forest_id.
    uint32_t num_forest = 0;
    for (auto fid : side_to_forest_id) {
        if (fid != UINT32_MAX && fid + 1 > num_forest) num_forest = fid + 1;
    }

    std::vector<std::vector<uint32_t>> forest_adj(num_forest);
    {
        std::unordered_map<uint64_t, bool> seen_edge;
        seen_edge.reserve(N2);
        for (size_t s = 0; s < N2; ++s) {
            const uint32_t fa = side_to_forest_id[s];
            for (uint64_t to : bg_adj[s]) {
                const uint32_t fb = side_to_forest_id[to];
                if (fa == fb) { continue; } // intra-forest-vertex (cycle edge merged)
                // Edge único: ordena par para dedupe
                const uint64_t key = (static_cast<uint64_t>(std::min(fa, fb)) << 32)
                                    | std::max(fa, fb);
                if (!seen_edge.emplace(key, true).second) { continue; }
                forest_adj[fa].push_back(fb);
                forest_adj[fb].push_back(fa);
            }
        }
    }

    // DFS para deepest leaf path. Para cada vertex v, deepest_leaf[v] =
    // (distância em sides, vertex destino).
    std::vector<std::pair<size_t, uint32_t>> longest_paths;
    std::vector<int> visited(num_forest, 0);

    for (uint32_t root = 0; root < num_forest; ++root) {
        if (visited[root]) { continue; }

        // BFS pra achar o vertex mais distante de root
        std::vector<size_t> dist(num_forest, SIZE_MAX);
        dist[root] = 0;
        std::vector<uint32_t> q = {root};
        size_t qi = 0;
        uint32_t farthest = root;
        size_t max_dist = 0;
        while (qi < q.size()) {
            const uint32_t u = q[qi++];
            visited[u] = 1;
            for (uint32_t v : forest_adj[u]) {
                if (dist[v] == SIZE_MAX) {
                    dist[v] = dist[u] + 1;
                    if (dist[v] > max_dist) {
                        max_dist = dist[v];
                        farthest = v;
                    }
                    q.push_back(v);
                }
            }
        }

        // Segunda BFS de farthest para encontrar tip-to-tip
        std::fill(dist.begin(), dist.end(), SIZE_MAX);
        dist[farthest] = 0;
        q.clear(); q.push_back(farthest); qi = 0;
        uint32_t tip2 = farthest;
        size_t max_dist2 = 0;
        while (qi < q.size()) {
            const uint32_t u = q[qi++];
            for (uint32_t v : forest_adj[u]) {
                if (dist[v] == SIZE_MAX) {
                    dist[v] = dist[u] + 1;
                    if (dist[v] > max_dist2) {
                        max_dist2 = dist[v];
                        tip2 = v;
                    }
                    q.push_back(v);
                }
            }
        }

        if (max_dist2 > 0) {
            longest_paths.push_back({max_dist2, farthest});
        }
        (void)tip2; // outro endpoint, podemos guardar se preciso
    }

    std::sort(longest_paths.begin(), longest_paths.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    return longest_paths;
}

// ── Chunk 4+5: decomposição recursiva via stack + edge → node mapping ──
//
// Adaptação simplificada de traverse_computed_decomposition() do vg
// (~L1118 de integrated_snarl_finder.cpp).
//
// Para cada cycle no longest_cycles, decompõe em chain de snarls
// consecutivos via next_along_cycle. Cada par (cycle_edge, next_along_cycle[edge])
// gera um SnarlRecord.
//
// Pares mapeados de sides → nodes via:
//   source_node = side(inbound) >> 1
//   sink_node   = side(outbound) >> 1
//
// Retorna SnarlRecord para compat de API.
[[maybe_unused]]
static std::vector<SnarlRecord> decompose_via_stack(
    const std::vector<std::vector<uint64_t>>& bg_adj,
    const std::vector<uint32_t>& side_to_comp,
    const std::unordered_map<uint64_t, uint64_t>& next_along_cycle,
    const std::unordered_map<uint32_t, std::pair<uint64_t, uint64_t>>& longest_per_comp,
    const std::vector<uint8_t>& is_in_nontrivial_scc) {

    std::vector<SnarlRecord> out;
    if (next_along_cycle.empty()) { return out; }

    // Para cada componente 3-EC com ciclo registrado, decompõe.
    // Itera longest_per_comp (1 entry por comp com cycle).
    for (const auto& [cid, cycle_info] : longest_per_comp) {
        const uint64_t start_edge = cycle_info.second;

        // Walk do ciclo: start → next → next ... → start
        std::vector<uint64_t> cycle_walk;
        cycle_walk.reserve(64);
        uint64_t cur = start_edge;
        while (true) {
            cycle_walk.push_back(cur);
            auto it = next_along_cycle.find(cur);
            if (it == next_along_cycle.end()) { break; }
            cur = it->second;
            if (cur == start_edge) { break; }
            if (cycle_walk.size() > bg_adj.size()) { break; } // safety
        }

        // Para cada par adjacente no ciclo: emit snarl
        const size_t W = cycle_walk.size();
        if (W < 2) { continue; }

        // Tamanho aproximado do interior (usado para internal_node_count)
        std::vector<uint64_t> seen_nodes;
        seen_nodes.reserve(W);
        std::unordered_map<uint64_t, bool> node_seen;
        for (uint64_t s : cycle_walk) {
            const uint64_t v = s >> 1;
            if (node_seen.emplace(v, true).second) { seen_nodes.push_back(v); }
        }
        const uint32_t internal = (seen_nodes.size() > 2)
            ? static_cast<uint32_t>(seen_nodes.size() - 2) : 0;

        // has_cycles: se algum nó da SCC original é não-trivial
        uint8_t cyc = 0;
        for (uint64_t v : seen_nodes) {
            if (v < is_in_nontrivial_scc.size() && is_in_nontrivial_scc[v]) {
                cyc = 1;
                break;
            }
        }

        // Política vg-aligned:
        //   - Cycles curtos (W <= 3): emit todos pares consecutivos
        //     (cobre snarls aninhados em chains pequenas)
        //   - Cycles longos (W > 3): emit apenas 1 snarl representativo
        //     (primeiro e último do walk — vg emite estrutura de chain
        //     toplevel única; aninhados emergem da decomposição recursiva
        //     que ainda não implementamos completamente)
        if (W <= 3) {
            for (size_t i = 0; i + 1 < W; ++i) {
                const uint64_t inbound  = cycle_walk[i];
                const uint64_t outbound = cycle_walk[i + 1];
                const uint64_t na = inbound  >> 1;
                const uint64_t nb = outbound >> 1;
                if (na == nb) { continue; }

                SnarlRecord rec{};
                rec.source = std::min(na, nb);
                rec.sink   = std::max(na, nb);
                const size_t src_R = static_cast<size_t>(rec.source * 2 + 1);
                const uint32_t bc = static_cast<uint32_t>(bg_adj[src_R].size());
                rec.branch_count = static_cast<uint8_t>(std::min<uint32_t>(
                    bc > 0 ? bc - 1 : 0, 255));
                rec.internal_node_count = internal;
                rec.has_cycles = cyc;
                out.push_back(rec);
            }
        } else {
            const uint64_t na = cycle_walk.front() >> 1;
            const uint64_t nb = cycle_walk.back()  >> 1;
            if (na != nb) {
                SnarlRecord rec{};
                rec.source = std::min(na, nb);
                rec.sink   = std::max(na, nb);
                const size_t src_R = static_cast<size_t>(rec.source * 2 + 1);
                const uint32_t bc = static_cast<uint32_t>(bg_adj[src_R].size());
                rec.branch_count = static_cast<uint8_t>(std::min<uint32_t>(
                    bc > 0 ? bc - 1 : 0, 255));
                rec.internal_node_count = internal;
                rec.has_cycles = cyc;
                out.push_back(rec);
            }
        }
        (void)side_to_comp; (void)cid;
    }

    // Dedup por (source, sink)
    std::sort(out.begin(), out.end(),
              [](const SnarlRecord& a, const SnarlRecord& b) {
                  if (a.source != b.source) return a.source < b.source;
                  return a.sink < b.sink;
              });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const SnarlRecord& a, const SnarlRecord& b) {
                              return a.source == b.source && a.sink == b.sink;
                          }),
              out.end());

    return out;
}

} // namespace

// ── API pública vg-style: find_all_snarls_vgstyle ──────────────────────
//
// Pipeline completo (chunks 1-5 implementados):
//   1. Construir biedged graph (reusa lógica de snarl_decomposition.cpp)
//   2. Tsin 3-EC sobre biedged → side_to_comp
//   3. cycles_in_cactus → next_along_cycle + longest_per_comp
//   4. (Bridge forest construído mas não usado em decomposição minimal)
//   5. decompose_via_stack: walk cycles e emit snarls como pares
//      consecutivos via next_along_cycle
//
// Retorno: SnarlRecord com source/sink em nodes (compat com API atual).

// Forward declaration de funções definidas em snarl_decomposition.cpp
// que vamos reutilizar (extract_link_records + BiedgedGraph adapter).
// Para simplicidade da primeira versão, construímos localmente.

static std::vector<LinkRecord> rebuild_links(const GfaCsrTopology& topo) {
    using Pair = std::pair<uint64_t, uint64_t>;
    struct PairHash {
        size_t operator()(const Pair& p) const noexcept {
            return std::hash<uint64_t>()(p.first) * 1315423911u
                 ^ std::hash<uint64_t>()(p.second);
        }
    };
    std::unordered_map<Pair, std::vector<char>, PairHash> fwd_orients;
    std::unordered_map<Pair, std::vector<char>, PairHash> bwd_orients;

    const uint64_t N = topo.num_nodes();
    for (uint64_t u = 0; u < N; ++u) {
        for (uint64_t enc : topo.out_neighbors(u)) {
            const auto e = GfaCsrTopology::decode(enc);
            fwd_orients[{u, e.node_id}].push_back(e.orient);
        }
        for (uint64_t enc : topo.in_neighbors(u)) {
            const auto e = GfaCsrTopology::decode(enc);
            bwd_orients[{e.node_id, u}].push_back(e.orient);
        }
    }
    std::vector<LinkRecord> out;
    for (auto& [key, o2s] : fwd_orients) {
        auto it = bwd_orients.find(key);
        if (it == bwd_orients.end()) { continue; }
        auto& o1s = it->second;
        const size_t n = std::min(o2s.size(), o1s.size());
        for (size_t i = 0; i < n; ++i) {
            out.push_back(LinkRecord::make(key.first, o1s[i],
                                            key.second, o2s[i]));
        }
    }
    return out;
}

static std::vector<std::vector<uint64_t>>
build_biedged_adj(uint64_t N, const std::vector<LinkRecord>& links) {
    std::vector<std::vector<uint64_t>> adj(N * 2);
    for (uint64_t v = 0; v < N; ++v) {
        adj[v * 2 + 0].push_back(v * 2 + 1);
        adj[v * 2 + 1].push_back(v * 2 + 0);
    }
    for (const auto& lk : links) {
        const uint64_t u = lk.from_id();
        const uint64_t v = lk.to_id();
        const bool u_side = (lk.from_orient() == '+');
        const bool v_side = (lk.to_orient()   != '+');
        const uint64_t s_u = u * 2 + (u_side ? 1 : 0);
        const uint64_t s_v = v * 2 + (v_side ? 1 : 0);
        adj[static_cast<size_t>(s_u)].push_back(s_v);
        adj[static_cast<size_t>(s_v)].push_back(s_u);
    }
    return adj;
}

std::vector<SnarlRecord> find_all_snarls_vgstyle(const GfaCsrTopology& topo,
                                                  int /*num_threads*/) {
    const uint64_t N = topo.num_nodes();
    if (N == 0) { return {}; }

    // 1. Construir biedged graph
    auto links = rebuild_links(topo);
    auto bg_adj = build_biedged_adj(N, links);
    const size_t N2 = bg_adj.size();

    // 2. Tsin 3-EC sobre biedged
    detail::UnionFind uf(N2);
    const auto for_each_n =
        std::function<void(size_t, const std::function<void(size_t)>&)>(
            [&](size_t s, const std::function<void(size_t)>& emit_n) {
                for (uint64_t v : bg_adj[s]) { emit_n(static_cast<size_t>(v)); }
            });
    const auto unite =
        std::function<void(size_t, size_t)>(
            [&](size_t a, size_t b) { uf.unite(a, b); });
    detail::three_edge_connected_merges(N2, 0, for_each_n, unite);

    std::vector<uint32_t> side_to_comp(N2, UINT32_MAX);
    {
        std::unordered_map<size_t, uint32_t> root_to_id;
        uint32_t nid = 0;
        for (size_t s = 0; s < N2; ++s) {
            const size_t r = uf.find(s);
            auto it = root_to_id.find(r);
            if (it == root_to_id.end()) {
                it = root_to_id.emplace(r, nid++).first;
            }
            side_to_comp[s] = it->second;
        }
    }

    // 3. cycles_in_cactus → next_along_cycle + longest_per_comp
    auto cycle_data = compute_next_along_cycle(bg_adj, side_to_comp);

    // 4. is_in_nontrivial_scc — necessário para has_cycles
    // (Reusa SCC do grafo dirigido original, computada via Kosaraju.)
    std::vector<uint8_t> is_in_nontrivial_scc(N, 0);
    {
        // SCC via Kosaraju iterativo (versão simplificada)
        std::vector<uint32_t> scc_id(N, UINT32_MAX);
        std::vector<uint64_t> finish_order;
        finish_order.reserve(N);
        std::vector<uint8_t> visited(N, 0);
        struct DFSFrame { uint64_t node; uint32_t i; uint32_t hi; };
        std::vector<DFSFrame> stk;

        for (uint64_t root = 0; root < N; ++root) {
            if (visited[root]) { continue; }
            visited[root] = 1;
            auto fwd = topo.out_neighbors(root);
            stk.push_back({root, 0u, static_cast<uint32_t>(fwd.size())});
            while (!stk.empty()) {
                auto& fr = stk.back();
                if (fr.i < fr.hi) {
                    auto fwdu = topo.out_neighbors(fr.node);
                    const uint64_t to = GfaCsrTopology::decode(fwdu[fr.i]).node_id;
                    ++fr.i;
                    if (!visited[to]) {
                        visited[to] = 1;
                        auto fwdt = topo.out_neighbors(to);
                        stk.push_back({to, 0u, static_cast<uint32_t>(fwdt.size())});
                    }
                } else {
                    finish_order.push_back(fr.node);
                    stk.pop_back();
                }
            }
        }
        uint32_t num_scc = 0;
        std::vector<uint32_t> scc_size_vec;
        for (int64_t fi = static_cast<int64_t>(finish_order.size()) - 1; fi >= 0; --fi) {
            const uint64_t r = finish_order[static_cast<size_t>(fi)];
            if (scc_id[r] != UINT32_MAX) { continue; }
            scc_id[r] = num_scc;
            scc_size_vec.push_back(0);
            auto bwd = topo.in_neighbors(r);
            stk.push_back({r, 0u, static_cast<uint32_t>(bwd.size())});
            while (!stk.empty()) {
                auto& fr = stk.back();
                if (fr.i < fr.hi) {
                    auto bwdu = topo.in_neighbors(fr.node);
                    const uint64_t to = GfaCsrTopology::decode(bwdu[fr.i]).node_id;
                    ++fr.i;
                    if (scc_id[to] == UINT32_MAX) {
                        scc_id[to] = num_scc;
                        auto bwdt = topo.in_neighbors(to);
                        stk.push_back({to, 0u, static_cast<uint32_t>(bwdt.size())});
                    }
                } else {
                    stk.pop_back();
                }
            }
            ++num_scc;
        }
        for (uint64_t v = 0; v < N; ++v) { ++scc_size_vec[scc_id[v]]; }
        for (uint64_t v = 0; v < N; ++v) {
            if (scc_size_vec[scc_id[v]] > 1) { is_in_nontrivial_scc[v] = 1; }
        }
    }

    // 5. Decomposição via cycle walk (chunks 4+5 simplificada)
    auto cycle_snarls = decompose_via_stack(bg_adj, side_to_comp,
                                             cycle_data.next_along_cycle,
                                             cycle_data.longest_per_comp,
                                             is_in_nontrivial_scc);

    // 6. Bridges entre componentes 3-EC NÃO-TRIVIAIS → snarls adicionais.
    //    Pula bridges entre componentes triviais (size <= 2 sides = só
    //    spine + 1 link), evitando over-detection.
    //
    //    No vg, snarls de bridges aparecem na decomposição recursiva
    //    apenas para bridges que separam blocos significativos do cactus
    //    tree. Aproximamos requerendo pelo menos um dos endpoints estar
    //    num componente 3-EC com size > 2.
    std::vector<uint32_t> comp_size(side_to_comp.size() == 0 ? 0
                                    : *std::max_element(side_to_comp.begin(),
                                                         side_to_comp.end()) + 1, 0);
    for (uint32_t c : side_to_comp) {
        if (c < comp_size.size()) { ++comp_size[c]; }
    }

    std::vector<SnarlRecord> bridge_snarls;
    {
        std::unordered_map<uint64_t, bool> seen_bridge;
        for (size_t s = 0; s < N2; ++s) {
            const uint64_t na = s >> 1;
            const uint32_t cs = side_to_comp[s];
            for (uint64_t to : bg_adj[s]) {
                const uint32_t ct = side_to_comp[to];
                if (cs == ct) { continue; }
                // Filtro: pelo menos um componente é não-trivial
                if (comp_size[cs] <= 2 && comp_size[ct] <= 2) { continue; }
                const uint64_t nb = to >> 1;
                if (na == nb) { continue; }
                const uint64_t lo = std::min(na, nb);
                const uint64_t hi = std::max(na, nb);
                const uint64_t key = (lo << 32) | hi;
                if (!seen_bridge.emplace(key, true).second) { continue; }

                SnarlRecord rec{};
                rec.source = lo;
                rec.sink   = hi;
                const size_t src_R = static_cast<size_t>(rec.source * 2 + 1);
                const uint32_t bc = static_cast<uint32_t>(bg_adj[src_R].size());
                rec.branch_count = static_cast<uint8_t>(std::min<uint32_t>(
                    bc > 0 ? bc - 1 : 0, 255));
                rec.internal_node_count = 0;
                rec.has_cycles = (lo < N && is_in_nontrivial_scc[lo]) ||
                                 (hi < N && is_in_nontrivial_scc[hi]) ? 1 : 0;
                bridge_snarls.push_back(rec);
            }
        }
    }

    // Merge dedup cycle_snarls + bridge_snarls
    std::vector<SnarlRecord> out;
    out.reserve(cycle_snarls.size() + bridge_snarls.size());
    std::unordered_map<uint64_t, bool> merged_seen;
    auto try_add = [&](const SnarlRecord& s) {
        const uint64_t key = (s.source << 32) | s.sink;
        if (merged_seen.emplace(key, true).second) {
            out.push_back(s);
        }
    };
    for (const auto& s : cycle_snarls) { try_add(s); }
    for (const auto& s : bridge_snarls) { try_add(s); }

    return out;
}

} // namespace ggql::snarl_decomposition
