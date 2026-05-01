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
//   - dfs_comp, find_unique_including_selfloop, find_unique_excluding_selfloop
//   - dfs_depth, merge, sese_minbracket
//   - sese (algoritmo principal SESE)
//   - hairp (detecção de hairpins)
//   - dfs_nesting (hierarquia parent/child)
//   - end_gene, root_leaf_path, upd_node, mark_nodes (validação brute force)
//   - Decompose pipeline completo
//
// Macros do Billi (pb, F, S, etc.) substituídas por nomes claros.
//
// Refs: github.com/at-cg/billi src/{include,subcommand}/{commons,decompose}.{cpp,hpp}

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <climits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ggql::snarl_decomposition {

namespace billi_port {

using pii = std::pair<int, int>;

// Estrutura encapsulando o estado do algoritmo Billi (que originalmente
// usa variáveis globais). Cada chamada cria uma instância nova → thread-safe.
struct BilliState {
    // ── Generic ──────────────────────────────────────────────────────
    int maxd = 0;
    int tot_gray = 0;
    std::vector<bool> mark;
    std::vector<pii> edge_label;

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

    // ── Input (GFA → biedged graph) ──────────────────────────────────
    int n = 0;            // # nós GFA
    int edges_count = 0;  // # links
    std::vector<bool> has_self_loop;
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
    std::vector<int> panbubble_end, hairpin_end;
    std::vector<int> parent_bubble, parent_hairpin;
    std::vector<int> entry_time, exit_time;
    std::vector<int> stack_trace;
    std::vector<bool> is_bridge;
    std::vector<pii> possible_hairpins;
    std::map<pii, std::vector<pii>> canonical_sese;
    std::map<pii, int> st;
    std::vector<BracketList*> bl;
    std::vector<std::vector<Edge*>> remove_brackets;

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

    void dfs_comp(int u) {
        mark[u] = true;
        biedged_connected_comp[u] = component;
        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (mark[v]) continue;
            dfs_comp(v);
        }
    }

    int find_unique_including_selfloop(int u) {
        std::set<int> s;
        for (pii child : g_compacted[u]) {
            if (child.first == dual[u]) continue;
            s.insert(child.first);
            break;
        }
        return static_cast<int>(s.size());
    }

    bool in_subtree(int v, int u, int exit_time_u) {
        return entry_time[v] >= entry_time[u] && exit_time[v] <= exit_time_u;
    }

    int find_unique_excluding_selfloop(int u, int v) {
        std::set<int> s;
        for (pii child : g_compacted[u]) {
            if (child.first == u || child.first == dual[u] || child.first == v)
                continue;
            s.insert(child.first);
            break;
        }
        return static_cast<int>(s.size());
    }

    void dfs_depth(int u, int parent) {
        if (parent != -1) depth[u] = depth[parent] + 1;
        mark[u] = true;
        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (mark[v]) continue;
            dfs_depth(v, u);
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

    void sese_minbracket(int u, int parent, int& x, int& val) {
        mark[u] = true;
        if (parent != -1) depth[u] = depth[parent] + 1;
        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (v == parent || v == u) continue;
            if (mark[v]) {
                if (depth[v] > depth[u]) continue;
            } else sese_minbracket(v, u, x, val);
        }
        bl[u] = new BracketList();
        for (pii child : g_compacted[u]) {
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
        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (depth[v] >= depth[u] - 1) continue;
            rm_cnt[v]++;
            bl[u]->sz++;
        }
    }

    void sese(int u, int parent) {
        stack_trace.push_back(u);
        mark[u] = true;
        entry_time[u] = vtime++;

        bool can_end = true;
        int h0 = maxd, h1 = maxd, h2 = maxd;

        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (v == dual[u] && child.second != -1) { can_end = false; break; }
        }
        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (v == parent || v == u) {
                h0 = std::min(h0, depth[v]);
                continue;
            }
            if (parent == -1 && child.second != -1) continue;
            if (mark[v]) {
                if (depth[v] > depth[u]) continue;
                h0 = std::min(h0, depth[v]);
            } else sese(v, u);
        }
        bl[u] = new BracketList();
        for (pii child : g_compacted[u]) {
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
        for (Edge* ed : remove_brackets[u]) {
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
            delete ed;
            bl[u]->sz--;
        }
        remove_brackets[u].clear();
        if (bl[u]->d == depth[u]) bl[u]->d = maxd;

        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (depth[v] >= depth[u] - 1) continue;
            if (parent == -1 && child.second != -1) continue;
            Edge* ed = new Edge(child.second);
            remove_brackets[v].push_back(ed);
            BracketList* tmp = new BracketList(1, depth[v], ed, ed);
            merge(bl[u], tmp);
            delete tmp;
        }

        if (h2 <= h0 && h0 != maxd) {
            int w = stack_trace[h2];
            Edge* ed = new Edge(cnt_gray_edge); cnt_gray_edge++;
            remove_brackets[w].push_back(ed);
            BracketList* tmp = new BracketList(1, depth[w], ed, ed);
            merge(bl[u], tmp);
            delete tmp;
            edge_label.push_back({u, w});
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
    }

    bool hairp(int u, int parent) {
        mark[u] = true;
        bool child_bridge = false;
        int cnt = 0;
        for (pii child : g_compacted[u]) {
            if (child.first == parent) continue;
            cnt++;
            if (mark[child.first]) continue;
            child_bridge |= hairp(child.first, u);
        }
        if (!child_bridge && is_bridge[black_edge_id[u]] && cnt != 0) {
            possible_hairpins.push_back({u, u});
        }
        return child_bridge | is_bridge[black_edge_id[u]];
    }

    void dfs_nesting(int u, int parent) {
        mark[u] = true;
        if (hairpin_end[u] != -1) stack_trace.push_back(u);
        for (pii child : g_compacted[u]) {
            int v = child.first;
            if (mark[v]) continue;
            dfs_nesting(v, u);
        }
        if (hairpin_end[u] != -1) stack_trace.pop_back();
        if (parent == dual[u]) {
            if (panbubble_end[u] != -1 && hairpin_end[u] == -1) {
                int sz = static_cast<int>(stack_trace.size());
                if (sz > 0 && panbubble_end[stack_trace[sz - 1]] == panbubble_end[u]) {
                    stack_trace.pop_back();
                    if (sz > 1) {
                        if (panbubble_end[stack_trace[sz - 2]] != -1)
                            parent_bubble[panbubble_end[u]] = panbubble_end[stack_trace[sz - 2]];
                        else if (hairpin_end[stack_trace[sz - 2]] != -1)
                            parent_hairpin[panbubble_end[u]] = hairpin_end[stack_trace[sz - 2]];
                    }
                }
            }
            if (panbubble_end[parent] != -1) stack_trace.push_back(parent);
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
    bool mark_nodes(int u, pii rs, int ty) {
        if (is_bridge[black_edge_id[u]]
            && (!root_leaf_path(dual[rs.first], u)
                || !root_leaf_path(u, dual[rs.second]))) return false;
        if (!end_gene(u, rs)) upd_node(u, ty);
        for (pii child : g_compacted[dual[u]]) {
            int v = child.first;
            if (child.second >= tip_start[biedged_connected_comp[rs.second]]
                || child.second == -1
                || end_gene(v, rs)) continue;
            if (visit_order[v] == ty) continue;
            if (!mark_nodes(v, rs, ty)) return false;
        }
        return true;
    }
};

// Constrói o estado Billi a partir da nossa GfaCsrTopology.
// Reproduz get_ne + make_graph do Billi sem ler arquivo GFA.
static void build_state_from_topo(BilliState& s, const GfaCsrTopology& topo) {
    const uint64_t N = topo.num_nodes();
    s.n = static_cast<int>(N);

    // Reconstrói LinkRecords com o1+o2 via cross-correlation
    using Pair = std::pair<uint64_t, uint64_t>;
    struct PairHash {
        size_t operator()(const Pair& p) const noexcept {
            return std::hash<uint64_t>()(p.first) * 1315423911u
                 ^ std::hash<uint64_t>()(p.second);
        }
    };
    std::unordered_map<Pair, std::vector<char>, PairHash> fwd_orients;
    std::unordered_map<Pair, std::vector<char>, PairHash> bwd_orients;
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

    // Setup biedged: g[2*n] tem black edges + gray edges
    s.g.assign(2 * s.n, {});
    s.has_self_loop.assign(2 * s.n, false);
    s.maxd = 2 * s.n + 100;

    // Black edges (id=-1) entre 2*i e 2*i+1 PRIMEIRO
    for (int i = 0; i < s.n; ++i) {
        s.g[2*i].push_back({2*i + 1, -1});
        s.g[2*i + 1].push_back({2*i, -1});
    }

    // Gray edges (links GFA) — port direto de make_graph()
    for (auto& [key, o2s] : fwd_orients) {
        auto it = bwd_orients.find(key);
        if (it == bwd_orients.end()) continue;
        const auto& o1s = it->second;
        const size_t n_links = std::min(o2s.size(), o1s.size());
        for (size_t i = 0; i < n_links; ++i) {
            int n1 = static_cast<int>(key.first);
            int n2 = static_cast<int>(key.second);
            char s1 = o1s[i], s2 = o2s[i];
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
                s.edge_label.push_back({l, u});
            }
            for (size_t j = 1; j < s.g[r].size(); ++j) {
                int u = s.g[r][j].first;
                if (s.mark[u]) continue;
                if (r != u) s.g_compacted[r].push_back({u, s.cnt_gray_edge});
                s.g_compacted[u].push_back({r, s.cnt_gray_edge++});
                s.edge_label.push_back({r, u});
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
            s.edge_label.push_back({i, u});
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
    s.g.clear();

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
            s.sese_minbracket(s.first_node[it], -1, s.Start[it], val);
        } else {
            s.Start[it] = s.tips[it][0];
            int u = s.Start[it];
            for (size_t i = 1; i < s.tips[it].size(); ++i) {
                if (s.tip_start[it] == -1) s.tip_start[it] = s.cnt_gray_edge;
                int v = s.tips[it][i];
                s.g_compacted[u].push_back({v, s.cnt_gray_edge});
                s.g_compacted[v].push_back({u, s.cnt_gray_edge++});
                s.edge_label.push_back({u, v});
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
        s.dfs_depth(s.Start[it], -1);
    }

    // SESE main
    s.remove_brackets.assign(2 * s.n, {});
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
        s.hairp(s.Start[it], -1);
    }

    // Brute force panbubbles validation
    std::fill(s.mark.begin(), s.mark.end(), false);
    s.visit_order.assign(2 * s.n, -1);
    s.to_chk.assign(s.n, -1);
    s.panbubble_end.assign(2 * s.n, -1);

    for (const auto& [key, vec] : s.canonical_sese) {
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
                    s.panbubble_end[rs.first] = s.panbubble_end[rs.second]
                        = static_cast<int>(s.valid_panbubbles.size());
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
    s.hairpin_end.assign(2 * s.n, -1);

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
            s.hairpin_end[rs.first] = static_cast<int>(s.valid_hairpins.size());
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
std::vector<SnarlRecord> find_all_snarls_billi_port(const GfaCsrTopology& topo,
                                                     int /*num_threads*/) {
    return billi_port::run_billi_pipeline(topo);
}

} // namespace ggql::snarl_decomposition
