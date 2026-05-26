// bench_suggest_profile — Mede overhead da varredura leve standalone vs
// baseline ingest cold. Serve como regressão para survey_gfa_light + heurística
// suggest_profile.
//
// Histórico: havia um cenário C (prepare+reuse Pass1) que foi descartado em
// 2026-05-25 — ganho teórico (~5 s) ficava dentro do ruído natural do FM-Index
// (±20 s entre runs). Ver bench_suggest_profile_2026-05-25.md.
//
// 2 cenários (cada um cold — apaga .idx antes):
//   A) baseline      : ingest cold direto
//   B) light survey  : survey_gfa_light + ingest cold (usa perfil sugerido)
//
// Reporta: tempo até sugestão (time-to-hint), tempo total, peak RSS.

#include "ggql/cli/console.hpp"
#include "ggql/ingest/ingestor.hpp"
#include "ggql/ingest/profile.hpp"

#include <chrono>
#include <filesystem>
#include <fmt/core.h>
#include <string>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

namespace cli = ggql::cli;
using hrclock = std::chrono::high_resolution_clock;
using sec_t = std::chrono::duration<double>;

static size_t read_process_peak_rss_kb() {
    struct rusage ru{};
    ::getrusage(RUSAGE_SELF, &ru);
    return static_cast<size_t>(ru.ru_maxrss);
}

struct ScenarioResult {
    std::string name;
    double t_to_hint_s = 0.0;  // tempo até survey sugerir um perfil
    double t_ingest_s  = 0.0;  // tempo da fase de ingest propriamente
    double t_total_s   = 0.0;  // soma
    size_t peak_rss_kb = 0;
    std::string suggested_profile;
};

static void print_kv(const std::string& k, const std::string& v) {
    fmt::println("  {:<30} {}", k, v);
}

static void print_result(const ScenarioResult& r) {
    cli::title("Cenário " + r.name);
    print_kv("Sugestão de perfil",
             r.suggested_profile.empty() ? "(não calculada)" : r.suggested_profile);
    print_kv("Tempo até sugestão",  fmt::format("{:.3f} s", r.t_to_hint_s));
    print_kv("Tempo de ingest",     fmt::format("{:.3f} s", r.t_ingest_s));
    print_kv("Tempo TOTAL (E2E)",   fmt::format("{:.3f} s", r.t_total_s));
    print_kv("Peak RSS",            fmt::format("{:.2f} GB",
                                                r.peak_rss_kb / 1024.0 / 1024.0));
    cli::blank();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::println(stderr, "Uso: bench_suggest_profile <gfa> [threads] [scenario:A|B|all]");
        return 1;
    }
    const std::string gfa = argv[1];
    const int threads = (argc >= 3) ? std::stoi(argv[2]) : 8;
    const std::string scenario = (argc >= 4) ? argv[3] : "all";
    const std::string cache_dir = gfa + ".idx";

    cli::title("bench_suggest_profile · reuse Pass1");
    print_kv("Arquivo", gfa);
    print_kv("Threads", std::to_string(threads));
    print_kv("Cenário", scenario);
    cli::blank();

    auto run_scenario_A = [&]() {
        // Cold ingest baseline — sem survey.
        ScenarioResult r{"A — baseline ingest cold"};
        std::filesystem::remove_all(cache_dir);

        const auto t_start = hrclock::now();
        ggql::GfaIngestorMmapParallel ing;
        const auto pf = ggql::flags_for(ggql::IngestProfile::Pangenome);
        auto res = ing.ingest(gfa, threads, 2.0, /*verbose=*/false, pf.save_metadata,
                              pf.build_topology, pf.build_fm_index, 0, false, cache_dir,
                              pf.build_gbwt, pf.build_snarls);
        const auto t_end = hrclock::now();
        if (!res) {
            fmt::println(stderr, "[A] ingest falhou: {}", res.error());
            return r;
        }
        r.t_to_hint_s = 0.0;  // sem sugestão
        r.t_ingest_s = sec_t(t_end - t_start).count();
        r.t_total_s = r.t_ingest_s;
        r.peak_rss_kb = read_process_peak_rss_kb();
        r.suggested_profile = "(skipped)";
        return r;
    };

    auto run_scenario_B = [&]() {
        // Light survey + ingest cold separado.
        ScenarioResult r{"B — light survey + ingest cold"};
        std::filesystem::remove_all(cache_dir);

        const auto t_start = hrclock::now();
        auto sv = ggql::survey_gfa_light(gfa, threads);
        const auto t_survey = hrclock::now();
        if (!sv) {
            fmt::println(stderr, "[B] survey falhou: {}", sv.error());
            return r;
        }
        const auto suggested = ggql::suggest_profile(*sv);
        r.suggested_profile = std::string(ggql::name_of(suggested));
        r.t_to_hint_s = sec_t(t_survey - t_start).count();

        // ingest reusa o perfil sugerido (decisão do usuário).
        ggql::GfaIngestorMmapParallel ing;
        const auto pf = ggql::flags_for(suggested);
        auto res = ing.ingest(gfa, threads, 2.0, false, pf.save_metadata, pf.build_topology,
                              pf.build_fm_index, 0, false, cache_dir, pf.build_gbwt,
                              pf.build_snarls);
        const auto t_end = hrclock::now();
        if (!res) {
            fmt::println(stderr, "[B] ingest falhou: {}", res.error());
            return r;
        }
        r.t_ingest_s = sec_t(t_end - t_survey).count();
        r.t_total_s = sec_t(t_end - t_start).count();
        r.peak_rss_kb = read_process_peak_rss_kb();
        return r;
    };

    std::vector<ScenarioResult> results;
    if (scenario == "A" || scenario == "all") { results.push_back(run_scenario_A()); }
    if (scenario == "B" || scenario == "all") { results.push_back(run_scenario_B()); }

    for (const auto& r : results) { print_result(r); }

    if (results.size() > 1) {
        cli::title("Comparação");
        fmt::println("  {:<30} {:>12} {:>12} {:>12}", "Cenário", "T→hint", "T_ingest", "T_total");
        for (const auto& r : results) {
            fmt::println("  {:<30} {:>10.3f} s {:>10.3f} s {:>10.3f} s", r.name, r.t_to_hint_s,
                         r.t_ingest_s, r.t_total_s);
        }
        cli::blank();
    }
    return 0;
}
