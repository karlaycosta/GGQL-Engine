// bench_fm_index — mede compute_payload_hash, search_kmer, load e save
// isolados de GfaSequenceIndex.
//
// Uso:
//   bench_fm_index <cache_dir> [--save-to <tmp_dir>] [--kmer <seq>]
//
// Sem --save-to mede só load + queries. Com --save-to mede também save.
// --kmer aceita uma sequência de teste (default: "ACGT").
//
// Saída chave=valor (uma métrica por linha).

#include "ggql/storage/fm_index.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
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

static size_t file_size(const std::string& p) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<size_t>(sz);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: bench_fm_index <cache_dir> "
                             "[--save-to <tmp_dir>] [--kmer <seq>]\n");
        return 1;
    }
    const std::string dir = argv[1];
    std::string save_to;
    std::string kmer = "ACGT";
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--save-to" && i + 1 < argc) {
            save_to = argv[++i];
        } else if (a == "--kmer" && i + 1 < argc) {
            kmer = argv[++i];
        }
    }

    const size_t rss0 = read_rss_kb();
    std::printf("size_fm_index_bin=%zu\n", file_size(dir + "/fm_index.bin"));

    // ── Load ─────────────────────────────────────────────────────────
    ggql::GfaSequenceIndex idx;
    {
        const auto t0 = clk::now();
        idx.load(dir);
        const auto t1 = clk::now();
        std::printf("load_ms=%.3f\n", ms(t1 - t0).count());
    }
    if (!idx.is_loaded()) {
        std::fprintf(stderr, "fm_index não carregado em %s\n", dir.c_str());
        return 2;
    }
    const size_t rss_after_load = read_rss_kb();
    std::printf("rss_after_load_kb=%zu\n", rss_after_load);
    std::printf("rss_delta_load_kb=%zu\n",
                rss_after_load > rss0 ? rss_after_load - rss0 : 0);

    // ── compute_payload_hash (alvo P1) ───────────────────────────────
    {
        const auto t0 = clk::now();
        const uint64_t h = idx.compute_payload_hash();
        const auto t1 = clk::now();
        std::printf("compute_payload_hash_ms=%.3f\n", ms(t1 - t0).count());
        std::printf("payload_hash_value=0x%016lx\n", h);
    }

    // Repete 3x para estabilidade
    double hash_runs[3];
    for (int r = 0; r < 3; ++r) {
        const auto t0 = clk::now();
        (void)idx.compute_payload_hash();
        const auto t1 = clk::now();
        hash_runs[r] = ms(t1 - t0).count();
    }
    std::printf("compute_payload_hash_run1_ms=%.3f\n", hash_runs[0]);
    std::printf("compute_payload_hash_run2_ms=%.3f\n", hash_runs[1]);
    std::printf("compute_payload_hash_run3_ms=%.3f\n", hash_runs[2]);

    // ── search_kmer ──────────────────────────────────────────────────
    {
        const auto t0 = clk::now();
        const auto hits = idx.search_kmer(kmer);
        const auto t1 = clk::now();
        std::printf("search_kmer_first_ms=%.3f\n", ms(t1 - t0).count());
        std::printf("search_kmer_hits=%zu\n", hits.size());
    }

    // 1000 buscas com k-mers aleatórios — mede latência média
    std::mt19937_64 rng(42);
    const char alphabet[] = "ACGT";
    std::string rkmer(8, 'A');
    constexpr int kIters = 1000;
    size_t total_hits = 0;
    const auto t0 = clk::now();
    for (int i = 0; i < kIters; ++i) {
        for (auto& c : rkmer) {
            c = alphabet[rng() % 4];
        }
        const auto hits = idx.search_kmer(rkmer);
        total_hits += hits.size();
    }
    const auto t1 = clk::now();
    std::printf("search_kmer_avg_us=%.3f\n",
                ms(t1 - t0).count() * 1000.0 / kIters);
    std::printf("search_kmer_total_hits=%zu\n", total_hits);
    std::printf("search_kmer_iters=%d\n", kIters);

    // ── Save (opcional) ──────────────────────────────────────────────
    if (!save_to.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(save_to, ec);
        const auto t0 = clk::now();
        idx.save(save_to);
        const auto t1 = clk::now();
        std::printf("save_ms=%.3f\n", ms(t1 - t0).count());
    }

    std::printf("rss_after_all_kb=%zu\n", read_rss_kb());
    return 0;
}
