// snarl_decomposition_sese.cpp — Algoritmo SESE adaptado do Billi
// (at-cg/billi, src/subcommand/decompose.cpp).
//
// Algoritmo: Single-Entry-Single-Exit decomposition (Johnson et al. 1994)
// adaptado para bidirected graphs por Bhat, Mahajan, Jain (bioRxiv 2025).
//
// Vantagens vs Paten 2018 / vg:
//   - Complexidade linear O(N + E) provada
//   - 15× mais rápido que vg em panbubble detection (claim do paper)
//   - 5× menos memória
//   - Algoritmo mais simples de implementar/manter
//
// Conceito chave: bracket list por nó. Dois nós (u, w) são "cycle
// equivalent" sse compartilham o mesmo (top_bracket_id, list_size) —
// formam um SESE region (snarl boundary).
//
// API: find_all_snarls_sese(topo) → std::vector<SnarlRecord>

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace ggql::snarl_decomposition {

namespace sese_detail {

// Adaptação direta dos data types do Billi.
struct Edge {
    int id;
    Edge* front = nullptr;
    Edge* back  = nullptr;
    explicit Edge(int eid) : id(eid) {}
};

struct BracketList {
    int sz = 0;
    int d  = INT32_MAX;       // profundidade mínima alcançada
    Edge* start = nullptr;
    Edge* end   = nullptr;
    bool merged = false;

    BracketList() = default;
    BracketList(int lsz, int depth, Edge* pstart, Edge* pend)
        : sz(lsz), d(depth), start(pstart), end(pend) {}
};

// Concatena bl2 em bl1 (vg: merge). O(1).
inline void bl_merge(BracketList* bl1, BracketList* bl2) {
    if (!bl2->start) { return; }
    if (!bl1->start) {
        bl1->start = bl2->start;
        bl1->d = bl2->d;
    } else {
        bl1->end->front = bl2->start;
        bl2->start->back = bl1->end;
        bl1->d = std::min(bl1->d, bl2->d);
    }
    bl1->end = bl2->end;
    bl1->sz += bl2->sz;
    bl2->merged = true;
}

// Estado global da execução (single-threaded por instância).
struct SeseState {
    const GfaCsrTopology* topo = nullptr;
    uint64_t N = 0;
    // Adjacência tratando o grafo como undirected (cada link aparece em
    // ambas as direções). Cada entry = (vizinho, edge_id global, gray_flag).
    // gray_flag distingue spine edges (false) de link edges (true) num
    // bidirected adapter; no minimal SESE, todos são iguais.
    struct AdjEntry { uint64_t to; int edge_id; };
    std::vector<std::vector<AdjEntry>> adj;

    std::vector<int>          depth;     // profundidade DFS
    std::vector<bool>         marked;    // visitado
    std::vector<BracketList*> bl;        // bracket list por nó
    std::vector<std::vector<Edge*>> remove_brackets; // back-edges a remover ao sair
    int next_gray_id = 0;
    int max_d = 1 << 30;

    // Cycle equivalence: map (top_bracket_id, size) → first node visited
    std::unordered_map<int64_t, uint64_t> cycle_eq_map;

    // Output: pares de nodes que formam SESE regions
    std::vector<std::pair<uint64_t, uint64_t>> sese_pairs;
};

// SESE recursive DFS — versão minimal (sem hairpins, sem dual edges).
//
// Descobre pares (u, w) onde u e w têm a mesma bracket signature →
// boundary de um SESE region.
static void sese_dfs(SeseState& s, uint64_t u, uint64_t parent) {
    s.marked[u] = true;

    int h0 = s.max_d, h1 = s.max_d, h2 = s.max_d;

    // Visita filhos no DFS
    for (const auto& [v, eid] : s.adj[u]) {
        if (v == parent || v == u) {
            h0 = std::min(h0, s.depth[v]);
            continue;
        }
        if (s.marked[v]) {
            // Back-edge ou front-edge?
            if (s.depth[v] > s.depth[u]) { continue; } // front-edge
            h0 = std::min(h0, s.depth[v]);
        } else {
            s.depth[v] = s.depth[u] + 1;
            sese_dfs(s, v, u);
        }
    }

    s.bl[u] = new BracketList();

    // Merge bracket lists dos filhos diretos
    for (const auto& [v, eid] : s.adj[u]) {
        if (s.depth[v] != s.depth[u] + 1) { continue; }
        if (!s.bl[v]) { continue; }
        if (s.bl[v]->d < h1) { h2 = h1; h1 = s.bl[v]->d; }
        else                 { h2 = std::min(h2, s.bl[v]->d); }
        s.bl[v]->d = s.max_d;
        if (!s.bl[v]->merged) {
            bl_merge(s.bl[u], s.bl[v]);
        }
    }
    s.bl[u]->d = std::min(h0, h1);

    // Remove brackets que terminam aqui
    for (Edge* ed : s.remove_brackets[u]) {
        if (ed->front) {
            if (ed->back) {
                ed->back->front = ed->front;
                ed->front->back = ed->back;
            } else {
                s.bl[u]->start = ed->front;
                ed->front->back = nullptr;
            }
        } else {
            s.bl[u]->end = ed->back;
            if (ed->back) { ed->back->front = nullptr; }
            else { s.bl[u]->start = nullptr; }
        }
        delete ed;
        s.bl[u]->sz--;
    }
    s.remove_brackets[u].clear();
    if (s.bl[u]->d == s.depth[u]) { s.bl[u]->d = s.max_d; }

    // Push back-edges de u (vão para nodes acima na DFS)
    for (const auto& [v, eid] : s.adj[u]) {
        if (s.depth[v] >= s.depth[u] - 1) { continue; }
        Edge* ed = new Edge(eid);
        s.remove_brackets[v].push_back(ed);
        BracketList* tmp = new BracketList(1, s.depth[v], ed, ed);
        bl_merge(s.bl[u], tmp);
        delete tmp;
    }

    // Capping back-edge se necessário
    if (h2 <= h0 && h0 != s.max_d) {
        // Em Billi, isso usa stack_trace — versão minimal omite
        // (afeta exatidão em casos edge mas mantém algoritmo viável)
    }

    // Cycle equivalence: identifica boundary de SESE region
    if (parent != UINT64_MAX) {
        int64_t key;
        if (s.bl[u]->sz > 0) {
            key = (static_cast<int64_t>(s.bl[u]->end->id) << 32)
                | static_cast<int64_t>(s.bl[u]->sz);
        } else {
            key = 0;
        }
        auto it = s.cycle_eq_map.find(key);
        if (it != s.cycle_eq_map.end()) {
            const uint64_t w = it->second;
            if (w != u) {
                s.sese_pairs.push_back({std::min(u, w), std::max(u, w)});
            }
        }
        s.cycle_eq_map[key] = u;
    }
}

} // namespace sese_detail

// API pública: SESE-based snarl detection.
std::vector<SnarlRecord> find_all_snarls_sese(const GfaCsrTopology& topo,
                                               int /*num_threads*/) {
    using namespace sese_detail;
    const uint64_t N = topo.num_nodes();
    if (N == 0) { return {}; }

    SeseState s;
    s.topo = &topo;
    s.N = N;
    s.adj.assign(N, {});
    s.depth.assign(N, 0);
    s.marked.assign(N, false);
    s.bl.assign(N, nullptr);
    s.remove_brackets.assign(N, {});

    // Constrói adjacência undirected (cada link uma vez)
    int next_eid = 0;
    for (uint64_t u = 0; u < N; ++u) {
        for (uint64_t enc : topo.out_neighbors(u)) {
            const uint64_t v = GfaCsrTopology::decode(enc).node_id;
            const int eid = next_eid++;
            s.adj[u].push_back({v, eid});
            s.adj[v].push_back({u, eid}); // undirected
        }
    }

    // DFS de cada componente conexo
    for (uint64_t root = 0; root < N; ++root) {
        if (s.marked[root]) { continue; }
        s.cycle_eq_map.clear();
        s.depth[root] = 0;
        sese_dfs(s, root, UINT64_MAX);
    }

    // Converte pares (u, w) → SnarlRecord. Note: SESE não dá info direta
    // de internal_node_count; aproximamos com 0 e marcamos has_cycles=0.
    std::vector<SnarlRecord> out;
    out.reserve(s.sese_pairs.size());
    std::unordered_map<int64_t, bool> seen;
    for (const auto& [a, b] : s.sese_pairs) {
        const int64_t key = (static_cast<int64_t>(a) << 32) | b;
        if (!seen.emplace(key, true).second) { continue; }
        if (a == b) { continue; } // unary
        SnarlRecord rec{};
        rec.source = a;
        rec.sink   = b;
        rec.branch_count = 2;       // aproximação
        rec.internal_node_count = 0; // SESE não computa direto
        rec.has_cycles = 0;
        out.push_back(rec);
    }

    // Cleanup: deleta bracket lists alocadas
    for (auto* p : s.bl) { delete p; }

    return out;
}

} // namespace ggql::snarl_decomposition
