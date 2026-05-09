// bench_boophf.cpp — Microbenchmark de baseline para BooPHF.
// Mede build time, lookup latency, peak RSS, e tamanho serializado.
// Output em CSV no stdout para facilitar diff entre runs:
//
//   metric,n,gamma,threads,fastmode,value
//   build_ms,1000000,2.0,4,0.03,134.2
//   lookup_ns_p50,1000000,2.0,4,0.03,18.5
//   ...
//
// Uso: ./bench_boophf > docs/baseline_boophf_<data>.csv

#include "mphf/BooPHF.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
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

struct Result {
    double build_ms;
    double lookup_ns_p50;
    double lookup_ns_p99;
    long peak_rss_kb_after_build;
    size_t serialized_bytes;
};

static Result bench_one(size_t n, double gamma, int threads, float fastmode) {
    auto keys = make_keys(n, 0xC0FFEE);

    // ── Build ─────────────────────────────────────────────────────────────
    auto t0 = clk::now();
    mphf_t m(n, keys, threads, gamma, fastmode);
    auto t1 = clk::now();
    double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    long rss_after = peak_rss_kb();

    // ── Lookup latency (1M ops em keys aleatórias do conjunto) ────────────
    constexpr size_t LOOKUP_OPS = 1'000'000;
    std::mt19937_64 rng(0xBADF00D);
    std::vector<uint64_t> probe(LOOKUP_OPS);
    for (size_t i = 0; i < LOOKUP_OPS; ++i) {
        probe[i] = keys[rng() % keys.size()];
    }
    std::vector<uint64_t> lat_ns;
    lat_ns.reserve(LOOKUP_OPS);

    // warmup
    volatile uint64_t sink = 0;
    for (size_t i = 0; i < 10'000; ++i) {
        sink ^= m.lookup(probe[i % LOOKUP_OPS]);
    }

    // medir em batches de 1024 (resolução de chrono ~ns)
    constexpr size_t BATCH = 1024;
    for (size_t i = 0; i + BATCH <= LOOKUP_OPS; i += BATCH) {
        auto a = clk::now();
        for (size_t j = 0; j < BATCH; ++j) {
            sink ^= m.lookup(probe[i + j]);
        }
        auto b = clk::now();
        double batch_ns = std::chrono::duration<double, std::nano>(b - a).count();
        lat_ns.push_back(static_cast<uint64_t>(batch_ns / BATCH));
    }
    (void)sink;

    std::sort(lat_ns.begin(), lat_ns.end());
    auto p50 = static_cast<double>(lat_ns[lat_ns.size() / 2]);
    auto p99 = static_cast<double>(lat_ns[lat_ns.size() * 99 / 100]);

    // ── Tamanho serializado ───────────────────────────────────────────────
    std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
    m.save(buf);
    buf.seekg(0, std::ios::end);
    size_t serialized = static_cast<size_t>(buf.tellg());

    return {build_ms, p50, p99, rss_after, serialized};
}

int main() {
    std::printf("metric,n,gamma,threads,fastmode,value\n");

    struct Config {
        size_t n;
        double gamma;
        int threads;
        float fastmode;
    };
    Config configs[] = {
        {100'000, 2.0, 1, 0.03F},
        {100'000, 2.0, 4, 0.03F},
        {1'000'000, 2.0, 1, 0.03F},
        {1'000'000, 2.0, 4, 0.03F},
        {1'000'000, 2.0, 4, 0.0F},   // fastmode off
        {1'000'000, 5.0, 4, 0.03F},  // gamma alto
        {10'000'000, 2.0, 4, 0.03F},
    };

    for (const auto& c : configs) {
        auto r = bench_one(c.n, c.gamma, c.threads, c.fastmode);
        auto fmt_row = [&](const char* metric, double v) {
            std::printf("%s,%zu,%.1f,%d,%.2f,%.2f\n", metric, c.n, c.gamma, c.threads,
                        static_cast<double>(c.fastmode), v);
        };
        fmt_row("build_ms", r.build_ms);
        fmt_row("lookup_ns_p50", r.lookup_ns_p50);
        fmt_row("lookup_ns_p99", r.lookup_ns_p99);
        fmt_row("peak_rss_kb", static_cast<double>(r.peak_rss_kb_after_build));
        fmt_row("serialized_bytes", static_cast<double>(r.serialized_bytes));
        fmt_row("bytes_per_elem", static_cast<double>(r.serialized_bytes) / static_cast<double>(c.n));
    }
    return 0;
}
