#pragma once

// gfa_cactus_finder.hpp — Detecção de bubbles/snarls via 3-ECC + grafo Cactus.
//
// Abordagem (duas fases):
//
//   Fase 1 — Contração por 3-ECC
//     Nós 3-aresta-conectados (≥3 caminhos aresta-disjuntos entre quaisquer
//     dois nós do grupo) são fundidos em um único "super-nó". O grafo resultante
//     é um grafo Cactus dirigido onde cada aresta restante é uma bridge (aresta
//     de corte) ou parte de um único ciclo simples.
//
//   Fase 2 — Detecção de bubbles no grafo contraído
//     O grafo Cactus tem muito menos nós que o grafo original; aplicar a mesma
//     busca BFS/intersecção convergente é muito mais rápida e dispensa o
//     parâmetro max_depth (o grafo Cactus é essencialmente uma floresta de
//     árvores com ciclos simples).
//
//     Cada bubble encontrada no grafo contraído é depois "expandida": os nós
//     originais de cada super-nó intermediário são incluídos na lista de
//     internal_nodes do Snarl.
//
// Interface pública:
//   struct Snarl { source, sink, internal_nodes, has_cycles }
//   std::vector<Snarl> find_snarls(const GfaCsrTopology&)
//   std::vector<GfaCsrTopology::Bubble> snarls_to_bubbles(const std::vector<Snarl>&)
//
// Complexidade:
//   Fase 1 (Tsin): O(n + m)
//   Fase 2 (BFS cactus): O(C + B) onde C = componentes 3-ECC, B = bridges
//   Expansão: O(n) total
//
// Restrições:
//   - NÃO modifica gfa_topology.hpp
//   - header-only, sem dependências externas além de STL e os nossos detail/

#include "ggql/storage/detail/tsin_3ecc.hpp"
#include "ggql/storage/detail/union_find.hpp"
#include "ggql/storage/gfa_topology.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ggql {

// ── Resultado: Snarl (superset do Bubble original) ────────────────────────
struct Snarl {
    uint64_t source;                      // nó de entrada do snarl
    uint64_t sink;                        // nó de saída do snarl
    std::vector<uint64_t> internal_nodes; // todos os nós internos (todos os ramos)
    bool has_cycles = false;              // algum 3-ECC interno tem >1 nó
};

// ── Helpers internos ──────────────────────────────────────────────────────
namespace detail_cactus {

// Grafo CSR simples e compacto para o grafo contraído.
struct CompactCsr {
    size_t num_nodes = 0;
    std::vector<uint32_t> offsets; // offsets[i]..offsets[i+1] = vizinhos de i
    std::vector<uint32_t> adj;     // IDs de componentes (destinos)

    void build(size_t n, std::vector<std::pair<uint32_t, uint32_t>> edges) {
        num_nodes = n;
        offsets.assign(n + 1, 0);
        // deduplica arestas paralelas (o cactus não deve tê-las, mas por segurança)
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        for (auto& [src, dst] : edges)
            ++offsets[src + 1];
        for (size_t i = 1; i <= n; ++i)
            offsets[i] += offsets[i - 1];
        adj.resize(edges.size());
        std::vector<uint32_t> pos(offsets.begin(), offsets.end() - 1);
        for (auto& [src, dst] : edges)
            adj[pos[src]++] = dst;
    }

    std::span<const uint32_t> out_neighbors(size_t node) const noexcept {
        return {adj.data() + offsets[node], offsets[node + 1] - offsets[node]};
    }

    uint32_t out_degree(size_t node) const noexcept { return offsets[node + 1] - offsets[node]; }
};

// Converte mapa de original→componente para índice comprimido 0..K-1.
// Preenche comp_nodes[k] = lista de nós originais no componente k.
inline std::vector<uint32_t>
compress_components(detail::UnionFind& uf, size_t num_nodes,
                    std::vector<std::vector<uint64_t>>& comp_nodes) // saída
{
    std::vector<uint32_t> node_to_comp(num_nodes, ~0u);
    uint32_t next_id = 0;
    std::unordered_map<uint64_t, uint32_t> root_to_id;
    root_to_id.reserve(num_nodes / 4 + 1);

    for (size_t i = 0; i < num_nodes; ++i) {
        uint64_t root = uf.find(static_cast<uint64_t>(i));
        auto [it, inserted] = root_to_id.try_emplace(root, 0u);
        if (inserted) {
            it->second = next_id++;
            comp_nodes.emplace_back();
        }
        uint32_t cid = it->second;
        node_to_comp[i] = cid;
        comp_nodes[cid].push_back(static_cast<uint64_t>(i));
    }
    return node_to_comp;
}

// Bubble detection by BFS + progressive intersection no grafo contraído.
// Equivalente à lógica de find_bubbles, mas sem max_depth (desnecessário
// no grafo cactus) e sem multi-thread (o grafo contraído é pequeno).
inline std::vector<std::pair<uint32_t, uint32_t>> detect_bubbles_in_cactus(const CompactCsr& g) {
    std::vector<std::pair<uint32_t, uint32_t>> result;
    const size_t N = g.num_nodes;
    if (N == 0)
        return result;

    // contadores de quantos ramos já visitaram cada nó do cactus
    std::vector<uint32_t> visit_count(N, 0);
    std::vector<uint32_t> dirty; // nós modificados (para reset eficiente)

    for (size_t src = 0; src < N; ++src) {
        if (g.out_degree(src) < 2)
            continue; // fonte deve ter ≥2 saídas

        auto branches = g.out_neighbors(src);
        const uint32_t nb = static_cast<uint32_t>(branches.size());

        dirty.clear();

        // BFS simultâneo por todos os ramos; cada ramo mantém sua própria fila.
        // Um nó é "candidato a sink" quando visit_count[n] == nb.
        // visited_from: por ramo, unordered_set → O(visited) não O(N).
        std::vector<std::vector<uint32_t>> queues(nb);
        std::vector<std::unordered_set<uint32_t>> visited_from(nb);

        bool found_sink = false;
        uint32_t sink = ~0u;

        // Inicializa filas com os nós imediatamente adjacentes à fonte
        for (uint32_t b = 0; b < nb; ++b) {
            uint32_t first = branches[b];
            if (first == static_cast<uint32_t>(src))
                continue; // self-loop: ignora
            queues[b].push_back(first);
            visited_from[b].insert(first);
            ++visit_count[first];
            dirty.push_back(first);
        }

        // Limite de nós para evitar explosão em grafos que não têm snarl aqui
        constexpr size_t MAX_VISITED = 8192;
        size_t total_visited = nb;

        bool active = true;
        while (active && !found_sink) {
            active = false;
            for (uint32_t b = 0; b < nb && !found_sink; ++b) {
                std::vector<uint32_t> next;
                for (uint32_t node : queues[b]) {
                    for (uint32_t nb_node : g.out_neighbors(node)) {
                        if (nb_node == static_cast<uint32_t>(src))
                            continue; // não voltar à fonte
                        if (visited_from[b].count(nb_node))
                            continue;
                        visited_from[b].insert(nb_node);
                        ++total_visited;
                        if (++visit_count[nb_node] == nb) {
                            sink = nb_node;
                            found_sink = true;
                            break;
                        }
                        dirty.push_back(nb_node);
                        next.push_back(nb_node);
                        active = true;
                    }
                    if (found_sink)
                        break;
                }
                queues[b] = std::move(next);
            }
            if (total_visited > MAX_VISITED)
                break; // segurança
        }

        // Reset contadores
        for (uint32_t n : dirty)
            visit_count[n] = 0;

        if (found_sink)
            result.emplace_back(static_cast<uint32_t>(src), sink);
    }
    return result;
}

} // namespace detail_cactus

// ── Função principal ──────────────────────────────────────────────────────

inline std::vector<Snarl> find_snarls(const GfaCsrTopology& topo) {
    const size_t N = static_cast<size_t>(topo.num_nodes());
    if (N == 0)
        return {};

    // ── Fase 1: 3-ECC → UnionFind ────────────────────────────────────────────
    detail::UnionFind uf(N);

    // Vizinhança não-dirigida (unindo arestas de saída e de entrada).
    auto for_each_neighbor = [&](size_t u, const std::function<void(size_t)>& emit) {
        for (uint64_t enc : topo.out_neighbors(static_cast<uint64_t>(u)))
            emit(static_cast<size_t>(GfaCsrTopology::decode(enc).node_id));
        for (uint64_t enc : topo.in_neighbors(static_cast<uint64_t>(u)))
            emit(static_cast<size_t>(GfaCsrTopology::decode(enc).node_id));
    };

    detail::three_edge_connected_merges(N, for_each_neighbor,
                                        [&](size_t a, size_t b) { uf.unite(a, b); });

    // ── Fase 2: Contração → grafo menor (cactus) ──────────────────────────────
    std::vector<std::vector<uint64_t>> comp_nodes;
    auto node_to_comp = detail_cactus::compress_components(uf, N, comp_nodes);
    const size_t num_comp = comp_nodes.size();

    // Arestas do grafo contraído: (comp_src, comp_dst), apenas bridges
    std::vector<std::pair<uint32_t, uint32_t>> cactus_edges;
    cactus_edges.reserve(N / 4 + 1);

    for (uint64_t u = 0; u < static_cast<uint64_t>(N); ++u) {
        uint32_t cu = node_to_comp[u];
        for (uint64_t enc : topo.out_neighbors(u)) {
            uint64_t v = GfaCsrTopology::decode(enc).node_id;
            uint32_t cv = node_to_comp[v];
            if (cu != cv)
                cactus_edges.emplace_back(cu, cv);
        }
    }

    detail_cactus::CompactCsr cactus;
    cactus.build(num_comp, std::move(cactus_edges));

    // ── Fase 3: Detectar bubbles no grafo contraído ─────────────────────────
    auto contracted_bubbles = detail_cactus::detect_bubbles_in_cactus(cactus);

    // ── Fase 4: Expandir para nós originais ───────────────────────────────────
    std::vector<Snarl> snarls;
    snarls.reserve(contracted_bubbles.size());

    for (auto [src_comp, sink_comp] : contracted_bubbles) {
        // Coletar os nós contidos em cada componente intermediário.
        // Para simplificar, usamos o primeiro nó de cada componente de borda
        // como representante de source e sink.
        Snarl s;
        s.source = comp_nodes[src_comp].front();
        s.sink = comp_nodes[sink_comp].front();

        // internal_nodes: todos os nós das componentes entre source e sink
        // (excluindo as próprias componentes de source e sink)
        // Faz BFS no cactus entre src_comp e sink_comp para achar intermediários
        std::vector<bool> seen(num_comp, false);
        std::vector<uint32_t> q;
        seen[src_comp] = true;
        q.push_back(src_comp);
        std::vector<uint32_t> path_comps;

        // BFS simples para encontrar componentes intermediárias
        for (size_t qi = 0; qi < q.size(); ++qi) {
            uint32_t cur = q[qi];
            for (uint32_t nxt : cactus.out_neighbors(cur)) {
                if (seen[nxt])
                    continue;
                seen[nxt] = true;
                q.push_back(nxt);
                if (nxt != sink_comp)
                    path_comps.push_back(nxt);
            }
        }

        // Preenche internal_nodes com todos os nós originais nas componentes
        // intermediárias; detecta ciclos (componentes com >1 nó original)
        for (uint32_t cid : path_comps) {
            for (uint64_t orig_node : comp_nodes[cid]) {
                s.internal_nodes.push_back(orig_node);
            }
            if (comp_nodes[cid].size() > 1)
                s.has_cycles = true;
        }

        if (!s.internal_nodes.empty()) {
            snarls.push_back(std::move(s));
        }
    }

    return snarls;
}

// ── Conversão Snarl → Bubble (compatibilidade) ────────────────────────────
inline std::vector<Bubble> snarls_to_bubbles(const std::vector<Snarl>& snarls) {
    std::vector<Bubble> out;
    out.reserve(snarls.size());
    for (const auto& s : snarls) {
        out.push_back({s.source, s.sink, s.internal_nodes});
    }
    return out;
}

} // namespace ggql
