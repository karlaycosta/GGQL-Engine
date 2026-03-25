// bench_bubbles — benchmark isolado de find_bubbles na GfaCsrTopology.
//
// Fluxo:
//   1. Ingesta o GFA e constrói a topologia CSR (ingest com build_topology=true)
//   2. Executa find_tips / find_repeats / find_bubbles N vezes e mede latência
//   3. Exibe resumo com contagens e tempos por chamada
//
// Uso: bench_bubbles <arquivo.gfa> [threads] [max_depth] [runs] [max_nodes_per_branch] [threads_bfs]
//   threads             = threads de ingest (padrão: 4)
//   max_depth           = profundidade BFS (padrão: 5)
//   runs                = repetições de cada algoritmo (padrão: 3)
//   max_nodes_per_branch= cap por ramo (padrão: 0 = sem limite)
//   threads_bfs         = threads para find_bubbles (padrão: 1 = serial)

#include <chrono>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ggql/ingest/gfa_ingestor.hpp"

using hrclock  = std::chrono::high_resolution_clock;
using ms_t     = std::chrono::duration<double, std::milli>;

static void separator(const char* title) {
    std::println("\n══════════════════════════════════════════");
    std::println("  {}", title);
    std::println("══════════════════════════════════════════");
}

// Executa fn() `runs` vezes, retorna (resultado da 1ª chamada, vetor de tempos em ms)
template<typename Fn>
static auto bench(Fn&& fn, int runs) {
    using R = std::decay_t<decltype(fn())>;
    R first{};
    std::vector<double> times;
    times.reserve(static_cast<size_t>(runs));
    for (int i = 0; i < runs; ++i) {
        auto t0 = hrclock::now();
        auto r  = fn();
        times.push_back(ms_t(hrclock::now() - t0).count());
        if (i == 0) first = std::move(r);
    }
    return std::pair{std::move(first), std::move(times)};
}

static void print_times(std::string_view label, const std::vector<double>& times) {
    double sum = 0, mn = times[0], mx = times[0];
    for (double t : times) { sum += t; if (t < mn) mn = t; if (t > mx) mx = t; }
    std::println("  {:20s}  avg={:8.1f} ms  min={:8.1f} ms  max={:8.1f} ms  (runs={})",
                 label, sum / times.size(), mn, mx, times.size());
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(stderr, "Uso: bench_bubbles <arquivo.gfa> [threads] [max_depth] [runs]");
        return 1;
    }
    const std::string path      = argv[1];
    const int         threads      = (argc >= 3) ? std::stoi(argv[2]) : 4;
    const int         max_depth    = (argc >= 4) ? std::stoi(argv[3]) : 5;
    const int         runs         = (argc >= 5) ? std::stoi(argv[4]) : 3;
    const size_t      max_nodes    = (argc >= 6) ? static_cast<size_t>(std::stoull(argv[5])) : 0;
    const int         threads_bfs  = (argc >= 7) ? std::stoi(argv[6]) : 1;

    std::println("Arquivo          : {}", path);
    std::println("Threads ingest   : {}", threads);
    std::println("max_depth        : {}", max_depth);
    std::println("Runs/algo        : {}", runs);
    std::println("max_nodes/branch : {}", max_nodes == 0 ? "ilimitado" : std::to_string(max_nodes));
    std::println("Threads BFS      : {}", threads_bfs);

    // ── Ingest + build topology ────────────────────────────────────────────
    separator("Ingest + CSR topology");
    ggql::GfaIngestorMmapParallel ingestor;
    auto res = ingestor.ingest(path, threads, /*gamma=*/2.0, /*verbose=*/true,
                               /*db_path=*/"", /*build_topology=*/true);
    if (!res) {
        std::println(stderr, "Erro ingest: {}", res.error());
        return 1;
    }
    const ggql::GfaCsrTopology* topo = ingestor.topology();
    if (!topo) {
        std::println(stderr, "Topologia nao construida.");
        return 1;
    }
    std::println("\n  nos    : {}", topo->num_nodes());
    std::println("  arestas: {}", topo->num_edges());
    std::println("  memoria: {:.1f} MB", topo->memory_bytes() / (1024.0 * 1024.0));

    // ── find_tips ─────────────────────────────────────────────────────────
    separator("Algoritmos de analise de grafo");
    {
        auto [tips, times] = bench([&]{ return topo->find_tips(); }, runs);
        print_times("find_tips", times);
        std::println("  -> tips encontrados: {}", tips.size());
    }

    // ── find_repeats ──────────────────────────────────────────────────────
    {
        auto [rep, times] = bench([&]{ return topo->find_repeats(2); }, runs);
        print_times("find_repeats(>=2)", times);
        std::println("  -> repeats encontrados: {}", rep.size());
    }

    // ── find_bubbles ──────────────────────────────────────────────────────
    {
        auto [bub, times] = bench([&]{
            return topo->find_bubbles(max_depth, max_nodes, threads_bfs);
        }, runs);
        print_times("find_bubbles", times);
        std::println("  -> bubbles encontradas: {}", bub.size());

        if (!bub.empty()) {
            // Exibe as 5 primeiras bolhas como sanidade
            std::println("\n  Primeiras {} bolhas:", std::min<size_t>(5, bub.size()));
            for (size_t i = 0; i < std::min<size_t>(5, bub.size()); ++i) {
                std::println("    [{}] source={} sink={} branches={}",
                             i, bub[i].source, bub[i].sink, bub[i].branches.size());
            }
        }
    }

    std::println("\nBenchmark concluido.");
    return 0;
}
