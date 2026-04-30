// snarl_decomposition.cpp — Fase 1 do framework Paten 2018.
//
// Algoritmo: Onodera et al. 2013 estendido (ver header). Reuso do mesmo
// pipeline de find_bubbles em graph_algorithms.cpp (Kosaraju + topological
// sweep com saldo out_minus_in), mas populando SnarlRecord completo —
// branch_count via out_degree DAG do source, internal_node_count contado
// durante sweep, has_cycles detectando intermediárias em SCCs não-triviais.

#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <cstdint>
#include <span>

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

    return out;
}

} // namespace ggql::snarl_decomposition
