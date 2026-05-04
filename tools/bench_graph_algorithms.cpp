// bench_graph_algorithms — isola tempos de funções de graph_algorithms.cpp.
//
// Mede no modo warm (cache do bench_topology já existente):
//   - build_path_stats(lengths)             — alvo P1 (paralelizar)
//   - path_nodes(pid)                       — alvo P2 (vector& out)
//   - make_component_remap(num_threads)     — referência
//   - find_bubbles(0, 0, num_threads)       — referência
//
// Uso:
//   bench_graph_algorithms <cache_dir> [--threads N]
//
// Saída em chave=valor (uma métrica por linha) — fácil diff/parsing.

#include "ggql/ingest/metadata_store.hpp"
#include "ggql/storage/topology.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

using clk = std::chrono::high_resolution_clock;
using ms = std::chrono::duration<double, std::milli>;

static size_t read_rss_kb() {
    std::ifstream f("/proc/self/statm");
    size_t total = 0, rss = 0;
    f >> total >> rss;
    return rss * static_cast<size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "uso: bench_graph_algorithms <cache_dir> [--threads N]\n");
        return 1;
    }
    const std::string dir = argv[1];
    int num_threads = 8;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--threads" && i + 1 < argc) {
            num_threads = std::stoi(argv[++i]);
        }
    }

    std::printf("threads=%d\n", num_threads);

    // ── Load topology + metadata ─────────────────────────────────────
    ggql::GfaCsrTopology topo;
    {
        const auto t0 = clk::now();
        topo.load(dir);
        const auto t1 = clk::now();
        std::printf("topology_load_ms=%.3f\n", ms(t1 - t0).count());
    }
    std::printf("num_nodes=%zu\n", static_cast<size_t>(topo.num_nodes()));
    std::printf("path_count=%zu\n", topo.path_count());

    ggql::GfaMetadataStore meta;
    {
        const auto t0 = clk::now();
        meta.load(dir);
        const auto t1 = clk::now();
        std::printf("metadata_load_ms=%.3f\n", ms(t1 - t0).count());
    }

    const size_t rss_after_load = read_rss_kb();
    std::printf("rss_after_load_kb=%zu\n", rss_after_load);

    // ── build_path_stats (P1 alvo) ───────────────────────────────────
    // Mede o trecho dominado por path_nodes() em loop sobre todos os paths.
    if (topo.path_count() > 0) {
        const auto t0 = clk::now();
        topo.build_path_stats(meta.lengths_span());
        const auto t1 = clk::now();
        const double bp_ms = ms(t1 - t0).count();
        std::printf("build_path_stats_ms=%.3f\n", bp_ms);
        std::printf("build_path_stats_avg_us_per_path=%.3f\n",
                    bp_ms * 1000.0 / static_cast<double>(topo.path_count()));
    } else {
        std::printf("build_path_stats_ms=0.000\n");
    }

    // ── path_nodes (P2 alvo) ─────────────────────────────────────────
    // Mede latência média de extração de path. Alocação de vector está dentro.
    if (topo.path_count() > 0) {
        const size_t kIters = std::min<size_t>(topo.path_count(), 1000);
        size_t total_nodes = 0;
        const auto t0 = clk::now();
        for (size_t i = 0; i < kIters; ++i) {
            const auto nodes = topo.path_nodes(i);
            total_nodes += nodes.size();
        }
        const auto t1 = clk::now();
        const double total_ms = ms(t1 - t0).count();
        std::printf("path_nodes_avg_us=%.3f\n", total_ms * 1000.0 / kIters);
        std::printf("path_nodes_total_nodes_extracted=%zu\n", total_nodes);
        std::printf("path_nodes_iters=%zu\n", kIters);
    }

    // ── make_component_remap ─────────────────────────────────────────
    // Aloca caches; chamamos uma vez (não muda o objeto, é const).
    {
        const auto t0 = clk::now();
        const auto remap_res = topo.make_component_remap(num_threads);
        const auto t1 = clk::now();
        std::printf("make_component_remap_ms=%.3f\n", ms(t1 - t0).count());
        std::printf("num_components=%u\n", remap_res.num_components);
    }

    // ── find_bubbles ─────────────────────────────────────────────────
    // Custoso; faz Kosaraju + sweep. Os parâmetros max_depth e
    // max_nodes_per_branch são ignorados na implementação atual (B6).
    {
        const auto t0 = clk::now();
        const auto bubbles = topo.find_bubbles(num_threads);
        const auto t1 = clk::now();
        std::printf("find_bubbles_ms=%.3f\n", ms(t1 - t0).count());
        std::printf("num_bubbles=%zu\n", bubbles.size());
    }

    std::printf("rss_after_all_kb=%zu\n", read_rss_kb());
    return 0;
}
