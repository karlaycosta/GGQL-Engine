// bench_ingest — benchmark de carregamento e consultas na GfaCsrTopology.
//
// Carrega o GFA via GfaIngestorMmapParallel com --topology ativo e sem banco
// SQLite, medindo cada fase do pipeline e executando as consultas de análise
// de grafo N vezes para obter métricas de latência reproduzíveis.
//
// Uso:
//   bench_ingest <arquivo.gfa> [threads] [gamma] [max_depth] [runs]
//                              [max_nodes_per_branch] [threads_bfs]
//
//   threads             = threads de ingest e build CSR   (padrão: 4)
//   gamma               = fator de carga MPHF             (padrão: 2.0)
//   max_depth           = profundidade BFS find_bubbles    (padrão: 5)
//   runs                = repetições por algoritmo         (padrão: 3)
//   max_nodes_per_branch= cap por ramo BFS  (0=ilimitado)  (padrão: 0)
//   threads_bfs         = threads para find_bubbles        (padrão: 1)

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <numeric>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

#include "ggql/ingest/gfa_ingestor.hpp"

using hrclock = std::chrono::high_resolution_clock;
using ms_t    = std::chrono::duration<double, std::milli>;

// ── Helpers de formatação ─────────────────────────────────────────────────

static void separator(const char* title) {
    std::println("\n══════════════════════════════════════════════════════");
    std::println("  {}", title);
    std::println("══════════════════════════════════════════════════════");
}

// Executa fn() `runs` vezes; devolve (resultado da 1ª chamada, tempos em ms).
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

static void print_times(std::string_view label, const std::vector<double>& ts) {
    double sum = 0, mn = ts[0], mx = ts[0];
    for (double t : ts) { sum += t; if (t < mn) mn = t; if (t > mx) mx = t; }
    double avg = sum / static_cast<double>(ts.size());
    std::println("  {:28s}  avg={:9.1f} ms  min={:9.1f} ms  max={:9.1f} ms  (N={})",
                 label, avg, mn, mx, ts.size());
}

// ── Histograma de graus ───────────────────────────────────────────────────

static void print_degree_histogram(const ggql::GfaCsrTopology& topo) {
    // Buckets: 0, 1, 2, 3, 4, 5–9, 10–19, 20–49, 50–99, ≥100
    constexpr int NBUCKETS = 10;
    const char* labels[NBUCKETS] = {
        "0", "1", "2", "3", "4", "5-9", "10-19", "20-49", "50-99", ">=100"
    };
    auto bucket = [](uint64_t d) -> int {
        if (d <= 4)  return static_cast<int>(d);
        if (d <=  9) return 5;
        if (d <= 19) return 6;
        if (d <= 49) return 7;
        if (d <= 99) return 8;
        return 9;
    };

    std::array<uint64_t, NBUCKETS> fwd_hist{}, bwd_hist{};
    fwd_hist.fill(0); bwd_hist.fill(0);
    uint64_t max_out = 0, max_in = 0;
    uint64_t sum_out = 0, sum_in = 0;

    const uint64_t N = topo.num_nodes();
    for (uint64_t i = 0; i < N; ++i) {
        const uint64_t od = topo.out_degree(i);
        const uint64_t id = topo.in_degree(i);
        ++fwd_hist[bucket(od)];
        ++bwd_hist[bucket(id)];
        if (od > max_out) max_out = od;
        if (id > max_in)  max_in  = id;
        sum_out += od;
        sum_in  += id;
    }

    std::println("  {:>10s}  {:>18s}  {:>18s}", "grau", "out-degree (nós)", "in-degree  (nós)");
    for (int b = 0; b < NBUCKETS; ++b) {
        std::println("  {:>10s}  {:>18}  {:>18}", labels[b], fwd_hist[b], bwd_hist[b]);
    }
    const double avg_out = N ? static_cast<double>(sum_out) / N : 0.0;
    const double avg_in  = N ? static_cast<double>(sum_in)  / N : 0.0;
    std::println("  {:>10s}  {:>18.3f}  {:>18.3f}", "media", avg_out, avg_in);
    std::println("  {:>10s}  {:>18}  {:>18}", "max", max_out, max_in);
}

// ── Breakdown de memória CSR ──────────────────────────────────────────────

static void print_memory_breakdown(const ggql::GfaCsrTopology& topo) {
    constexpr double MB = 1024.0 * 1024.0;
    const uint64_t N = topo.num_nodes();
    const uint64_t E = topo.num_edges();

    // Cálculo manual por componente (espelha exatamente memory_bytes()).
    const double e_fwd_mb  = static_cast<double>(E)       * 8.0  / MB;
    const double e_bwd_mb  = static_cast<double>(E)       * 8.0  / MB;
    const double off_fwd_mb= static_cast<double>(N + 1)   * 4.0  / MB;
    const double off_bwd_mb= static_cast<double>(N + 1)   * 4.0  / MB;
    const double total_mb  = static_cast<double>(topo.memory_bytes()) / MB;

    std::println("  {:30s}  {:>10.1f} MB   ({} entradas × 8 B)", "E_fwd (arestas saída)",    e_fwd_mb,   E);
    std::println("  {:30s}  {:>10.1f} MB   ({} entradas × 8 B)", "E_bwd (arestas entrada)",  e_bwd_mb,   E);
    std::println("  {:30s}  {:>10.1f} MB   ({} entradas × 4 B)", "offsets_fwd",              off_fwd_mb, N + 1);
    std::println("  {:30s}  {:>10.1f} MB   ({} entradas × 4 B)", "offsets_bwd",              off_bwd_mb, N + 1);
    std::println("  {:30s}  {:>10.1f} MB", "TOTAL CSR", total_mb);
}

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::println(stderr,
            "Uso: bench_ingest <arquivo.gfa> [threads] [gamma] [max_depth] [runs]"
            " [max_nodes_per_branch] [threads_bfs]");
        return 1;
    }

    const std::string path     = argv[1];
    const int      threads     = (argc >= 3) ? std::stoi(argv[2]) : 4;
    const double   gamma       = (argc >= 4) ? std::stod(argv[3]) : 2.0;
    const int      max_depth   = (argc >= 5) ? std::stoi(argv[4]) : 5;
    const int      runs        = (argc >= 6) ? std::stoi(argv[5]) : 3;
    const size_t   max_nodes   = (argc >= 7) ? static_cast<size_t>(std::stoull(argv[6])) : 0;
    const int      threads_bfs = (argc >= 8) ? std::stoi(argv[7]) : 1;

    std::println("Arquivo          : {}", path);
    std::println("Threads ingest   : {}", threads);
    std::println("Gamma MPHF       : {}", gamma);
    std::println("max_depth        : {}", max_depth);
    std::println("Runs/algo        : {}", runs);
    std::println("max_nodes/branch : {}", max_nodes == 0 ? "ilimitado" : std::to_string(max_nodes));
    std::println("Threads BFS      : {}", threads_bfs);
    std::println("SQLite           : desabilitado (benchmark de topologia pura)");

    // ── Fase 1: Ingest + build CSR topology (sem SQLite) ──────────────────
    separator("Ingest + build CSR topology");

    ggql::GfaIngestorMmapParallel ingestor;
    auto res = ingestor.ingest(path, threads, gamma, /*verbose=*/true,
                               /*db_path=*/"", /*build_topology=*/true);
    if (!res) {
        std::println(stderr, "Erro no ingest: {}", res.error());
        return 1;
    }
    const auto& st = *res;

    std::println("\n  ── Breakdown de tempo ──────────────────────────────");
    std::println("  passada1 (io+parse)  : {:9.1f} ms", st.elapsed_io1_s    * 1000);
    std::println("  build MPHF           : {:9.1f} ms", st.elapsed_build_s  * 1000);
    std::println("  passada2 (ingest)    : {:9.1f} ms", st.elapsed_ingest_s * 1000);
    std::println("  build CSR topology   : {:9.1f} ms", st.elapsed_topology_s * 1000);
    std::println("  TOTAL                : {:9.1f} ms", st.elapsed_total_s  * 1000);
    std::println("  segmentos            : {}", st.segment_count);
    std::println("  links                : {}", st.link_count);
    std::println("  nos (MPHF keys)      : {}", st.node_count);

    const ggql::GfaCsrTopology* topo = ingestor.topology();
    if (!topo) {
        std::println(stderr, "Topologia nao construida.");
        return 1;
    }

    // ── Fase 2: Estrutura CSR — memoria e distribuicao de graus ───────────
    separator("Estrutura CSR — memoria");
    std::println("  nos    : {}", topo->num_nodes());
    std::println("  arestas: {} (por direcao)", topo->num_edges());
    print_memory_breakdown(*topo);

    separator("Distribuicao de graus");
    print_degree_histogram(*topo);

    // ── Fase 3: Benchmarks de consultas ───────────────────────────────────
    {
        const std::string title = "Benchmarks de consultas (runs=" + std::to_string(runs) + ")";
        separator(title.c_str());
    }

    // find_tips
    {
        auto [tips, times] = bench([&]{ return topo->find_tips(); }, runs);
        print_times("find_tips", times);
        std::println("    -> tips encontrados: {}", tips.size());
    }

    // find_repeats com limiares diferentes
    for (uint64_t min_deg : {uint64_t{2}, uint64_t{4}, uint64_t{10}}) {
        auto [reps, times] = bench([&]{ return topo->find_repeats(min_deg); }, runs);
        print_times("find_repeats(>=" + std::to_string(min_deg) + ")", times);
        std::println("    -> repeats encontrados: {}", reps.size());
    }

    // find_bubbles
    {
        auto [bubs, times] = bench([&]{
            return topo->find_bubbles(max_depth, max_nodes, threads_bfs);
        }, runs);
        print_times("find_bubbles", times);
        std::println("    -> bubbles encontradas: {}", bubs.size());

        if (!bubs.empty()) {
            std::println("\n  Primeiras {} bolhas:", std::min<size_t>(5, bubs.size()));
            for (size_t i = 0; i < std::min<size_t>(5, bubs.size()); ++i) {
                std::println("    [{}] source={} sink={} branches={}",
                             i, bubs[i].source, bubs[i].sink, bubs[i].branches.size());
            }
        }
    }

    std::println("\nBenchmark concluido.");
    return 0;
}
