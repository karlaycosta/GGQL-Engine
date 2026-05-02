// snarl_decomposition_billi_port.cpp — PORT LITERAL do algoritmo Billi.
//
// Adaptação direta de at-cg/billi (Bhat, Mahajan, Jain 2025) para nossa
// stack. Mantém EXATAMENTE a estrutura e lógica do código de Billi, com
// adaptações apenas para:
//   - Substituir parser GFA do Billi pela leitura de GfaCsrTopology
//   - Envolver em namespace para evitar conflito com find_all_snarls_*
//     (variáveis globais do Billi tornam-se members de uma struct)
//   - Output via std::vector<SnarlRecord> em vez de stdout
//
// Funções portadas literalmente:
//   - dfs_comp, find_unique_including_selfloop
//   - dfs_depth, merge, sese_minbracket
//   - sese (algoritmo principal SESE)
//   - hairp (detecção de hairpins)
//   - end_gene, root_leaf_path, upd_node, mark_nodes (validação brute force)
//   - Decompose pipeline completo
//
// Macros do Billi (pb, F, S, etc.) substituídas por nomes claros.
//
// Refs: github.com/at-cg/billi src/{include,subcommand}/{commons,decompose}.{cpp,hpp}

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdint>
#include <deque>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ggql::snarl_decomposition {

namespace billi_port {

using pii = std::pair<int, int>;

// Estrutura encapsulando o estado do algoritmo Billi (que originalmente
// usa variáveis globais). Cada chamada cria uma instância nova → thread-safe.
struct BilliState {
    // ── Edge / BracketList (estruturas literal do Billi) ─────────────
    struct Edge {
        int id;
        Edge* front = nullptr;
        Edge* back  = nullptr;
        explicit Edge(int eid) : id(eid) {}
    };

    struct BracketList {
        int sz = 0, d = 0;
        Edge* start = nullptr;
        Edge* end_  = nullptr;
        bool merged = false;
        BracketList() = default;
        BracketList(int lsz, int depth, Edge* pstart, Edge* pend)
            : sz(lsz), d(depth), start(pstart), end_(pend) {}
    };

    // ── Pool allocators (pointer-stable, no leaks) ────────────────────
    std::deque<Edge> edge_pool;
    std::deque<BracketList> bracket_pool;

    Edge* alloc_edge(int eid) { return &edge_pool.emplace_back(eid); }
    BracketList* alloc_bracket() { return &bracket_pool.emplace_back(); }
    BracketList* alloc_bracket(int lsz, int d, Edge* ps, Edge* pe) {
        return &bracket_pool.emplace_back(lsz, d, ps, pe);
    }

    // ── Generic ──────────────────────────────────────────────────────
    int maxd = 0;
    int tot_gray = 0;
    std::vector<uint8_t> mark;  // uint8_t em vez de bool (hot path)

    // ── Input (GFA → biedged graph) ──────────────────────────────────
    int n = 0;            // # nós GFA
    int edges_count = 0;  // # links
    std::vector<uint8_t> has_self_loop;
    std::vector<std::vector<pii>> g; // (node_id, gray_edge_id)

    // ── Compacted Graph ──────────────────────────────────────────────
    int cnt_gray_edge = 0;
    int cnt_black_edge = 0;
    std::vector<int> dual;
    std::queue<pii> q_compact;
    int component = 0;
    std::vector<int> black_edge_id;
    std::vector<int> ty_end;
    std::vector<int> rm_cnt;
    std::vector<int> tip_start;
    std::vector<int> first_node;
    std::vector<int> Start;
    std::vector<int> depth;
    std::vector<int> biedged_connected_comp;
    std::vector<std::vector<int>> tips;
    std::vector<std::vector<pii>> g_compacted;

    // ── SESE ────────────────────────────────────────────────────────
    int vtime = 0;
    std::vector<int> entry_time, exit_time;
    std::vector<int> stack_trace;
    std::vector<uint8_t> is_bridge;
    std::vector<pii> possible_hairpins;

    // Hash splitmix64 para pii — distribuição uniforme, baixa colisão.
    struct PiiHash {
        static uint64_t splitmix64(uint64_t x) noexcept {
            x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
            x ^= x >> 27; x *= 0x94d049bb133111ebULL;
            x ^= x >> 31;
            return x;
        }
        size_t operator()(const pii& p) const noexcept {
            const uint64_t a = static_cast<uint32_t>(p.first);
            const uint64_t b = static_cast<uint32_t>(p.second);
            return static_cast<size_t>(splitmix64((a << 32) | b));
        }
    };
    // canonical_sese: ordem afeta seleção canônica de panbubbles.
    // unordered_map para inserts O(1) + sort no final preserva paridade Billi.
    std::unordered_map<pii, std::vector<pii>, PiiHash> canonical_sese;
    std::unordered_map<pii, int, PiiHash> st;
    std::vector<BracketList*> bl;
    // Sparse: a maioria dos nós nunca recebe pending_removal. Mapa esparso
    // economiza ~5 GB no HPRC (vs vector<vector<>> com 2N envelopes vazios).
    std::unordered_map<int, std::vector<Edge*>> pending_removals;

    // ── Brute force ─────────────────────────────────────────────────
    int counter = 0;
    std::vector<int> considered;
    std::vector<short> to_chk;
    std::vector<short> visit_order;

    // ── Output ──────────────────────────────────────────────────────
    std::vector<pii> valid_panbubbles, valid_hairpins;

    int offset = 1; // heuristic mode (=1); -e flag aumenta para INT32_MAX/10

    // ─────────────────────────────────────────────────────────────────
    // Métodos do algoritmo (port literal de billi/decompose.cpp)
    // ─────────────────────────────────────────────────────────────────

    void add_edge(std::vector<std::vector<pii>>& gg, int id1, int id2) {
        gg[id1].push_back({id2, tot_gray});
        if (id1 != id2) gg[id2].push_back({id1, tot_gray});
        ++tot_gray;
    }

    bool chk_vertex(int id) {
        return g[id].size() == 2 && g[id][0].first != g[id][1].first
               && !has_self_loop[id];
    }
    bool chk_for_compaction(int id) {
        if (chk_vertex(id)) {
            int u = g[id][1].first;
            return chk_vertex(u);
        }
        return false;
    }

    // Iterativo — sem risco de stack overflow
    void dfs_comp(int root) {
        std::vector<int> stk = {root};
        mark[root] = true;
        biedged_connected_comp[root] = component;
        while (!stk.empty()) {
            int u = stk.back(); stk.pop_back();
            for (const auto& child : g_compacted[u]) {
                int v = child.first;
                if (mark[v]) continue;
                mark[v] = true;
                biedged_connected_comp[v] = component;
                stk.push_back(v);
            }
        }
    }

    // Retorna 1 se u tem ao menos 1 vizinho diferente do dual, 0 caso contrário.
    int find_unique_including_selfloop(int u) {
        for (const auto& child : g_compacted[u]) {
            if (child.first != dual[u]) return 1;
        }
        return 0;
    }

    // Iterativo via iterator-stack — preserva ORDEM DE VISITA do recursivo
    // (descer no primeiro filho antes de partir para o segundo). Uma versão
    // simplificada que dá pop_back direto inverte a ordem (LIFO) e produz
    // uma DFS-tree diferente, gerando depths inconsistentes que quebram sese.
    void dfs_depth(int root) {
        struct Frame { int u; size_t ci; };
        std::vector<Frame> stk = {{root, 0}};
        mark[root] = true;
        depth[root] = 0;
        while (!stk.empty()) {
            auto& f = stk.back();
            bool pushed = false;
            while (f.ci < g_compacted[f.u].size()) {
                int v = g_compacted[f.u][f.ci++].first;
                if (mark[v]) continue;
                mark[v] = true;
                depth[v] = depth[f.u] + 1;
                stk.push_back({v, 0});
                pushed = true;
                break;
            }
            if (!pushed) stk.pop_back();
        }
    }

    void merge(BracketList* bl1, BracketList* bl2) {
        if (!bl2->start) return;
        if (!bl1->start) {
            bl1->start = bl2->start;
            bl1->d = bl2->d;
        } else {
            bl1->end_->front = bl2->start;
            bl2->start->back = bl1->end_;
            bl1->d = std::min(bl1->d, bl2->d);
        }
        bl1->end_ = bl2->end_;
        bl1->sz += bl2->sz;
        bl2->merged = true;
    }

    // Iterativo com post-order via child_idx tracking
    void sese_minbracket(int root, int& x, int& val) {
        struct Frame { int u, parent; size_t ci; };
        std::vector<Frame> stk = {{root, -1, 0}};
        mark[root] = true;
        depth[root] = 0;
        while (!stk.empty()) {
            auto& f = stk.back();
            bool pushed = false;
            while (f.ci < g_compacted[f.u].size()) {
                const auto& child = g_compacted[f.u][f.ci++];
                int v = child.first;
                if (v == f.parent || v == f.u) continue;
                if (mark[v]) continue;
                mark[v] = true;
                depth[v] = depth[f.u] + 1;
                stk.push_back({v, f.u, 0});
                pushed = true;
                break;
            }
            if (!pushed) {
                int u = f.u;
                bl[u] = alloc_bracket();
                for (const auto& child : g_compacted[u]) {
                    int v = child.first;
                    if (depth[v] != depth[u] + 1) continue;
                    if (bl[v] && !bl[v]->merged) {
                        if (u == dual[v]) {
                            int bridge_cnt = bl[v]->sz;
                            if (bridge_cnt < val) { val = bridge_cnt; x = u; }
                        }
                        bl[u]->sz += bl[v]->sz;
                        bl[v]->merged = true;
                    }
                }
                bl[u]->sz -= rm_cnt[u];
                for (const auto& child : g_compacted[u]) {
                    int v = child.first;
                    if (depth[v] >= depth[u] - 1) continue;
                    rm_cnt[v]++;
                    bl[u]->sz++;
                }
                stk.pop_back();
            }
        }
    }

    // Iterativo com post-order — preserva semântica exata do SESE recursivo.
    // Pre-order calcula h0 antes de qualquer recursão; correto pois graph cleaning
    // garante que cada vizinho aparece no máximo 1× em g_compacted[u].
    void sese(int root, int root_parent) {
        // Frame compacto: ints primeiro (alignof=4), uint32_t ci (g_compacted[u]
        // tem no máx ~dozens de entries no biedged), 2 bools no final.
        // 28 bytes vs 40 bytes do layout naive (size_t ci + bools intercalados).
        struct Frame {
            int u, parent;
            int h0, h1, h2;
            uint32_t ci;     // child iteration index
            bool can_end;
            bool pre_done;
        };
        std::vector<Frame> stk;
        // Push root frame
        stk.push_back({root, root_parent, maxd, maxd, maxd, 0u, true, false});

        while (!stk.empty()) {
            auto& f = stk.back();

            // Pre-order work (runs once per frame)
            if (!f.pre_done) {
                f.pre_done = true;
                stack_trace.push_back(f.u);
                mark[f.u] = true;
                entry_time[f.u] = vtime++;

                // Compute can_end
                f.can_end = true;
                for (const auto& child : g_compacted[f.u]) {
                    if (child.first == dual[f.u] && child.second != -1) {
                        f.can_end = false; break;
                    }
                }
                // Compute h0 for non-recursive edges (parent, self, back-edges)
                for (const auto& child : g_compacted[f.u]) {
                    int v = child.first;
                    if (v == f.parent || v == f.u) {
                        f.h0 = std::min(f.h0, depth[v]);
                        continue;
                    }
                    if (f.parent == -1 && child.second != -1) continue;
                    if (mark[v]) {
                        if (depth[v] <= depth[f.u])
                            f.h0 = std::min(f.h0, depth[v]);
                    }
                }
            }

            // Find next unvisited child to "recurse" into
            bool pushed = false;
            while (f.ci < g_compacted[f.u].size()) {
                const auto& child = g_compacted[f.u][f.ci++];
                int v = child.first;
                if (v == f.parent || v == f.u) continue;
                if (f.parent == -1 && child.second != -1) continue;
                if (mark[v]) continue;
                // Push child frame
                stk.push_back({v, f.u, maxd, maxd, maxd, 0u, true, false});
                pushed = true;
                break;
            }

            if (!pushed) {
                // Post-order work — all children processed
                int u = f.u;
                int parent = f.parent;
                bool can_end = f.can_end;
                int h0 = f.h0, h1 = f.h1, h2 = f.h2;

                bl[u] = alloc_bracket();
                for (const auto& child : g_compacted[u]) {
                    int v = child.first;
                    if (depth[v] != depth[u] + 1) continue;
                    if (parent == -1 && child.second != -1) continue;
                    if (bl[v]) {
                        if (bl[v]->d < h1) { h2 = h1; h1 = bl[v]->d; }
                        else                { h2 = std::min(h2, bl[v]->d); }
                        bl[v]->d = maxd;
                    }
                    if (bl[v] && !bl[v]->merged) merge(bl[u], bl[v]);
                }
                bl[u]->d = std::min(h0, h1);

                if (auto pr_it = pending_removals.find(u);
                    pr_it != pending_removals.end()) {
                    for (Edge* ed : pr_it->second) {
                        if (ed->front) {
                            if (ed->back) {
                                ed->back->front = ed->front;
                                ed->front->back = ed->back;
                            } else {
                                bl[u]->start = ed->front;
                                ed->front->back = nullptr;
                            }
                        } else {
                            bl[u]->end_ = ed->back;
                            if (ed->back) ed->back->front = nullptr;
                            else          bl[u]->start = nullptr;
                        }
                        bl[u]->sz--;
                    }
                    pending_removals.erase(pr_it);
                }
                if (bl[u]->d == depth[u]) bl[u]->d = maxd;

                for (const auto& child : g_compacted[u]) {
                    int v = child.first;
                    if (depth[v] >= depth[u] - 1) continue;
                    if (parent == -1 && child.second != -1) continue;
                    Edge* ed = alloc_edge(child.second);
                    pending_removals[v].push_back(ed);
                    auto* tmp = alloc_bracket(1, depth[v], ed, ed);
                    merge(bl[u], tmp);
                }

                if (h2 <= h0 && h0 != maxd) {
                    int w = stack_trace[h2];
                    Edge* ed = alloc_edge(cnt_gray_edge); cnt_gray_edge++;
                    pending_removals[w].push_back(ed);
                    auto* tmp = alloc_bracket(1, depth[w], ed, ed);
                    merge(bl[u], tmp);
                }

                if (parent != -1 && parent == dual[u] && can_end) {
                    pii key;
                    if (bl[u]->sz > 0) {
                        int br = bl[u]->end_->id;
                        key = {br, bl[u]->sz};
                    } else {
                        key = {0, 0};
                    }
                    auto it = st.find(key);
                    if (it != st.end()) {
                        int w = it->second;
                        canonical_sese[key].push_back({u, w});
                    }
                    st[key] = parent;
                }
                if (bl[u]->sz == 0 && can_end) {
                    is_bridge[black_edge_id[u]] = true;
                }
                exit_time[u] = vtime++;
                stack_trace.pop_back();
                stk.pop_back();
            }
        }
    }

    // Iterativo com post-order e propagação de child_bridge.
    // Caller ignora retorno → void.
    void hairp(int root) {
        struct Frame { int u, parent; size_t ci; bool child_bridge; int cnt; };
        std::vector<Frame> stk = {{root, -1, 0, false, 0}};
        mark[root] = true;
        while (!stk.empty()) {
            auto& f = stk.back();
            bool pushed = false;
            while (f.ci < g_compacted[f.u].size()) {
                const auto& child = g_compacted[f.u][f.ci++];
                if (child.first == f.parent) continue;
                f.cnt++;
                if (mark[child.first]) continue;
                mark[child.first] = true;
                stk.push_back({child.first, f.u, 0, false, 0});
                pushed = true;
                break;
            }
            if (!pushed) {
                int u = f.u;
                bool cb = f.child_bridge;
                int cnt = f.cnt;
                bool result = cb | static_cast<bool>(is_bridge[black_edge_id[u]]);
                if (!cb && is_bridge[black_edge_id[u]] && cnt != 0) {
                    possible_hairpins.push_back({u, u});
                }
                stk.pop_back();
                if (!stk.empty()) stk.back().child_bridge |= result;
            }
        }
    }

    bool end_gene(int u, pii rs) {
        return u == rs.first || u == dual[rs.first]
               || u == rs.second || u == dual[rs.second];
    }
    bool root_leaf_path(int u, int v) {
        return entry_time[u] <= entry_time[v] && exit_time[u] >= exit_time[v];
    }
    void upd_node(int u, int ty) {
        visit_order[u] = static_cast<short>(ty);
        int gene = black_edge_id[u];
        if (ty == 0) {
            if (to_chk[gene] == -1) {
                counter++;
                to_chk[gene] = static_cast<short>(ty_end[u]);
                considered.push_back(u);
            } else {
                if (ty_end[u] != to_chk[gene]) to_chk[gene] = 2;
            }
        } else {
            if (to_chk[gene] == 2) {
                counter--;
                to_chk[gene] = -2;
            } else {
                short required = static_cast<short>(1 - ty_end[u]);
                if (to_chk[gene] == required || to_chk[gene] == -1) {
                    if (to_chk[gene] == -1) considered.push_back(u);
                    counter--;
                    to_chk[gene] = -2;
                }
            }
        }
    }
    // Iterativo. Sem skip por visit_order[u] no início — o recursivo original
    // SEMPRE executa is_bridge/upd_node em cada entrada (proteção contra
    // revisita do mesmo u é feita por to_chk dentro de upd_node).
    bool mark_nodes(int root, pii rs, int ty) {
        std::vector<std::pair<int, size_t>> stk = {{root, 0}};
        while (!stk.empty()) {
            auto& [u, ci] = stk.back();
            if (ci == 0) {
                if (is_bridge[black_edge_id[u]]
                    && (!root_leaf_path(dual[rs.first], u)
                        || !root_leaf_path(u, dual[rs.second]))) return false;
                if (!end_gene(u, rs)) upd_node(u, ty);
            }
            bool pushed = false;
            const auto& adj = g_compacted[dual[u]];
            while (ci < adj.size()) {
                const auto& child = adj[ci++];
                int v = child.first;
                if (child.second >= tip_start[biedged_connected_comp[rs.second]]
                    || child.second == -1
                    || end_gene(v, rs)) continue;
                if (visit_order[v] == ty) continue;
                stk.push_back({v, 0});
                pushed = true;
                break;
            }
            if (!pushed) stk.pop_back();
        }
        return true;
    }
};

// Constrói o estado Billi a partir da nossa GfaCsrTopology.
// Reproduz get_ne + make_graph do Billi sem ler arquivo GFA — extraindo
// orientações diretamente da CSR, sem hashmap intermediário.
//
// Pareamento s1/s2 sem hashmap: cada link aparece uma vez em E_fwd_ (com
// to-orient = s2) e uma vez em E_bwd_ (com from-orient = s1). O LSD radix
// sort do build_csr é estável → para qualquer (u, v), a i-ésima ocorrência
// de v em out_neighbors(u) corresponde à i-ésima ocorrência de u em
// in_neighbors(v). Pareamos por posição.
//
// Custo: O(E · d_avg) tempo, O(0) memória extra — vs O(E) memória de dois
// hashmaps (~14 GB) e até T cópias locais (~30+ GB transitórios em HPRC) na
// versão anterior.
static void build_state_from_topo(BilliState& s, const GfaCsrTopology& topo) {
    const uint64_t N = topo.num_nodes();
    assert(N <= static_cast<uint64_t>(INT32_MAX) &&
           "snarl_decomposition: num_nodes excede INT32_MAX — overflow em biedged IDs");
    s.n = static_cast<int>(N);

    // Setup biedged: g[2*n] tem black edges + gray edges
    s.g.assign(2 * s.n, {});
    s.has_self_loop.assign(2 * s.n, false);
    s.maxd = 2 * s.n + 100;

    // Black edges (id=-1) entre 2*i e 2*i+1 PRIMEIRO
    for (int i = 0; i < s.n; ++i) {
        s.g[2*i].push_back({2*i + 1, -1});
        s.g[2*i + 1].push_back({2*i, -1});
    }

    // Gray edges — extração streaming da CSR.
    for (uint64_t u = 0; u < N; ++u) {
        const auto out = topo.out_neighbors(u);
        for (size_t k = 0; k < out.size(); ++k) {
            const auto e_out = GfaCsrTopology::decode(out[k]);
            const uint64_t v = e_out.node_id;

            // occ = nº de ocorrências anteriores de v em out_neighbors(u);
            // serve para parear corretamente em multi-edges (raríssimo, mas
            // possível via self-loops com orientações distintas).
            size_t occ = 0;
            for (size_t j = 0; j < k; ++j) {
                if ((out[j] >> 1) == v) ++occ;
            }

            // s1 = (occ)-ésima ocorrência de u em in_neighbors(v).
            char s1 = '+';
            bool found = false;
            const auto in_v = topo.in_neighbors(v);
            size_t seen = 0;
            for (uint32_t enc : in_v) {
                if ((enc >> 1) != u) continue;
                if (seen == occ) {
                    s1 = (enc & 1u) ? '-' : '+';
                    found = true;
                    break;
                }
                ++seen;
            }
            if (!found) continue; // GFA inconsistente — pula link órfão

            int n1 = static_cast<int>(u);
            int n2 = static_cast<int>(v);
            char s2 = e_out.orient;
            // Consistência de direção (do Billi)
            if (n1 > n2) {
                std::swap(n1, n2);
                s1 = (s1 == '+') ? '-' : '+';
                s2 = (s2 == '+') ? '-' : '+';
                std::swap(s1, s2);
            }
            int id1 = n1 << 1;
            int id2 = n2 << 1;
            if (s1 == '+') id1++;
            if (s2 == '-') id2++;
            s.add_edge(s.g, id1, id2);
            if (n1 == n2 && s1 != s2) s.has_self_loop[id1] = true;
            ++s.edges_count;
        }
    }
}

// Pipeline principal — port literal de run_decompose() do Billi.
static std::vector<SnarlRecord> run_billi_pipeline(const GfaCsrTopology& topo) {
    BilliState s;
    build_state_from_topo(s, topo);

    if (s.n == 0) return {};

    // *** Graph Cleaning ***
    // Removing parallel gray edges
    for (int i = 0; i < 2 * s.n; ++i) {
        std::sort(s.g[i].begin() + 1, s.g[i].end());
        int add = 0;
        if (s.g[i].size() >= 2 && s.g[i][0].first == s.g[i][1].first) add = 1;
        s.g[i].erase(std::unique(s.g[i].begin() + add, s.g[i].end(),
                                  [](const pii& a, const pii& b) { return a.first == b.first; }),
                     s.g[i].end());
    }

    // Compaction (remove unitigs)
    s.mark.assign(2 * s.n, false);
    s.dual.assign(2 * s.n, -1);
    s.black_edge_id.assign(2 * s.n, -1);
    s.ty_end.assign(2 * s.n, -1);
    s.g_compacted.assign(2 * s.n, {});

    for (int i = 0; i < 2 * s.n; ++i) {
        if (s.mark[i]) continue;
        if (s.chk_for_compaction(i)) {
            s.q_compact.push({i ^ 1, 0});
            s.q_compact.push({s.g[i][1].first ^ 1, 1});
            int left = i, right = s.g[i][1].first;
            s.mark[left] = s.mark[right] = true;
            while (!s.q_compact.empty()) {
                pii pid = s.q_compact.front(); s.q_compact.pop();
                if (s.chk_for_compaction(pid.first)) {
                    s.mark[pid.first] = true;
                    int neigh = s.g[pid.first][1].first;
                    s.mark[neigh] = true;
                    if (s.mark[neigh ^ 1]) continue;
                    if (pid.second == 0) {
                        left = neigh;
                        s.q_compact.push({left ^ 1, 0});
                    } else {
                        right = neigh;
                        s.q_compact.push({right ^ 1, 1});
                    }
                }
            }
            int l = left ^ 1, r = right ^ 1;
            s.g_compacted[l].push_back({r, -1});
            s.g_compacted[r].push_back({l, -1});
            s.dual[l] = r; s.dual[r] = l;
            s.ty_end[l] = 0; s.ty_end[r] = 1;
            s.black_edge_id[l] = s.black_edge_id[r] = s.cnt_black_edge++;
            for (size_t j = 1; j < s.g[l].size(); ++j) {
                int u = s.g[l][j].first;
                if (s.mark[u] && u != r) continue;
                if (l != u) s.g_compacted[l].push_back({u, s.cnt_gray_edge});
                s.g_compacted[u].push_back({l, s.cnt_gray_edge++});
            }
            for (size_t j = 1; j < s.g[r].size(); ++j) {
                int u = s.g[r][j].first;
                if (s.mark[u]) continue;
                if (r != u) s.g_compacted[r].push_back({u, s.cnt_gray_edge});
                s.g_compacted[u].push_back({r, s.cnt_gray_edge++});
            }
            s.mark[l] = s.mark[r] = true;
        }
    }
    for (int i = 0; i < 2 * s.n; ++i) {
        if (s.mark[i]) continue;
        if (!s.mark[i ^ 1]) {
            s.g_compacted[i].push_back({i ^ 1, -1});
            s.g_compacted[i ^ 1].push_back({i, -1});
            s.dual[i ^ 1] = i; s.dual[i] = i ^ 1;
            s.ty_end[i] = 0; s.ty_end[i ^ 1] = 1;
            s.black_edge_id[i] = s.black_edge_id[i ^ 1] = s.cnt_black_edge++;
        }
        for (size_t j = 1; j < s.g[i].size(); ++j) {
            int u = s.g[i][j].first;
            if (s.mark[u]) continue;
            if (i != u) s.g_compacted[i].push_back({u, s.cnt_gray_edge});
            s.g_compacted[u].push_back({i, s.cnt_gray_edge++});
        }
        s.mark[i] = true;
    }

    // Swap black edge to position 0 in each adj
    for (int i = 0; i < 2 * s.n; ++i) {
        for (auto& child : s.g_compacted[i]) {
            if (child.second == -1) {
                std::swap(child, s.g_compacted[i][0]);
                break;
            }
        }
    }
    // cnt_black_edge é incrementado uma vez por nó canônico; deve caber em [0, n].
    assert(s.cnt_black_edge <= s.n);
    // Libera o envelope (~200MB em CHM13): clear() mantém capacity de 2*n pointers.
    { decltype(s.g) tmp; s.g.swap(tmp); }

    // Connected components in compacted
    std::fill(s.mark.begin(), s.mark.end(), false);
    s.biedged_connected_comp.assign(2 * s.n, 0);
    for (int i = 0; i < 2 * s.n; ++i) {
        if (s.mark[i] || s.g_compacted[i].empty()) continue;
        s.dfs_comp(i);
        s.first_node.push_back(i);
        s.component++;
    }

    // Identifying tips
    s.tips.assign(s.component, {});
    int cnt_zero = 0;
    for (int i = 0; i < 2 * s.n; ++i) {
        if (s.g_compacted[i].empty()) continue;
        if (s.find_unique_including_selfloop(i) == 0) {
            s.tips[s.biedged_connected_comp[i]].push_back(i);
        }
    }
    for (int i = 0; i < s.component; ++i) {
        if (s.tips[i].empty()) cnt_zero++;
    }
    if (cnt_zero != 0) return {}; // Billi também aborta nesse caso

    // Finding S node + adding extra gray edges for tips
    constexpr int maxv = INT32_MAX;
    s.tip_start.assign(s.component, -1);
    s.Start.assign(s.component, -1);
    s.depth.assign(2 * s.n, 0);
    s.bl.assign(2 * s.n, nullptr);
    s.rm_cnt.assign(2 * s.n, 0);
    std::fill(s.mark.begin(), s.mark.end(), false);

    for (int it = 0; it < s.component; ++it) {
        if (s.tips[it].empty()) {
            int val = s.edges_count;
            s.sese_minbracket(s.first_node[it], s.Start[it], val);
        } else {
            s.Start[it] = s.tips[it][0];
            int u = s.Start[it];
            for (size_t i = 1; i < s.tips[it].size(); ++i) {
                if (s.tip_start[it] == -1) s.tip_start[it] = s.cnt_gray_edge;
                int v = s.tips[it][i];
                s.g_compacted[u].push_back({v, s.cnt_gray_edge});
                s.g_compacted[v].push_back({u, s.cnt_gray_edge++});
            }
        }
        if (s.tips[it].size() <= 1) s.tip_start[it] = maxv;
    }
    s.rm_cnt.clear(); s.first_node.clear();

    // Depth DFS
    std::fill(s.depth.begin(), s.depth.end(), 0);
    std::fill(s.mark.begin(), s.mark.end(), false);
    s.entry_time.assign(2 * s.n, 0);
    s.exit_time.assign(2 * s.n, 0);
    for (int it = 0; it < s.component; ++it) {
        s.dfs_depth(s.Start[it]);
    }

    // SESE main
    s.pending_removals.clear();
    s.is_bridge.assign(s.n, false);
    s.bl.assign(2 * s.n, nullptr);
    std::fill(s.mark.begin(), s.mark.end(), false);

    for (int it = 0; it < s.component; ++it) {
        s.st.clear();
        s.sese(s.Start[it], -1);
        if (s.tips[it].empty()) {
            s.st.clear();
            s.mark[s.Start[it]] = s.mark[s.dual[s.Start[it]]] = false;
            s.sese(s.dual[s.Start[it]], -1);
        }
    }
    std::fill(s.mark.begin(), s.mark.end(), false);
    for (int it = 0; it < s.component; ++it) {
        s.hairp(s.Start[it]);
    }

    // Brute force panbubbles validation
    std::fill(s.mark.begin(), s.mark.end(), false);
    s.visit_order.assign(2 * s.n, -1);
    s.to_chk.assign(s.n, -1);

    // Ordenação requerida: paridade Billi exige iteração em ordem crescente
    // de (top_bracket, sz). std::map fazia isso de graça; aqui copiamos
    // para vector + sort O(K log K), mais cache-friendly e mais rápido que RB-tree.
    std::vector<std::pair<pii, std::vector<pii>>> sorted_sese;
    sorted_sese.reserve(s.canonical_sese.size());
    for (auto& kv : s.canonical_sese) {
        sorted_sese.emplace_back(kv.first, std::move(kv.second));
    }
    s.canonical_sese.clear();
    std::sort(sorted_sese.begin(), sorted_sese.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [key, vec] : sorted_sese) {
        int sz = static_cast<int>(vec.size());
        for (int i = 0; i < sz; ) {
            bool is_valid = true;
            int j = -1, cnt = 0;
            for (j = i; j < sz; ++j) {
                pii rs = {vec[j].first, vec[i].second};
                if (j != i && s.dual[vec[j].second] != vec[j - 1].first) {
                    is_valid = false;
                    break;
                }
                if (!s.root_leaf_path(rs.first, rs.second)) continue;
                cnt++;
                if (cnt > s.offset) { is_valid = false; break; }
                is_valid = true;

                for (int u : s.considered) {
                    s.visit_order[u] = s.visit_order[s.dual[u]] = -1;
                    s.to_chk[s.black_edge_id[u]] = -1;
                }
                s.considered.clear();
                s.counter = 0;

                is_valid = s.mark_nodes(s.dual[rs.first], rs, 0);
                if (!is_valid) continue;
                is_valid &= s.mark_nodes(s.dual[rs.second], rs, 1);
                is_valid &= (s.counter == 0);

                if (is_valid) {
                    s.valid_panbubbles.push_back(rs);
                    i = j + (j == i ? 1 : 0);
                    break;
                }
            }
            if (!is_valid || j == sz) i++;
        }
    }
    s.considered.clear();

    // Hairpins validation
    std::fill(s.mark.begin(), s.mark.end(), false);
    std::fill(s.visit_order.begin(), s.visit_order.end(), -1);
    std::fill(s.to_chk.begin(), s.to_chk.end(), -1);

    for (size_t i = 0; i < s.possible_hairpins.size(); ++i) {
        pii rs = s.possible_hairpins[i];
        if ((s.g_compacted[rs.first].size() == 2
             && s.g_compacted[rs.first][1].first == s.g_compacted[rs.first][0].first)
            || s.g_compacted[rs.first].size() <= 1) continue;

        bool is_valid = true;
        for (int u : s.considered) {
            s.visit_order[u] = s.visit_order[s.dual[u]] = -1;
            s.to_chk[s.black_edge_id[u]] = -1;
        }
        s.considered.clear();
        s.counter = 0;

        is_valid = s.mark_nodes(s.dual[rs.first], rs, 0);
        if (!is_valid) continue;
        is_valid &= s.mark_nodes(s.dual[rs.second], rs, 1);
        is_valid &= (s.counter == 0);

        if (is_valid) {
            s.valid_hairpins.push_back(rs);
        }
    }

    // Converte para SnarlRecord (mapeia IDs Billi → IDs GFA originais)
    std::vector<SnarlRecord> out;
    out.reserve(s.valid_panbubbles.size() + s.valid_hairpins.size());
    for (const auto& rs : s.valid_panbubbles) {
        const uint64_t a = static_cast<uint64_t>(rs.first  >> 1);
        const uint64_t b = static_cast<uint64_t>(rs.second >> 1);
        if (a == b) continue;
        SnarlRecord rec{};
        rec.source = std::min(a, b);
        rec.sink   = std::max(a, b);
        rec.branch_count = 2;
        rec.internal_node_count = 0;
        rec.has_cycles = 0;
        out.push_back(rec);
    }
    for (const auto& rs : s.valid_hairpins) {
        const uint64_t a = static_cast<uint64_t>(rs.first  >> 1);
        const uint64_t b = static_cast<uint64_t>(rs.second >> 1);
        SnarlRecord rec{};
        rec.source = std::min(a, b);
        rec.sink   = std::max(a, b);
        rec.branch_count = 2;
        rec.internal_node_count = 0;
        rec.has_cycles = 1;
        out.push_back(rec);
    }
    return out;
}

} // namespace billi_port

// API pública: port literal do algoritmo Billi (não wrapper).
std::vector<SnarlRecord> find_all_snarls(const GfaCsrTopology& topo,
                                          int /*num_threads*/) {
    return billi_port::run_billi_pipeline(topo);
}

} // namespace ggql::snarl_decomposition
