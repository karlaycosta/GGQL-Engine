// bench_snarl.cpp — Microbenchmark do snarl_decomposition::find_all_snarls.
//
// Carrega um .idx pré-construído (CSR + EF) e roda apenas a fase de snarls,
// medindo wall, peak RSS e contagem. Permite iterar em otimizações de
// snarl sem repagar o custo de gBWT/FM-Index/PASS2.
//
// Uso: bench_snarl <cache_dir.idx> [N=5] [threads=1]

#include "ggql/storage/topology.hpp"
#include "ggql/storage/snarl_decomposition.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

static size_t read_rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("VmRSS:")) {
            std::string_view sv(line);
            sv.remove_prefix(6);
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
            size_t v = 0;
            std::from_chars(sv.data(), sv.data() + sv.size(), v);
            return v;
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso: %s <cache_dir.idx> [N=5] [threads=1]\n", argv[0]);
        return 1;
    }
    const std::string idx = argv[1];
    const int N = (argc >= 3) ? std::atoi(argv[2]) : 5;
    const int threads = (argc >= 4) ? std::atoi(argv[3]) : 1;

    ggql::GfaCsrTopology topo;
    try {
        topo.load(idx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Falha load: %s\n", e.what());
        return 2;
    }

    const size_t rss_after_load_kb = read_rss_kb();
    std::printf("=== %s (nodes=%lu, edges=%lu, N=%d, threads=%d) ===\n",
                idx.c_str(),
                static_cast<unsigned long>(topo.num_nodes()),
                static_cast<unsigned long>(topo.num_edges()),
                N, threads);
    std::printf("RSS após load topology: %.2f GB\n", rss_after_load_kb / 1024.0 / 1024.0);

    using clk = std::chrono::high_resolution_clock;
    std::vector<double> times_ms;
    times_ms.reserve(N);
    size_t last_count = 0;
    size_t peak_rss_kb = rss_after_load_kb;
    for (int i = 0; i < N; ++i) {
        const auto t0 = clk::now();
        auto r = ggql::snarl_decomposition::find_all_snarls(topo, threads);
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        const size_t rss_now = read_rss_kb();
        if (rss_now > peak_rss_kb) peak_rss_kb = rss_now;
        times_ms.push_back(ms);
        last_count = r.size();
        std::printf("  iter %d: %.2f s  (snarls=%zu, RSS=%.2f GB)\n",
                    i + 1, ms / 1000.0, r.size(), rss_now / 1024.0 / 1024.0);
    }
    std::sort(times_ms.begin(), times_ms.end());
    double sum = 0;
    for (double t : times_ms) sum += t;
    const double avg = sum / N;
    const double med = times_ms[N / 2];
    std::printf("min=%.2f s  median=%.2f s  avg=%.2f s  max=%.2f s\n",
                times_ms.front() / 1000.0, med / 1000.0,
                avg / 1000.0, times_ms.back() / 1000.0);
    std::printf("snarls=%zu  peak_RSS=%.2f GB  Δ_RSS=%.2f GB\n",
                last_count, peak_rss_kb / 1024.0 / 1024.0,
                (peak_rss_kb - rss_after_load_kb) / 1024.0 / 1024.0);
    return 0;
}
