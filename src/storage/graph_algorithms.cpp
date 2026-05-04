// graph_algorithms.cpp — Algoritmos de grafo: bubbles, remap, componentes,
// particionamento.

#include "ggql/storage/topology.hpp"

#include "ggql/cli/console.hpp"
#include "ggql/util/memory_monitor.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring> // strerror
#include <fmt/core.h>
#include <gbwt/dynamic_gbwt.h> // text_buffer_type, ENDMARKER
#include <numeric>
#include <stdexcept>
#include <string>
#include <unistd.h> // ::unlink

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

namespace {
// MemoryMonitor + available_ram_kb consolidados em
// include/ggql/util/memory_monitor.hpp para evitar duplicação com
// path_index.cpp.
using ggql::util::MemoryMonitor;
using ggql::util::available_ram_kb;
} // namespace

std::vector<Bubble> GfaCsrTopology::find_bubbles(int num_threads) const {
    if (num_nodes_ == 0) {
        return {};
    }
    const uint64_t N = num_nodes_;

    // Popula cache de offsets antes do DFS (com paralelismo disponível).
    ensure_offsets_cache(num_threads);

    // B5: Frame compartilhado entre as duas fases de Kosaraju (antes,
    // duplicado em escopos aninhados). `node` = vértice, `ei` = cursor
    // na adjacency list (forward em 1a, backward em 1b).
    struct Frame {
        uint64_t node;
        uint32_t ei;
    };

    // ── FASE 1 — Condensação via Kosaraju iterativo O(V + E) ────────────────
    std::vector<uint32_t> scc_id(N, UINT32_MAX);

    // 1a. DFS forward → empilha nós por finish-time crescente.
    std::vector<uint64_t> finish_order;
    {
        finish_order.reserve(N);
        std::vector<Frame> stk;
        // Reserva proporcional a N para evitar realocações em grafos grandes.
        stk.reserve(std::min(static_cast<size_t>(1u << 20), static_cast<size_t>(N)));
        std::vector<uint8_t> visited(N, 0);

        for (uint64_t root = 0; root < N; ++root) {
            if (visited[root]) {
                continue;
            }
            stk.push_back({root, offsets_cache_fwd_[root]});
            visited[root] = 1;
            while (!stk.empty()) {
                auto& [u, ei] = stk.back();
                const uint32_t end = offsets_cache_fwd_[u + 1];
                if (ei < end) {
                    uint64_t v = E_fwd_[ei] >> 1;
                    ++ei;
                    if (!visited[v]) {
                        visited[v] = 1;
                        stk.push_back({v, offsets_cache_fwd_[v]});
                    }
                } else {
                    visited[u] = 2;
                    finish_order.push_back(u);
                    stk.pop_back();
                }
            }
        }
    }

    // 1b. DFS sobre G^T em ordem finish decrescente → SCCs.
    uint32_t num_scc = 0;
    {
        std::vector<Frame> stk;
        // Reserva proporcional a N para evitar realocações em grafos grandes.
        stk.reserve(std::min(static_cast<size_t>(1u << 20), static_cast<size_t>(N)));
        for (int64_t fi = static_cast<int64_t>(finish_order.size()) - 1; fi >= 0; --fi) {
            uint64_t root = finish_order[static_cast<size_t>(fi)];
            if (scc_id[root] != UINT32_MAX) {
                continue;
            }
            stk.push_back({root, offsets_cache_bwd_[root]});
            scc_id[root] = num_scc;
            while (!stk.empty()) {
                auto& [u, ei] = stk.back();
                const uint32_t end = offsets_cache_bwd_[u + 1];
                if (ei < end) {
                    uint64_t v = E_bwd_[ei] >> 1;
                    ++ei;
                    if (scc_id[v] == UINT32_MAX) {
                        scc_id[v] = num_scc;
                        stk.push_back({v, offsets_cache_bwd_[v]});
                    }
                } else {
                    stk.pop_back();
                }
            }
            ++num_scc;
        }
    }
    finish_order.clear();
    finish_order.shrink_to_fit();

    // 1c. Tamanho das SCCs via counting-sort.
    std::vector<uint32_t> scc_size(num_scc, 0);
    for (uint64_t v = 0; v < N; ++v) {
        ++scc_size[scc_id[v]];
    }

    // ── FASE 2 — Sweep topológico no DAG de SCCs O(V + E) ───────────────────
    // Counting-sort para topo_order.
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

    // in_dag[v] = in-degree inter-SCC de v.
    std::vector<uint32_t> in_dag(N, 0);
    for (uint64_t u = 0; u < N; ++u) {
        const uint32_t su = scc_id[u];
        for (uint32_t ei = offsets_cache_fwd_[u], hi = offsets_cache_fwd_[u + 1]; ei < hi; ++ei) {
            const uint64_t w = E_fwd_[ei] >> 1;
            if (scc_id[w] != su) {
                ++in_dag[w];
            }
        }
    }

    // Sweep de Onodera (Onodera et al. 2013, Fig. 1).
    //
    // Invariante: out_minus_in é o saldo acumulado de (out_dag - in_dag) para
    // todos os nós singulares do grafo já visitados. Quando out_minus_in == 0
    // em um nó v que esgotou todos os seus in-edges inter-SCC (in_open==in_dag),
    // v é necessariamente o sink da bolha identificada pela fonte no topo da stack.
    //
    // Prova: em um grafo com in_dag e out_dag inter-SCC, a soma de todos os
    // (out_dag - in_dag) de um source até um sink é zero (conservação de fluxo
    // em grafo sem entradas/saídas externas para a bolha).
    std::vector<uint32_t> in_open(N, 0);
    struct SrcEntry {
        uint64_t node;
    };
    std::vector<SrcEntry> src_stack;
    src_stack.reserve(256);
    int64_t out_minus_in = 0;
    std::vector<Bubble> bubbles;

    for (uint64_t ti = 0; ti < N; ++ti) {
        const uint64_t v = topo_order[ti];
        const uint32_t sv = scc_id[v];
        const bool sing = (scc_size[sv] == 1);

        // Computa out-degree inter-SCC e incrementa in_open dos sucessores.
        uint32_t out_dag_v = 0;
        for (uint32_t ei = offsets_cache_fwd_[v], hi = offsets_cache_fwd_[v + 1]; ei < hi; ++ei) {
            const uint64_t w = E_fwd_[ei] >> 1;
            if (scc_id[w] != sv) {
                ++out_dag_v;
                ++in_open[w];
            }
        }

        if (sing) {
            // Acumula a contribuição de v no saldo global de ramos abertos.
            // DEVE acontecer para TODOS os nós (inclusive fontes e sinks).
            out_minus_in += static_cast<int64_t>(out_dag_v) - static_cast<int64_t>(in_dag[v]);

            // Sink detection: todos os in-edges inter-SCC de v chegaram
            // e o saldo de ramos está zerado → v é um sink de bolha.
            if (in_dag[v] > 0 && in_open[v] == in_dag[v] && out_minus_in == 0 &&
                !src_stack.empty()) {
                const SrcEntry src = src_stack.back();
                src_stack.pop_back();
                bubbles.push_back({src.node, v, {}});
            }

            // Source detection: v abre >= 2 ramos inter-SCC → potencial fonte.
            // Empilha APÓS verificar sink para suportar bolhas aninhadas/consecutivas.
            if (out_dag_v >= 2) {
                src_stack.push_back({v});
            }
        }
    }

    // Libera o cache após uso (não o reter permanentemente — vide comentário no .hpp).
    offsets_cache_fwd_.clear();
    offsets_cache_fwd_.shrink_to_fit();
    offsets_cache_bwd_.clear();
    offsets_cache_bwd_.shrink_to_fit();

    return bubbles;
}


// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::remap_node_ids(const std::vector<uint32_t>& remap) {
    // Throw em vez de assert: detecta mismatch tanto em debug quanto release.
    if (remap.size() != num_nodes_) {
        throw std::invalid_argument(
            "remap_node_ids: remap.size()=" + std::to_string(remap.size()) +
            " != num_nodes_=" + std::to_string(num_nodes_));
    }
    const size_t N = num_nodes_;
    const size_t E_sz = E_fwd_.size();
    const size_t E_bsz = E_bwd_.size();
    // Distância de prefetch para os loops scatter fwd e bwd. Reduz cache miss
    // em cursor[remap[u+kPrefetchAhead]] para N grande (~340 MB de cursor[]
    // no HPRC com 85M nós).
    static constexpr size_t kPrefetchAhead = 32;

    const int nt = [] {
#ifdef _OPENMP
        return omp_get_max_threads();
#else
        return 1;
#endif
    }();

    // ── Re-encode neighbors em E_fwd_ e E_bwd_ ────────────────────────────
    // encoded = (neighbor_id << 1) | orient_bit; troca apenas neighbor_id.
    // Paralelo: cada elemento é independente → embaraçosamente paralelo.
#pragma omp parallel for schedule(static) num_threads(nt)
    for (size_t i = 0; i < E_sz; ++i) {
        E_fwd_[i] = (remap[E_fwd_[i] >> 1] << 1) | (E_fwd_[i] & 1u);
    }

#pragma omp parallel for schedule(static) num_threads(nt)
    for (size_t i = 0; i < E_bsz; ++i) {
        E_bwd_[i] = (remap[E_bwd_[i] >> 1] << 1) | (E_bwd_[i] & 1u);
    }

    // ── Reconstrói cache a partir da EF antiga (old from_ids) ─────────────
    // Paraleliza via OMP (mesmo padrão de ensure_offsets_cache).
    // Serial: ~3,4 s para N=85M; OMP 4t: ~45 ms.
    offsets_cache_fwd_.resize(N + 1);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(nt)
#endif
    for (uint64_t i = 0; i <= N; ++i) {
        offsets_cache_fwd_[i] = static_cast<uint32_t>(ef_fwd_sel_(i + 1));
    }

    // ── Reordena E_fwd_ por counting sort O(E + N) e reconstrói ef_fwd_ ───
    // Elimina FwdEdge struct (~8×E bytes) e std::stable_sort O(E log E).
    // remap é bijeção → cada new_from tem exatamente 1 old_from;
    // ordem dos vizinhos dentro de cada nó é preservada naturalmente.
    {
        // 1. Histograma: grau de saída por new_from
        std::vector<uint32_t> new_off(N + 1, 0u);
        for (size_t u = 0; u < N; ++u) {
            new_off[remap[u] + 1] += offsets_cache_fwd_[u + 1] - offsets_cache_fwd_[u];
        }

        // 2. Prefix sum → new_off[u] = início do adjacency list de u no novo E_fwd_
        for (size_t i = 1; i <= N; ++i) {
            new_off[i] += new_off[i - 1];
        }

        // 3. Scatter: escreve arestas na nova ordem usando cursor separado
        // Prefetch do cursor kPrefetchAhead iterações à frente.
        std::vector<uint32_t> E_fwd_new(E_sz);
        std::vector<uint32_t> cursor(new_off.begin(), new_off.begin() + N);
        for (size_t u = 0; u < N; ++u) {
            if (u + kPrefetchAhead < N) {
                __builtin_prefetch(&cursor[remap[u + kPrefetchAhead]], /*rw=*/1, /*locality=*/0);
            }
            uint32_t& pos = cursor[remap[u]];
            const uint32_t lo = offsets_cache_fwd_[u];
            const uint32_t hi = offsets_cache_fwd_[u + 1];
            for (uint32_t ei = lo; ei < hi; ++ei) {
                E_fwd_new[pos++] = E_fwd_[ei];
            }
        }
        E_fwd_ = std::move(E_fwd_new);

        // 4. Rebuild ef_fwd_ — new_off já é o offset array correto
        sdsl::sd_vector_builder builder(E_sz + 1, N + 1, /*multiset=*/true);
        for (size_t i = 0; i <= N; ++i) {
            builder.set(static_cast<uint64_t>(new_off[i]));
        }
        ef_fwd_ = sdsl::sd_vector<>(builder);
        sdsl::util::init_support(ef_fwd_sel_, &ef_fwd_);
        offsets_cache_fwd_ = std::move(new_off);
    }

    // ── Analogamente para E_bwd_ ───────────────────────────────────────────
    // Paraleliza via OMP (mesmo padrão de ensure_offsets_cache).
    offsets_cache_bwd_.resize(N + 1);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(nt)
#endif
    for (uint64_t i = 0; i <= N; ++i) {
        offsets_cache_bwd_[i] = static_cast<uint32_t>(ef_bwd_sel_(i + 1));
    }

    {
        std::vector<uint32_t> new_off(N + 1, 0u);
        for (size_t u = 0; u < N; ++u) {
            new_off[remap[u] + 1] += offsets_cache_bwd_[u + 1] - offsets_cache_bwd_[u];
        }
        for (size_t i = 1; i <= N; ++i) {
            new_off[i] += new_off[i - 1];
        }

        std::vector<uint32_t> E_bwd_new(E_bsz);
        std::vector<uint32_t> cursor(new_off.begin(), new_off.begin() + N);
        for (size_t u = 0; u < N; ++u) {
            if (u + kPrefetchAhead < N) {
                __builtin_prefetch(&cursor[remap[u + kPrefetchAhead]], /*rw=*/1, /*locality=*/0);
            }
            uint32_t& pos = cursor[remap[u]];
            const uint32_t lo = offsets_cache_bwd_[u];
            const uint32_t hi = offsets_cache_bwd_[u + 1];
            for (uint32_t ei = lo; ei < hi; ++ei) {
                E_bwd_new[pos++] = E_bwd_[ei];
            }
        }
        E_bwd_ = std::move(E_bwd_new);

        sdsl::sd_vector_builder builder(E_bsz + 1, N + 1, /*multiset=*/true);
        for (size_t i = 0; i <= N; ++i) {
            builder.set(static_cast<uint64_t>(new_off[i]));
        }
        ef_bwd_ = sdsl::sd_vector<>(builder);
        sdsl::util::init_support(ef_bwd_sel_, &ef_bwd_);
        offsets_cache_bwd_ = std::move(new_off);
    }

    cli::step_info("remap_node_ids: E_fwd/bwd reordenados e EF reconstruídos");
}

// ════════════════════════════════════════════════════════════════════════════
// make_component_remap — Union-Find para detectar componentes conectados e
// atribuir IDs sequenciais contíguos por componente (maior primeiro).
//
// Complexidade: O(N × α(N)) para UF + O(N log K) para sort.
// ════════════════════════════════════════════════════════════════════════════
GfaCsrTopology::RemapResult GfaCsrTopology::make_component_remap(int num_threads) const {
    const size_t N = num_nodes_;
    RemapResult result;
    if (N == 0) {
        return result;
    }

    ensure_offsets_cache(num_threads);

    // ── Union-Find: path halving + union by rank ──────────────────────────────
    std::vector<uint32_t> parent(N);
    std::iota(parent.begin(), parent.end(), 0u);
    std::vector<uint8_t> uf_rank(N, 0u);

    const auto find = [&](uint32_t x) -> uint32_t {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]]; // path halving
            x = parent[x];
        }
        return x;
    };
    const auto unite = [&](uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) {
            return;
        }
        if (uf_rank[a] < uf_rank[b]) {
            std::swap(a, b);
        }
        parent[b] = a;
        if (uf_rank[a] == uf_rank[b]) {
            ++uf_rank[a];
        }
    };

    // Itera pelas arestas fwd — suficiente para conectividade não-dirigida
    for (uint32_t u = 0; u < static_cast<uint32_t>(N); ++u) {
        const uint32_t lo = offsets_cache_fwd_[u];
        const uint32_t hi = offsets_cache_fwd_[u + 1];
        for (uint32_t ei = lo; ei < hi; ++ei) {
            unite(u, E_fwd_[ei] >> 1);
        }
    }

    // ── Compacta raízes → comp_id, conta tamanho de cada componente ──────────
    std::vector<uint32_t> root_to_comp(N, std::numeric_limits<uint32_t>::max());
    std::vector<uint32_t> comp_sizes;
    comp_sizes.reserve(1024);

    std::vector<uint32_t> node_old_comp(N);
    for (uint32_t u = 0; u < static_cast<uint32_t>(N); ++u) {
        const uint32_t root = find(u);
        if (root_to_comp[root] == std::numeric_limits<uint32_t>::max()) {
            root_to_comp[root] = static_cast<uint32_t>(comp_sizes.size());
            comp_sizes.push_back(0u);
        }
        node_old_comp[u] = root_to_comp[root];
        ++comp_sizes[root_to_comp[root]];
    }

    const uint32_t K = static_cast<uint32_t>(comp_sizes.size());

    // ── Ordena componentes por tamanho decrescente ────────────────────────────
    std::vector<uint32_t> sorted_comps(K);
    std::iota(sorted_comps.begin(), sorted_comps.end(), 0u);
    std::sort(sorted_comps.begin(), sorted_comps.end(),
              [&](uint32_t a, uint32_t b) { return comp_sizes[a] > comp_sizes[b]; });

    // comp_new_id[old_comp] = new_comp (0 = maior, K-1 = menor)
    std::vector<uint32_t> comp_new_id(K);
    for (uint32_t i = 0; i < K; ++i) {
        comp_new_id[sorted_comps[i]] = i;
    }

    // comp_offset[i] = início dos IDs do componente i em new ID space
    std::vector<uint32_t> comp_offset(K + 1, 0u);
    for (uint32_t i = 0; i < K; ++i) {
        comp_offset[i + 1] = comp_offset[i] + comp_sizes[sorted_comps[i]];
    }

    // Cursores por componente para atribuição de IDs sequenciais
    std::vector<uint32_t> cursor(K);
    for (uint32_t i = 0; i < K; ++i) {
        cursor[i] = comp_offset[i];
    }

    result.remap.resize(N);
    result.node_component.resize(N);
    result.num_components = K;

    for (uint32_t u = 0; u < static_cast<uint32_t>(N); ++u) {
        const uint32_t new_cid = comp_new_id[node_old_comp[u]];
        const uint32_t new_id  = cursor[new_cid]++;
        result.remap[u]               = new_id;
        result.node_component[new_id] = new_cid;
    }

    // B4: K > 0 é garantido porque (a) função retorna cedo se N==0 (linha
    // anterior), (b) Union-Find sobre N>0 produz pelo menos 1 componente.
    // Defendemos contra futuras mudanças que possam violar isso.
    if (K == 0) {
        throw std::logic_error(
            "[topology] make_component_remap: K=0 com N=" + std::to_string(N));
    }
    cli::step_info(fmt::format("make_component_remap: {} componentes, maior={} nós",
                               K, comp_sizes[sorted_comps[0]]));
    return result;
}

// ════════════════════════════════════════════════════════════════════════════
// repartition_groups_topological — redistribui grupos gBWT por componente
// conectado, garantindo que cada job de construção paralela contenha paths
// exclusivamente de um componente → merge sem conflito de sigma.
//
// target_n = número de componentes (= remap_res.num_components).
// Grupos in-RAM são mantidos in-RAM quando possível; grupos de disco ficam em
// disco.  Quando um componente recebe entradas de ambas as fontes, as
// sequências in-RAM são drenadas para disco para manter consistência.
// ════════════════════════════════════════════════════════════════════════════
std::vector<GfaCsrTopology::GbwtGroup>
GfaCsrTopology::repartition_groups_topological(std::vector<GbwtGroup> input_groups,
                                               const std::vector<uint32_t>& node_component,
                                               size_t target_n, uint8_t node_width,
                                               const std::string& data_dir) const {
    if (target_n == 0 || input_groups.empty()) {
        return std::move(input_groups);
    }

    // ── Sigma por componente: max(encoded_value)+1 ────────────────────────────
    // encoded_value = gbwt_encode_node(node_id, orient) = ((node_id+1)<<1)|orient
    // máximo = ((max_id+1)*2)+1 = 2*max_id + 3
    std::vector<uint64_t> comp_sigma(target_n, 0ull);
    for (size_t nid = 0; nid < node_component.size(); ++nid) {
        const uint32_t c = node_component[nid];
        if (c < target_n) {
            comp_sigma[c] = std::max(comp_sigma[c], gbwt_encode_node(nid, 1u));
        }
    }
    for (auto& s : comp_sigma) {
        ++s; // alphabet size = max_encoded + 1
    }

    std::vector<GbwtGroup> out(target_n);
    for (size_t i = 0; i < target_n; ++i) {
        out[i].sigma = comp_sigma[i];
    }

    std::vector<bool>                                    output_is_disk(target_n, false);
    std::vector<std::string>                             comp_tmp(target_n);
    std::vector<std::unique_ptr<gbwt::text_buffer_type>> comp_buf(target_n);

    // B2: contador atômico estático garante unicidade entre chamadas
    // múltiplas na mesma instância (uintptr_t do `this` sozinho colidiria).
    // Mantemos o `this` no nome para inspeção em logs/cores.
    static std::atomic<uint64_t> call_seq{0};
    const uint64_t seq = call_seq.fetch_add(1, std::memory_order_relaxed);

    // Garante buffer de disco para o componente c; drena in_ram_seqs acumuladas.
    const auto ensure_disk_buf = [&](size_t c) -> gbwt::text_buffer_type& {
        if (!comp_buf[c]) {
            comp_tmp[c] = data_dir + "/gbwt_comp_" + std::to_string(c) + "_" +
                          std::to_string(reinterpret_cast<uintptr_t>(this)) + "_" +
                          std::to_string(seq) + ".tmp";
            comp_buf[c] = std::make_unique<gbwt::text_buffer_type>(comp_tmp[c], std::ios::out,
                                                                    64u << 20, node_width);
            output_is_disk[c] = true;
            // Drena sequências in-RAM já acumuladas para este componente
            for (const auto& seq : out[c].in_ram_seqs) {
                for (auto v : seq) {
                    comp_buf[c]->push_back(v);
                }
                comp_buf[c]->push_back(gbwt::ENDMARKER);
            }
            out[c].in_ram_seqs.clear();
            out[c].in_ram_seqs.shrink_to_fit();
        }
        return *comp_buf[c];
    };

    // Devolve comp_id do path a partir do primeiro nó válido
    const auto path_comp = [&](const gbwt::vector_type& seq) -> size_t {
        for (auto enc : seq) {
            if (enc != gbwt::ENDMARKER) {
                const uint64_t nid = gbwt_decode_node(static_cast<uint64_t>(enc));
                if (nid < node_component.size()) {
                    return std::min<size_t>(node_component[nid], target_n - 1);
                }
            }
        }
        return 0;
    };

    for (auto& g : input_groups) {
        if (!g.in_ram_seqs.empty()) {
            for (size_t si = 0; si < g.in_ram_seqs.size(); ++si) {
                const size_t c = path_comp(g.in_ram_seqs[si]);
                if (output_is_disk[c]) {
                    auto& buf = *comp_buf[c];
                    for (auto v : g.in_ram_seqs[si]) {
                        buf.push_back(v);
                    }
                    buf.push_back(gbwt::ENDMARKER);
                } else {
                    out[c].in_ram_seqs.push_back(std::move(g.in_ram_seqs[si]));
                }
                if (si < g.path_names.size()) {
                    out[c].path_names.push_back(std::move(g.path_names[si]));
                }
            }
        } else if (!g.tmp_path.empty()) {
            gbwt::text_buffer_type in_buf(g.tmp_path, std::ios::in, 64u << 20, node_width);
            gbwt::vector_type cur;
            cur.reserve(1u << 16);
            size_t pidx = 0;

            for (gbwt::size_type val : in_buf) {
                if (val == gbwt::ENDMARKER) {
                    if (!cur.empty()) {
                        const size_t c = path_comp(cur);
                        auto& buf = ensure_disk_buf(c);
                        for (auto v : cur) {
                            buf.push_back(v);
                        }
                        buf.push_back(gbwt::ENDMARKER);
                        if (pidx < g.path_names.size()) {
                            out[c].path_names.push_back(std::move(g.path_names[pidx]));
                        }
                        ++pidx;
                        cur.clear();
                    }
                } else {
                    cur.push_back(val);
                }
            }
            // Sequência sem ENDMARKER final (defensivo)
            if (!cur.empty()) {
                const size_t c = path_comp(cur);
                auto& buf = ensure_disk_buf(c);
                for (auto v : cur) {
                    buf.push_back(v);
                }
                buf.push_back(gbwt::ENDMARKER);
                if (pidx < g.path_names.size()) {
                    out[c].path_names.push_back(std::move(g.path_names[pidx]));
                }
            }
            // B3: unlink pode falhar (FS read-only, permissões); registra
            // warning mas não trata como fatal — tmp file é descartável.
            // Alinha com B4 do path_index.
            if (::unlink(g.tmp_path.c_str()) != 0 && errno != ENOENT) {
                cli::note(fmt::format("falha ao remover tmp '{}': {}",
                                      g.tmp_path, std::strerror(errno)));
            }
        }
    }

    // Fecha buffers e atribui tmp_path
    for (size_t i = 0; i < target_n; ++i) {
        if (comp_buf[i]) {
            comp_buf[i]->close();
            out[i].tmp_path = comp_tmp[i];
        }
    }

    // Remove grupos sem paths
    std::erase_if(out, [](const GbwtGroup& g) { return g.path_names.empty(); });

    return out;
}

// ── path_nodes ────────────────────────────────────────────────────────────────
//
// Extrai a sequência de IDs internos de nós de um path via gBWT.
// O gBWT armazena cada nó com encoding gbwt_encode_node (shift +1 para
// reservar valor 0 a ENDMARKER). Decodificamos com gbwt_decode_node.
//
// B7: gbwts_ e gbwt_offsets_ devem ter tamanhos consistentes
// (gbwts_.size() == gbwt_offsets_.size() em uso normal).
// O cache load valida; mismatch só ocorre se alguém mexer manualmente.

// P2: overload com out-param para reusar buffer entre chamadas.
// Evita malloc por chamada — relevante em build_path_stats (loop de N paths).
void GfaCsrTopology::path_nodes(size_t global_path_id,
                                 std::vector<uint64_t>& out) const {
    out.clear();
    if (gbwts_.empty() || gbwt_offsets_.empty() ||
        global_path_id >= path_names_.size()) {
        return;
    }

    // Localiza o shard via busca binária em gbwt_offsets_.
    auto it = std::upper_bound(gbwt_offsets_.begin(), gbwt_offsets_.end(),
                               global_path_id);
    if (it == gbwt_offsets_.begin()) { return; }
    --it;

    const size_t shard    = static_cast<size_t>(std::distance(gbwt_offsets_.begin(), it));
    const size_t local_id = global_path_id - *it;

    if (shard >= gbwts_.size() || !gbwts_[shard]) { return; }
    if (local_id >= static_cast<size_t>(gbwts_[shard]->sequences())) { return; }

    const auto seq = gbwts_[shard]->extract(static_cast<gbwt::size_type>(local_id));

    out.reserve(seq.size());
    for (const auto enc : seq) {
        if (enc == gbwt::ENDMARKER) { continue; }
        out.push_back(gbwt_decode_node(static_cast<uint64_t>(enc)));
    }
}

// Wrapper conveniente que aloca o vetor por chamada — preservado para callers
// existentes (query_planner.cpp:1004). Para hot loops, prefira o overload
// com out-param acima.
std::vector<uint64_t> GfaCsrTopology::path_nodes(size_t global_path_id) const {
    std::vector<uint64_t> nodes;
    path_nodes(global_path_id, nodes);
    return nodes;
}

// ── build_path_stats ─────────────────────────────────────────────────────────
//
// Itera todos os paths via GBWT uma única vez para pré-computar node_count e
// comprimento em bp por path. Custo idêntico ao loop de plan_paths, mas pago
// apenas no ingest — queries posteriores usam acesso O(1) aos arrays.

void GfaCsrTopology::build_path_stats(std::span<const uint32_t> lengths) {
    const size_t n = path_names_.size();
    path_node_counts_.resize(n);
    path_bp_lengths_.resize(n);
    // P1: paraleliza o loop sobre paths. Cada path_nodes(pid, buf) é
    // independente (read-only sobre gBWT) e writes em índices únicos não
    // conflitam. schedule(dynamic, 8) lida com paths de tamanhos
    // heterogêneos (no chm13 path médio ~160k nós; varia 1000×).
    // P2: cada thread mantém um buffer local reusável (firstprivate) para
    // evitar malloc/free a cada path_nodes — vetor cresce até a maior
    // sequência vista pela thread e é reaproveitado.
#ifdef _OPENMP
#pragma omp parallel
#endif
    {
        std::vector<uint64_t> buf;
#ifdef _OPENMP
#pragma omp for schedule(dynamic, 8)
#endif
        for (size_t pid = 0; pid < n; ++pid) {
            path_nodes(pid, buf);
            path_node_counts_[pid] = static_cast<uint32_t>(buf.size());
            int64_t total = 0;
            for (const uint64_t nid : buf) {
                if (nid < lengths.size()) {
                    total += static_cast<int64_t>(lengths[nid]);
                }
            }
            path_bp_lengths_[pid] = total;
        }
    }
}

} // namespace ggql
