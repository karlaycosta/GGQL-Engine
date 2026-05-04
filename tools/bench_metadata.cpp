// bench_metadata — isola tempo de load() e queries de GfaMetadataStore.
// Uso:
//   bench_metadata <cache_dir>
//
// Imprime em formato chave=valor (fácil de parsear) tempos em ms para:
//   - load_total
//   - first_lookup_name (constrói índice lazy)
//   - second_lookup_name (hit do cache)
//   - segment_id_from_offset (média de 1000 chamadas)
//
// Também imprime peak RSS final via /proc/self/statm.

#include "ggql/ingest/metadata_store.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
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
        std::fprintf(stderr, "uso: bench_metadata <cache_dir>\n");
        return 1;
    }
    const std::string dir = argv[1];

    const size_t rss0 = read_rss_kb();
    auto t0 = clk::now();
    ggql::GfaMetadataStore m;
    m.load(dir);
    auto t1 = clk::now();
    const size_t rss1 = read_rss_kb();

    const size_t n = m.size();
    std::printf("num_nodes=%zu\n", n);
    std::printf("load_ms=%.3f\n", ms(t1 - t0).count());
    std::printf("rss_after_load_kb=%zu\n", rss1);
    std::printf("rss_delta_kb=%zu\n", rss1 > rss0 ? rss1 - rss0 : 0);

    if (n == 0) {
        return 0;
    }

    // First lookup_name: constrói o índice lazy (call_once).
    const std::string sample_name = std::string(m.original_id(n / 2));
    auto t2 = clk::now();
    auto v = m.lookup_name(sample_name);
    auto t3 = clk::now();
    std::printf("first_lookup_name_ms=%.3f\n", ms(t3 - t2).count());
    std::printf("first_lookup_hit=%d\n", v.has_value() ? 1 : 0);
    const size_t rss2 = read_rss_kb();
    std::printf("rss_after_first_lookup_kb=%zu\n", rss2);

    // Segunda chamada — só hit no map.
    auto t4 = clk::now();
    constexpr int kIters = 10000;
    int hits = 0;
    for (int i = 0; i < kIters; ++i) {
        if (m.lookup_name(sample_name)) { ++hits; }
    }
    auto t5 = clk::now();
    std::printf("second_lookup_avg_ns=%.1f\n", ms(t5 - t4).count() * 1e6 / kIters);
    std::printf("second_lookup_hits=%d\n", hits);

    // segment_id_from_offset — sortea offsets aleatórios.
    std::mt19937_64 rng(42);
    const auto& offs = m.all_seq_offsets();
    if (offs.size() < 2) {
        return 0;
    }
    const uint64_t max_off = offs.back() > 0 ? offs.back() - 1 : 0;
    std::uniform_int_distribution<uint64_t> dist(0, max_off);
    auto t6 = clk::now();
    int found = 0;
    constexpr int kOffIters = 1000;
    for (int i = 0; i < kOffIters; ++i) {
        if (m.segment_id_from_offset(dist(rng))) { ++found; }
    }
    auto t7 = clk::now();
    std::printf("segment_id_from_offset_avg_ns=%.1f\n",
                ms(t7 - t6).count() * 1e6 / kOffIters);
    std::printf("segment_id_from_offset_hits=%d\n", found);

    // Range query (length).
    if (m.has_length_index()) {
        auto t8 = clk::now();
        auto span = m.ids_in_length_range(100, 10000);
        auto t9 = clk::now();
        std::printf("ids_in_length_range_ms=%.3f\n", ms(t9 - t8).count());
        std::printf("ids_in_length_range_count=%zu\n", span.size());
    }

    return 0;
}
