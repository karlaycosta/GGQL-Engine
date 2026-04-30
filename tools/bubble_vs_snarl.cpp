// bubble_vs_snarl.cpp — compara duas implementações de detecção de bolhas
// no mesmo grafo já ingerido, para diagnóstico vs ferramentas externas
// (vg snarls, BubbleGun).
//
//   - find_snarls_streaming (cactus_finder.hpp) — usado hoje em FIND BUBBLE.
//     Algoritmo: Tsin 3-ECC + cactus + BFS. Detecta apenas snarls que
//     envolvem bridges entre componentes 3-edge-connected.
//
//   - GfaCsrTopology::find_bubbles (graph_algorithms.cpp) — não usado em
//     produção. Algoritmo: Onodera et al. 2013 (Kosaraju SCC + topological
//     sweep). Detecta superbubbles dentro do DAG de SCCs.
//
// Uso: bubble_vs_snarl <cache_dir.idx>

#include "ggql/storage/topology.hpp"
#include "ggql/storage/snarl_decomposition.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso: %s <cache_dir.idx>\n", argv[0]);
        return 1;
    }
    const std::string idx = argv[1];

    ggql::GfaCsrTopology topo;
    try {
        topo.load(idx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Falha ao carregar topology: %s\n", e.what());
        return 2;
    }

    std::printf("=== Topology em %s ===\n", idx.c_str());
    std::printf("num_nodes:   %lu\n", static_cast<unsigned long>(topo.num_nodes()));
    std::printf("num_edges:   %lu\n", static_cast<unsigned long>(topo.num_edges()));
    std::printf("\n");

    // ── 1. find_snarls (cactus) — já gravado em cache ────────────────────────
    if (topo.has_snarls()) {
        const auto snarls = topo.snarls();
        std::printf("snarls (find_snarls_streaming, cactus):    %lu\n",
                    static_cast<unsigned long>(snarls.size()));
    } else {
        std::printf("snarls (find_snarls_streaming, cactus):    [snarls.bin ausente]\n");
    }

    // ── 2. find_bubbles (Onodera 2013) — roda agora ──────────────────────────
    using clk = std::chrono::high_resolution_clock;
    const auto t0 = clk::now();
    auto bubbles = topo.find_bubbles(/*max_depth=*/0, /*max_nodes_per_branch=*/0,
                                     /*num_threads=*/1);
    const double sec = std::chrono::duration<double>(clk::now() - t0).count();
    std::printf("bubbles (find_bubbles, Onodera 2013):       %lu  (em %.3f s)\n",
                static_cast<unsigned long>(bubbles.size()), sec);

    // ── 3. find_all_snarls (Paten 2018, Fases 1+2) ────────────────────────────
    // Defina GGQL_DEBUG_BIEDGED=1 no env para ver estatísticas internas
    // (bridges spine vs link, components 2-edge-conn) em stderr.
    const auto t1 = clk::now();
    auto snarls_paten = ggql::snarl_decomposition::find_all_snarls(topo, 1);
    const double sec_paten = std::chrono::duration<double>(clk::now() - t1).count();
    std::printf("snarls (find_all_snarls, Paten 2018 F1+F2): %lu  (em %.3f s)\n",
                static_cast<unsigned long>(snarls_paten.size()), sec_paten);
    if (!snarls_paten.empty()) {
        size_t with_cycles = 0, dag = 0;
        size_t total_internal = 0;
        for (const auto& s : snarls_paten) {
            (s.has_cycles ? with_cycles : dag)++;
            total_internal += s.internal_node_count;
        }
        std::printf("  ultrabubbles=%lu, com ciclos=%lu, total nós internos=%lu\n",
                    static_cast<unsigned long>(dag),
                    static_cast<unsigned long>(with_cycles),
                    static_cast<unsigned long>(total_internal));
    }

    // ── 4. find_all_snarls_vgstyle (refactor inspirado em vg) ────────────────
    const auto t2 = clk::now();
    auto snarls_vgstyle = ggql::snarl_decomposition::find_all_snarls_vgstyle(topo, 1);
    const double sec_vg = std::chrono::duration<double>(clk::now() - t2).count();
    std::printf("snarls (find_all_snarls_vgstyle, refactor):  %lu  (em %.3f s)\n",
                static_cast<unsigned long>(snarls_vgstyle.size()), sec_vg);

    return 0;
}
