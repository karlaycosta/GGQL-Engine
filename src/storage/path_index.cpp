// path_index.cpp — Construção paralela do índice de paths e persistência
// (save/load) de GfaCsrTopology. Usa PathStorage (sem gBWT comprimido).

#include "ggql/storage/topology.hpp"

#include "ggql/cli/console.hpp"
#include "ggql/cli/progress_tracker.hpp"
#include "ggql/util/memory_monitor.hpp"

// Headers pesados — incluídos só aqui, não no .hpp público.
#include "ggql/gbwt/node_buffer.hpp"
#include "ggql/gbwt/path_storage.hpp"
#include "ggql/storage/checksum_builder.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <future>
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

// ── Serialização compartilhada save() ↔ save_path_index_only() ──────────

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

// Formato path_stores.bin (Fase 4):
//   [num_shards: u64]
//   Para cada shard:
//     [present: u8] (1 = não-nulo, 0 = nullptr)
//     Se present: PathStorage::save() inline (header próprio + checksum)
// Sem header global — cada shard é validado individualmente pelo seu checksum.
inline void
write_path_stores_payload(std::ofstream&                                       out,
                          const std::vector<std::unique_ptr<PathStorage>>& path_stores) {
    const uint64_t n_shards = static_cast<uint64_t>(path_stores.size());
    out.write(reinterpret_cast<const char*>(&n_shards), sizeof(n_shards));
    for (const auto& ps : path_stores) {
        const uint8_t present = ps ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&present), sizeof(present));
        if (ps) { ps->save(out); }
    }
}

inline void
read_path_stores_payload(std::istream&                                  in,
                         std::vector<std::unique_ptr<PathStorage>>& out_stores) {
    uint64_t n_shards = 0;
    in.read(reinterpret_cast<char*>(&n_shards), sizeof(n_shards));
    if (!in.good()) {
        throw std::runtime_error("[topology] path_stores.bin: falha ao ler num_shards");
    }
    out_stores.clear();
    out_stores.reserve(static_cast<size_t>(n_shards));
    for (uint64_t i = 0; i < n_shards; ++i) {
        uint8_t present = 0;
        in.read(reinterpret_cast<char*>(&present), sizeof(present));
        if (!in.good()) {
            throw std::runtime_error(
                "[topology] path_stores.bin: falha ao ler flag present do shard " +
                std::to_string(i));
        }
        if (present) {
            out_stores.push_back(
                std::make_unique<PathStorage>(PathStorage::load(in)));
        } else {
            out_stores.emplace_back();
        }
    }
}

} // namespace

// ════════════════════════════════════════════════════════════════════════════
// build_path_storage_parallel — constrói path_stores_ direto a partir de
// GbwtGroups. Custo: O(total_nodes), sem indireção via LF-mapping. Cada
// shard é independente (sem merge final, diferente do gBWT antigo).
// ════════════════════════════════════════════════════════════════════════════
GfaCsrTopology::GbwtTimings
GfaCsrTopology::build_path_storage_parallel(std::vector<GbwtGroup> groups,
                                             uint8_t                node_width,
                                             int                    num_threads) {
    using clk = std::chrono::high_resolution_clock;
    using sec = std::chrono::duration<double>;

    GbwtTimings t{};
    MemoryMonitor monitor;          // amostra RSS em background (10 Hz, ~0.01% CPU)
    auto        wall_start = clk::now();

    // Reset estado prévio para idempotência.
    path_stores_.clear();
    gbwt_offsets_.clear();
    path_names_.clear();

    const size_t N = groups.size();
    path_stores_.resize(N);

    size_t est_total_paths = 0;
    for (const auto& g : groups) { est_total_paths += g.path_names.size(); }
    path_names_.reserve(est_total_paths);

    std::atomic<uint64_t> total_nodes_acc{0};

    const int requested_threads = (num_threads > 0) ? num_threads : 1;

    // ── Budget-based parallelism ──────────────────────────────────────────
    // PathStorageBuilder acumula `vector<node_type> nodes_` sem compressão
    // durante o build. Pico por job ≈ total_nós_do_shard × sizeof(node_type).
    // Sem o clamp abaixo, datasets como HPRC (~10⁹ símbolos no maior shard)
    // × num_threads em paralelo facilmente excedem 32 GB de RAM.
    //
    // Modelo: estima nós por shard via in_ram_seqs.size() (somatório) ou
    // tamanho do tmp_path em bits / node_width. Toma o pior shard como
    // custo unitário e divide o budget (75% da RAM disponível) por ele.
    namespace fs = std::filesystem;
    constexpr size_t kBytesPerNode      = sizeof(PathStorage::node_type);
    constexpr size_t kPerJobOverhead    = 32u << 20; // dec buffer + reserves
    size_t           max_shard_nodes    = 0;
    size_t           total_est_nodes    = 0;
    const uint64_t   bits_per_sym       = std::max<uint8_t>(node_width, 1);
    for (const auto& g : groups) {
        size_t shard_nodes = 0;
        if (!g.in_ram_seqs.empty()) {
            for (const auto& seq : g.in_ram_seqs) { shard_nodes += seq.size(); }
        } else if (!g.tmp_path.empty()) {
            try {
                const size_t bytes_on_disk = static_cast<size_t>(fs::file_size(g.tmp_path));
                shard_nodes = static_cast<size_t>((bytes_on_disk * 8ull) / bits_per_sym);
            } catch (...) { shard_nodes = 0; }
        }
        max_shard_nodes  = std::max(max_shard_nodes, shard_nodes);
        total_est_nodes += shard_nodes;
    }
    const size_t cost_per_job = max_shard_nodes * kBytesPerNode + kPerJobOverhead;
    const size_t avail_bytes  = available_ram_kb() * 1024ull;
    const size_t budget       = (avail_bytes > 0) ? (avail_bytes * 75ull / 100ull)
                                                  : (size_t(8) << 30);
    size_t       max_parallel = (cost_per_job > 0) ? std::max<size_t>(1, budget / cost_per_job)
                                                   : static_cast<size_t>(requested_threads);
    if (max_parallel > static_cast<size_t>(requested_threads)) {
        max_parallel = static_cast<size_t>(requested_threads);
    }
    const int omp_threads = static_cast<int>(std::max<size_t>(1, max_parallel));

    if (omp_threads < requested_threads) {
        cli::step_info(fmt::format(
            "PathStorage build: clamp {} → {} threads (maior shard ~{} MB, "
            "budget {:.1f} GB)",
            requested_threads, omp_threads,
            (cost_per_job + (1u << 20) - 1) / (1u << 20),
            budget / 1e9));
    }

    // Cada shard é independente — paraleliza por shard. Não há merge final
    // (cada PathStorage é auto-contido).
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) num_threads(omp_threads)
#endif
    for (size_t i = 0; i < N; ++i) {
        auto& g = groups[i];
        if (g.in_ram_seqs.empty() && g.tmp_path.empty()) {
            path_stores_[i] = std::make_unique<PathStorage>();
            continue;
        }

        PathStorageBuilder                       builder;
        std::vector<PathStorage::node_type>      dec;
        size_t                                   local_syms = 0;

        // Decode inline em uint32_t — equivalente a `gbwt_decode_node(enc)`
        // mas sem o triplo cast (uint32→uint64→subtract→uint32). Para enc≥2
        // (garantido pelo encoding +1 anti-ENDMARKER), `(enc>>1)-1` em uint32
        // produz o mesmo resultado que em uint64 e ajuda o auto-vectorizer.
        const auto decode32 = [](PathStorage::node_type enc) noexcept {
            return static_cast<PathStorage::node_type>((enc >> 1) - 1u);
        };

        if (!g.in_ram_seqs.empty()) {
            for (const auto& seq : g.in_ram_seqs) {
                if (seq.empty()) { continue; }
                dec.clear();
                dec.reserve(seq.size());
                for (auto enc : seq) { dec.push_back(decode32(enc)); }
                builder.insert(dec);
                local_syms += dec.size();
            }
        } else {
            ggql::NodeBuffer buf(g.tmp_path, std::ios::in, 64u << 20, node_width);
            dec.clear();
            dec.reserve(1u << 16);
            for (std::uint64_t val : buf) {
                if (val == ggql::kEndmarker) {
                    if (!dec.empty()) {
                        builder.insert(dec);
                        local_syms += dec.size();
                        dec.clear();
                    }
                } else {
                    dec.push_back(decode32(static_cast<PathStorage::node_type>(val)));
                }
            }
            if (!dec.empty()) {
                builder.insert(dec);
                local_syms += dec.size();
            }
            (void)::unlink(g.tmp_path.c_str());
        }

        path_stores_[i] = std::make_unique<PathStorage>(builder.finish());
        total_nodes_acc.fetch_add(local_syms, std::memory_order_relaxed);
    }

    // path_names em ordem de shard (serial — preserva ordem global).
    for (size_t i = 0; i < N; ++i) {
        for (auto& name : groups[i].path_names) {
            path_names_.push_back(std::move(name));
        }
    }

    rebuild_offsets_();

    t.wall_s      = sec(clk::now() - wall_start).count();
    t.total_nodes = total_nodes_acc.load();
    t.cpu_avg_s   = t.wall_s;
    t.cpu_max_s   = t.wall_s;

    // Pico de RSS observado durante o build (em KB). Capturado pelo monitor
    // em background. Útil para validar empiricamente o modelo de cost_per_job
    // do clamp acima e ajustar 75%-budget se necessário.
    monitor.stop();
    t.peak_rss_kb = monitor.peak_kb();

    // index_size_b reporta o tamanho efetivo dos PathStorages em RAM —
    // próximo do tamanho em disco do path_stores.bin (sem o overhead do
    // header de 32 B por shard). Preserva a UI do ingestor.
    size_t total_bytes = 0;
    for (const auto& ps : path_stores_) {
        if (ps) { total_bytes += ps->memory_bytes(); }
    }
    t.index_size_b = total_bytes;
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

    auto f_pstores = std::async(std::launch::async, [&] {
        if (!path_stores_.empty()) {
            const auto path = dir + "/path_stores.bin";
            auto out = open_out_or_throw(path);
            write_path_stores_payload(out, path_stores_);
            check_write_ok(out, path);
        }
    });

    f_efwd.get();
    f_ebwd.get();
    f_ef_fwd.get();
    f_ef_bwd.get();
    f_topo.get();
    f_pstats.get();
    f_pstores.get();

    // snarls.bin é pequeno (ou inexistente); manter sequencial após a barreira.
    save_snarls_only(dir);

    cli::ok("índice salvo", dir);
}

// ════════════════════════════════════════════════════════════════════════════
// save_path_index_only — grava apenas path_stores.bin + topology.bin +
// path_stats.bin (Fase 2 do upgrade incremental). E_*/ef_*/metadata.bin
// permanecem intactos.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::save_path_index_only(const std::string& dir) const {
    namespace fs = std::filesystem;
    fs::create_directories(dir);

    // path_stores.bin — só grava se há shards (qualquer ingest com paths)
    if (!path_stores_.empty()) {
        const auto path = dir + "/path_stores.bin";
        auto out = open_out_or_throw(path);
        write_path_stores_payload(out, path_stores_);
        check_write_ok(out, path);
    }

    // topology.bin — reescreve porque path_names_ e gbwt_offsets_ mudaram.
    // E_*/ef_* não são tocados.
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
// save_path_stores_only — grava apenas path_stores.bin (Fase 4).
// Usado para upgrade incremental de caches gerados antes da migração:
// load() existing → set_path_backend(PathStorage) → save_path_stores_only().
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::save_path_stores_only(const std::string& dir) const {
    if (path_stores_.empty()) { return; }

    namespace fs = std::filesystem;
    fs::create_directories(dir);

    const auto path = dir + "/path_stores.bin";
    auto out = open_out_or_throw(path);
    write_path_stores_payload(out, path_stores_);
    check_write_ok(out, path);
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

    // ── Carregamento paralelo dos arquivos independentes (PERF) ───────────
    // path_stores.bin é o caminho default; gbwts.bin (legacy) é detectado mas
    // não desserializado — após onda 3 não há mais libgbwt para entender o
    // formato. Caches antigos precisam ser regenerados via `ggql ingest`.
    auto f_fwd = std::async(std::launch::async, [&]() { load_vector_impl(E_fwd_, "E_fwd.bin"); });
    auto f_bwd = std::async(std::launch::async, [&]() { load_vector_impl(E_bwd_, "E_bwd.bin"); });
    auto f_ef_fwd = std::async(std::launch::async,
                               [&]() { sdsl::load_from_file(ef_fwd_, dir + "/ef_fwd.sdsl"); });
    auto f_ef_bwd = std::async(std::launch::async,
                               [&]() { sdsl::load_from_file(ef_bwd_, dir + "/ef_bwd.sdsl"); });

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

    // ── path_stores.bin: caminho único após onda 3. Se ausente mas gbwts.bin
    // presente, é cache pré-Fase-6 — exige reingest via `ggql ingest` (a
    // libgbwt foi removida do build, não há leitor de gbwts.bin no binário).
    {
        const auto pstores_path = dir + "/path_stores.bin";
        const auto gbwts_path   = dir + "/gbwts.bin";
        if (fs::exists(pstores_path)) {
            auto in = open_in_or_throw(pstores_path);
            read_path_stores_payload(in, path_stores_);
        } else if (fs::exists(gbwts_path)) {
            throw std::runtime_error(
                "[topology] cache antigo encontrado (apenas gbwts.bin, sem "
                "path_stores.bin). A libgbwt foi removida nesta versão; "
                "regenere o cache executando `ggql ingest <gfa> --idx " + dir + "`.");
        }
        // Sem nenhum dos dois: cache sem paths (CSR-only), válido para queries
        // que não tocam paths.
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
