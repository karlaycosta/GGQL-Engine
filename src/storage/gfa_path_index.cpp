// gfa_path_index.cpp — Construção paralela gBWT e persistência (save/load) de GfaCsrTopology.
//

#include "ggql/storage/gfa_topology.hpp"

#include "ggql/cli/console.hpp"
#include "ggql/cli/progress_tracker.hpp"

// Headers pesados — incluídos só aqui, não no .hpp público.
#include "ggql/storage/checksum_builder.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <execution>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/core.h>
#include <fstream>
#include <future>
#include <gbwt/dynamic_gbwt.h> // GBWTBuilder, DynamicGBWT
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h> // ::unlink

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

namespace {

/// Helper para monitoramento isolado de Resident Set Size (RSS).
/// Usa std::jthread com stop_token (C++23) — auto-join no destrutor.
class MemoryMonitor {
  public:
    explicit MemoryMonitor(std::chrono::milliseconds interval = std::chrono::milliseconds(100))
        : interval_(interval) {
        worker_ = std::jthread([this, interval](std::stop_token st) {
            while (!st.stop_requested()) {
                const size_t current = get_current_rss_kb();
                size_t m = peak_.load(std::memory_order_relaxed);
                while (current > m && !peak_.compare_exchange_weak(m, current)) {
                    // spin or retry
                }
                std::this_thread::sleep_for(interval);
            }
        });
    }

    ~MemoryMonitor() { stop(); }

    void stop() {
        worker_.request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    size_t peak_kb() const { return peak_.load(); }

  private:
    static size_t get_current_rss_kb() {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.starts_with("VmRSS:")) {
                std::string_view sv(line);
                sv.remove_prefix(6);
                while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
                    sv.remove_prefix(1);
                }
                size_t val = 0;
                std::from_chars(sv.data(), sv.data() + sv.size(), val);
                return val; // Linux: KB
            }
        }
        return 0;
    }

    std::atomic<size_t> peak_{0};
    std::chrono::milliseconds interval_;
    std::jthread worker_;
};

/// Lê MemAvailable de /proc/meminfo (Linux).
/// Retorna 0 se não conseguir ler (fallback conservador no chamador).
static size_t available_ram_kb() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("MemAvailable:")) {
            std::string_view sv(line);
            sv.remove_prefix(13);
            while (!sv.empty() && sv.front() == ' ') {
                sv.remove_prefix(1);
            }
            size_t val = 0;
            std::from_chars(sv.data(), sv.data() + sv.size(), val);
            return val;
        }
    }
    return 0;
}

/// Barra de progresso ANSI para stderr — thread-safe apenas para o chamador;
/// exibe somente quando stderr é um terminal (isatty).
// (ProgressBar removido — usando ggql::cli::step_progress)

} // namespace

// gfa_path_index.cpp — Construção paralela gBWT e persistência (save/load).
// ════════════════════════════════════════════════════════════════════════════
// build_path_index_parallel
// ════════════════════════════════════════════════════════════════════════════
GfaCsrTopology::GbwtTimings GfaCsrTopology::build_path_index_parallel(std::vector<GbwtGroup> groups,
                                                                      uint8_t node_width,
                                                                      int num_threads) {
    using clk = std::chrono::steady_clock;
    using sec = std::chrono::duration<double>;

    std::erase_if(groups, [](const GbwtGroup& g) { return g.path_names.empty(); });

    // BUG-TOPO-5: ordena grupos por sigma desc para que o modelo de custo use
    // os grupos reais em ordem de tamanho (maior custo primeiro).
    std::sort(groups.begin(), groups.end(), [](const GbwtGroup& a, const GbwtGroup& b) {
        return a.sigma > b.sigma;
    });

    if (groups.empty()) {
        gbwts_.clear();
        gbwt_offsets_.clear();
        path_names_.clear();
        return {};
    }

    if (num_nodes_ > (size_t(1) << 30)) {
        throw std::overflow_error(
            "build_path_index_parallel: num_nodes=" + std::to_string(num_nodes_) +
            " excede o limite de GBWT_SAVE_MEMORY (2^30 nós).");
    }

    // ── Modelo de custo: GBWTBuilder por job (adaptado do gbwtgraph::gfa_to_gbwt) ─
    //
    // Para GBWTBuilder (double-buffer), o custo de RAM por job é determinístico:
    //   custo_job = 2 × batch_size × node_bytes  (input_buffer + construction_buffer)
    //
    // VG usa a fórmula de guess_parallel_gbwt_jobs (index_registry.cpp):
    //   bytes_per_node = 135 × log10(haplotype_count + 1)
    //   job_size = node_count / 10  (maior componente = ~10% do genoma)
    //   bytes_per_job = batch_size × 8  (2 buffers × 4 bytes)
    //   → adiciona jobs enquanto memory_usage <= available_memory
    //
    // Nossa versão: usa os tamanhos reais dos grupos (after repartition).

    const size_t total_paths_count = [&]() {
        size_t n = 0;
        for (const auto& g : groups) {
            n += g.path_names.size();
        }
        return std::max(n, size_t(1));
    }();
    const size_t desired_N_for_cost = static_cast<size_t>(std::max(1, num_threads));
    const size_t avail_bytes = available_ram_kb() * 1024ull;
    const size_t budget = (avail_bytes > 0) ? (avail_bytes * 75ull / 100ull) : (size_t(4) << 30);

    // Batch size para GBWTBuilder — mesmo que VG: INSERT_BATCH_SIZE como padrão.
    // O batch determina o custo de RAM de cada GBWTBuilder.
    const size_t node_bytes_cost = ((node_width + 7u) / 8u);
    const size_t default_batch = static_cast<size_t>(gbwt::DynamicGBWT::INSERT_BATCH_SIZE);
    // Custo de RAM de 1 GBWTBuilder = 2 × batch_size × node_bytes (dois buffers)
    const size_t cost_per_builder = 2 * default_batch * std::max(node_bytes_cost, size_t(1));

    // Custo estimado do grupo mais pesado: usa fórmula do VG (bytes_per_node × job_size)
    // onde bytes_per_node = 135 × log10(paths + 1) e job_size = sigma do grupo.
    // Isso estima o tamanho do GBWT comprimido parcial que fica em RAM até o merge.
    size_t max_gbwt_partial = 0;
    for (const auto& g : groups) {
        const size_t paths_j = std::max(g.path_names.size(), size_t(1));
        const double bytes_per_node =
            135.0 * std::max(std::log10(static_cast<double>(paths_j) + 1.0), 1.0);
        const size_t sigma_j = (g.sigma > 0) ? g.sigma : num_nodes_;
        const size_t gbwt_partial_j =
            static_cast<size_t>(bytes_per_node) * sigma_j + cost_per_builder;
        max_gbwt_partial = std::max(max_gbwt_partial, gbwt_partial_j);
    }
    if (max_gbwt_partial == 0) {
        // Fallback: 1 componente grande
        const double bytes_per_node =
            135.0 * std::max(std::log10(static_cast<double>(total_paths_count) + 1.0), 1.0);
        max_gbwt_partial = static_cast<size_t>(bytes_per_node) * num_nodes_ + cost_per_builder;
    }
    const size_t cost_one_dynGBWT = max_gbwt_partial; // nome mantido para compatibilidade com logs

    // Número de jobs paralelos: adiciona jobs enquanto cabe no budget.
    // BUG-TOPO-5: usa custo real de cada grupo (sigma + path_count) em vez da
    // heurística geométrica 0.95^n, que subestimava componentes de tamanhos similares.
    // Grupos já estão ordenados por sigma desc → acumula do maior para o menor.
    size_t insert_limit_real = 1;
    {
        size_t mem = max_gbwt_partial; // 1 job (grupo [0], o maior)
        while (insert_limit_real < desired_N_for_cost && insert_limit_real < groups.size()) {
            const auto& gj = groups[insert_limit_real];
            const size_t paths_j = std::max(gj.path_names.size(), size_t(1));
            const double bytes_per_node_j =
                135.0 * std::max(std::log10(static_cast<double>(paths_j) + 1.0), 1.0);
            const size_t sigma_j = (gj.sigma > 0) ? gj.sigma : num_nodes_;
            const size_t next_job_cost =
                static_cast<size_t>(bytes_per_node_j) * sigma_j + cost_per_builder;
            if (mem + next_job_cost > budget) {
                break;
            }
            mem += next_job_cost;
            ++insert_limit_real;
        }
    }

    // Diagnóstico: mostra sigma médio dos grupos
    {
        uint64_t sigma_sum = 0;
        size_t sigma_cnt = 0;
        for (const auto& g : groups) {
            if (g.sigma > 0) {
                sigma_sum += g.sigma;
                ++sigma_cnt;
            }
        }
        const double sigma_avg =
            (sigma_cnt > 0) ? static_cast<double>(sigma_sum) / static_cast<double>(sigma_cnt)
                            : static_cast<double>(num_nodes_);
        cli::step_info(fmt::format(
            "cost model: sigma_avg={:.1f}M (vs num_nodes={:.1f}M), speedup potencial={:.1f}x",
            sigma_avg / 1e6, static_cast<double>(num_nodes_) / 1e6,
            static_cast<double>(num_nodes_) / (sigma_avg > 0 ? sigma_avg : 1.0)));
    }

    // ── Fallback GBWTBuilder: pipeline streaming de double-buffer ────────────
    //
    // Usa modo serial quando:
    //   (a) 1 DynamicGBWT já não cabe no budget, OU
    //   (b) insert_limit seria = 1 (paralelo não traz ganho — e acumula N GBWTs
    //       comprimidos simultaneamente em RAM, o que pode causar OOM)
    //
    // No modo serial, um único GBWTBuilder processa todos os grupos em série e
    // produz 1 GBWT final — pico: 1 DynamicGBWT + 2 × batch_buffer.
    //
    // Modo paralelo só é usado quando insert_limit >= 2, i.e., quando pelo menos
    // 2 DynamicGBWTs cabem ao mesmo tempo na RAM — o que garante speedup real.
    if (cost_one_dynGBWT >= budget || insert_limit_real <= 1) {
        cli::step_info(fmt::format(
            "modo GBWTBuilder serial · cost/grupo {:.1f} GB · budget {:.1f} GB · insert_limit {}",
            cost_one_dynGBWT / 1e9, budget / 1e9, insert_limit_real));
        // Batch: igual ao VG — fixo em INSERT_BATCH_SIZE (100M símbolos).
        // NÃO inflar: buffers de 2×batch×node_bytes ficam enormes (ex: 8 GB
        // com budget=42 GB) e somam ao DynamicGBWT em crescimento → OOM.
        const size_t batch_syms = static_cast<size_t>(gbwt::DynamicGBWT::INSERT_BATCH_SIZE);

        GbwtTimings t;
        MemoryMonitor monitor;
        auto wall_start = clk::now();
        uint64_t total_nodes_acc = 0;

        // BUG-TOPO-3: tracker inline substituído por ggql::ProgressTracker.
        const size_t step = std::max(size_t(1), groups.size() / 100);
        ggql::ProgressTracker tracker_serial(groups.size(), "gBWT serial", true, "grupos/s", 1.0);
        gbwt::GBWTBuilder builder(node_width, static_cast<gbwt::size_type>(batch_syms));
        for (size_t i = 0; i < groups.size(); ++i) {
            auto& g = groups[i];
            if (!g.in_ram_seqs.empty()) {
                for (const auto& seq : g.in_ram_seqs) {
                    builder.insert(seq, /*both_orientations=*/false);
                    total_nodes_acc += seq.size();
                }
            } else if (!g.tmp_path.empty()) {
                gbwt::text_buffer_type txt(g.tmp_path, std::ios::in, 64u << 20, node_width);
                total_nodes_acc += txt.size();
                gbwt::vector_type seq;
                seq.reserve(1u << 20);
                for (gbwt::size_type val : txt) {
                    if (val == gbwt::ENDMARKER) {
                        if (!seq.empty()) {
                            builder.insert(seq, /*both_orientations=*/false);
                            seq.clear();
                        }
                    } else {
                        seq.push_back(val);
                    }
                }
                if (!seq.empty()) {
                    builder.insert(seq, /*both_orientations=*/false);
                }
                ::unlink(g.tmp_path.c_str());
            }
            for (auto& name : g.path_names) {
                path_names_.push_back(std::move(name));
            }

            if (i % step == 0 || i == groups.size() - 1) {
                tracker_serial.add_progress(step);
            }
        }
        tracker_serial.finish();
        builder.finish();

        gbwts_.clear();
        gbwts_.push_back(std::make_unique<gbwt::GBWT>(builder.index));
        builder.index = gbwt::DynamicGBWT(); // libera antes de rebuild
        rebuild_offsets_();

        t.wall_s = sec(clk::now() - wall_start).count();
        t.cpu_avg_s = t.wall_s;
        t.cpu_max_s = t.wall_s;
        monitor.stop();
        t.total_nodes = total_nodes_acc;
        t.peak_rss_kb = monitor.peak_kb();
        t.index_size_b = gbwts_[0] ? sdsl::size_in_bytes(*gbwts_[0]) : 0;
        return t;
    }

    // ── Modo paralelo: GBWTBuilder por job via OMP (igual ao gbwtgraph::gfa_to_gbwt) ─
    //
    // Algoritmo exato do gbwtgraph/src/gfa.cpp → parse_paths():
    //   1. partial_indexes[jobs.size()] pré-alocado
    //   2. #pragma omp parallel for schedule(dynamic, 1) com parallel_jobs threads
    //   3. Cada job: GBWTBuilder(node_width, batch_size) local → insere paths → finish()
    //      → partial_indexes[i] = GBWT(builder.index) → builder.index = DynamicGBWT()
    //   4. Merge único: GBWT(partial_indexes) — vetor de GBWTs já comprimidos
    //
    // Diferença crítica vs DynamicGBWT:
    //   GBWTBuilder usa double-buffer (input + construction, cada um de batch_size símbolos)
    //   → pico de RAM por job = 2 × batch_size × node_bytes (muito menor que DynamicGBWT completo)
    //   DynamicGBWT guarda o índice incremental inteiro em RAM durante inserção

    const size_t parallel_jobs = insert_limit_real; // = número de threads OMP
    const size_t N = groups.size();

    // Batch_size: igual ao VG/gbwtgraph — fixo em INSERT_BATCH_SIZE (100M símbolos).
    // Não inflamos o batch porque: (a) o gargalo é o DynamicGBWT, não os buffers,
    // e (b) VG usa INSERT_BATCH_SIZE × 8 bytes no formula de custo assumindo 4B/símbolo.
    // Inflar o batch consumiria RAM extra sem ganho de performance.
    const size_t batch_size = static_cast<size_t>(gbwt::DynamicGBWT::INSERT_BATCH_SIZE);

    cli::step_info(fmt::format(
        "modo paralelo GBWTBuilder · jobs={} · batch={}M · budget={:.1f} GB · grupos={}",
        parallel_jobs, batch_size / 1000000, budget / 1e9, N));

    // partial_indexes[i]: resultado comprimido do job i (após finish + GBWT(builder.index))
    std::vector<gbwt::GBWT> partial_indexes(N);

    size_t est_total_paths = 0;
    for (const auto& g : groups) {
        est_total_paths += g.path_names.size();
    }

    // BUG-TOPO-3: GbwTracker substituído por ggql::ProgressTracker generalizado.
    ggql::ProgressTracker tracker_gbwt(est_total_paths, "gBWT", true, "paths/s", 1.0);

    GbwtTimings t;
    MemoryMonitor monitor;
    auto wall_start = clk::now();

    // Define número de threads OMP — igual ao gbwtgraph
    const int omp_threads = static_cast<int>(std::max(parallel_jobs, size_t(1)));
    // BUG-TOPO-2: omp_set_num_threads define estado global — pragma já usa num_threads(omp_threads).

    // current_paths[thread_num]: buffer de sequência temporal por thread
    std::vector<gbwt::vector_type> current_paths(static_cast<size_t>(omp_threads));
    std::atomic<uint64_t> total_nodes_acc{0};

// ── Loop paralelo: maior job primeiro (grupos já ordenados pelo repartition) ──
#pragma omp parallel for schedule(dynamic, 1) num_threads(omp_threads)
    for (size_t i = 0; i < N; ++i) {
        const int tid = omp_get_thread_num();
        gbwt::vector_type& cur_path = current_paths[static_cast<size_t>(tid)];

        // GBWTBuilder local à thread — double-buffer: não mantém índice completo em RAM
        gbwt::GBWTBuilder builder(node_width, static_cast<gbwt::size_type>(batch_size));

        const auto& g = groups[i];
        size_t syms = 0;

        if (!g.in_ram_seqs.empty()) {
            size_t local_paths = 0;
            for (const auto& seq : g.in_ram_seqs) {
                builder.insert(seq, /*both_orientations=*/false);
                syms += seq.size();
                if (++local_paths >= 1000) {
                    tracker_gbwt.add_progress(local_paths);
                    local_paths = 0;
                }
            }
            if (local_paths > 0) {
                tracker_gbwt.add_progress(local_paths);
            }
        } else if (!g.tmp_path.empty()) {
            gbwt::text_buffer_type txt(g.tmp_path, std::ios::in, 64u << 20, node_width);
            cur_path.clear();
            size_t local_paths = 0;
            for (gbwt::size_type val : txt) {
                if (val == gbwt::ENDMARKER) {
                    if (!cur_path.empty()) {
                        builder.insert(cur_path, /*both_orientations=*/false);
                        syms += cur_path.size();
                        cur_path.clear();
                        if (++local_paths >= 1000) {
                            tracker_gbwt.add_progress(local_paths);
                            local_paths = 0;
                        }
                    }
                } else {
                    cur_path.push_back(val);
                }
            }
            if (!cur_path.empty()) {
                builder.insert(cur_path, /*both_orientations=*/false);
                syms += cur_path.size();
                cur_path.clear();
                if (++local_paths >= 1000) {
                    tracker_gbwt.add_progress(local_paths);
                    local_paths = 0;
                }
            }
            if (local_paths > 0) {
                tracker_gbwt.add_progress(local_paths);
            }
            ::unlink(g.tmp_path.c_str());
        }

        builder.finish();
        // Comprime para GBWT estático e libera o DynamicGBWT imediatamente (igual ao gbwtgraph)
        partial_indexes[i] = gbwt::GBWT(builder.index);
        builder.index = gbwt::DynamicGBWT(); // libera RAM do builder

        total_nodes_acc.fetch_add(syms, std::memory_order_relaxed);
    }
    tracker_gbwt.finish();

    // ── Coleta nomes de paths (em ordem de job) ─────────────────────────────
    path_names_.clear();
    for (size_t i = 0; i < N; ++i) {
        for (auto& name : groups[i].path_names) {
            path_names_.push_back(std::move(name));
        }
    }

    // ── Merge único: GBWT(partial_indexes) — exato do gbwtgraph ─────────────
    // Recebe vetor de GBWTs já comprimidos → merge eficiente (ordem determinística)
    cli::step_info(fmt::format("merging {} partial indexes", N));
    gbwts_.clear();
    gbwts_.push_back(std::make_unique<gbwt::GBWT>(partial_indexes));
    // Libera os parciais imediatamente
    partial_indexes.clear();
    partial_indexes.shrink_to_fit();
    rebuild_offsets_();

    t.wall_s = sec(clk::now() - wall_start).count();
    monitor.stop();
    t.total_nodes = total_nodes_acc.load();
    t.peak_rss_kb = monitor.peak_kb();
    t.cpu_avg_s = t.wall_s; // OMP: difícil medir por thread sem overhead
    t.cpu_max_s = t.wall_s;
    t.index_size_b = gbwts_[0] ? sdsl::size_in_bytes(*gbwts_[0]) : 0;

    return t;
}


// ════════════════════════════════════════════════════════════════════════════
// save — serializa todos os componentes para disco.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::save(const std::string& dir) const {
    namespace fs = std::filesystem;
    fs::create_directories(dir);

    // Helper: salva vetor uint32_t com verificação de erros (FIX BUG 1)
    auto save_vector = [&](const std::vector<uint32_t>& v, const std::string& name) {
        std::ofstream out(dir + "/" + name, std::ios::binary);
        if (!out) {
            throw std::runtime_error("[topology::save] Falha ao abrir " + name);
        }
        const uint64_t sz = static_cast<uint64_t>(v.size());
        out.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
        if (sz > 0) {
            out.write(reinterpret_cast<const char*>(v.data()),
                      static_cast<std::streamsize>(sz * sizeof(uint32_t)));
        }

        const uint64_t hash = ChecksumBuilder().add(sz).add(v).finalize();
        out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));

        if (!out) {
            throw std::runtime_error("[topology::save] Erro de I/O ao escrever " + name);
        }
    };

    save_vector(E_fwd_, "E_fwd.bin");
    save_vector(E_bwd_, "E_bwd.bin");

    sdsl::store_to_file(ef_fwd_, dir + "/ef_fwd.sdsl");
    sdsl::store_to_file(ef_bwd_, dir + "/ef_bwd.sdsl");

    // Salva gBWTs — conta apenas não-nulos (FIX BUG 6)
    {
        std::ofstream out(dir + "/gbwts.bin", std::ios::binary);
        if (!out) {
            throw std::runtime_error("[topology::save] Falha ao abrir gbwts.bin");
        }
        // Conta apenas ponteiros não-nulos para evitar desserialização incorreta no load
        uint64_t n_valid = 0;
        for (const auto& g : gbwts_) {
            if (g) {
                ++n_valid;
            }
        }
        out.write(reinterpret_cast<const char*>(&n_valid), sizeof(n_valid));
        for (const auto& g : gbwts_) {
            if (g) {
                g->simple_sds_serialize(out);
            }
        }
        if (!out) {
            throw std::runtime_error("[topology::save] Erro de I/O ao escrever gbwts.bin");
        }
    }

    // Salva metadados com uint64_t explícito por elemento (FIX PERF 5: portabilidade 32/64-bit)
    {
        std::ofstream out(dir + "/topology.bin", std::ios::binary);
        if (!out) {
            throw std::runtime_error("[topology::save] Falha ao abrir topology.bin");
        }
        out.write(reinterpret_cast<const char*>(&num_nodes_), sizeof(num_nodes_));

        ChecksumBuilder cb;
        cb.add(num_nodes_);

        const uint64_t n_off = static_cast<uint64_t>(gbwt_offsets_.size());
        out.write(reinterpret_cast<const char*>(&n_off), sizeof(n_off));
        cb.add(n_off);

        for (const size_t off : gbwt_offsets_) {
            const uint64_t v = static_cast<uint64_t>(off);
            out.write(reinterpret_cast<const char*>(&v), sizeof(v));
            cb.add(v);
        }

        const uint64_t n_paths = static_cast<uint64_t>(path_names_.size());
        out.write(reinterpret_cast<const char*>(&n_paths), sizeof(n_paths));
        cb.add(n_paths);

        for (const auto& s : path_names_) {
            const uint64_t slen = static_cast<uint64_t>(s.size());
            out.write(reinterpret_cast<const char*>(&slen), sizeof(slen));
            out.write(s.data(), static_cast<std::streamsize>(slen));
            cb.add(s);
        }

        const uint64_t hash = cb.finalize();
        out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));

        if (!out) {
            throw std::runtime_error("[topology::save] Erro de I/O ao escrever topology.bin");
        }
    }
    // Salva cache de estatísticas de path (node_count + bp_length por path)
    if (has_path_stats()) {
        std::ofstream out(dir + "/path_stats.bin", std::ios::binary);
        if (!out) {
            throw std::runtime_error("[topology::save] Falha ao abrir path_stats.bin");
        }
        const uint64_t n = static_cast<uint64_t>(path_node_counts_.size());
        out.write(reinterpret_cast<const char*>(&n), sizeof(n));
        out.write(reinterpret_cast<const char*>(path_node_counts_.data()),
                  static_cast<std::streamsize>(n * sizeof(uint32_t)));
        out.write(reinterpret_cast<const char*>(path_bp_lengths_.data()),
                  static_cast<std::streamsize>(n * sizeof(int64_t)));
        if (!out) {
            throw std::runtime_error("[topology::save] Erro de I/O ao escrever path_stats.bin");
        }
    }

    cli::ok("índice salvo", dir);
}

// ════════════════════════════════════════════════════════════════════════════
// load — restaura todos os componentes do disco.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::load(const std::string& dir) {
    // Helper: carrega vetor uint32_t com verificação completa de integridade (XXH64).
    auto load_vector_impl = [&](std::vector<uint32_t>& v, const std::string& name) {
        std::ifstream in(dir + "/" + name, std::ios::binary);
        if (!in) {
            throw std::runtime_error("[topology::load] Arquivo ausente: " + name);
        }
        uint64_t sz = 0;
        in.read(reinterpret_cast<char*>(&sz), sizeof(sz));
        if (static_cast<std::streamsize>(in.gcount()) != static_cast<std::streamsize>(sizeof(sz))) {
            throw std::runtime_error("[topology::load] Header truncado em " + name);
        }
        if (sz > 4'000'000'000ULL) {
            throw std::runtime_error("[topology::load] Tamanho suspeito em " + name + " (" +
                                     std::to_string(sz) + ") — cache corrompido?");
        }
        v.resize(static_cast<size_t>(sz));
        if (sz > 0) {
            in.read(reinterpret_cast<char*>(v.data()),
                    static_cast<std::streamsize>(sz * sizeof(uint32_t)));
            if (static_cast<uint64_t>(in.gcount()) != sz * sizeof(uint32_t)) {
                throw std::runtime_error("[topology::load] Dados truncados em " + name);
            }
        }
        uint64_t stored_hash = 0;
        in.read(reinterpret_cast<char*>(&stored_hash), sizeof(stored_hash));
        if (static_cast<std::streamsize>(in.gcount()) !=
            static_cast<std::streamsize>(sizeof(stored_hash))) {
            throw std::runtime_error("[topology::load] Hash ausente/truncado em " + name);
        }
        const uint64_t actual_hash = ChecksumBuilder().add(sz).add(v).finalize();
        if (stored_hash != actual_hash) {
            throw std::runtime_error("[topology::load] Hash divergente em " + name +
                                     " (arquivo corrompido!)");
        }
    };

    // ── Carregamento paralelo dos arquivos independentes (PERF 3) ─────────────
    auto f_fwd = std::async(std::launch::async, [&]() { load_vector_impl(E_fwd_, "E_fwd.bin"); });
    auto f_bwd = std::async(std::launch::async, [&]() { load_vector_impl(E_bwd_, "E_bwd.bin"); });

    auto f_meta = std::async(std::launch::async, [&]() {
        std::ifstream in(dir + "/topology.bin", std::ios::binary);
        if (!in) {
            throw std::runtime_error("[topology::load] topology.bin ausente");
        }

        in.read(reinterpret_cast<char*>(&num_nodes_), sizeof(num_nodes_));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(num_nodes_))) {
            throw std::runtime_error("[topology::load] topology.bin truncado (num_nodes)");
        }

        ChecksumBuilder cb;
        cb.add(num_nodes_);

        uint64_t n_off = 0;
        in.read(reinterpret_cast<char*>(&n_off), sizeof(n_off));
        cb.add(n_off);

        gbwt_offsets_.resize(static_cast<size_t>(n_off));
        for (uint64_t i = 0; i < n_off; ++i) {
            uint64_t v = 0;
            in.read(reinterpret_cast<char*>(&v), sizeof(v));
            gbwt_offsets_[i] = static_cast<size_t>(v);
            cb.add(v);
        }

        uint64_t n_paths = 0;
        in.read(reinterpret_cast<char*>(&n_paths), sizeof(n_paths));
        cb.add(n_paths);

        path_names_.resize(static_cast<size_t>(n_paths));
        for (uint64_t i = 0; i < n_paths; ++i) {
            uint64_t slen = 0;
            in.read(reinterpret_cast<char*>(&slen), sizeof(slen));
            path_names_[i].resize(static_cast<size_t>(slen));
            if (slen > 0) {
                in.read(path_names_[i].data(), static_cast<std::streamsize>(slen));
            }
            cb.add(path_names_[i]);
        }

        uint64_t stored_hash = 0;
        in.read(reinterpret_cast<char*>(&stored_hash), sizeof(stored_hash));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(stored_hash))) {
            throw std::runtime_error("[topology::load] topology.bin truncado (hash)");
        }
        if (stored_hash != cb.finalize()) {
            throw std::runtime_error(
                "[topology::load] Hash divergente em topology.bin (arquivo corrompido!)");
        }
    });

    // Aguarda as 3 leituras paralelas — exceções propagam via .get()
    f_fwd.get();
    f_bwd.get();
    f_meta.get();

    // ── SDSL: ef_fwd e ef_bwd também podem ser carregados em paralelo ─────────
    auto f_ef_fwd = std::async(std::launch::async,
                               [&]() { sdsl::load_from_file(ef_fwd_, dir + "/ef_fwd.sdsl"); });
    auto f_ef_bwd = std::async(std::launch::async,
                               [&]() { sdsl::load_from_file(ef_bwd_, dir + "/ef_bwd.sdsl"); });
    f_ef_fwd.get();
    f_ef_bwd.get();

    // IMPORTANTE: Re-vincular os suportes SDSL após carregamento (crítico para consultas)
    sdsl::util::init_support(ef_fwd_sel_, &ef_fwd_);
    sdsl::util::init_support(ef_bwd_sel_, &ef_bwd_);

    // ── gBWTs: um único arquivo serializado sequencialmente (FIX BUG 6) ────────
    {
        std::ifstream in(dir + "/gbwts.bin", std::ios::binary);
        if (in) {
            uint64_t n = 0;
            in.read(reinterpret_cast<char*>(&n), sizeof(n));
            if (static_cast<std::streamsize>(in.gcount()) !=
                static_cast<std::streamsize>(sizeof(n))) {
                throw std::runtime_error("[topology::load] gbwts.bin truncado no count");
            }
            gbwts_.clear();
            gbwts_.reserve(static_cast<size_t>(n));
            for (uint64_t i = 0; i < n; ++i) {
                auto g = std::make_unique<gbwt::GBWT>();
                g->simple_sds_load(in);
                if (!in && !in.eof()) {
                    throw std::runtime_error("[topology::load] Falha ao desserializar gbwt[" +
                                             std::to_string(i) + "]");
                }
                gbwts_.push_back(std::move(g));
            }
        }
    }

    // Invalida caches de offsets (reconstruídos lazily na próxima consulta)
    offsets_cache_fwd_.clear();
    offsets_cache_bwd_.clear();

    // Carrega estatísticas de path (opcional — ausente em índices antigos, fallback gracioso)
    {
        std::ifstream in(dir + "/path_stats.bin", std::ios::binary);
        if (in) {
            uint64_t n = 0;
            in.read(reinterpret_cast<char*>(&n), sizeof(n));
            if (in && n == static_cast<uint64_t>(path_names_.size())) {
                path_node_counts_.resize(static_cast<size_t>(n));
                path_bp_lengths_.resize(static_cast<size_t>(n));
                in.read(reinterpret_cast<char*>(path_node_counts_.data()),
                        static_cast<std::streamsize>(n * sizeof(uint32_t)));
                in.read(reinterpret_cast<char*>(path_bp_lengths_.data()),
                        static_cast<std::streamsize>(n * sizeof(int64_t)));
                if (!in) {
                    path_node_counts_.clear();
                    path_bp_lengths_.clear();
                }
            }
        }
    }

    cli::step_ok("índice carregado", dir);
}

} // namespace ggql
