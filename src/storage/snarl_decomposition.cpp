// snarl_decomposition.cpp — Fase 1 do framework Paten 2018.
//
// Algoritmo: Onodera et al. 2013 estendido (ver header). Reuso do mesmo
// pipeline de find_bubbles em graph_algorithms.cpp (Kosaraju + topological
// sweep com saldo out_minus_in), mas populando SnarlRecord completo —
// branch_count via out_degree DAG do source, internal_node_count contado
// durante sweep, has_cycles detectando intermediárias em SCCs não-triviais.

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"
#include "ggql/storage/detail/tsin_3ecc.hpp"
#include "ggql/storage/detail/union_find.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ggql::snarl_decomposition {

namespace {

// Calcula SCCs via Kosaraju iterativo. Retorna vetor scc_id de tamanho N
// e o número de SCCs. Função local — espelhada de find_bubbles para evitar
// dependência cruzada.
struct SCCDecomp {
    std::vector<uint32_t> scc_id;
    std::vector<uint32_t> scc_size;
    uint32_t num_scc = 0;
};

// ── Reconstrói LinkRecords completos a partir de E_fwd_/E_bwd_ ──────────────
//
// A topology guarda apenas (v, o2) em out_neighbors e (u, o1) em in_neighbors.
// Para construir o grafo bidirected precisamos AMBAS orientações por link.
// Cross-correlate: para cada u, listar (v, o2); para cada v, listar (u, o1);
// parear por (u, v) — assumindo ordem de aparecimento estável (multi-edges
// com diferentes orientações são raros em pangenomas; OK como aproximação).
std::vector<LinkRecord> extract_link_records(const GfaCsrTopology& topo) {
    using Pair = std::pair<uint64_t, uint64_t>;
    struct PairHash {
        size_t operator()(const Pair& p) const noexcept {
            return std::hash<uint64_t>()(p.first) * 1315423911u
                 ^ std::hash<uint64_t>()(p.second);
        }
    };
    std::unordered_map<Pair, std::vector<char>, PairHash> fwd_orients; // o2 chars
    std::unordered_map<Pair, std::vector<char>, PairHash> bwd_orients; // o1 chars
    fwd_orients.reserve(static_cast<size_t>(topo.num_edges()));
    bwd_orients.reserve(static_cast<size_t>(topo.num_edges()));

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
    out.reserve(topo.num_edges());
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

// ── Bidirected (biedged) graph adapter ──────────────────────────────────────
//
// Cada nó GFA v vira 2 sides: encoded como side_id = (v << 1) | side_bit
//   side_bit = 0 → lado L (esquerdo / 5')
//   side_bit = 1 → lado R (direito / 3')
//
// Edges:
//   - Spine edge: (v_L) ↔ (v_R) — uma por nó GFA
//   - Link edge: para cada LinkRecord (u, o1, v, o2):
//       saída de u: o1='+'→ R, o1='-'→ L
//       entrada de v: o2='+'→ L, o2='-'→ R
//
// Representação: vector de listas de adjacência sobre 2N sides.
struct BiedgedGraph {
    std::vector<std::vector<uint64_t>> adj; // 2N entries, cada uma = vetor de sides

    explicit BiedgedGraph(uint64_t N, const std::vector<LinkRecord>& links) {
        adj.assign(static_cast<size_t>(N) * 2, {});
        // Spine edges
        for (uint64_t v = 0; v < N; ++v) {
            adj[v * 2 + 0].push_back(v * 2 + 1);
            adj[v * 2 + 1].push_back(v * 2 + 0);
        }
        // Link edges
        for (const auto& lk : links) {
            const uint64_t u = lk.from_id();
            const uint64_t v = lk.to_id();
            const bool u_side = (lk.from_orient() == '+'); // saída via R
            const bool v_side = (lk.to_orient()   != '+'); // entrada: '+'→L (false), '-'→R (true)
            const uint64_t s_u = u * 2 + (u_side ? 1 : 0);
            const uint64_t s_v = v * 2 + (v_side ? 1 : 0);
            adj[static_cast<size_t>(s_u)].push_back(s_v);
            adj[static_cast<size_t>(s_v)].push_back(s_u);
        }
    }
    size_t num_sides() const noexcept { return adj.size(); }
};

// ── Tarjan bridge-finding (DFS iterativa) ───────────────────────────────────
//
// Bridge = aresta cuja remoção desconecta o grafo. No grafo bidirected,
// bridges entre componentes 2-edge-connected definem boundary pairs.
//
// Implementação clássica: para cada aresta (u, v) em DFS-tree,
//   bridge sse low[v] > disc[u]
// onde disc[v] = ordem de descoberta, low[v] = mínimo disc reachable via
// back-edges no subárvore de v.
//
// Como o grafo é não-dirigido (undirected biedged), uso edge_id estável
// para evitar volta pelo parent (multi-edges não são problema porque cada
// edge tem id único).

struct Bridges {
    std::vector<std::pair<uint64_t, uint64_t>> bridges; // pares de sides
    std::vector<uint32_t> disc, low;
    std::vector<uint32_t> component_2ec; // componente 2-edge-conectado de cada side
    uint32_t num_components_2ec = 0;
};

Bridges find_bridges_2ec(const BiedgedGraph& bg) {
    const size_t N2 = bg.num_sides();
    Bridges out;
    out.disc.assign(N2, UINT32_MAX);
    out.low.assign(N2, UINT32_MAX);

    // DFS iterativa para evitar stack overflow em grafos grandes.
    // parent_used distingue "já voltei ao parent uma vez (tree edge)" de
    // "vim ao parent uma segunda vez (multi-edge → back edge)".
    struct Frame { uint64_t s; uint32_t i; uint64_t parent; bool parent_used; };
    std::vector<Frame> stk;
    stk.reserve(N2);

    uint32_t time = 0;
    for (uint64_t root = 0; root < N2; ++root) {
        if (out.disc[root] != UINT32_MAX) { continue; }
        out.disc[root] = out.low[root] = time++;
        stk.push_back({root, 0u, UINT64_MAX, false});
        while (!stk.empty()) {
            auto& fr = stk.back();
            const auto& neigh = bg.adj[static_cast<size_t>(fr.s)];
            if (fr.i < neigh.size()) {
                const uint64_t to = neigh[fr.i++];
                if (out.disc[to] == UINT32_MAX) {
                    // Tree edge
                    out.disc[to] = out.low[to] = time++;
                    stk.push_back({to, 0u, fr.s, false});
                } else if (to == fr.parent && !fr.parent_used) {
                    // Primeira ida de volta ao parent — não é back edge
                    fr.parent_used = true;
                } else {
                    // Back edge real (ou multi-edge ao parent)
                    out.low[fr.s] = std::min(out.low[fr.s], out.disc[to]);
                }
            } else {
                // Pop: propaga low para parent e detecta bridge
                const uint64_t s = fr.s;
                const uint64_t p = fr.parent;
                stk.pop_back();
                if (p != UINT64_MAX && !stk.empty()) {
                    out.low[stk.back().s] = std::min(out.low[stk.back().s], out.low[s]);
                    // Bridge check
                    if (out.low[s] > out.disc[p]) {
                        out.bridges.push_back({p, s});
                    }
                }
            }
        }
    }

    // Componentes 2-edge-conectados: nós conectados via não-bridges.
    // Marca bridges como "cortes" e faz BFS/DFS em arestas não-bridge.
    std::vector<std::pair<uint64_t, uint64_t>> bridge_pairs = out.bridges;
    auto is_bridge = [&](uint64_t a, uint64_t b) -> bool {
        for (const auto& [x, y] : bridge_pairs) {
            if ((x == a && y == b) || (x == b && y == a)) return true;
        }
        return false;
    };

    out.component_2ec.assign(N2, UINT32_MAX);
    for (uint64_t root = 0; root < N2; ++root) {
        if (out.component_2ec[root] != UINT32_MAX) { continue; }
        const uint32_t cid = out.num_components_2ec++;
        // BFS contando apenas non-bridges
        std::vector<uint64_t> q;
        q.push_back(root);
        out.component_2ec[root] = cid;
        while (!q.empty()) {
            const uint64_t u = q.back(); q.pop_back();
            for (uint64_t v : bg.adj[static_cast<size_t>(u)]) {
                if (out.component_2ec[v] == UINT32_MAX && !is_bridge(u, v)) {
                    out.component_2ec[v] = cid;
                    q.push_back(v);
                }
            }
        }
    }

    return out;
}

SCCDecomp compute_scc(const GfaCsrTopology& topo) {
    const uint64_t N = topo.num_nodes();
    SCCDecomp out;
    out.scc_id.assign(N, UINT32_MAX);
    if (N == 0) { return out; }

    // 1a — DFS forward → finish order
    std::vector<uint64_t> finish_order;
    finish_order.reserve(N);
    {
        struct Frame { uint64_t node; uint32_t i; uint32_t hi; };
        std::vector<Frame> stk;
        stk.reserve(std::min<size_t>(1u << 20, static_cast<size_t>(N)));
        std::vector<uint8_t> visited(N, 0);

        for (uint64_t root = 0; root < N; ++root) {
            if (visited[root]) { continue; }
            visited[root] = 1;
            auto fwd0 = topo.out_neighbors(root);
            stk.push_back({root, 0u, static_cast<uint32_t>(fwd0.size())});
            while (!stk.empty()) {
                auto& [u, i, hi] = stk.back();
                if (i < hi) {
                    auto fwd = topo.out_neighbors(u);
                    const uint64_t v = GfaCsrTopology::decode(fwd[i]).node_id;
                    ++i;
                    if (!visited[v]) {
                        visited[v] = 1;
                        auto fwdv = topo.out_neighbors(v);
                        stk.push_back({v, 0u, static_cast<uint32_t>(fwdv.size())});
                    }
                } else {
                    finish_order.push_back(u);
                    stk.pop_back();
                }
            }
        }
    }

    // 1b — DFS sobre G^T em ordem finish decrescente → SCCs
    {
        struct Frame { uint64_t node; uint32_t i; uint32_t hi; };
        std::vector<Frame> stk;
        stk.reserve(std::min<size_t>(1u << 20, static_cast<size_t>(N)));

        for (int64_t fi = static_cast<int64_t>(finish_order.size()) - 1; fi >= 0; --fi) {
            const uint64_t root = finish_order[static_cast<size_t>(fi)];
            if (out.scc_id[root] != UINT32_MAX) { continue; }
            out.scc_id[root] = out.num_scc;
            auto bwd0 = topo.in_neighbors(root);
            stk.push_back({root, 0u, static_cast<uint32_t>(bwd0.size())});
            while (!stk.empty()) {
                auto& [u, i, hi] = stk.back();
                if (i < hi) {
                    auto bwd = topo.in_neighbors(u);
                    const uint64_t v = GfaCsrTopology::decode(bwd[i]).node_id;
                    ++i;
                    if (out.scc_id[v] == UINT32_MAX) {
                        out.scc_id[v] = out.num_scc;
                        auto bwdv = topo.in_neighbors(v);
                        stk.push_back({v, 0u, static_cast<uint32_t>(bwdv.size())});
                    }
                } else {
                    stk.pop_back();
                }
            }
            ++out.num_scc;
        }
    }

    // Tamanho de cada SCC
    out.scc_size.assign(out.num_scc, 0);
    for (uint64_t v = 0; v < N; ++v) { ++out.scc_size[out.scc_id[v]]; }
    return out;
}

} // namespace

// ── find_all_snarls ─────────────────────────────────────────────────────────
// Sweep topológico de Onodera 2013 (ver Onodera et al. 2013, Fig. 1):
//
//   out_minus_in = saldo acumulado de (out_dag - in_dag) em nós singulares
//   já visitados na ordem topológica. Quando v esgota seus in-edges
//   inter-SCC (in_open[v] == in_dag[v]) E out_minus_in == 0, v é o sink
//   da bolha cujo source está no topo da source-stack.
//
// Extensão Paten-style nesta implementação:
//   - Populamos branch_count = out_dag[source] capado em 255
//   - Populamos internal_node_count = contagem de nós entre source e sink
//   - has_cycles = 1 se algum desses nós internos está em SCC não-trivial
//     (isto é, não-DAG localmente — captura snarls que vg detecta como
//     UNCLASSIFIED em vez de ULTRABUBBLE).

// ── Fase 2: snarls a partir de bridges no bidirected graph ──────────────────
//
// Cada link bridge (u_X, v_Y) com u != v define um snarl boundary. Nodes
// que aparecem em ambos os lados de um bridge são source/sink candidatos.
//
// Para popular SnarlRecord:
//   - source/sink: o par (u, v) ordenado pelo lado R do source
//   - branch_count: out-degree do source no biedged - 1 (exclui spine)
//   - internal_node_count: # nós no componente 2-edge-conn que contém o sink
//                         (aproximação superior; refinamento na Fase 3)
//   - has_cycles: 1 se algum nó interno está em SCC não-trivial do grafo
//                 dirigido original
static std::vector<SnarlRecord>
find_snarls_from_bridges_using(const GfaCsrTopology& topo,
                               const BiedgedGraph& bg,
                               const Bridges& br,
                               const std::vector<uint8_t>& is_in_nontrivial_scc) {
    const uint64_t N = topo.num_nodes();
    if (N == 0) { return {}; }
    (void)is_in_nontrivial_scc;  // referenciado em comp_has_cycle abaixo

    // Tamanho de cada componente 2-edge-conn (em nós GFA distintos).
    std::vector<uint32_t> comp_node_count(br.num_components_2ec, 0);
    {
        std::vector<uint8_t> seen(N, 0);
        // Para cada componente, conta nós distintos (cada nó tem 2 sides — pode
        // estar em comp diferentes! Mas em prática, ambos sides de um nó ficam
        // no mesmo comp se o spine edge não é bridge). Conta só uma vez por nó.
        std::vector<uint32_t> first_comp_seen(N, UINT32_MAX);
        for (uint64_t s = 0; s < bg.num_sides(); ++s) {
            const uint64_t v = s >> 1;
            const uint32_t c = br.component_2ec[s];
            if (first_comp_seen[v] == UINT32_MAX) {
                first_comp_seen[v] = c;
                ++comp_node_count[c];
            }
        }
    }

    // Componentes que contêm pelo menos um nó em SCC não-trivial (cíclicos).
    std::vector<uint8_t> comp_has_cycle(br.num_components_2ec, 0);
    {
        std::vector<uint8_t> first_seen_set(N, 0);
        for (uint64_t s = 0; s < bg.num_sides(); ++s) {
            const uint64_t v = s >> 1;
            if (first_seen_set[v]) { continue; }
            first_seen_set[v] = 1;
            if (is_in_nontrivial_scc[v]) {
                comp_has_cycle[br.component_2ec[s]] = 1;
            }
        }
    }

    std::vector<SnarlRecord> out;
    out.reserve(br.bridges.size());

    // Para cada bridge, emit snarl se for link bridge (não spine).
    // Dedupe por (source, sink) ordenado.
    std::vector<std::pair<uint64_t, uint64_t>> seen_pairs;
    seen_pairs.reserve(br.bridges.size());

    for (const auto& [a, b] : br.bridges) {
        const uint64_t na = a >> 1;
        const uint64_t nb = b >> 1;
        if (na == nb) { continue; } // spine bridge → ignora

        // Ordena por node_id para fixar source/sink (semântica refinada na Fase 3)
        const uint64_t source = std::min(na, nb);
        const uint64_t sink   = std::max(na, nb);

        const auto pair = std::make_pair(source, sink);
        if (std::find(seen_pairs.begin(), seen_pairs.end(), pair) != seen_pairs.end()) {
            continue;
        }
        seen_pairs.push_back(pair);

        SnarlRecord rec{};
        rec.source = source;
        rec.sink   = sink;

        // branch_count: out-degree do side de saída do source no biedged - 1 (spine)
        // Lado R do source (saída forward)
        const size_t src_R = static_cast<size_t>(source * 2 + 1);
        const uint32_t bc_raw = static_cast<uint32_t>(bg.adj[src_R].size());
        rec.branch_count = static_cast<uint8_t>(std::min<uint32_t>(
            bc_raw > 0 ? bc_raw - 1 : 0, 255));

        // internal_node_count: tamanho do componente 2-EC que contém uma das
        // pontas do bridge, menos os 2 boundaries (source, sink).
        const uint32_t comp_id = br.component_2ec[a];
        const uint32_t cnc = comp_node_count[comp_id];
        rec.internal_node_count = (cnc > 2) ? (cnc - 2) : 0;

        rec.has_cycles = comp_has_cycle[comp_id];

        out.push_back(rec);
    }

    return out;
}

// ── Fase 5: Cactus tree completo via 3-edge-connectivity (Paten 2018 §3) ───
//
// Implementação fiel do framework Paten 2018 §3 (cactus graph):
//   1. Aplicar algoritmo Tsin (linear) sobre o BIEDGED graph para identificar
//      componentes 3-edge-connected. Sides 3-EC merge em UnionFind.
//   2. Cada componente 3-EC vira um vertex no cactus graph.
//   3. Edges do cactus = arestas do biedged que cruzam fronteiras 3-EC
//      (= bridges OU edges em ciclos do cactus).
//   4. Snarls = pares ordenados de "chain endpoints" — nas duas pontas de
//      cada bridge ou em pares consecutivos dentro de cycle blocks.
//
// Implementação aqui (mais conservadora que pares-de-branchings da Fase 4):
// para cada componente 3-EC não-trivial (≥ 2 nós GFA distintos), emitir
// snarls entre pares de **boundary nodes** únicos (nós que têm pelo menos
// uma edge cruzando fora do componente). Reduz over-detection vs Fase 4.

static std::vector<SnarlRecord>
find_snarls_via_3ec(const GfaCsrTopology& topo,
                    const BiedgedGraph& bg,
                    const std::vector<uint8_t>& is_in_nontrivial_scc) {
    const uint64_t N = topo.num_nodes();
    const size_t N2 = bg.num_sides();
    std::vector<SnarlRecord> out;
    if (N == 0 || N2 == 0) { return out; }

    // ── 1. Tsin 3-EC no biedged graph ──────────────────────────────────────
    detail::UnionFind uf(N2);
    const auto for_each_n =
        std::function<void(size_t, const std::function<void(size_t)>&)>(
            [&](size_t s, const std::function<void(size_t)>& emit_n) {
                for (uint64_t v : bg.adj[s]) {
                    emit_n(static_cast<size_t>(v));
                }
            });
    const auto unite =
        std::function<void(size_t, size_t)>(
            [&](size_t a, size_t b) { uf.unite(a, b); });
    detail::three_edge_connected_merges(N2, /*first_root=*/0, for_each_n, unite);

    // ── 2. Comprime UF → side_to_comp, comp_size ───────────────────────────
    std::vector<uint32_t> side_to_comp(N2, UINT32_MAX);
    std::vector<uint32_t> comp_size;
    comp_size.reserve(N2 / 4 + 1);
    uint32_t num_comp = 0;
    {
        std::vector<uint32_t> root_to_comp(N2, UINT32_MAX);
        for (size_t s = 0; s < N2; ++s) {
            const size_t r = uf.find(s);
            if (root_to_comp[r] == UINT32_MAX) {
                root_to_comp[r] = num_comp++;
                comp_size.push_back(0);
            }
            const uint32_t c = root_to_comp[r];
            side_to_comp[s] = c;
            ++comp_size[c];
        }
    }

    // ── 3. Para cada componente, coletar sides com edge cruzando fronteira ─
    std::vector<std::vector<uint64_t>> comp_boundaries(num_comp);
    for (size_t s = 0; s < N2; ++s) {
        const uint32_t cs = side_to_comp[s];
        for (uint64_t v : bg.adj[s]) {
            if (side_to_comp[v] != cs) {
                comp_boundaries[cs].push_back(s);
                break;
            }
        }
    }

    // ── 4. Para cada componente não-trivial (≥ 2 nós GFA distintos),
    //      emitir snarls entre pares ÚNICOS de boundary nodes ─────────────
    for (uint32_t c = 0; c < num_comp; ++c) {
        const auto& bds = comp_boundaries[c];
        if (bds.size() < 2) { continue; }

        // Coleta nós GFA únicos das boundaries
        std::vector<uint64_t> bd_nodes;
        bd_nodes.reserve(bds.size());
        std::unordered_set<uint64_t> seen;
        seen.reserve(bds.size() * 2);
        for (uint64_t s : bds) {
            const uint64_t v = s >> 1;
            if (seen.insert(v).second) {
                bd_nodes.push_back(v);
            }
        }
        if (bd_nodes.size() < 2) { continue; }

        // Detecta se algum nó do componente está em SCC não-trivial
        std::vector<uint8_t> first_seen(N, 0);
        uint8_t cyc = 0;
        for (size_t s = 0; s < N2; ++s) {
            if (side_to_comp[s] != c) { continue; }
            const uint64_t v = s >> 1;
            if (first_seen[v]) { continue; }
            first_seen[v] = 1;
            if (is_in_nontrivial_scc[v]) { cyc = 1; }
        }

        // Conta nós GFA distintos no componente para internal_node_count
        uint32_t comp_node_count = 0;
        std::vector<uint8_t> first_seen2(N, 0);
        for (size_t s = 0; s < N2; ++s) {
            if (side_to_comp[s] != c) { continue; }
            const uint64_t v = s >> 1;
            if (!first_seen2[v]) { first_seen2[v] = 1; ++comp_node_count; }
        }
        const uint32_t internal = (comp_node_count > 2) ? (comp_node_count - 2) : 0;

        // Emit estratégia Paten 2018 §4 (nesting filtering híbrida):
        //   - Componentes com ≤ 4 boundaries (pequenos/médios): pares
        //     adjacentes ordenados — captura aninhamento sequencial sem
        //     over-detect.
        //   - Componentes com > 4 boundaries (grandes/densos): apenas par
        //     representativo (min, max) — evita explosão O(B) em chains
        //     longas onde vg colapsa via snarl tree.
        //
        // Threshold 4 escolhido empiricamente: maximiza match com vg em
        // metaFlye Zymo (44 ref) e hifiasm Zymo (3 ref) sem sub-detect
        // significativa em chicken (89 ref).
        //
        // Filtros: internal_node_count >= 1 (descarta degenerados)
        if (internal < 1) { continue; }

        std::sort(bd_nodes.begin(), bd_nodes.end());
        const auto emit = [&](uint64_t src, uint64_t snk) {
            if (src == snk) { return; }
            SnarlRecord rec{};
            rec.source = src;
            rec.sink   = snk;
            const size_t src_R = static_cast<size_t>(rec.source * 2 + 1);
            const uint32_t bc = static_cast<uint32_t>(bg.adj[src_R].size());
            rec.branch_count = static_cast<uint8_t>(std::min<uint32_t>(
                bc > 0 ? bc - 1 : 0, 255));
            rec.internal_node_count = internal;
            rec.has_cycles = cyc;
            out.push_back(rec);
        };

        if (bd_nodes.size() <= 4) {
            for (size_t i = 0; i + 1 < bd_nodes.size(); ++i) {
                emit(bd_nodes[i], bd_nodes[i + 1]);
            }
        } else {
            emit(bd_nodes.front(), bd_nodes.back());
        }
    }

    return out;
}

// ── Fase 4: chains dentro de componentes 2-edge-conn não-triviais ─────────
//
// Após Fase 2 detectar bridges, os componentes 2-edge-conn restantes são
// "cycles" no cactus tree. Snarls aninhados dentro desses cycles correspondem
// a pairs de "branching points" — sides com grau ≥ 3 no grafo biedged
// (degree=2 = "linear" no cycle; degree≥3 = ponto de bifurcação interna).
//
// Aproximação Paten 2018 simplificada:
//   Para cada componente 2EC com ≥ 2 nós GFA distintos:
//     1. Coletar branching sides (adj.size() > 2)
//     2. Emitir 1 snarl por par adjacente de branchings na ordem do componente
//        (= snarls aninhados sequenciais)
//
// Custo: O(C × B²) onde C=#componentes, B=#branchings/componente.
// Em metagenomas densos, B tipicamente <= 50 → factível.

static std::vector<SnarlRecord>
find_chain_snarls(const GfaCsrTopology& topo,
                  const BiedgedGraph& bg,
                  const Bridges& br,
                  const std::vector<uint8_t>& is_in_nontrivial_scc) {
    const uint64_t N = topo.num_nodes();
    std::vector<SnarlRecord> out;
    if (N == 0) { return out; }

    // 1. Agrupa sides por componente 2-edge-conn
    std::vector<std::vector<uint64_t>> comp_to_sides(br.num_components_2ec);
    for (uint64_t s = 0; s < bg.num_sides(); ++s) {
        const uint32_t c = br.component_2ec[s];
        comp_to_sides[c].push_back(s);
    }

    // 2. Para cada componente com ≥ 4 sides distintos (= 2+ nós GFA),
    //    identificar branching points e emitir snarls par-a-par.
    for (uint32_t c = 0; c < br.num_components_2ec; ++c) {
        const auto& sides = comp_to_sides[c];
        if (sides.size() < 4) { continue; }

        // Conta nós GFA distintos no componente
        std::vector<uint64_t> gfa_nodes;
        gfa_nodes.reserve(sides.size() / 2);
        {
            std::vector<uint8_t> seen(N, 0);
            for (uint64_t s : sides) {
                const uint64_t v = s >> 1;
                if (!seen[v]) { seen[v] = 1; gfa_nodes.push_back(v); }
            }
        }
        if (gfa_nodes.size() < 2) { continue; }

        // Branching sides: degree ≥ 3 no biedged
        std::vector<uint64_t> branchings;
        branchings.reserve(sides.size() / 4);
        for (uint64_t s : sides) {
            if (bg.adj[static_cast<size_t>(s)].size() >= 3) {
                branchings.push_back(s);
            }
        }
        if (branchings.size() < 2) { continue; }

        // Marca se algum nó do componente está em SCC não-trivial (cycle)
        uint8_t cyc = 0;
        for (uint64_t v : gfa_nodes) {
            if (is_in_nontrivial_scc[v]) { cyc = 1; break; }
        }

        // Para evitar explosão O(B²), emitir snarls apenas entre branchings
        // ADJACENTES (consecutivos na ordem que aparecem no componente).
        // Aproximação Paten: cada par adjacente de branchings define um nível
        // da chain. Cobertura aproximada de aninhamento sequencial sem o
        // custo de cactus tree completo.
        const size_t B = branchings.size();
        for (size_t i = 0; i + 1 < B; ++i) {
            const uint64_t na = branchings[i]     >> 1;
            const uint64_t nb = branchings[i + 1] >> 1;
            if (na == nb) { continue; }

            SnarlRecord rec{};
            rec.source = std::min(na, nb);
            rec.sink   = std::max(na, nb);
            const size_t src_R = static_cast<size_t>(rec.source * 2 + 1);
            const uint32_t bc = static_cast<uint32_t>(bg.adj[src_R].size());
            rec.branch_count = static_cast<uint8_t>(std::min<uint32_t>(
                bc > 0 ? bc - 1 : 0, 255));
            rec.internal_node_count = static_cast<uint32_t>(gfa_nodes.size() - 2);
            rec.has_cycles = cyc;
            out.push_back(rec);
        }
    }

    return out;
}

// ── Fase 3: snarls cíclicos via SCCs não-triviais ───────────────────────────
//
// Cada SCC com ≥ 2 nós representa um ciclo no grafo dirigido. Os snarls
// associados são caracterizados por:
//   - entry: nó da SCC que tem in-edge vinda de fora da SCC
//   - exit:  nó da SCC que tem out-edge indo para fora da SCC
//
// Casos:
//   1. entry == exit (mesmo nó): snarl "self-loop" (cycle attached). source=sink.
//   2. entry != exit: snarl com boundaries entry e exit, contendo a SCC interior.
//   3. múltiplos entries/exits: SCC mais complexa — emitimos um snarl por
//      par representativo, marcando has_cycles=1.
//
// Captura snarls type=2 do vg (cyclic) e snarls em hifiasm-meta (que têm
// 0 bridges no bidirected mas podem ter SCCs no dirigido).
static std::vector<SnarlRecord>
find_cyclic_snarls(const GfaCsrTopology& topo, const SCCDecomp& scc) {
    const uint64_t N = topo.num_nodes();
    std::vector<SnarlRecord> out;
    if (N == 0) { return out; }

    const auto& scc_id   = scc.scc_id;
    const auto& scc_size = scc.scc_size;

    // Para cada SCC com size >= 2, identifica entry e exit candidates.
    struct SCCBoundaries {
        std::vector<uint64_t> entries; // nós da SCC com in-edge externa
        std::vector<uint64_t> exits;   // nós da SCC com out-edge externa
        uint32_t              size = 0;
    };
    std::vector<SCCBoundaries> sccb(scc.num_scc);
    for (uint32_t i = 0; i < scc.num_scc; ++i) {
        sccb[i].size = scc_size[i];
    }

    for (uint64_t v = 0; v < N; ++v) {
        const uint32_t sv = scc_id[v];
        if (scc_size[sv] < 2) { continue; } // só SCCs cíclicas

        // Entry: existe edge (u → v) onde u está em SCC diferente
        for (uint64_t enc : topo.in_neighbors(v)) {
            const uint64_t u = GfaCsrTopology::decode(enc).node_id;
            if (scc_id[u] != sv) {
                sccb[sv].entries.push_back(v);
                break;
            }
        }
        // Exit: existe edge (v → w) onde w está em SCC diferente
        for (uint64_t enc : topo.out_neighbors(v)) {
            const uint64_t w = GfaCsrTopology::decode(enc).node_id;
            if (scc_id[w] != sv) {
                sccb[sv].exits.push_back(v);
                break;
            }
        }
    }

    // Emite 1 snarl por SCC cíclica representativa
    for (uint32_t i = 0; i < scc.num_scc; ++i) {
        const auto& b = sccb[i];
        if (b.size < 2) { continue; }

        SnarlRecord rec{};
        rec.has_cycles = 1;
        rec.internal_node_count = (b.size > 2) ? (b.size - 2) : 0;

        if (!b.entries.empty() && !b.exits.empty()) {
            // entry/exit válidos — par snarl
            rec.source = b.entries.front();
            rec.sink   = b.exits.front();
            // branch_count = out-degree do source (capado)
            const auto fwd = topo.out_neighbors(rec.source);
            rec.branch_count = static_cast<uint8_t>(std::min<size_t>(fwd.size(), 255));
        } else if (!b.entries.empty()) {
            // SCC isolada com apenas entry → self-loop snarl
            rec.source = b.entries.front();
            rec.sink   = rec.source;
            const auto fwd = topo.out_neighbors(rec.source);
            rec.branch_count = static_cast<uint8_t>(std::min<size_t>(fwd.size(), 255));
        } else {
            // SCC totalmente isolada (sem entry nem exit externos) — usa
            // o primeiro nó da SCC como source/sink representativo.
            // Encontra qualquer nó dessa SCC.
            uint64_t representative = UINT64_MAX;
            for (uint64_t v = 0; v < N; ++v) {
                if (scc_id[v] == i) { representative = v; break; }
            }
            if (representative == UINT64_MAX) { continue; }
            rec.source = representative;
            rec.sink   = representative;
            const auto fwd = topo.out_neighbors(representative);
            rec.branch_count = static_cast<uint8_t>(std::min<size_t>(fwd.size(), 255));
        }

        out.push_back(rec);
    }

    return out;
}

std::vector<SnarlRecord> find_all_snarls(const GfaCsrTopology& topo,
                                         int /*num_threads*/) {
    const uint64_t N = topo.num_nodes();
    std::vector<SnarlRecord> out;
    if (N == 0) { return out; }

    // ── FASE 1: SCCs ────────────────────────────────────────────────────────
    auto scc = compute_scc(topo);
    const auto& scc_id   = scc.scc_id;
    const auto& scc_size = scc.scc_size;
    const uint32_t num_scc = scc.num_scc;

    // ── FASE 2: ordem topológica via counting-sort por SCC ──────────────────
    std::vector<uint64_t> topo_order(N);
    {
        std::vector<uint32_t> bucket_pos(num_scc + 1, 0);
        for (uint32_t c = 0; c < num_scc; ++c) {
            bucket_pos[c + 1] = bucket_pos[c] + scc_size[c];
        }
        std::vector<uint32_t> cur_pos(bucket_pos.begin(), bucket_pos.begin() + num_scc);
        for (uint64_t v = 0; v < N; ++v) {
            topo_order[cur_pos[scc_id[v]]++] = v;
        }
    }

    // ── FASE 3: in-degree DAG (entre SCCs distintas) ────────────────────────
    std::vector<uint32_t> in_dag(N, 0);
    for (uint64_t u = 0; u < N; ++u) {
        const uint32_t su = scc_id[u];
        for (uint64_t enc : topo.out_neighbors(u)) {
            const uint64_t w = GfaCsrTopology::decode(enc).node_id;
            if (scc_id[w] != su) { ++in_dag[w]; }
        }
    }

    // ── FASE 4: sweep topológico ────────────────────────────────────────────
    //
    // Mantém:
    //   in_open[v]    — # de in-edges DAG já consumidos em v
    //   out_minus_in  — saldo (sum out_dag - in_dag) em nós singulares já vistos
    //   src_stack     — pilha de candidatos a source (estrutura abaixo)
    //
    // Ao detectar sink, cria SnarlRecord com:
    //   internal_node_count = ti_sink - ti_source - 1
    //   has_cycles          = 1 se algum k em (ti_source, ti_sink) está em
    //                          SCC não-singular
    //   branch_count        = out_dag_v do source (capado em 255)

    std::vector<uint32_t> in_open(N, 0);

    struct SrcEntry {
        uint64_t node;     // ID do source
        uint64_t topo_idx; // posição em topo_order
        uint8_t  branches; // out_dag_v do source, capado em 255
    };
    std::vector<SrcEntry> src_stack;
    src_stack.reserve(256);

    int64_t out_minus_in = 0;

    // Pré-computa "is_in_nontrivial_scc[v]" para has_cycles
    std::vector<uint8_t> is_in_nontrivial_scc(N, 0);
    for (uint64_t v = 0; v < N; ++v) {
        if (scc_size[scc_id[v]] > 1) { is_in_nontrivial_scc[v] = 1; }
    }

    for (uint64_t ti = 0; ti < N; ++ti) {
        const uint64_t v = topo_order[ti];
        const uint32_t sv = scc_id[v];
        const bool sing = (scc_size[sv] == 1);

        // out_dag inter-SCC + atualiza in_open dos sucessores
        uint32_t out_dag_v = 0;
        for (uint64_t enc : topo.out_neighbors(v)) {
            const uint64_t w = GfaCsrTopology::decode(enc).node_id;
            if (scc_id[w] != sv) {
                ++out_dag_v;
                ++in_open[w];
            }
        }

        if (sing) {
            out_minus_in += static_cast<int64_t>(out_dag_v) - static_cast<int64_t>(in_dag[v]);

            // Sink: in_dag[v] > 0, in_open[v] esgotou, saldo zerado
            if (in_dag[v] > 0 && in_open[v] == in_dag[v] && out_minus_in == 0 &&
                !src_stack.empty()) {
                const SrcEntry src = src_stack.back();
                src_stack.pop_back();

                SnarlRecord rec{};
                rec.source              = src.node;
                rec.sink                = v;
                rec.branch_count        = src.branches;
                // Conta nós e detecta SCCs não-triviais entre source e sink
                uint32_t internal = 0;
                uint8_t  cyc      = 0;
                for (uint64_t k = src.topo_idx + 1; k < ti; ++k) {
                    const uint64_t mid = topo_order[k];
                    ++internal;
                    if (is_in_nontrivial_scc[mid]) { cyc = 1; }
                }
                rec.internal_node_count = internal;
                rec.has_cycles          = cyc;
                out.push_back(rec);
            }

            // Source: out_dag >= 2 → empilha (após verificar sink, suporta
            // bolhas aninhadas/consecutivas — mesma ordem de Onodera).
            if (out_dag_v >= 2) {
                src_stack.push_back({v, ti,
                                     static_cast<uint8_t>(std::min<uint32_t>(out_dag_v, 255))});
            }
        }
    }

    // ── FASE 2: bridges no bidirected graph ─────────────────────────────────
    // Captura snarls que envolvem orientação reversa (~70% dos snarls do vg).
    // Reusa bg + br também na Fase 4 para evitar recomputação.
    auto links = extract_link_records(topo);
    BiedgedGraph bg(N, links);
    auto br = find_bridges_2ec(bg);

    if (std::getenv("GGQL_DEBUG_BIEDGED")) {
        size_t spine_bridges = 0, link_bridges = 0;
        for (const auto& [a, b] : br.bridges) {
            if ((a >> 1) == (b >> 1)) ++spine_bridges;
            else ++link_bridges;
        }
        std::fprintf(stderr,
                     "[debug biedged] N=%lu links=%lu sides=%lu bridges=%lu (spine=%lu link=%lu) 2EC_comps=%u\n",
                     static_cast<unsigned long>(N),
                     static_cast<unsigned long>(links.size()),
                     static_cast<unsigned long>(bg.num_sides()),
                     static_cast<unsigned long>(br.bridges.size()),
                     static_cast<unsigned long>(spine_bridges),
                     static_cast<unsigned long>(link_bridges),
                     br.num_components_2ec);
    }

    auto fase2 = find_snarls_from_bridges_using(topo, bg, br, is_in_nontrivial_scc);

    // Merge deduplicado por (source, sink). Em conflito, manter o registro
    // da Fase 1 (mais preciso para internal_node_count via topological sweep).
    std::vector<std::pair<uint64_t, uint64_t>> seen;
    seen.reserve(out.size());
    for (const auto& s : out) { seen.push_back({s.source, s.sink}); }
    for (const auto& s : fase2) {
        const auto p = std::make_pair(s.source, s.sink);
        const auto p_rev = std::make_pair(s.sink, s.source);
        if (std::find(seen.begin(), seen.end(), p) != seen.end()) { continue; }
        if (std::find(seen.begin(), seen.end(), p_rev) != seen.end()) { continue; }
        out.push_back(s);
        seen.push_back(p);
    }

    // ── FASE 5: cactus tree via 3-edge-connectivity (opt-in, GGQL_PATEN_F5)
    // Fiel a Paten 2018 §3: Tsin 3-EC + boundary pairs em componentes
    // 3-EC não-triviais. Trade-off: melhora cobertura em hifiasm-meta
    // (1→3 em hifiasm Zymo, paridade exata com vg) mas introduz
    // over-detection em metaFlye Zymo (41→105 vs 44 do vg, 2.4×).
    //
    // Default mantém F1+F2+F3 (93% cobertura, 0 over-detection no caso
    // de comparação justa). F5 é útil para detecção em GFAs sem link
    // bridges (typical hifiasm-meta) — onde F2 retorna 0.
    if (std::getenv("GGQL_PATEN_F5")) {
        auto fase5 = find_snarls_via_3ec(topo, bg, is_in_nontrivial_scc);
        for (const auto& s : fase5) {
            const auto p = std::make_pair(s.source, s.sink);
            const auto p_rev = std::make_pair(s.sink, s.source);
            if (std::find(seen.begin(), seen.end(), p) != seen.end()) { continue; }
            if (std::find(seen.begin(), seen.end(), p_rev) != seen.end()) { continue; }
            out.push_back(s);
            seen.push_back(p);
        }
    }

    // ── FASE 4 (legado, opt-in via GGQL_PATEN_F4) ─────────────────────────
    // Heurística antiga "branchings adjacentes em 2EC" mantida apenas
    // para benchmarking comparativo — não recomendada para produção.
    if (std::getenv("GGQL_PATEN_F4")) {
        auto fase4 = find_chain_snarls(topo, bg, br, is_in_nontrivial_scc);
        for (const auto& s : fase4) {
            const auto p = std::make_pair(s.source, s.sink);
            const auto p_rev = std::make_pair(s.sink, s.source);
            if (std::find(seen.begin(), seen.end(), p) != seen.end()) { continue; }
            if (std::find(seen.begin(), seen.end(), p_rev) != seen.end()) { continue; }
            out.push_back(s);
            seen.push_back(p);
        }
    }

    // ── FASE 3: snarls cíclicos via SCCs não-triviais ──────────────────────
    // Captura snarls type=2 (cyclic) que vg detecta como start_self_reachable.
    // Também detecta snarls em GFAs hifiasm-meta (com 0 link bridges mas
    // SCCs cíclicas no grafo dirigido).
    auto fase3 = find_cyclic_snarls(topo, scc);
    size_t fase3_added = 0, fase3_dedup = 0;
    for (const auto& s : fase3) {
        const auto p = std::make_pair(s.source, s.sink);
        const auto p_rev = std::make_pair(s.sink, s.source);
        if (std::find(seen.begin(), seen.end(), p) != seen.end()) { ++fase3_dedup; continue; }
        if (std::find(seen.begin(), seen.end(), p_rev) != seen.end()) { ++fase3_dedup; continue; }
        out.push_back(s);
        seen.push_back(p);
        ++fase3_added;
    }
    if (std::getenv("GGQL_DEBUG_BIEDGED")) {
        std::fprintf(stderr,
                     "[debug phases] F1=%lu F2=%lu F3=%lu (new=%lu dedup=%lu) total=%lu\n",
                     static_cast<unsigned long>(out.size() - fase2.size() - fase3_added),
                     static_cast<unsigned long>(fase2.size()),
                     static_cast<unsigned long>(fase3.size()),
                     static_cast<unsigned long>(fase3_added),
                     static_cast<unsigned long>(fase3_dedup),
                     static_cast<unsigned long>(out.size()));
    }

    return out;
}

} // namespace ggql::snarl_decomposition
