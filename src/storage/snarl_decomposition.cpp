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
    //
    // Adjacência CSR plana economiza ~5 GB em pangenomas grandes vs
    // vector<vector<pii>> (220 M envelopes vazios × 24 B = 5.3 GB no HPRC).
    // Black edge sempre fica em ed[off[id]+0] por construção.
    struct CsrAdj {
        std::vector<uint32_t> off; // size 2n+1
        std::vector<pii> ed;       // size = off[2n]
        size_t deg(int id) const noexcept {
            return static_cast<size_t>(off[id + 1] - off[id]);
        }
        pii& at(int id, size_t k) noexcept { return ed[off[id] + k]; }
        const pii& at(int id, size_t k) const noexcept { return ed[off[id] + k]; }
    };

    int n = 0;            // # nós GFA
    int edges_count = 0;  // # links
    std::vector<uint8_t> has_self_loop;
    CsrAdj g; // adjacência biedged (black + gray); descartada após compaction

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
    // g_compacted é vector<vector<>> durante a fase de build (compaction +
    // swap-black + tip-add) porque sofre push_back dinâmico. Após o último
    // write é feito FREEZE para a CSR plana g_compacted_csr (~5 GB economia
    // sustentada no HPRC durante todo o SESE/hairp/mark_nodes).
    std::vector<std::vector<pii>> g_compacted;
    CsrAdj g_compacted_csr;

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

    bool chk_vertex(int id) {
        return g.deg(id) == 2 && g.at(id, 0).first != g.at(id, 1).first
               && !has_self_loop[id];
    }
    bool chk_for_compaction(int id) {
        if (chk_vertex(id)) {
            int u = g.at(id, 1).first;
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
            const size_t deg_u = g_compacted_csr.deg(f.u);
            while (f.ci < deg_u) {
                int v = g_compacted_csr.at(f.u, f.ci++).first;
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
                {
                    const size_t deg_u = g_compacted_csr.deg(f.u);
                    for (size_t k = 0; k < deg_u; ++k) {
                        const auto& child = g_compacted_csr.at(f.u, k);
                        if (child.first == dual[f.u] && child.second != -1) {
                            f.can_end = false; break;
                        }
                    }
                }
                // Compute h0 for non-recursive edges (parent, self, back-edges)
                {
                    const size_t deg_u = g_compacted_csr.deg(f.u);
                    for (size_t k = 0; k < deg_u; ++k) {
                        const auto& child = g_compacted_csr.at(f.u, k);
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
            }

            // Find next unvisited child to "recurse" into
            bool pushed = false;
            const size_t deg_u = g_compacted_csr.deg(f.u);
            while (f.ci < deg_u) {
                const auto& child = g_compacted_csr.at(f.u, f.ci++);
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
                {
                    const size_t deg_u_post = g_compacted_csr.deg(u);
                    for (size_t k = 0; k < deg_u_post; ++k) {
                        const auto& child = g_compacted_csr.at(u, k);
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

                {
                    const size_t deg_u_back = g_compacted_csr.deg(u);
                    for (size_t k = 0; k < deg_u_back; ++k) {
                        const auto& child = g_compacted_csr.at(u, k);
                        int v = child.first;
                        if (depth[v] >= depth[u] - 1) continue;
                        if (parent == -1 && child.second != -1) continue;
                        Edge* ed = alloc_edge(child.second);
                        pending_removals[v].push_back(ed);
                        auto* tmp = alloc_bracket(1, depth[v], ed, ed);
                        merge(bl[u], tmp);
                    }
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
            const size_t deg_u = g_compacted_csr.deg(f.u);
            while (f.ci < deg_u) {
                const auto& child = g_compacted_csr.at(f.u, f.ci++);
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
            const int du = dual[u];
            const size_t deg_du = g_compacted_csr.deg(du);
            while (ci < deg_du) {
                const auto& child = g_compacted_csr.at(du, ci++);
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
// orientações diretamente da CSR, sem hashmap intermediário, e populando
// adjacência biedged em CSR plana (sem vector<vector<>>).
//
// Pareamento s1/s2 sem hashmap: cada link aparece uma vez em E_fwd_ (com
// to-orient = s2) e uma vez em E_bwd_ (com from-orient = s1). O LSD radix
// sort do build_csr é estável → para qualquer (u, v), a i-ésima ocorrência
// de v em out_neighbors(u) corresponde à i-ésima ocorrência de u em
// in_neighbors(v). Pareamos por posição.
//
// Build em 3 passes para CSR plana:
//   1. count[id]  — degree por nó biedged (black + gray contributions)
//   2. offsets    — prefix sum
//   3. fill       — black primeiro (vai para off[id]+0), depois gray
//
// Custo: O(E · d_avg) tempo (resolve_link executado 2× — ~15 s extra no HPRC).
// Memória: O(0) overhead transitório vs ~5 GB do envelope vector<vector<>>.
static void build_state_from_topo(BilliState& s, const GfaCsrTopology& topo) {
    const uint64_t N = topo.num_nodes();
    assert(N <= static_cast<uint64_t>(INT32_MAX) &&
           "snarl_decomposition: num_nodes excede INT32_MAX — overflow em biedged IDs");
    s.n = static_cast<int>(N);

    s.has_self_loop.assign(2 * s.n, false);
    s.maxd = 2 * s.n + 100;

    // Resolve um link via pareamento por posição na CSR.
    // Retorna false se in_neighbors(v) não tem entry correspondente (GFA órfão).
    auto resolve_link = [&topo](uint64_t u, size_t k,
                                int& id1, int& id2,
                                int& n1, int& n2,
                                char& s1, char& s2) -> bool {
        const auto out = topo.out_neighbors(u);
        const auto e_out = GfaCsrTopology::decode(out[k]);
        const uint64_t v = e_out.node_id;

        size_t occ = 0;
        for (size_t j = 0; j < k; ++j) {
            if ((out[j] >> 1) == v) ++occ;
        }

        s1 = '+';
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
        if (!found) return false;

        n1 = static_cast<int>(u);
        n2 = static_cast<int>(v);
        s2 = e_out.orient;
        if (n1 > n2) {
            std::swap(n1, n2);
            s1 = (s1 == '+') ? '-' : '+';
            s2 = (s2 == '+') ? '-' : '+';
            std::swap(s1, s2);
        }
        id1 = n1 << 1;
        id2 = n2 << 1;
        if (s1 == '+') id1++;
        if (s2 == '-') id2++;
        return true;
    };

    // ── Pass 1: count entries por nó biedged ────────────────────────────
    std::vector<uint32_t> count(2 * s.n, 0u);
    // Black edges: cada par (2i, 2i+1) contribui 1 entry para cada lado.
    for (int i = 0; i < s.n; ++i) {
        count[2*i]++;
        count[2*i + 1]++;
    }
    for (uint64_t u = 0; u < N; ++u) {
        const size_t out_sz = topo.out_neighbors(u).size();
        for (size_t k = 0; k < out_sz; ++k) {
            int id1, id2, n1, n2;
            char s1, s2;
            if (!resolve_link(u, k, id1, id2, n1, n2, s1, s2)) continue;
            count[id1]++;
            if (id1 != id2) count[id2]++;
        }
    }

    // ── Pass 2: prefix sum → offsets ────────────────────────────────────
    s.g.off.assign(2 * s.n + 1, 0u);
    for (int i = 0; i < 2 * s.n; ++i) {
        s.g.off[i + 1] = s.g.off[i] + count[i];
    }
    s.g.ed.resize(s.g.off[2 * s.n]);
    // count vira cursor (escritas avançam cursor[id] a partir de off[id]).
    auto& cursor = count; // reaproveita buffer
    for (int i = 0; i < 2 * s.n; ++i) cursor[i] = s.g.off[i];

    // ── Pass 3a: black edges PRIMEIRO (ficam em off[id] + 0) ────────────
    for (int i = 0; i < s.n; ++i) {
        s.g.ed[cursor[2*i]++]     = {2*i + 1, -1};
        s.g.ed[cursor[2*i + 1]++] = {2*i,     -1};
    }

    // ── Pass 3b: gray edges ─────────────────────────────────────────────
    for (uint64_t u = 0; u < N; ++u) {
        const size_t out_sz = topo.out_neighbors(u).size();
        for (size_t k = 0; k < out_sz; ++k) {
            int id1, id2, n1, n2;
            char s1, s2;
            if (!resolve_link(u, k, id1, id2, n1, n2, s1, s2)) continue;
            s.g.ed[cursor[id1]++] = {id2, s.tot_gray};
            if (id1 != id2) s.g.ed[cursor[id2]++] = {id1, s.tot_gray};
            ++s.tot_gray;
            if (n1 == n2 && s1 != s2) s.has_self_loop[id1] = true;
            ++s.edges_count;
        }
    }
    // cursor (= count buffer) descartado ao sair do escopo.
}

// Pipeline principal — port literal de run_decompose() do Billi.
static std::vector<SnarlRecord> run_billi_pipeline(const GfaCsrTopology& topo) {
    BilliState s;
    build_state_from_topo(s, topo);

    if (s.n == 0) return {};

    // *** Graph Cleaning ***
    // Sort + dedup in-place sobre a CSR plana. Black edge (em ed[off[i]+0])
    // sempre sobrevive; gray slice [off[i]+1, off[i+1]) é ordenado e
    // colapsado por .first. Compactação in-place é segura porque write[i]
    // ≤ off[i] ao começar de cada nó (counts só encolhem).
    {
        std::vector<uint32_t> new_off(2 * s.n + 1, 0u);
        size_t write = 0;
        for (int i = 0; i < 2 * s.n; ++i) {
            new_off[i] = static_cast<uint32_t>(write);
            const size_t lo = s.g.off[i];
            const size_t hi = s.g.off[i + 1];
            if (hi == lo) continue;

            if (hi > lo + 1) {
                std::sort(s.g.ed.begin() + lo + 1, s.g.ed.begin() + hi);
            }
            const int black_first = s.g.ed[lo].first;
            const bool include_black = (hi - lo >= 2 && black_first == s.g.ed[lo + 1].first);

            // Black sempre escrito primeiro.
            s.g.ed[write++] = s.g.ed[lo];

            size_t j = lo + 1;
            if (include_black) {
                // Pula grays cuja .first == black.first (colapsados pelo
                // unique do Billi original quando estão contíguos no sort).
                while (j < hi && s.g.ed[j].first == black_first) ++j;
            }

            if (j < hi) {
                s.g.ed[write++] = s.g.ed[j];
                int prev_first = s.g.ed[j].first;
                ++j;
                for (; j < hi; ++j) {
                    if (s.g.ed[j].first != prev_first) {
                        s.g.ed[write++] = s.g.ed[j];
                        prev_first = s.g.ed[j].first;
                    }
                }
            }
        }
        new_off[2 * s.n] = static_cast<uint32_t>(write);
        s.g.off = std::move(new_off);
        s.g.ed.resize(write);
        s.g.ed.shrink_to_fit();
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
            s.q_compact.push({s.g.at(i, 1).first ^ 1, 1});
            int left = i, right = s.g.at(i, 1).first;
            s.mark[left] = s.mark[right] = true;
            while (!s.q_compact.empty()) {
                pii pid = s.q_compact.front(); s.q_compact.pop();
                if (s.chk_for_compaction(pid.first)) {
                    s.mark[pid.first] = true;
                    int neigh = s.g.at(pid.first, 1).first;
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
            for (size_t j = 1, dl = s.g.deg(l); j < dl; ++j) {
                int u = s.g.at(l, j).first;
                if (s.mark[u] && u != r) continue;
                if (l != u) s.g_compacted[l].push_back({u, s.cnt_gray_edge});
                s.g_compacted[u].push_back({l, s.cnt_gray_edge++});
            }
            for (size_t j = 1, dr = s.g.deg(r); j < dr; ++j) {
                int u = s.g.at(r, j).first;
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
        for (size_t j = 1, di = s.g.deg(i); j < di; ++j) {
            int u = s.g.at(i, j).first;
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
    // Libera os buffers da CSR plana de g (~4 GB no HPRC).
    s.g.off.clear(); s.g.off.shrink_to_fit();
    s.g.ed.clear();  s.g.ed.shrink_to_fit();

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

    // ── Freeze g_compacted (vector<vector<>>) → g_compacted_csr (CSR plana) ──
    // Após este ponto não há mais writes a g_compacted; libera o
    // vector<vector<>> (~5 GB no HPRC) e usa CsrAdj para todos os reads
    // subsequentes (dfs_depth, sese, hairp, mark_nodes, validação hairpin).
    {
        s.g_compacted_csr.off.assign(2 * s.n + 1, 0u);
        for (int i = 0; i < 2 * s.n; ++i) {
            s.g_compacted_csr.off[i + 1] = s.g_compacted_csr.off[i]
                + static_cast<uint32_t>(s.g_compacted[i].size());
        }
        s.g_compacted_csr.ed.resize(s.g_compacted_csr.off[2 * s.n]);
        for (int i = 0; i < 2 * s.n; ++i) {
            const auto& src = s.g_compacted[i];
            std::copy(src.begin(), src.end(),
                      s.g_compacted_csr.ed.begin() + s.g_compacted_csr.off[i]);
        }
        // Libera o vector<vector<>>.
        s.g_compacted.clear();
        s.g_compacted.shrink_to_fit();
    }

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
        if ((s.g_compacted_csr.deg(rs.first) == 2
             && s.g_compacted_csr.at(rs.first, 1).first == s.g_compacted_csr.at(rs.first, 0).first)
            || s.g_compacted_csr.deg(rs.first) <= 1) continue;

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
