// csr_builder.cpp — Construção CSR, Elias-Fano e offsets cache de GfaCsrTopology.

#include "ggql/storage/topology.hpp"

#include "ggql/cli/progress_tracker.hpp"
#include "ggql/storage/gbwt_backend.hpp"

#include <algorithm>
#include <cstdint>
#include <execution>
#include <numeric>

#ifdef _OPENMP
    #include <omp.h>
#endif

namespace ggql {

// ════════════════════════════════════════════════════════════════════════════
// Destrutor — definido aqui onde GbwtBackend e tipo completo (necessario
// para destruir unique_ptr<GbwtBackend> com forward decl no header).
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
      offsets_cache_bwd_(std::move(other.offsets_cache_bwd_)),
      path_stores_(std::move(other.path_stores_)),
      gbwt_offsets_(std::move(other.gbwt_offsets_)),
      path_names_(std::move(other.path_names_)) {
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
        path_stores_ = std::move(other.path_stores_);
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

    size_t paths_sz = 0;
    for (const auto& ps : path_stores_) {
        if (ps) {
            paths_sz += ps->memory_bytes();
        }
    }
    for (const auto& s : path_names_) {
        paths_sz += s.size();
    }
    return topo + paths_sz;
}

// ════════════════════════════════════════════════════════════════════════════
// radix_sort_by_from — LSD radix sort paralelo, O(n/p) por passo.
//
// Otimizações:
//   1. Passes fixos via num_nodes (n_passes_real) — sem varredura serial
//      para encontrar max_val.
//   2. alignas(64) em Hist: sizeof(Hist) múltiplo de 64 → cada lhist[t]
//      ocupa cache lines distintas → sem false sharing entre threads.
//   3. Paridade de passes: pad para n_passes_total par força
//      `src == links.data()` ao retornar — evita std::copy final de até
//      ~2,8 GB no HPRC. Pass extra (~120 ms) < copy alternativo (~280 ms).
//   4. Prefetch no scatter (lookahead=16): amortiza latência L3-miss em
//      `dst[offsets[pfdigit]]`.
//   5. Prefix-sum global fusionado num único loop digit-major (256×nt
//      writes em starts[]) — elimina total[] e global_pfx[] intermediários.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::radix_sort_by_from(std::vector<LinkRecord>& links, uint64_t num_nodes,
                                        int num_threads) {
    const size_t n = links.size();
    if (n < 2) {
        return;
    }

    // B4: clamp para evitar div-by-zero em chunk = (n + nt - 1) / nt
    // se caller passar num_threads <= 0 (improvável mas defensivo).
    const int nt = std::max(1, num_threads);

    // Passes fixos via num_nodes — elimina varredura serial para max_val.
    int n_passes_real = 0;
    for (uint64_t v = (num_nodes > 0 ? num_nodes - 1 : 0); v; v >>= 8) {
        ++n_passes_real;
    }
    if (n_passes_real == 0) {
        n_passes_real = 1;
    }

    // Pad para par — ver comentário "Otimização 3" acima.
    const int n_passes_total = (n_passes_real % 2 == 0) ? n_passes_real : n_passes_real + 1;

    std::vector<LinkRecord> buf(n);
    LinkRecord* src = links.data();
    LinkRecord* dst = buf.data();

    const size_t chunk =
        (n + static_cast<size_t>(nt) - 1) / static_cast<size_t>(nt);

    // alignas(64) → sizeof(Hist) múltiplo de 64; elimina false sharing.
    struct alignas(64) Hist {
        std::array<uint32_t, 256> h;
    };
    std::vector<Hist> lhist(static_cast<size_t>(nt));

    for (int pass = 0; pass < n_passes_total; ++pass) {
        const int shift = (pass < n_passes_real) ? pass * 8 : 0;

        // 1. Histograma paralelo
#pragma omp parallel num_threads(nt)
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

        // 2. Prefix-sum global — loop fusionado (3 passes → 1)
        std::vector<std::array<uint32_t, 256>> starts(static_cast<size_t>(nt));
        {
            uint32_t running = 0;
            for (int d = 0; d < 256; ++d) {
                uint32_t digit_total = 0;
                for (int t = 0; t < nt; ++t) {
                    starts[static_cast<size_t>(t)][static_cast<size_t>(d)] = running + digit_total;
                    digit_total += lhist[static_cast<size_t>(t)].h[static_cast<size_t>(d)];
                }
                running += digit_total;
            }
        }

        // 3. Scatter paralelo com prefetch
#pragma omp parallel num_threads(nt)
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
                // [[likely]]: branch só falsa nas últimas 16 iterações de
                // cada chunk (~16/chunk_size ≈ 1e-6 no HPRC).
                if (i + kPrefetchAhead < hi) [[likely]] {
                    const uint8_t pfdigit = (src[i + kPrefetchAhead].from_id() >> shift) & 0xFFu;
                    __builtin_prefetch(&dst[offsets[pfdigit]], 1, 0);
                }
                const uint8_t digit = (src[i].from_id() >> shift) & 0xFFu;
                dst[offsets[digit]++] = src[i];
            }
        }

        std::swap(src, dst);
    }
    // n_passes_total par → src == links.data() ao retornar.
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

    // Offsets CSR temporários — varredura sequencial sobre array já ordenado.
    // Nota: NÃO usar #pragma GCC ivdep aqui. Links com mesmo from_id (após
    // sort) escrevem no mesmo `tmp_off[k]`, gerando dependência verdadeira
    // entre iterações sucessivas. ivdep promete ausência de loop-carried
    // dependence, o que seria mentira → vetorização incorreta perderia
    // contagens. Compilador detecta e mantém o loop serial corretamente.
    std::vector<uint32_t> tmp_off(num_nodes_ + 1, 0u);
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
        // step amortiza o custo de add_progress (evita steady_clock::now() a cada nó).
        const size_t step = std::max(size_t(1), total_ef / 200); // atualiza a cada ~0,5%
        const char* lbl = is_fwd ? "Elias-Fano fwd" : "Elias-Fano bwd";
        ggql::ProgressTracker tracker_ef(total_ef, lbl, true, "nodes/s", 1.0);

        // Contador decrescente elimina divisão modular a cada iteração.
        size_t countdown = step;
        for (uint64_t i = 0; i <= num_nodes_; ++i) {
            builder.set(static_cast<uint64_t>(tmp_off[i]));
            // [[unlikely]]: progress tick só dispara a cada `step` iter
            // (~0,5% das chamadas).
            if (--countdown == 0) [[unlikely]] {
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
// rebuild_offsets_ — recalcula gbwt_offsets_ a partir de path_stores_.
// gbwt_offsets_ mapeia global_path_id → (shard, local_id) via upper_bound.
// ════════════════════════════════════════════════════════════════════════════
void GfaCsrTopology::rebuild_offsets_() {
    gbwt_offsets_.resize(path_stores_.size() + 1);
    gbwt_offsets_[0] = 0;
    for (size_t i = 0; i < path_stores_.size(); ++i) {
        gbwt_offsets_[i + 1] = gbwt_offsets_[i] +
                                (path_stores_[i] ? path_stores_[i]->num_paths() : 0);
    }
}

} // namespace ggql
