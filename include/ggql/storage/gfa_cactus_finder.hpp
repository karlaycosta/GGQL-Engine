#pragma once

// gfa_cactus_finder.hpp — Detecção de snarls via 3-ECC + grafo Cactus + BFS.
//
// API (callback-based, sem acumulação em RAM):
//   void find_snarls_streaming(
//       const GfaCsrTopology& topo,
//       const std::function<void(const SnarlRecord&)>& emit,
//       size_t max_visited_per_source = 8192);
//
// Pipeline:
//   Fase 1 — Tsin 3-ECC O(V+E) → UnionFind de nós 3-aresta-conectados.
//   Fase 2 — Compressão de componentes → node_to_comp[], comp_representative[],
//            comp_size[] (sem vector-of-vectors, sem nested allocs).
//   Fase 3 — Construção do CSR cactus (apenas bridges entre componentes).
//   Fase 4 — BFS multi-branch sobre o cactus, com EpochTracker bitmask
//            (sem unordered_set por branch). Emite SnarlRecord ao convergir.
//
// Otimizações de performance vs. revisão anterior:
//   * Sem `vector<vector<uint64_t>> comp_nodes` (~1 GB no HPRC).
//   * Sem `unordered_set<uint32_t>` por branch por source.
//   * Sem `vector<Snarl>` acumulado — emit() escreve direto em snarls.bin.
//   * Sem `internal_nodes` materializado — só count + has_cycles.
//   * Branch mask em uint32_t → suporta até 32 ramos por source (cap; raro >32).
//
// Restrições:
//   - NÃO modifica gfa_topology.hpp.
//   - Header-only, dependências: STL + ggql/detail/{tsin_3ecc,union_find}.

#include "ggql/storage/detail/tsin_3ecc.hpp"
#include "ggql/storage/detail/union_find.hpp"
#include "ggql/storage/gfa_topology.hpp"
#include "ggql/storage/gfa_topology_types.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace ggql {

namespace detail_cactus {

// ── EpochTracker ─────────────────────────────────────────────────────────────
//
// Estado por nó do cactus: 32 bits epoch + 32 bits branch_mask em uint64_t.
// Reset entre sources via incremento global de cur_epoch — O(1) sem zeroar.
//
// Limitação: até 32 ramos por source (`uint32_t` mask). Sources com out_degree
// > 32 são raros em pangenoma; capamos sem perder snarls grandes (apenas
// processamos os primeiros 32 ramos).
struct EpochTracker {
    std::vector<uint64_t> last_visit; // high 32 = epoch, low 32 = branch_mask
    uint32_t              cur_epoch = 0;

    explicit EpochTracker(size_t n_cactus_nodes) : last_visit(n_cactus_nodes, 0) {}

    void next_source() noexcept { ++cur_epoch; }

    // Tenta marcar v como visitado pelo branch b (0..31).
    // Retorna popcount do mask resultante (= número total de branches que
    // visitaram v incluindo este). Se já estava marcado para b, retorna 0.
    uint32_t mark(uint32_t v, uint32_t b) noexcept {
        const uint64_t cur  = last_visit[v];
        const uint32_t e    = static_cast<uint32_t>(cur >> 32);
        const uint32_t mask = static_cast<uint32_t>(cur);
        const uint32_t bbit = (1u << b);

        if (e != cur_epoch) {
            // Primeira visita nesta source.
            last_visit[v] = (static_cast<uint64_t>(cur_epoch) << 32) | bbit;
            return 1;
        }
        if (mask & bbit) {
            return 0; // já visitado por este branch
        }
        const uint32_t new_mask = mask | bbit;
        last_visit[v]           = (static_cast<uint64_t>(cur_epoch) << 32) | new_mask;
        return std::popcount(new_mask);
    }

    // Verifica se v foi tocado nesta source (por qualquer branch).
    bool touched(uint32_t v) const noexcept {
        return static_cast<uint32_t>(last_visit[v] >> 32) == cur_epoch;
    }
};

// ── CompactCsr — grafo cactus contraído ─────────────────────────────────────
struct CompactCsr {
    uint32_t              num_nodes = 0;
    std::vector<uint32_t> offsets; // tamanho num_nodes+1
    std::vector<uint32_t> adj;     // ids destino

    void build(uint32_t n, std::vector<std::pair<uint32_t, uint32_t>>& edges) {
        num_nodes = n;
        offsets.assign(static_cast<size_t>(n) + 1, 0);

        // Dedup. Na maioria dos pangenomas existem múltiplas arestas originais
        // entre as mesmas duas componentes — colapsadas aqui.
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

        for (const auto& [src, dst] : edges) {
            ++offsets[static_cast<size_t>(src) + 1];
        }
        for (size_t i = 1; i <= n; ++i) {
            offsets[i] += offsets[i - 1];
        }
        adj.resize(edges.size());

        // Use uma cópia mutável para indexar; vetor `pos` evita destruir offsets.
        std::vector<uint32_t> pos(offsets.begin(), offsets.begin() + n);
        for (const auto& [src, dst] : edges) {
            adj[pos[src]++] = dst;
        }
    }

    uint32_t out_degree(uint32_t node) const noexcept {
        return offsets[node + 1] - offsets[node];
    }
    const uint32_t* out_begin(uint32_t node) const noexcept { return adj.data() + offsets[node]; }
    const uint32_t* out_end(uint32_t node) const noexcept { return adj.data() + offsets[node + 1]; }
};

// ── compress_components — sem vec-of-vec ────────────────────────────────────
//
// Atribui IDs densos [0, num_comp) baseado nas raízes do UnionFind, em uma
// única passagem sobre 0..num_nodes. Saída:
//   node_to_comp[i]        = id do componente de i
//   comp_representative[c] = primeiro node id encontrado em c (ordem 0..N)
//   comp_size[c]           = tamanho do componente c
//
// Memória: O(num_nodes + num_comp) — sem nested allocs.
inline uint32_t compress_components(detail::UnionFind&     uf,
                                    size_t                 num_nodes,
                                    std::vector<uint32_t>& node_to_comp,
                                    std::vector<uint64_t>& comp_representative,
                                    std::vector<uint32_t>& comp_size) {
    node_to_comp.assign(num_nodes, ~0u);
    comp_representative.clear();
    comp_size.clear();

    // Mapa raiz_uf → comp_id usando vetor (raiz é índice válido 0..num_nodes).
    // Mais barato que unordered_map em ~30-50% para grafos densos.
    std::vector<uint32_t> root_to_comp(num_nodes, ~0u);

    uint32_t next_id = 0;
    for (size_t i = 0; i < num_nodes; ++i) {
        const auto root = static_cast<uint32_t>(uf.find(i));
        uint32_t   cid  = root_to_comp[root];
        if (cid == ~0u) {
            cid                  = next_id++;
            root_to_comp[root]   = cid;
            comp_representative.push_back(i); // primeiro nó visto = representante
            comp_size.push_back(0);
        }
        node_to_comp[i] = cid;
        ++comp_size[cid];
    }
    return next_id;
}

} // namespace detail_cactus

// ── find_snarls_streaming — entry point ─────────────────────────────────────
//
// Encontra todos os snarls do grafo e emite SnarlRecord via callback.
// Garantias:
//   - emit() é chamado exatamente uma vez por snarl encontrado.
//   - SnarlRecord.source/sink são IDs originais (não comp ids).
//   - Single-threaded nesta versão; paralelização em revisão futura.
//
// Parâmetros:
//   max_visited_per_source — limite de nós do cactus visitados por BFS de uma
//   source. Mantém complexidade total limitada em O(num_sources × MAX). Default
//   8192 funciona para SM/10W; para HPRC considerar 65536+.
inline void find_snarls_streaming(
    const GfaCsrTopology& topo,
    const std::function<void(const SnarlRecord&)>& emit,
    size_t max_visited_per_source = 8192) {

    using namespace detail_cactus;

    const auto N = static_cast<size_t>(topo.num_nodes());
    if (N == 0) {
        return;
    }

    // ── Fase 1: Tsin 3-ECC → UnionFind ──────────────────────────────────────
    //
    // Tentamos paralelizar por WCC, mas pangenomas tipicamente têm um único
    // WCC grande (CHM13 10W: 1 WCC com 100% dos nós) — paralelização por WCC
    // vira no-op + overhead. Single-thread é o ótimo prático aqui.
    detail::UnionFind uf(N);
    auto for_each_neighbor = [&](size_t u, const std::function<void(size_t)>& emit_n) {
        for (uint64_t enc : topo.out_neighbors(static_cast<uint64_t>(u))) {
            emit_n(static_cast<size_t>(GfaCsrTopology::decode(enc).node_id));
        }
        for (uint64_t enc : topo.in_neighbors(static_cast<uint64_t>(u))) {
            emit_n(static_cast<size_t>(GfaCsrTopology::decode(enc).node_id));
        }
    };
    detail::three_edge_connected_merges(
        N, /*first_root=*/0, for_each_neighbor,
        [&](size_t a, size_t b) { uf.unite(a, b); });

    // ── Fase 2: comprime UF → node_to_comp, comp_representative, comp_size ─
    std::vector<uint32_t> node_to_comp;
    std::vector<uint64_t> comp_representative;
    std::vector<uint32_t> comp_size;
    const uint32_t        num_comp =
        compress_components(uf, N, node_to_comp, comp_representative, comp_size);

    // UF não é mais necessário — libera para reduzir peak RSS.
    uf = detail::UnionFind{0};

    if (num_comp <= 1) {
        return; // grafo inteiramente 3-conectado = sem snarls
    }

    // ── Fase 3: arestas inter-componente → cactus CSR ──────────────────────
    std::vector<std::pair<uint32_t, uint32_t>> cactus_edges;
    cactus_edges.reserve(N / 4 + 1);

    for (uint64_t u = 0; u < static_cast<uint64_t>(N); ++u) {
        const uint32_t cu = node_to_comp[static_cast<size_t>(u)];
        for (uint64_t enc : topo.out_neighbors(u)) {
            const uint64_t v  = GfaCsrTopology::decode(enc).node_id;
            const uint32_t cv = node_to_comp[static_cast<size_t>(v)];
            if (cu != cv) {
                cactus_edges.emplace_back(cu, cv);
            }
        }
    }

    CompactCsr cactus;
    cactus.build(num_comp, cactus_edges);
    // libera o vetor temporário antes do BFS pesado
    cactus_edges.clear();
    cactus_edges.shrink_to_fit();

    // ── Fase 4: BFS multi-branch sobre o cactus, emite SnarlRecord ─────────
    //
    // Para cada componente src com out_degree ≥ 2, fazemos BFS simultâneo a
    // partir dos vizinhos imediatos. Quando um nó é alcançado por TODOS os
    // ramos (popcount(branch_mask) == nb), é o sink. Acumulamos o conjunto de
    // componentes intermediárias para calcular internal_node_count e has_cycles.
    //
    // PARALELIZAÇÃO: cada source é independente — paralelizamos com OpenMP
    // dynamic schedule (sources têm tempo bem variável: a maioria não converge,
    // algumas fazem BFS profunda). Cada thread tem seu próprio EpochTracker e
    // buffers; emite via buffer local + flush sob critical no final.

    constexpr uint32_t kMaxBranches = 32;

#pragma omp parallel
    {
        EpochTracker tracker(num_comp);
        std::vector<std::vector<uint32_t>> cur_queues(kMaxBranches);
        std::vector<std::vector<uint32_t>> next_queues(kMaxBranches);
        for (auto& q : cur_queues)  q.reserve(64);
        for (auto& q : next_queues) q.reserve(64);
        std::vector<uint32_t> visited_comps;
        visited_comps.reserve(256);
        std::vector<SnarlRecord> local_buf;
        local_buf.reserve(256);

#pragma omp for schedule(dynamic, 256) nowait
    for (uint32_t src = 0; src < num_comp; ++src) {
        const uint32_t out_deg = cactus.out_degree(src);
        if (out_deg < 2) {
            continue;
        }
        // Cap em 32 ramos (limitação do bitmask). Raro em pangenoma.
        const uint32_t nb = std::min<uint32_t>(out_deg, kMaxBranches);

        tracker.next_source();
        visited_comps.clear();
        for (uint32_t b = 0; b < nb; ++b) {
            cur_queues[b].clear();
            next_queues[b].clear();
        }

        // Marca src como tocado pelo epoch atual mas SEM contar como ramo
        // (não setamos bit). Evita que um branch volte ao src.
        tracker.last_visit[src] =
            (static_cast<uint64_t>(tracker.cur_epoch) << 32) | 0u;

        // Popula filas iniciais com os nós adjacentes a src.
        const uint32_t* nbs = cactus.out_begin(src);
        bool found_sink = false;
        uint32_t sink_comp = ~0u;

        for (uint32_t b = 0; b < nb; ++b) {
            const uint32_t first = nbs[b];
            if (first == src) {
                continue; // self-loop
            }
            const uint32_t cnt = tracker.mark(first, b);
            if (cnt > 0) {
                visited_comps.push_back(first);
            }
            if (cnt == nb) {
                sink_comp  = first;
                found_sink = true;
                break;
            }
            cur_queues[b].push_back(first);
        }

        size_t total_visited = visited_comps.size();

        while (!found_sink) {
            bool any_active = false;
            for (uint32_t b = 0; b < nb && !found_sink; ++b) {
                next_queues[b].clear();
                for (uint32_t node : cur_queues[b]) {
                    const uint32_t* be = cactus.out_begin(node);
                    const uint32_t* en = cactus.out_end(node);
                    for (; be != en; ++be) {
                        const uint32_t w = *be;
                        if (w == src) {
                            continue;
                        }
                        const bool was_touched = tracker.touched(w);
                        const uint32_t cnt    = tracker.mark(w, b);
                        if (cnt == 0) {
                            continue;
                        }
                        if (!was_touched) {
                            visited_comps.push_back(w);
                            ++total_visited;
                        }
                        if (cnt == nb) {
                            sink_comp  = w;
                            found_sink = true;
                            break;
                        }
                        next_queues[b].push_back(w);
                        any_active = true;
                    }
                    if (found_sink) {
                        break;
                    }
                }
                std::swap(cur_queues[b], next_queues[b]);
            }
            if (!any_active) {
                break;
            }
            if (total_visited > max_visited_per_source) {
                break;
            }
        }

        if (!found_sink) {
            continue;
        }

        // ── Calcula internal_node_count e has_cycles ────────────────────────
        // visited_comps contém todas as componentes tocadas pela BFS (excl.
        // src; sink está incluído). Intermediárias = visited \ {sink}.
        uint32_t internal_count = 0;
        bool     has_cycles     = false;
        for (uint32_t c : visited_comps) {
            if (c == sink_comp) {
                continue;
            }
            const uint32_t sz = comp_size[c];
            internal_count += sz;
            if (sz > 1) {
                has_cycles = true;
            }
        }

        SnarlRecord rec{};
        rec.source              = comp_representative[src];
        rec.sink                = comp_representative[sink_comp];
        rec.internal_node_count = internal_count;
        rec.branch_count        = static_cast<uint8_t>(nb);
        rec.has_cycles          = has_cycles ? uint8_t{1} : uint8_t{0};
        rec._pad[0] = rec._pad[1] = 0;
        local_buf.push_back(rec);
    }
        // Flush buffer local da thread sob lock — snarls de threads diferentes
        // saem em ordens diferentes a cada execução; ordem não é garantida.
#pragma omp critical(snarl_flush)
        {
            for (const auto& r : local_buf) {
                emit(r);
            }
        }
    } // end omp parallel
}

} // namespace ggql
