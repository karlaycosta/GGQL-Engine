// bench_topology — isola tempo de GfaCsrTopology::load() e save().
// Usado para medir P1/P2 (paralelização de I/O em path_index.cpp).
//
// Uso:
//   bench_topology <cache_dir> [--save-to <tmp_dir>]
//
// Sem --save-to, mede apenas load (cold/warm depende do page cache).
// Com --save-to <dir>, mede load + save (escreve cópia em <dir>;
// caller é responsável por limpar). Os arquivos .sdsl/E_*/gbwts/topology são
// reescritos em ordem que espelha GfaCsrTopology::save().
//
// Saída: chave=valor, uma métrica por linha (fácil de diff/parsing).

#include "ggql/storage/topology.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
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

static size_t file_size_or_zero(const std::string& path) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<size_t>(sz);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "uso: bench_topology <cache_dir> [--save-to <tmp_dir>]\n");
        return 1;
    }
    const std::string dir = argv[1];
    std::string save_to;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--save-to" && i + 1 < argc) {
            save_to = argv[++i];
        }
    }

    const size_t rss0 = read_rss_kb();

    // Tamanhos dos arquivos do cache (informativos).
    const size_t sz_e_fwd = file_size_or_zero(dir + "/E_fwd.bin");
    const size_t sz_e_bwd = file_size_or_zero(dir + "/E_bwd.bin");
    const size_t sz_ef_fwd = file_size_or_zero(dir + "/ef_fwd.sdsl");
    const size_t sz_ef_bwd = file_size_or_zero(dir + "/ef_bwd.sdsl");
    const size_t sz_path_stores = file_size_or_zero(dir + "/path_stores.bin");
    const size_t sz_gbwts = file_size_or_zero(dir + "/gbwts.bin");
    const size_t sz_topology = file_size_or_zero(dir + "/topology.bin");
    const size_t sz_path_stats = file_size_or_zero(dir + "/path_stats.bin");
    const size_t sz_snarls = file_size_or_zero(dir + "/snarls.bin");

    std::printf("size_E_fwd_bin=%zu\n", sz_e_fwd);
    std::printf("size_E_bwd_bin=%zu\n", sz_e_bwd);
    std::printf("size_ef_fwd_sdsl=%zu\n", sz_ef_fwd);
    std::printf("size_ef_bwd_sdsl=%zu\n", sz_ef_bwd);
    std::printf("size_path_stores_bin=%zu\n", sz_path_stores);
    std::printf("size_gbwts_bin=%zu\n", sz_gbwts);
    std::printf("size_topology_bin=%zu\n", sz_topology);
    std::printf("size_path_stats_bin=%zu\n", sz_path_stats);
    std::printf("size_snarls_bin=%zu\n", sz_snarls);

    // ── Load ─────────────────────────────────────────────────────────
    auto t0 = clk::now();
    ggql::GfaCsrTopology topo;
    topo.load(dir);
    auto t1 = clk::now();
    const size_t rss_after_load = read_rss_kb();

    std::printf("num_nodes=%zu\n", static_cast<size_t>(topo.num_nodes()));
    std::printf("path_count=%zu\n", topo.path_count());
    std::printf("load_ms=%.3f\n", ms(t1 - t0).count());
    std::printf("rss_after_load_kb=%zu\n", rss_after_load);
    std::printf("rss_delta_load_kb=%zu\n",
                rss_after_load > rss0 ? rss_after_load - rss0 : 0);

    // ── Save (opcional) ──────────────────────────────────────────────
    if (!save_to.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(save_to, ec);
        auto t2 = clk::now();
        topo.save(save_to);
        auto t3 = clk::now();
        const size_t rss_after_save = read_rss_kb();
        std::printf("save_ms=%.3f\n", ms(t3 - t2).count());
        std::printf("rss_after_save_kb=%zu\n", rss_after_save);
    }

    return 0;
}
