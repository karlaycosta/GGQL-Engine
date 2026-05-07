// bench_gbwt — Benchmark da performance do gBWT.
//
// Variante de bench_gbwt_perf que usa gfa_ingestor_vg.hpp:
//   - Remapeia IDs MPHF → IDs sequenciais por componente
//   - Reduz sigma do DynamicGBWT de ~85M para ~M_i (tamanho do componente)
//   - Habilita construção paralela efetiva (igual ao vg)
//
// Uso:
//   bench_gbwt <arquivo.gfa> [threads] [max_paths]
//
//   threads    = threads para parse e construção paralela     (padrão: 8)
//   max_paths  = limite de caminhos a carregar (0=ilimitado)  (padrão: 0)

#include "ggql/cli/console.hpp"
#include "ggql/ingest/ingestor.hpp"
#include "ggql/ingest/profile.hpp"

#include <chrono>
#include <fmt/color.h>
#include <fmt/core.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace cli = ggql::cli;
using hrclock = std::chrono::high_resolution_clock;
using sec_t = std::chrono::duration<double>;

// ── Medição de RSS do Processo ──────────────────────────────────────────────

static size_t read_process_rss_kb() {
    std::ifstream f("/proc/self/statm");
    size_t pages_total = 0, pages_rss = 0;
    f >> pages_total >> pages_rss;
    return pages_rss * static_cast<size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
}

int main(int argc, char* argv[]) {
    // Coleta args posicionais e flags (--quiet/-q, --profile/-p <name>,
    // --path-backend/-b <pathstorage|gbwt|auto>).
    bool quiet = false;
    ggql::IngestProfile profile = ggql::IngestProfile::Pangenome;
    std::optional<ggql::PathBackendKind> path_backend_override;
    std::vector<const char*> pos;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a == "--quiet" || a == "-q") {
            quiet = true;
        } else if ((a == "--profile" || a == "-p") && i + 1 < argc) {
            const std::string_view pv{argv[++i]};
            auto parsed = ggql::parse_profile(pv);
            if (!parsed) {
                fmt::println(stderr,
                             "Perfil desconhecido: '{}'  (use: pangenome|metagenomics|assembly|comparative)",
                             pv);
                return 1;
            }
            profile = *parsed;
        } else if ((a == "--path-backend" || a == "-b") && i + 1 < argc) {
            const std::string_view bv{argv[++i]};
            auto parsed = ggql::parse_path_backend(bv);
            if (!parsed) {
                fmt::println(stderr,
                             "Backend desconhecido: '{}'  (use: pathstorage|gbwt|auto)",
                             bv);
                return 1;
            }
            path_backend_override = *parsed;
        } else {
            pos.push_back(argv[i]);
        }
    }

    if (pos.empty()) {
        fmt::println(stderr,
                     "Uso: bench_gbwt <arquivo.gfa> [threads] [max_paths] [--quiet|-q] "
                     "[--profile|-p <name>] [--path-backend|-b <name>]\n"
                     "  --profile pangenome     (default) tudo habilitado\n"
                     "  --profile metagenomics  sem gBWT, sem snarls\n"
                     "  --profile assembly      sem gBWT, com snarls\n"
                     "  --profile comparative   com gBWT, sem snarls\n"
                     "  --path-backend pathstorage  (default) array uint32 cru, ratio 4 B/no\n"
                     "  --path-backend gbwt         BWT comprimido, ratio ~1-2 bits/no, extract via LF\n"
                     "  --path-backend auto         heuristica futura (= pathstorage hoje)");
        return 1;
    }

    if (quiet) { cli::set_quiet(true); }

    const std::string path = pos[0];
    const int threads = (pos.size() >= 2) ? std::stoi(pos[1]) : 8;
    const size_t max_p_arg = (pos.size() >= 3) ? std::stoul(pos[2]) : 0;
    const size_t max_paths = (max_p_arg == 0) ? std::numeric_limits<size_t>::max() : max_p_arg;

    const std::string cache_dir = path + ".idx";
    auto pf = ggql::flags_for(profile);
    if (path_backend_override) {
        pf.path_backend = *path_backend_override;
    }

    cli::title("bench_gbwt · GfaCsrTopology gBWT");
    cli::kv("Arquivo", path);
    cli::kv("Threads", std::to_string(threads));
    cli::kv("Perfil", std::string(ggql::name_of(profile)));
    cli::kv("Backend", std::string(ggql::name_of(pf.path_backend)));
    cli::kv("Max paths", (max_p_arg == 0) ? "ilimitado" : std::to_string(max_p_arg));
    cli::kv("Cache idx", cache_dir);

    const size_t rss_initial = read_process_rss_kb();

    ggql::GfaIngestorMmapParallel ingestor;

    auto res = ingestor.ingest(path, threads,
                               2.0,             // gamma padrão MPHF
                               !quiet,          // verbose
                               pf.save_metadata,
                               pf.build_topology,
                               pf.build_fm_index,
                               max_paths,
                               false,           // keep_gbwt_tmp
                               cache_dir,
                               pf.build_gbwt,
                               pf.build_snarls,
                               pf.path_backend);

    if (!res) {
        cli::blank();
        cli::fail(fmt::format("Ingest falhou: {}", res.error()));
        return 1;
    }

    const auto& st = *res;
    const size_t rss_final = read_process_rss_kb();

    cli::title("Métricas de Performance gBWT");
    cli::blank();

    cli::section("Tempos de Execução");
    cli::kv_time("Tempo Real (Wall-clock)", st.elapsed_gbwt_s);
    cli::kv_time("Tempo CPU Médio/Thread", st.gbwt_cpu_avg_s);
    cli::kv_time("Tempo CPU Máximo (Thread)", st.gbwt_cpu_max_s);
    const double control_overhead = std::max(0.0, st.elapsed_gbwt_s - st.gbwt_cpu_max_s);
    if (control_overhead > 0.001) {
        cli::kv_time("Overhead de Controle/Merge", control_overhead);
    }

    cli::section("Memória e Escalonamento");
    if (st.gbwt_peak_rss_kb > 0) {
        cli::kv_size("Pico RSS (Build Isolado)", static_cast<size_t>(st.gbwt_peak_rss_kb) * 1024);
    }
    cli::kv_size("ΔRSS Processo (Total)",
                 (rss_final > rss_initial ? (rss_final - rss_initial) * 1024 : 0));
    cli::kv_size("Tamanho do Índice", st.gbwt_index_size_b);

    cli::section("Throughput e Eficiência");
    const double nodes_per_s = (st.elapsed_gbwt_s > 0)
                                   ? static_cast<double>(st.gbwt_total_nodes) / st.elapsed_gbwt_s
                                   : 0.0;
    const double bits_per_node =
        (st.gbwt_total_nodes > 0)
            ? static_cast<double>(st.gbwt_index_size_b) * 8.0 / st.gbwt_total_nodes
            : 0.0;

    cli::kv_count("Total de Caminhos", st.path_count);
    cli::kv_count("Total de Nós (Símbolos)", st.gbwt_total_nodes);

    // Formata Throughput com unidades K/M se for grande
    std::string rate_str;
    if (nodes_per_s > 1e6) {
        rate_str = fmt::format("{:.2f} M nós/s", nodes_per_s / 1e6);
    } else if (nodes_per_s > 1e3) {
        rate_str = fmt::format("{:.1f} K nós/s", nodes_per_s / 1e3);
    } else {
        rate_str = fmt::format("{:.0f} nós/s", nodes_per_s);
    }
    cli::kv("Throughput de Construção", rate_str);
    cli::kv_number("Eficiência de Compressão", bits_per_node, "bits/nó");

    cli::section("Contexto de Ingestão");
    cli::kv_time("Fase 1 (MPHF)", st.elapsed_io1_s + st.elapsed_build_s);
    cli::kv_time("Fase 2 (Links/Paths)", st.elapsed_ingest_s);
    cli::kv_time("Fase 3 (FM-Index)", st.elapsed_fm_index_s);
    cli::kv_time("Fase 4 (CSR Topology)", st.elapsed_topology_s);
    cli::kv_time("TOTAL Pipeline", st.elapsed_total_s);

    cli::blank();
    cli::ok("Benchmark concluído com sucesso");
    cli::blank();

    return 0;
}
