// bench_path_storage — mede extração de paths via PathStorage no cache atual.
// Cenário "build_path_stats sintético": loop sobre todos os paths somando
// node ids em xor (evita dead-code elim e aproxima o trabalho real). Compara
// o dispatch de `GfaCsrTopology::path_nodes` com acesso direto ao
// `PathStorage` construído off-band a partir do mesmo dataset.
//
// Uso:
//   bench_path_storage <cache_dir> [--paths N] [--save-path-stores]
//
// Sem --paths, processa todos os paths do cache. Com --paths N, limita a N
// (útil para datasets enormes onde a duplicação em RAM seria proibitiva).
//
// --save-path-stores: persiste path_stores.bin no cache_dir. Útil para upgrade
// incremental de caches antigos (apenas com gbwts.bin) — `load()` materializa
// path_stores_ on-the-fly e este flag escreve em disco para evitar a migração
// no próximo carregamento.
//
// Saída: chave=valor, uma métrica por linha.

#include "ggql/gbwt/path_storage.hpp"
#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

using clk = std::chrono::high_resolution_clock;
using ms  = std::chrono::duration<double, std::milli>;

static size_t read_rss_kb() {
    std::ifstream f("/proc/self/statm");
    size_t total = 0, rss = 0;
    f >> total >> rss;
    return rss * static_cast<size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
}

static int max_threads() {
#ifdef _OPENMP
    return omp_get_max_threads();
#else
    return 1;
#endif
}

// Cenário 1: dispatch via topology (path_nodes — caminho que callers usam).
static std::uint64_t bench_topology_serial(const ggql::GfaCsrTopology& topo,
                                            size_t                      n,
                                            double&                     ms_out) {
    std::vector<std::uint64_t> buf;
    std::uint64_t              acc = 0;
    auto                       t0  = clk::now();
    for (size_t pid = 0; pid < n; ++pid) {
        topo.path_nodes(pid, buf);
        for (auto nid : buf) { acc ^= nid; }
    }
    auto t1 = clk::now();
    ms_out  = ms(t1 - t0).count();
    return acc;
}

static std::uint64_t bench_topology_parallel(const ggql::GfaCsrTopology& topo,
                                              size_t                      n,
                                              double&                     ms_out) {
    std::uint64_t global_acc = 0;
    auto          t0         = clk::now();
#pragma omp parallel reduction(^ : global_acc)
    {
        std::vector<std::uint64_t> buf;
#pragma omp for schedule(dynamic, 8)
        for (size_t pid = 0; pid < n; ++pid) {
            topo.path_nodes(pid, buf);
            for (auto nid : buf) { global_acc ^= nid; }
        }
    }
    auto t1 = clk::now();
    ms_out  = ms(t1 - t0).count();
    return global_acc;
}

// Cenário 2: PathStorage::extract_into — equivalente direto (cópia).
static std::uint64_t bench_pstore_extract_serial(const ggql::PathStorage& ps,
                                                 size_t                         n,
                                                 double&                        ms_out) {
    std::vector<std::uint64_t> buf;
    std::uint64_t              acc = 0;
    auto                       t0  = clk::now();
    for (size_t pid = 0; pid < n; ++pid) {
        ps.extract_into(pid, buf);
        for (auto nid : buf) { acc ^= nid; }
    }
    auto t1 = clk::now();
    ms_out  = ms(t1 - t0).count();
    return acc;
}

static std::uint64_t bench_pstore_extract_parallel(const ggql::PathStorage& ps,
                                                   size_t                         n,
                                                   double&                        ms_out) {
    std::uint64_t global_acc = 0;
    auto          t0         = clk::now();
#pragma omp parallel reduction(^ : global_acc)
    {
        std::vector<std::uint64_t> buf;
#pragma omp for schedule(dynamic, 8)
        for (size_t pid = 0; pid < n; ++pid) {
            ps.extract_into(pid, buf);
            for (auto nid : buf) { global_acc ^= nid; }
        }
    }
    auto t1 = clk::now();
    ms_out  = ms(t1 - t0).count();
    return global_acc;
}

// Cenário 3: PathStorage::nodes_view — zero-copy span.
static std::uint64_t bench_pstore_view_serial(const ggql::PathStorage& ps,
                                              size_t                         n,
                                              double&                        ms_out) {
    std::uint64_t acc = 0;
    auto          t0  = clk::now();
    for (size_t pid = 0; pid < n; ++pid) {
        const auto v = ps.nodes_view(pid);
        for (auto nid : v) { acc ^= nid; }
    }
    auto t1 = clk::now();
    ms_out  = ms(t1 - t0).count();
    return acc;
}

static std::uint64_t bench_pstore_view_parallel(const ggql::PathStorage& ps,
                                                size_t                         n,
                                                double&                        ms_out) {
    std::uint64_t global_acc = 0;
    auto          t0         = clk::now();
#pragma omp parallel reduction(^ : global_acc)
    {
#pragma omp for schedule(dynamic, 8)
        for (size_t pid = 0; pid < n; ++pid) {
            const auto v = ps.nodes_view(pid);
            for (auto nid : v) { global_acc ^= nid; }
        }
    }
    auto t1 = clk::now();
    ms_out  = ms(t1 - t0).count();
    return global_acc;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "uso: bench_path_storage <cache_dir> [--paths N] [--save-path-stores]\n");
        return 1;
    }
    const std::string dir = argv[1];
    size_t            paths_limit = 0;  // 0 = todos
    bool              save_path_stores = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--paths" && i + 1 < argc) {
            paths_limit = static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (a == "--save-path-stores") {
            save_path_stores = true;
        }
    }

    // ── Load topology ────────────────────────────────────────────────
    auto t_load0 = clk::now();
    ggql::GfaCsrTopology topo;
    topo.load(dir);
    auto t_load1 = clk::now();
    std::printf("threads=%d\n", max_threads());
    std::printf("load_ms=%.3f\n", ms(t_load1 - t_load0).count());
    std::printf("path_count=%zu\n", topo.path_count());
    std::printf("path_stores_present_at_load=%d\n",
                topo.has_path_storage_backend() ? 1 : 0);

    const size_t n = paths_limit == 0
                       ? topo.path_count()
                       : std::min(paths_limit, topo.path_count());
    std::printf("paths_used=%zu\n", n);

    // ── Materializa todos os paths (off-band) p/ popular PathStorage local ─
    // Buf vem como vector<uint64_t> (API pública); converte para
    // vector<PathStorage::node_type> = vector<uint32_t> antes de armazenar.
    auto t_mat0 = clk::now();
    std::vector<std::vector<ggql::PathStorage::node_type>> all_paths;
    all_paths.reserve(n);
    {
        std::vector<std::uint64_t> buf;
        for (size_t pid = 0; pid < n; ++pid) {
            topo.path_nodes(pid, buf);
            std::vector<ggql::PathStorage::node_type> p32;
            p32.reserve(buf.size());
            for (auto v : buf) {
                p32.push_back(static_cast<ggql::PathStorage::node_type>(v));
            }
            all_paths.emplace_back(std::move(p32));
        }
    }
    auto t_mat1 = clk::now();
    std::printf("materialize_paths_ms=%.3f\n", ms(t_mat1 - t_mat0).count());

    size_t total_nodes = 0;
    for (const auto& p : all_paths) { total_nodes += p.size(); }
    std::printf("total_nodes_in_paths=%zu\n", total_nodes);

    const size_t rss_after_mat = read_rss_kb();
    std::printf("rss_after_materialize_kb=%zu\n", rss_after_mat);

    // ── Build PathStorage off-band (não conta no bench de extração) ──
    auto t_build0 = clk::now();
    auto ps = ggql::PathStorage::from_paths(std::move(all_paths));
    auto t_build1 = clk::now();
    std::printf("pstore_build_ms=%.3f\n", ms(t_build1 - t_build0).count());
    std::printf("pstore_num_paths=%zu\n", ps.num_paths());
    std::printf("pstore_total_nodes=%zu\n", ps.total_nodes());
    std::printf("pstore_memory_mb=%.2f\n",
                static_cast<double>(ps.memory_bytes()) / (1024.0 * 1024.0));

    // ── Benchmarks ──────────────────────────────────────────────────
    double ms_topo_s = 0, ms_topo_p = 0;
    double ms_ext_s  = 0, ms_ext_p  = 0;
    double ms_view_s = 0, ms_view_p = 0;

    const auto acc_t_s = bench_topology_serial(topo, n, ms_topo_s);
    const auto acc_t_p = bench_topology_parallel(topo, n, ms_topo_p);
    const auto acc_e_s = bench_pstore_extract_serial(ps, n, ms_ext_s);
    const auto acc_e_p = bench_pstore_extract_parallel(ps, n, ms_ext_p);
    const auto acc_v_s = bench_pstore_view_serial(ps, n, ms_view_s);
    const auto acc_v_p = bench_pstore_view_parallel(ps, n, ms_view_p);

    // Sanity: todos os accs devem bater (mesmo dataset, xor é commutativo).
    const bool acc_match = (acc_t_s == acc_e_s) && (acc_e_s == acc_v_s)
                        && (acc_t_p == acc_e_p) && (acc_e_p == acc_v_p)
                        && (acc_t_s == acc_t_p);
    std::printf("accumulators_match=%d\n", acc_match ? 1 : 0);
    std::printf("acc_xor=0x%016lx\n", static_cast<unsigned long>(acc_t_s));

    std::printf("topology_serial_ms=%.3f\n", ms_topo_s);
    std::printf("topology_parallel_ms=%.3f\n", ms_topo_p);
    std::printf("pstore_extract_into_serial_ms=%.3f\n", ms_ext_s);
    std::printf("pstore_extract_into_parallel_ms=%.3f\n", ms_ext_p);
    std::printf("pstore_view_serial_ms=%.3f\n", ms_view_s);
    std::printf("pstore_view_parallel_ms=%.3f\n", ms_view_p);

    auto safe_ratio = [](double a, double b) {
        return b > 0.0 ? a / b : 0.0;
    };
    // Overhead do dispatch da topology vs acesso direto.
    std::printf("dispatch_overhead_serial=%.2fx\n",
                safe_ratio(ms_topo_s, ms_ext_s));
    std::printf("dispatch_overhead_parallel=%.2fx\n",
                safe_ratio(ms_topo_p, ms_ext_p));

    // dispatch_consistency: garante que o caminho topology produz o mesmo
    // resultado que o PathStorage off-band (defesa contra regressão em
    // path_nodes / rebuild_offsets_).
    std::printf("dispatch_consistency=%d\n", acc_match ? 1 : 0);

    if (save_path_stores) {
        auto t_sv0 = clk::now();
        topo.save_path_stores_only(dir);
        auto t_sv1 = clk::now();
        std::printf("save_path_stores_ms=%.3f\n", ms(t_sv1 - t_sv0).count());
    }

    return acc_match ? 0 : 2;
}
