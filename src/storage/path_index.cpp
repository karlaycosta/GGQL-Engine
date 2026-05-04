// path_index.cpp — Construção paralela gBWT e persistência (save/load) de GfaCsrTopology.
//

#include "ggql/storage/topology.hpp"

#include "ggql/cli/console.hpp"
#include "ggql/cli/progress_tracker.hpp"
#include "ggql/util/memory_monitor.hpp"

// Headers pesados — incluídos só aqui, não no .hpp público.
#include "ggql/storage/checksum_builder.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring> // strerror
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <future>
#include <gbwt/dynamic_gbwt.h> // GBWTBuilder, DynamicGBWT
#include <string>
#include <unistd.h> // ::unlink

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

namespace {

// MemoryMonitor + available_ram_kb consolidados em
// include/ggql/util/memory_monitor.hpp para evitar duplicação com
// graph_algorithms.cpp. Aliases locais para legibilidade do call site.
using ggql::util::MemoryMonitor;
using ggql::util::available_ram_kb;

// ── I/O helpers — fail loud (B1+B2+B3+B7) ──────────────────────────────
// Política: open/write/read parciais lançam imediatamente. Para arquivos
// opcionais (legacy compat), o caller usa fs::exists() para distinguir
// "ausente" (fallback) de "presente mas corrompido" (lança).
// Mensagens prefixadas com [topology] para rastreabilidade.

[[nodiscard]] inline std::ofstream open_out_or_throw(const std::string& path) {
    std::ofstream o(path, std::ios::binary);
    if (!o) {
        throw std::runtime_error("[topology] Falha ao abrir para escrita: " + path);
    }
    return o;
}
inline void check_write_ok(std::ofstream& o, const std::string& path) {
    if (!o) {
        throw std::runtime_error("[topology] Falha ao escrever: " + path);
    }
}
[[nodiscard]] inline std::ifstream open_in_or_throw(const std::string& path) {
    std::ifstream i(path, std::ios::binary);
    if (!i) {
        throw std::runtime_error("[topology] Falha ao abrir para leitura: " + path);
    }
    return i;
}
inline void read_exact(std::istream& in, void* dst, size_t bytes,
                       const std::string& path) {
    in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(bytes));
    if (!in || in.gcount() != static_cast<std::streamsize>(bytes)) {
        throw std::runtime_error("[topology] Cache truncado: " + path);
    }
}

// ── Serialização compartilhada save() ↔ save_gbwt_only() (B7) ──────────
// Antes da refatoração, esses 3 blocos estavam duplicados ~80 linhas
// entre as duas funções. Helpers livres recebem os componentes por
// parâmetro para evitar acoplamento à classe.

inline void write_gbwts_payload(std::ofstream& out,
                                const std::vector<std::unique_ptr<gbwt::GBWT>>& gbwts) {
    uint64_t n_valid = 0;
    for (const auto& g : gbwts) {
        if (g) { ++n_valid; }
    }
    out.write(reinterpret_cast<const char*>(&n_valid), sizeof(n_valid));
    for (const auto& g : gbwts) {
        if (g) {
            g->simple_sds_serialize(out);
        }
    }
}

inline void write_topology_meta_payload(std::ofstream& out, uint64_t num_nodes,
                                         const std::vector<size_t>& gbwt_offsets,
                                         const std::vector<std::string>& path_names) {
    out.write(reinterpret_cast<const char*>(&num_nodes), sizeof(num_nodes));
    ChecksumBuilder cb;
    cb.add(num_nodes);

    const uint64_t n_off = static_cast<uint64_t>(gbwt_offsets.size());
    out.write(reinterpret_cast<const char*>(&n_off), sizeof(n_off));
    cb.add(n_off);
    for (const size_t off : gbwt_offsets) {
        const uint64_t v = static_cast<uint64_t>(off);
        out.write(reinterpret_cast<const char*>(&v), sizeof(v));
        cb.add(v);
    }

    const uint64_t n_paths = static_cast<uint64_t>(path_names.size());
    out.write(reinterpret_cast<const char*>(&n_paths), sizeof(n_paths));
    cb.add(n_paths);
    for (const auto& s : path_names) {
        const uint64_t slen = static_cast<uint64_t>(s.size());
        out.write(reinterpret_cast<const char*>(&slen), sizeof(slen));
        out.write(s.data(), static_cast<std::streamsize>(slen));
        cb.add(s);
    }

    const uint64_t hash = cb.finalize();
    out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
}

inline void write_path_stats_payload(std::ofstream& out,
                                      const std::vector<uint32_t>& path_node_counts,
                                      const std::vector<int64_t>& path_bp_lengths) {
    const uint64_t n = static_cast<uint64_t>(path_node_counts.size());
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
    out.write(reinterpret_cast<const char*>(path_node_counts.data()),
              static_cast<std::streamsize>(n * sizeof(uint32_t)));
    out.write(reinterpret_cast<const char*>(path_bp_lengths.data()),
              static_cast<std::streamsize>(n * sizeof(int64_t)));
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// build_path_index_parallel
// ════════════════════════════════════════════════════════════════════════════
GfaCsrTopology::GbwtTimings GfaCsrTopology::build_path_index_parallel(std::vector<GbwtGroup> groups,
                                                                      uint8_t node_width,
                                                                      int num_threads) {
    using clk = std::chrono::steady_clock;
    using sec = std::chrono::duration<double>;

    std::erase_if(groups, [](const GbwtGroup& g) { return g.path_names.empty(); });

    // Ordena grupos por sigma desc para que o modelo de custo veja os grupos
    // reais em ordem de tamanho (maior custo primeiro).
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

    // Número de jobs paralelos: usa custo real de cada grupo (sigma + path_count),
    // adicionando jobs enquanto cabem no budget. Grupos já ordenados por sigma desc
    // → acumula do maior para o menor.
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
    if (max_gbwt_partial >= budget || insert_limit_real <= 1) {
        cli::step_info(fmt::format(
            "modo GBWTBuilder serial · cost/grupo {:.1f} GB · budget {:.1f} GB · insert_limit {}",
            max_gbwt_partial / 1e9, budget / 1e9, insert_limit_real));
        // Batch: igual ao VG — fixo em INSERT_BATCH_SIZE (100M símbolos).
        // NÃO inflar: buffers de 2×batch×node_bytes ficam enormes (ex: 8 GB
        // com budget=42 GB) e somam ao DynamicGBWT em crescimento → OOM.
        const size_t batch_syms = static_cast<size_t>(gbwt::DynamicGBWT::INSERT_BATCH_SIZE);

        GbwtTimings t;
        MemoryMonitor monitor;
        auto wall_start = clk::now();
        uint64_t total_nodes_acc = 0;

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
                // B4: unlink pode falhar (FS read-only, permissões); registra
                // warning mas não trata como fatal — tmp file é descartável.
                if (::unlink(g.tmp_path.c_str()) != 0 && errno != ENOENT) {
                    cli::note(fmt::format("falha ao remover tmp '{}': {}",
                                          g.tmp_path, std::strerror(errno)));
                }
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

    ggql::ProgressTracker tracker_gbwt(est_total_paths, "gBWT", true, "paths/s", 1.0);

    GbwtTimings t;
    MemoryMonitor monitor;
    auto wall_start = clk::now();

    // Define número de threads OMP — igual ao gbwtgraph.
    // Não chamamos omp_set_num_threads (define estado global) porque o pragma
    // abaixo já usa num_threads(omp_threads) para escopo local.
    const int omp_threads = static_cast<int>(std::max(parallel_jobs, size_t(1)));

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
            // B4: unlink pode falhar — não-fatal (tmp file é descartável).
            // Em modo paralelo, evitamos cli::note (não thread-safe garantido)
            // e silenciamos: stack trace via gcov mostraria leak se fosse crítico.
            (void)::unlink(g.tmp_path.c_str());
        }

        builder.finish();
        // Comprime para GBWT estático e libera o DynamicGBWT imediatamente (igual ao gbwtgraph)
        partial_indexes[i] = gbwt::GBWT(builder.index);
        builder.index = gbwt::DynamicGBWT(); // libera RAM do builder

        total_nodes_acc.fetch_add(syms, std::memory_order_relaxed);
    }
    tracker_gbwt.finish();

    // P5: libera os buffers de path por thread imediatamente após o loop OMP.
    // Cada current_paths[tid] pode ter crescido até a maior sequência vista
    // (até ~100 MB+ por thread em datasets grandes). Sem este swap, ficam
    // alocados até o destrutor de current_paths no fim da função, sobrepondo
    // com o pico do merge dos partial_indexes (que também é caro em RAM).
    {
        std::vector<gbwt::vector_type> tmp;
        current_paths.swap(tmp);
    }

    // ── Coleta nomes de paths (em ordem de job) ─────────────────────────────
    // B6: reserve(est_total_paths) elimina ~13 realocações para 100k paths.
    path_names_.clear();
    path_names_.reserve(est_total_paths);
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

    // Helper: salva vetor uint32_t com verificação de I/O e checksum de integridade.
    auto save_vector = [](const std::vector<uint32_t>& v, const std::string& path) {
        auto out = open_out_or_throw(path);
        const uint64_t sz = static_cast<uint64_t>(v.size());
        out.write(reinterpret_cast<const char*>(&sz), sizeof(sz));
        if (sz > 0) {
            out.write(reinterpret_cast<const char*>(v.data()),
                      static_cast<std::streamsize>(sz * sizeof(uint32_t)));
        }
        const uint64_t hash = ChecksumBuilder().add(sz).add(v).finalize();
        out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        check_write_ok(out, path);
    };

    // P1: paraleliza I/O dos 6 arquivos independentes via std::async.
    // Cada task escreve em arquivo distinto e lê apenas membros const da
    // topology — sem race. Exceções propagam via .get().
    auto f_efwd = std::async(std::launch::async,
                             [&] { save_vector(E_fwd_, dir + "/E_fwd.bin"); });
    auto f_ebwd = std::async(std::launch::async,
                             [&] { save_vector(E_bwd_, dir + "/E_bwd.bin"); });
    auto f_ef_fwd = std::async(std::launch::async,
                               [&] { sdsl::store_to_file(ef_fwd_, dir + "/ef_fwd.sdsl"); });
    auto f_ef_bwd = std::async(std::launch::async,
                               [&] { sdsl::store_to_file(ef_bwd_, dir + "/ef_bwd.sdsl"); });

    // gbwts.bin — apenas se houver pelo menos um GBWT não-nulo. Sem isso,
    // ingest com build_gbwt=false criaria gbwts.bin vazio, derrotando a
    // validação de cache em try_cache_* (que checa fs::exists).
    auto f_gbwts = std::async(std::launch::async, [&] {
        bool has_gbwts = false;
        for (const auto& g : gbwts_) {
            if (g) { has_gbwts = true; break; }
        }
        if (has_gbwts) {
            const auto path = dir + "/gbwts.bin";
            auto out = open_out_or_throw(path);
            write_gbwts_payload(out, gbwts_);
            check_write_ok(out, path);
        }
    });

    auto f_topo = std::async(std::launch::async, [&] {
        const auto path = dir + "/topology.bin";
        auto out = open_out_or_throw(path);
        write_topology_meta_payload(out, num_nodes_, gbwt_offsets_, path_names_);
        check_write_ok(out, path);
    });

    auto f_pstats = std::async(std::launch::async, [&] {
        if (has_path_stats()) {
            const auto path = dir + "/path_stats.bin";
            auto out = open_out_or_throw(path);
            write_path_stats_payload(out, path_node_counts_, path_bp_lengths_);
            check_write_ok(out, path);
        }
    });

    f_efwd.get();
    f_ebwd.get();
    f_ef_fwd.get();
    f_ef_bwd.get();
    f_gbwts.get();
    f_topo.get();
    f_pstats.get();

    // snarls.bin é pequeno (ou inexistente); manter sequencial após a barreira.
    save_snarls_only(dir);

    cli::ok("índice salvo", dir);
}

// ════════════════════════════════════════════════════════════════════════════
// save_gbwt_only — grava apenas gbwts.bin + path_stats.bin (Fase 2 do upgrade
// incremental). Atualiza topology.bin com os novos gbwt_offsets_/path_names_,
// sem tocar em E_*/ef_*.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::save_gbwt_only(const std::string& dir) const {
    namespace fs = std::filesystem;
    fs::create_directories(dir);

    // gbwts.bin — só grava se há GBWTs não-nulos
    bool has_gbwts = false;
    for (const auto& g : gbwts_) {
        if (g) { has_gbwts = true; break; }
    }
    if (has_gbwts) {
        const auto path = dir + "/gbwts.bin";
        auto out = open_out_or_throw(path);
        write_gbwts_payload(out, gbwts_);
        check_write_ok(out, path);
    }

    // topology.bin — precisa ser reescrito porque agora contém path_names_
    // e gbwt_offsets_ atualizados. Reusa o formato existente (num_nodes_ +
    // gbwt_offsets_ + path_names_ + hash). E_*/ef_* não são tocados.
    {
        const auto path = dir + "/topology.bin";
        auto out = open_out_or_throw(path);
        write_topology_meta_payload(out, num_nodes_, gbwt_offsets_, path_names_);
        check_write_ok(out, path);
    }

    // path_stats.bin — opcional
    if (has_path_stats()) {
        const auto path = dir + "/path_stats.bin";
        auto out = open_out_or_throw(path);
        write_path_stats_payload(out, path_node_counts_, path_bp_lengths_);
        check_write_ok(out, path);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// save_snarls_only — grava apenas snarls.bin, sem reescrever CSR/gBWT/etc.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::save_snarls_only(const std::string& dir) const {
    if (snarls_.empty()) { return; }

    namespace fs = std::filesystem;
    fs::create_directories(dir);

    const auto path = dir + "/snarls.bin";
    auto out = open_out_or_throw(path);
    const uint64_t n = static_cast<uint64_t>(snarls_.size());
    out.write(reinterpret_cast<const char*>(&n), sizeof(n));
    out.write(reinterpret_cast<const char*>(snarls_.data()),
              static_cast<std::streamsize>(n * sizeof(SnarlRecord)));
    check_write_ok(out, path);
}

// ════════════════════════════════════════════════════════════════════════════
// load — restaura todos os componentes do disco.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::load(const std::string& dir) {
    namespace fs = std::filesystem;

    // Helper: carrega vetor uint32_t com verificação de integridade (XXH64).
    // Lança em qualquer corrupção (open fail, read parcial, hash divergente).
    auto load_vector_impl = [&](std::vector<uint32_t>& v, const std::string& name) {
        const auto path = dir + "/" + name;
        auto in = open_in_or_throw(path);
        uint64_t sz = 0;
        read_exact(in, &sz, sizeof(sz), path);
        if (sz > 4'000'000'000ULL) {
            throw std::runtime_error("[topology] Tamanho suspeito em " + path + " (" +
                                     std::to_string(sz) + ") — cache corrompido?");
        }
        v.resize(static_cast<size_t>(sz));
        if (sz > 0) {
            read_exact(in, v.data(), sz * sizeof(uint32_t), path);
        }
        uint64_t stored_hash = 0;
        read_exact(in, &stored_hash, sizeof(stored_hash), path);
        const uint64_t actual_hash = ChecksumBuilder().add(sz).add(v).finalize();
        if (stored_hash != actual_hash) {
            throw std::runtime_error("[topology] Hash divergente em " + path +
                                     " (arquivo corrompido!)");
        }
    };

    // ── gBWTs (B1): opcional via fs::exists; lança em corrupção ───────────
    // Padrão antigo `if (!in && !in.eof())` mascarava EOF prematuro silenciosamente
    // (GBWT incompleto era aceito como válido). Agora detectamos via:
    //   1. Sanity de file_size vs count antes do loop (pega "count enorme com
    //      payload mínimo");
    //   2. `in.fail()` após simple_sds_load (pega EOF puro ou erro de I/O);
    //   3. `simple_sds::InvalidData` que o próprio simple_sds_load lança ao
    //      detectar campos com valores inválidos (width fora de 1-64, etc.).
    //
    // LIMITAÇÃO conhecida: simple_sds_load do SDSL pode entrar em loop quando
    // recebe payload parcialmente truncado em pontos específicos da estrutura
    // (lê 0 bytes silenciosamente). Mitigação completa exigiria timeout
    // upstream — fora do escopo. Sanity check (1) cobre o caso extremo.
    auto load_gbwts = [&]() {
        const auto path = dir + "/gbwts.bin";
        if (!fs::exists(path)) {
            return; // legacy compat: vetor permanece vazio
        }
        auto in = open_in_or_throw(path);
        const auto file_sz = static_cast<size_t>(fs::file_size(path));
        uint64_t n = 0;
        read_exact(in, &n, sizeof(n), path);
        // Sanity: cada GBWT serializado tem pelo menos ~64 B de header.
        constexpr size_t kMinGbwtBytes = 64;
        const size_t min_expected = sizeof(uint64_t) + static_cast<size_t>(n) * kMinGbwtBytes;
        if (file_sz < min_expected) {
            throw std::runtime_error(
                "[topology] gbwts.bin tamanho insuficiente: " + std::to_string(file_sz) +
                " B, esperado >= " + std::to_string(min_expected) + " B (n=" +
                std::to_string(n) + ") — cache truncado/corrompido");
        }
        gbwts_.clear();
        gbwts_.reserve(static_cast<size_t>(n));
        for (uint64_t i = 0; i < n; ++i) {
            auto g = std::make_unique<gbwt::GBWT>();
            g->simple_sds_load(in); // pode lançar simple_sds::InvalidData
            if (in.fail()) {
                throw std::runtime_error("[topology] Falha ao desserializar gbwt[" +
                                         std::to_string(i) + "] em " + path +
                                         " (cache truncado/corrompido)");
            }
            gbwts_.push_back(std::move(g));
        }
    };

    // ── Carregamento paralelo dos arquivos independentes (PERF) ───────────
    // P2: gbwts.bin (~63 MB no chm13) entra na batch async em vez de ser
    // serial após a barreira. Não há dependência entre gbwts_ e os campos
    // populados pelas outras tasks (E_*, ef_*, num_nodes_/gbwt_offsets_/
    // path_names_).
    auto f_fwd = std::async(std::launch::async, [&]() { load_vector_impl(E_fwd_, "E_fwd.bin"); });
    auto f_bwd = std::async(std::launch::async, [&]() { load_vector_impl(E_bwd_, "E_bwd.bin"); });
    auto f_ef_fwd = std::async(std::launch::async,
                               [&]() { sdsl::load_from_file(ef_fwd_, dir + "/ef_fwd.sdsl"); });
    auto f_ef_bwd = std::async(std::launch::async,
                               [&]() { sdsl::load_from_file(ef_bwd_, dir + "/ef_bwd.sdsl"); });
    auto f_gbwts = std::async(std::launch::async, load_gbwts);

    auto f_meta = std::async(std::launch::async, [&]() {
        const auto path = dir + "/topology.bin";
        auto in = open_in_or_throw(path);
        read_exact(in, &num_nodes_, sizeof(num_nodes_), path);
        ChecksumBuilder cb;
        cb.add(num_nodes_);

        uint64_t n_off = 0;
        read_exact(in, &n_off, sizeof(n_off), path);
        cb.add(n_off);
        gbwt_offsets_.resize(static_cast<size_t>(n_off));
        for (uint64_t i = 0; i < n_off; ++i) {
            uint64_t v = 0;
            read_exact(in, &v, sizeof(v), path);
            gbwt_offsets_[i] = static_cast<size_t>(v);
            cb.add(v);
        }

        uint64_t n_paths = 0;
        read_exact(in, &n_paths, sizeof(n_paths), path);
        cb.add(n_paths);
        path_names_.resize(static_cast<size_t>(n_paths));
        for (uint64_t i = 0; i < n_paths; ++i) {
            uint64_t slen = 0;
            read_exact(in, &slen, sizeof(slen), path);
            path_names_[i].resize(static_cast<size_t>(slen));
            if (slen > 0) {
                read_exact(in, path_names_[i].data(), slen, path);
            }
            cb.add(path_names_[i]);
        }

        uint64_t stored_hash = 0;
        read_exact(in, &stored_hash, sizeof(stored_hash), path);
        if (stored_hash != cb.finalize()) {
            throw std::runtime_error("[topology] Hash divergente em " + path +
                                     " (arquivo corrompido!)");
        }
    });

    // Aguarda todas as leituras — exceções propagam via .get().
    f_fwd.get();
    f_bwd.get();
    f_ef_fwd.get();
    f_ef_bwd.get();
    f_gbwts.get();
    f_meta.get();

    // Re-vincular os suportes SDSL após carregamento (crítico para consultas).
    // Não pode ser feito dentro da task async pois bind por ponteiro precisa
    // que o vetor SDSL final esteja em endereço estável (fora do escopo da task).
    sdsl::util::init_support(ef_fwd_sel_, &ef_fwd_);
    sdsl::util::init_support(ef_bwd_sel_, &ef_bwd_);

    // Invalida caches de offsets (reconstruídos lazily na próxima consulta)
    offsets_cache_fwd_.clear();
    offsets_cache_bwd_.clear();

    // ── path_stats (B2): opcional, mas se presente deve estar íntegro ─────
    {
        const auto path = dir + "/path_stats.bin";
        if (fs::exists(path)) {
            auto in = open_in_or_throw(path);
            uint64_t n = 0;
            read_exact(in, &n, sizeof(n), path);
            if (n != static_cast<uint64_t>(path_names_.size())) {
                throw std::runtime_error(
                    "[topology] path_stats.bin desincronizado: n=" + std::to_string(n) +
                    " vs path_names=" + std::to_string(path_names_.size()) +
                    " — regenere o cache");
            }
            path_node_counts_.resize(static_cast<size_t>(n));
            path_bp_lengths_.resize(static_cast<size_t>(n));
            if (n > 0) {
                read_exact(in, path_node_counts_.data(), n * sizeof(uint32_t), path);
                read_exact(in, path_bp_lengths_.data(), n * sizeof(int64_t), path);
            }
        }
        // Cache antigo: path_node_counts_/path_bp_lengths_ permanecem vazios.
    }

    // ── snarls (B3): opcional ─────────────────────────────────────────────
    {
        const auto path = dir + "/snarls.bin";
        if (fs::exists(path)) {
            auto in = open_in_or_throw(path);
            uint64_t n = 0;
            read_exact(in, &n, sizeof(n), path);
            if (n >= (1ULL << 32)) {
                throw std::runtime_error("[topology] snarls.bin com count suspeito (" +
                                         std::to_string(n) + ") — cache corrompido?");
            }
            snarls_.resize(static_cast<size_t>(n));
            if (n > 0) {
                read_exact(in, snarls_.data(), n * sizeof(SnarlRecord), path);
            }
        }
        // Cache antigo: snarls_ permanece vazio.
    }

    cli::step_ok("índice carregado", dir);
}

} // namespace ggql
