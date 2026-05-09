// bench_boophf.cpp — Microbenchmark de baseline para BooPHF.
// Roda cada config N vezes (default 5) e reporta mediana / p99 / min / max
// de build_ms, mediana de lookup latency, peak RSS, e tamanho serializado.
//
// Output em CSV no stdout para facilitar diff entre runs:
//
//   metric,n,gamma,threads,fastmode,value
//   build_ms_p50,1000000,2.0,4,0.03,162.5
//   build_ms_p99,1000000,2.0,4,0.03,177.8
//   ...
//
// Uso: ./bench_boophf [N_RUNS] > docs/baseline_boophf_<data>.csv
//
// Limitação conhecida: peak_rss_kb é high-water mark do processo (getrusage),
// não da config — é cumulativo. Configs são executadas em ordem crescente de
// n para que o peak reflita a config maior. Para tracking fino de RSS por
// config, usar /proc/self/statm ou mallinfo2 entre runs.

#include "mphf/BooPHF.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <sstream>
#include <sys/resource.h>
#include <vector>

struct U64Hasher {
    [[nodiscard]] uint64_t operator()(uint64_t key, uint64_t seed = 0) const noexcept {
        uint64_t z = key + seed + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

using mphf_t = boomphf::mphf<uint64_t, U64Hasher>;
using clk = std::chrono::steady_clock;

static std::vector<uint64_t> make_keys(size_t n, uint64_t seed) {
    std::set<uint64_t> uniq;
    std::mt19937_64 rng(seed);
    while (uniq.size() < n) {
        uniq.insert(rng());
    }
    return {uniq.begin(), uniq.end()};
}

static long peak_rss_kb() {
    struct rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;  // Linux: KB; macOS: bytes
}

static double percentile(std::vector<double>& v, double pct) {
    std::sort(v.begin(), v.end());
    const size_t idx =
        std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * pct));
    return v[idx];
}

struct OneRun {
    double build_ms;
    double lookup_ns_p50;
    double lookup_ns_p99;
    long peak_rss_kb;
    size_t serialized_bytes;
};

static OneRun bench_once(size_t n, double gamma, int threads, float fastmode,
                         const std::vector<uint64_t>& keys) {
    auto t0 = clk::now();
    mphf_t m(n, keys, threads, gamma, fastmode);
    auto t1 = clk::now();
    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    constexpr size_t LOOKUP_OPS = 1'000'000;
    std::mt19937_64 rng(0xBADF00D);
    std::vector<uint64_t> probe(LOOKUP_OPS);
    for (size_t i = 0; i < LOOKUP_OPS; ++i) {
        probe[i] = keys[rng() % keys.size()];
    }
    std::vector<double> lat_ns;
    lat_ns.reserve(LOOKUP_OPS / 1024);

    volatile uint64_t sink = 0;
    for (size_t i = 0; i < 10'000; ++i) {
        sink ^= m.lookup(probe[i % LOOKUP_OPS]);
    }

    constexpr size_t BATCH = 1024;
    for (size_t i = 0; i + BATCH <= LOOKUP_OPS; i += BATCH) {
        auto a = clk::now();
        for (size_t j = 0; j < BATCH; ++j) {
            sink ^= m.lookup(probe[i + j]);
        }
        auto b = clk::now();
        double batch_ns = std::chrono::duration<double, std::nano>(b - a).count();
        lat_ns.push_back(batch_ns / BATCH);
    }
    (void)sink;

    double p50 = percentile(lat_ns, 0.50);
    double p99 = percentile(lat_ns, 0.99);

    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    m.save(buf);
    buf.seekg(0, std::ios::end);
    size_t serialized = static_cast<size_t>(buf.tellg());

    return {build_ms, p50, p99, peak_rss_kb(), serialized};
}

int main(int argc, char** argv) {
    int n_runs = 5;
    if (argc > 1) {
        n_runs = std::atoi(argv[1]);
        if (n_runs < 1) {
            n_runs = 1;
        }
    }

    std::printf("metric,n,gamma,threads,fastmode,value\n");

    struct Config {
        size_t n;
        double gamma;
        int threads;
        float fastmode;
    };
    // Ordem crescente de custo para que peak_rss_kb cresça monotonicamente.
    Config configs[] = {
        {100'000, 2.0, 1, 0.03F},
        {100'000, 2.0, 4, 0.03F},
        {1'000'000, 2.0, 1, 0.03F},
        {1'000'000, 2.0, 4, 0.03F},
        {1'000'000, 2.0, 4, 0.0F},
        {1'000'000, 5.0, 4, 0.03F},
        {10'000'000, 2.0, 4, 0.03F},
    };

    for (const auto& c : configs) {
        auto keys = make_keys(c.n, 0xC0FFEE);

        std::vector<double> build_samples;
        std::vector<double> lookup_p50_samples;
        std::vector<double> lookup_p99_samples;
        long peak_rss_first = 0;
        size_t serialized_first = 0;

        for (int run = 0; run < n_runs; ++run) {
            auto r = bench_once(c.n, c.gamma, c.threads, c.fastmode, keys);
            build_samples.push_back(r.build_ms);
            lookup_p50_samples.push_back(r.lookup_ns_p50);
            lookup_p99_samples.push_back(r.lookup_ns_p99);
            if (run == 0) {
                peak_rss_first = r.peak_rss_kb;
                serialized_first = r.serialized_bytes;
            }
        }

        auto fmt_row = [&](const char* metric, double v) {
            std::printf("%s,%zu,%.1f,%d,%.2f,%.2f\n", metric, c.n, c.gamma, c.threads,
                        static_cast<double>(c.fastmode), v);
        };

        // build_ms: distribuição
        std::vector<double> build_sorted = build_samples;
        std::sort(build_sorted.begin(), build_sorted.end());
        fmt_row("build_ms_min", build_sorted.front());
        fmt_row("build_ms_p50", build_sorted[build_sorted.size() / 2]);
        fmt_row("build_ms_p99", percentile(build_samples, 0.99));
        fmt_row("build_ms_max", build_sorted.back());

        // lookup latency: já é p50/p99 dentro de cada run; agregamos a mediana
        // dos p50s/p99s entre runs.
        fmt_row("lookup_ns_p50", percentile(lookup_p50_samples, 0.50));
        fmt_row("lookup_ns_p99", percentile(lookup_p99_samples, 0.50));

        // RSS e tamanho: invariantes entre runs — primeira run.
        fmt_row("peak_rss_kb", static_cast<double>(peak_rss_first));
        fmt_row("serialized_bytes", static_cast<double>(serialized_first));
        fmt_row("bytes_per_elem",
                static_cast<double>(serialized_first) / static_cast<double>(c.n));
    }
    return 0;
}
