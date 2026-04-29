// csr_builder.cpp — Construção CSR, Elias-Fano e offsets cache de GfaCsrTopology.
//

#include "ggql/storage/topology.hpp"

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

// csr_builder.cpp — Construção CSR, Elias-Fano e offsets cache.
// ════════════════════════════════════════════════════════════════════════════
// Destrutor — definido aqui onde gbwt::GBWT é tipo completo.
// unique_ptr<gbwt::GBWT> requer destrutor completo no ponto de instanciação.
// ════════════════════════════════════════════════════════════════════════════
GfaCsrTopology::~GfaCsrTopology() = default;

// ════════════════════════════════════════════════════════════════════════════
// Move Semantics — necessário para re-vincular ponteiros SDSL (select_1, etc)
// ════════════════════════════════════════════════════════════════════════════
GfaCsrTopology::GfaCsrTopology(GfaCsrTopology&& other) noexcept
    : num_nodes_(other.num_nodes_), E_fwd_(std::move(other.E_fwd_)),
      E_bwd_(std::move(other.E_bwd_)), ef_fwd_(std::move(other.ef_fwd_)),
      ef_fwd_sel_(std::move(other.ef_fwd_sel_)), ef_bwd_(std::move(other.ef_bwd_)),
      ef_bwd_sel_(std::move(other.ef_bwd_sel_)),
      offsets_cache_fwd_(std::move(other.offsets_cache_fwd_)),
      offsets_cache_bwd_(std::move(other.offsets_cache_bwd_)), gbwts_(std::move(other.gbwts_)),
      gbwt_offsets_(std::move(other.gbwt_offsets_)), path_names_(std::move(other.path_names_)) {
    // Re-vincula os suportes SDSL ao novo endereço dos vetores movimentados
    ef_fwd_sel_.set_vector(&ef_fwd_);
    ef_bwd_sel_.set_vector(&ef_bwd_);
}

GfaCsrTopology& GfaCsrTopology::operator=(GfaCsrTopology&& other) noexcept {
    if (this != &other) {
        num_nodes_ = other.num_nodes_;
        E_fwd_ = std::move(other.E_fwd_);
        E_bwd_ = std::move(other.E_bwd_);
        ef_fwd_ = std::move(other.ef_fwd_);
        ef_fwd_sel_ = std::move(other.ef_fwd_sel_);
        ef_bwd_ = std::move(other.ef_bwd_);
        ef_bwd_sel_ = std::move(other.ef_bwd_sel_);
        offsets_cache_fwd_ = std::move(other.offsets_cache_fwd_);
        offsets_cache_bwd_ = std::move(other.offsets_cache_bwd_);
        gbwts_ = std::move(other.gbwts_);
        gbwt_offsets_ = std::move(other.gbwt_offsets_);
        path_names_ = std::move(other.path_names_);

        ef_fwd_sel_.set_vector(&ef_fwd_);
        ef_bwd_sel_.set_vector(&ef_bwd_);
    }
    return *this;
}

// ════════════════════════════════════════════════════════════════════════════
// memory_bytes
// ════════════════════════════════════════════════════════════════════════════
size_t GfaCsrTopology::memory_bytes() const noexcept {
    size_t topo = E_fwd_.size() * sizeof(uint32_t) + sdsl::size_in_bytes(ef_fwd_) +
                  E_bwd_.size() * sizeof(uint32_t) + sdsl::size_in_bytes(ef_bwd_) +
                  offsets_cache_fwd_.size() * sizeof(uint32_t) +
                  offsets_cache_bwd_.size() * sizeof(uint32_t);

    // sdsl::size_in_bytes serializa para null-stream (sem alocação de buffer).
    size_t gbwt_sz = 0;
    for (const auto& g : gbwts_) {
        if (g) {
            gbwt_sz += sdsl::size_in_bytes(*g);
        }
    }
    for (const auto& s : path_names_) {
        gbwt_sz += s.size();
    }
    return topo + gbwt_sz;
}

// ════════════════════════════════════════════════════════════════════════════
// radix_sort_by_from — LSD radix sort paralelo, O(n/p) por passo.
//
// Otimizações:
//   1. Passes fixos via num_nodes: elimina varredura serial para max_val.
//   2. alignas(64) em lhist: elimina false sharing entre threads.
//   3. Paridade de passes: força n_passes_total par → resultado sempre em
//      links.data() → elimina std::copy de até 1.89 GB ao final.
//   4. Prefetch no scatter (lookahead=16): amortiza latência de L3-miss.
//   5. Loop de prefix-sum fusionado: elimina total[] e global_pfx[] (3→1 passes).
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::radix_sort_by_from(std::vector<LinkRecord>& links, uint64_t num_nodes,
                                        int num_threads) {
    const size_t n = links.size();
    if (n < 2) {
        return;
    }

    // OPT 1: passes fixos via num_nodes
    int n_passes_real = 0;
    for (uint64_t v = (num_nodes > 0 ? num_nodes - 1 : 0); v; v >>= 8) {
        ++n_passes_real;
    }
    if (n_passes_real == 0) {
        n_passes_real = 1;
    }

    // OPT 3: paridade de passes — garante src == links.data() ao retornar.
    const int n_passes_total = (n_passes_real % 2 == 0) ? n_passes_real : n_passes_real + 1;

    std::vector<LinkRecord> buf(n);
    LinkRecord* src = links.data();
    LinkRecord* dst = buf.data();

    const size_t chunk =
        (n + static_cast<size_t>(num_threads) - 1) / static_cast<size_t>(num_threads);

    // OPT 2: alignas(64) elimina false sharing
    struct alignas(64) Hist {
        std::array<uint32_t, 256> h;
    };
    std::vector<Hist> lhist(static_cast<size_t>(num_threads));

    for (int pass = 0; pass < n_passes_total; ++pass) {
        const int shift = (pass < n_passes_real) ? pass * 8 : 0;

        // 1. Histograma paralelo
#pragma omp parallel num_threads(num_threads)
        {
            int tid = 0;
#ifdef _OPENMP
            tid = omp_get_thread_num();
#endif
            size_t lo = static_cast<size_t>(tid) * chunk;
            size_t hi = std::min(lo + chunk, n);
            auto& h = lhist[static_cast<size_t>(tid)].h;
            h.fill(0);
            for (size_t i = lo; i < hi; ++i) {
                h[(src[i].from_id() >> shift) & 0xFFu]++;
            }
        }

        // 2. Prefix-sum global — OPT 5: loop fusionado (3 passes → 1)
        std::vector<std::array<uint32_t, 256>> starts(static_cast<size_t>(num_threads));
        {
            uint32_t running = 0;
            for (int d = 0; d < 256; ++d) {
                uint32_t digit_total = 0;
                for (int t = 0; t < num_threads; ++t) {
                    starts[static_cast<size_t>(t)][static_cast<size_t>(d)] = running + digit_total;
                    digit_total += lhist[static_cast<size_t>(t)].h[static_cast<size_t>(d)];
                }
                running += digit_total;
            }
        }

        // 3. Scatter paralelo com prefetch — OPT 4
#pragma omp parallel num_threads(num_threads)
        {
            int tid = 0;
#ifdef _OPENMP
            tid = omp_get_thread_num();
#endif
            size_t lo = static_cast<size_t>(tid) * chunk;
            size_t hi = std::min(lo + chunk, n);
            auto offsets = starts[static_cast<size_t>(tid)];

            constexpr size_t kPrefetchAhead = 16;
            for (size_t i = lo; i < hi; ++i) {
                if (i + kPrefetchAhead < hi) {
                    uint8_t pfdigit = (src[i + kPrefetchAhead].from_id() >> shift) & 0xFFu;
                    __builtin_prefetch(&dst[offsets[pfdigit]], 1, 0);
                }
                uint8_t digit = (src[i].from_id() >> shift) & 0xFFu;
                dst[offsets[digit]++] = src[i];
            }
        }

        std::swap(src, dst);
    }
    // OPT 3: n_passes_total é sempre par → src == links.data() garantido.
}

// ════════════════════════════════════════════════════════════════════════════
// build_csr — constrói E_* e Elias-Fano a partir de links.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::build_csr(std::vector<LinkRecord>& links, std::vector<uint32_t>& out_E,
                               sdsl::sd_vector<>& out_ef,
                               sdsl::sd_vector<>::select_1_type& out_ef_sel, bool is_fwd,
                               int num_threads) {
    const uint64_t m = links.size();

    if (m == 0) {
        out_E.clear();
        out_ef = sdsl::sd_vector<>();
        sdsl::util::init_support(out_ef_sel, &out_ef);
        return;
    }

    // Ordena por from_id — LSD radix sort paralelo O(n/p)
    radix_sort_by_from(links, num_nodes_, num_threads);

    // Offsets CSR temporários — varredura sequencial sobre array já ordenado
    std::vector<uint32_t> tmp_off(num_nodes_ + 1, 0u);
#ifdef __GNUC__
    #pragma GCC ivdep
#endif
    for (const auto& lk : links) {
        tmp_off[lk.from_id() + 1]++;
    }
    // Parallel prefix sum (C++23: std::inclusive_scan com par_unseq)
    std::inclusive_scan(std::execution::par_unseq, tmp_off.begin(), tmp_off.end(), tmp_off.begin());

    // Preenche E — par_unseq: sem dependência de dados → vetorizável.
    out_E.resize(m);
    std::transform(
        std::execution::par_unseq, links.begin(), links.end(), out_E.begin(),
        [](const LinkRecord& lk) noexcept { return static_cast<uint32_t>(lk.to_packed); });

    // Constrói Elias-Fano sobre posições de início.
    // multiset=true: permite valores duplicados (nós sem arestas).
    sdsl::sd_vector_builder builder(m + 1, num_nodes_ + 1, /*multiset=*/true);
    {
        const size_t total_ef = num_nodes_ + 1;
        // BUG-TOPO-3: substitui tracker inline por ggql::ProgressTracker.
        // step amortiza o custo de add_progress (evita steady_clock::now() a cada nó).
        const size_t step = std::max(size_t(1), total_ef / 200); // atualiza a cada ~0,5%
        const char* lbl = is_fwd ? "Elias-Fano fwd" : "Elias-Fano bwd";
        ggql::ProgressTracker tracker_ef(total_ef, lbl, true, "nodes/s", 1.0);

        // PERF-TOPO-3: contador decrescente elimina divisão modular a cada iteração.
        size_t countdown = step;
        for (uint64_t i = 0; i <= num_nodes_; ++i) {
            builder.set(static_cast<uint64_t>(tmp_off[i]));
            if (--countdown == 0) {
                tracker_ef.add_progress(step);
                countdown = step;
            }
        }
        tracker_ef.finish();
    }
    out_ef = sdsl::sd_vector<>(builder);
    sdsl::util::init_support(out_ef_sel, &out_ef);
    // tmp_off liberado ao sair — sem copiar para cache (lazy, opção 1C).
}

// ════════════════════════════════════════════════════════════════════════════
// ensure_offsets_cache — popula cache LAZY de offsets CSR via select_1 paralelo.
//
// select_1_type::operator() é const e thread-safe (acessa dados read-only).
// Chamado ANTES de qualquer região OMP em find_bubbles → sem data race.
// Ganho: 4.3M nós × ~40ns/select_1 ≈ 172ms serial → ~45ms a 4t.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::ensure_offsets_cache(int num_threads) const {
    if (offsets_cache_fwd_.empty() && num_nodes_ > 0) {
        offsets_cache_fwd_.resize(num_nodes_ + 1);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(num_threads)
#endif
        for (uint64_t i = 0; i <= num_nodes_; ++i) {
            offsets_cache_fwd_[i] = static_cast<uint32_t>(ef_fwd_sel_(i + 1));
        }
    }
    if (offsets_cache_bwd_.empty() && num_nodes_ > 0) {
        offsets_cache_bwd_.resize(num_nodes_ + 1);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(num_threads)
#endif
        for (uint64_t i = 0; i <= num_nodes_; ++i) {
            offsets_cache_bwd_[i] = static_cast<uint32_t>(ef_bwd_sel_(i + 1));
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
// find_bubbles — algoritmo Onodera et al. (2013), Kosaraju iterativo + sweep.
// Complexidade: O(V + E) no DAG condensado.
// ════════════════════════════════════════════════════════════════════════════
// ════════════════════════════════════════════════════════════════════════════
// rebuild_offsets_
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::rebuild_offsets_() noexcept {
    gbwt_offsets_.resize(gbwts_.size() + 1);
    gbwt_offsets_[0] = 0;
    for (size_t i = 0; i < gbwts_.size(); ++i) {
        gbwt_offsets_[i + 1] =
            gbwt_offsets_[i] + (gbwts_[i] ? static_cast<size_t>(gbwts_[i]->sequences()) : 0);
    }
}


} // namespace ggql
