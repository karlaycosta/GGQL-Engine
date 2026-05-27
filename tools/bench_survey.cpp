// bench_survey — micro-benchmark isolado de survey_gfa_light.
// Roda N iterações pra dar amostras suficientes ao perf.
//
// Uso: bench_survey <gfa> [threads=8] [iters=5]

#include "ggql/ingest/profile.hpp"

#include <chrono>
#include <fmt/core.h>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::println(stderr, "Uso: bench_survey <gfa> [threads=8] [iters=5]");
        return 1;
    }
    const std::string gfa = argv[1];
    const int threads = (argc >= 3) ? std::stoi(argv[2]) : 8;
    const int iters   = (argc >= 4) ? std::stoi(argv[3]) : 5;

    using clock = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;

    fmt::println("bench_survey · {} · threads={} · iters={}", gfa, threads, iters);
    fmt::println("{:-^60}", "");

    double total = 0;
    for (int i = 0; i < iters; ++i) {
        const auto t0 = clock::now();
        auto sv = ggql::survey_gfa_light(gfa, threads);
        const double dt = sec(clock::now() - t0).count();
        if (!sv) {
            fmt::println(stderr, "iter {}: FAIL: {}", i, sv.error());
            return 1;
        }
        total += dt;
        fmt::println("iter {}: {:.3f}s  segs={} links={} paths={} seq={} avg_len={:.1f}",
                     i, dt, sv->segment_count, sv->link_count, sv->path_count,
                     sv->total_sequence_bytes, sv->avg_path_length);
    }
    fmt::println("{:-^60}", "");
    fmt::println("mean: {:.3f}s  total: {:.3f}s", total / iters, total);
    return 0;
}
