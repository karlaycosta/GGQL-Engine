// gfa_fm_index.cpp — Implementação de GfaSequenceIndex.
//
// Inclui headers pesados (libsais64, omp, chrono, fstream, fmt) que antes
// estavam em gfa_fm_index.hpp, forçando recompilação em cada TU que incluía
// o índice. Agora compilado uma única vez em ggql_topology_vg.

#include "ggql/storage/gfa_fm_index.hpp"

#include "ggql/cli/console.hpp"
#include "ggql/cli/progress_tracker.hpp"
#include "ggql/storage/checksum_builder.hpp"
#include "ggql/utils.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <fstream>
#include <fmt/color.h>
#include <fmt/core.h>
#include <memory>
#include <stdexcept>
#ifdef _OPENMP
    #include <omp.h>
#endif

#include "libsais64.h"

namespace ggql {

// ── map_char ─────────────────────────────────────────────────────────────────
// Tabela de mapeamento char→símbolo [0..6] gerada em compile-time.
// map_char() vira um único load L1 — sem branches, sem tabela de saltos.
// Definida no .cpp: inlinada pelo compilador em build/search_kmer/locate
// dentro deste TU; LTO cobre chamadas de TUs externos.

uint8_t GfaSequenceIndex::map_char(char c) noexcept {
    static constexpr auto MAP = []() constexpr noexcept {
        std::array<uint8_t, 256> m{};
        for (auto& v : m) { v = 6; }
        m[static_cast<uint8_t>('$')] = 0;
        m[static_cast<uint8_t>('A')] = 1;
        m[static_cast<uint8_t>('a')] = 1;
        m[static_cast<uint8_t>('C')] = 2;
        m[static_cast<uint8_t>('c')] = 2;
        m[static_cast<uint8_t>('G')] = 3;
        m[static_cast<uint8_t>('g')] = 3;
        m[static_cast<uint8_t>('N')] = 4;
        m[static_cast<uint8_t>('n')] = 4;
        m[static_cast<uint8_t>('T')] = 5;
        m[static_cast<uint8_t>('t')] = 5;
        return m;
    }();
    return MAP[static_cast<uint8_t>(c)];
}

// ── get_occ ──────────────────────────────────────────────────────────────────
// Occ(c, idx): contagem de c em bwt_[0..idx] inclusive.
// O(1): contagem do bloco + popcount de bitmask; AoS garante localidade de cache.

uint64_t GfaSequenceIndex::get_occ(uint8_t c, int64_t idx) const noexcept {
    if (idx < 0) { return 0; }
    const int64_t blk = idx / 64;
    const int64_t bit = idx % 64;
    // Branchless: ~(~0ULL << 1 << bit) é válido para todo bit em [0,63] sem UB.
    const uint64_t lomask = ~(~0ULL << 1 << bit);
    const auto& b = blocks_[static_cast<size_t>(blk)];
    return b.occ[c] + static_cast<uint64_t>(std::popcount(b.mask[c] & lomask));
}

// ── get_occ_pair ─────────────────────────────────────────────────────────────
// PERF-FM-3: Occ(c, idxL) e Occ(c, idxR) num único load de OccBlock quando
// ambos os índices caem no mesmo bloco de 64 posições (economiza 1 cache miss
// por iteração do backward search assim que [L,R] cabe num bloco).

std::pair<uint64_t, uint64_t>
GfaSequenceIndex::get_occ_pair(uint8_t c, int64_t idxL, int64_t idxR) const noexcept {
    if (idxL < 0) { return {0, get_occ(c, idxR)}; }
    const int64_t blkL = idxL / 64;
    const int64_t blkR = idxR / 64;
    if (blkL != blkR) { return {get_occ(c, idxL), get_occ(c, idxR)}; }
    const auto& b = blocks_[static_cast<size_t>(blkL)];
    const uint64_t bitL = static_cast<uint64_t>(idxL % 64);
    const uint64_t bitR = static_cast<uint64_t>(idxR % 64);
    const uint64_t lomaskL = ~(~0ULL << 1 << bitL);
    const uint64_t lomaskR = ~(~0ULL << 1 << bitR);
    const uint64_t mask_c  = b.mask[c];
    return {
        b.occ[c] + static_cast<uint64_t>(std::popcount(mask_c & lomaskL)),
        b.occ[c] + static_cast<uint64_t>(std::popcount(mask_c & lomaskR))
    };
}

// ── locate ───────────────────────────────────────────────────────────────────
// Localiza SA[i] via LF-mapping com text-position sampling.
// O(SA_SAMPLE): lookup O(1) no bitmap sdsl em vez de busca binária.
//
// Multi-sentinel safety: linhas BWT com valor '$' são amostradas no build(),
// então o LF-walk normalmente encontra uma amostra antes de fazer LF-step
// sobre um '$'. O guard c==0 é rede de segurança para corrupção.

uint64_t GfaSequenceIndex::locate(int64_t i) const noexcept {
    int64_t steps = 0;
    int64_t cur = i;
    // BUG-FM-2: limite evita loop infinito se blocks_ estiver corrompido.
    for (int64_t guard = 0; guard <= SA_SAMPLE * 2; ++guard) {
        if (cur < 0 || cur >= static_cast<int64_t>(N_)) {
            return kInvalidPos;
        }
        if (sampled_bv_[static_cast<size_t>(cur)]) {
            const int64_t val = sampled_vals_[sampled_rank_(static_cast<size_t>(cur))];
            return static_cast<uint64_t>((val + steps) % static_cast<int64_t>(N_));
        }
        const uint8_t c = bwt_[static_cast<size_t>(cur)];
        // Sentinel guard: '$' (c==0) tem rank ambíguo com múltiplos sentinelas.
        if (c == 0) { return kInvalidPos; }
        cur = static_cast<int64_t>(C_[c]) + static_cast<int64_t>(get_occ(c, cur - 1));
        ++steps;
    }
    return kInvalidPos;
}

// ── search_kmer ──────────────────────────────────────────────────────────────
// Backward search. Retorna offsets (0-based) na string concatenada.
// Rejeita silenciosamente k-mers vazios, com '$' ou com caracteres fora de
// {A,C,G,N,T} — nesses casos não há match possível.

std::vector<uint64_t> GfaSequenceIndex::search_kmer(std::string_view kmer) const {
    if (kmer.empty() || N_ == 0) { return {}; }
    for (const char ch : kmer) {
        const uint8_t m = map_char(ch);
        if (m == 0 || m == 6) { return {}; } // 0='$' (sentinel), 6=char inválido
    }

    int64_t L = 0;
    int64_t R = static_cast<int64_t>(N_) - 1;

    for (int i = static_cast<int>(kmer.size()) - 1; i >= 0; --i) {
        const uint8_t c = map_char(kmer[i]);
        // PERF-FM-3: get_occ_pair funde as duas chamadas em 1 cache miss
        // quando L-1 e R caem no mesmo OccBlock.
        const auto [occL, occR] = get_occ_pair(c, L - 1, R);
        L = static_cast<int64_t>(C_[c]) + static_cast<int64_t>(occL);
        R = static_cast<int64_t>(C_[c]) + static_cast<int64_t>(occR) - 1;
        if (L > R) { return {}; }
    }

    std::vector<uint64_t> offsets;
    offsets.reserve(static_cast<size_t>(R - L + 1));
    for (int64_t i = L; i <= R; ++i) {
        const uint64_t pos = locate(i);
        if (pos != kInvalidPos) {
            offsets.push_back(pos);
        }
    }
    return offsets;
}

// ── build ────────────────────────────────────────────────────────────────────

void GfaSequenceIndex::build(const std::vector<char>& seq_chars, int num_threads) {
    const int64_t N = static_cast<int64_t>(seq_chars.size());
    if (N == 0) { return; }

    using clock = std::chrono::high_resolution_clock;
    const auto t0 = clock::now();

    const int nt = std::max(1, num_threads);
    cli::step_info(fmt::format("SA (libsais64_omp) · N={} · threads={}", cli::format_count(N), nt));

    // ── Pré-mapeamento paralelo: ASCII → alfabeto denso 0..6 ─────────────────
    // Permite que libsais trabalhe com alfabeto compacto e que o BWT loop
    // leia símbolos já mapeados (elimina map_char por iteração).
    std::vector<uint8_t> mapped(static_cast<size_t>(N));
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(nt)
    for (int64_t i = 0; i < N; ++i) {
        mapped[static_cast<size_t>(i)] = map_char(seq_chars[static_cast<size_t>(i)]);
    }
#else
    for (int64_t i = 0; i < N; ++i) {
        mapped[static_cast<size_t>(i)] = map_char(seq_chars[static_cast<size_t>(i)]);
    }
#endif

    // freq[0..255]: libsais escreve a contagem de cada byte em mapped[].
    // Como mapped está em [0..6], freq[0..6] é o char_counts desejado.
    int64_t freq[256] = {0};

    // ── SA temporário (liberado após BWT extraction) ──────────────────────────
    // make_unique_for_overwrite: evita 8N bytes de stores zero (C++20/23).
    auto sa_buf = std::make_unique_for_overwrite<int64_t[]>(static_cast<size_t>(N));
    int64_t* const sa = sa_buf.get();

#ifdef LIBSAIS_OPENMP
    if (libsais64_omp(mapped.data(), sa, N, 0, freq, static_cast<int64_t>(num_threads)) != 0) {
        throw std::runtime_error("Falha fatal em libsais64_omp");
    }
#else
    if (libsais64(mapped.data(), sa, N, 0, freq) != 0) {
        throw std::runtime_error("Falha fatal em libsais64");
    }
#endif

    const auto t1 = clock::now();
    cli::step_ok("SA pronto",
                 ggql::format_seconds(std::chrono::duration<double>(t1 - t0).count()));

    // ── BWT + text-position sampling: scan paralelo ───────────────────────────
    // Cada thread cobre uma faixa contígua de linhas BWT (schedule static),
    // logo os samples de cada thread já estão ordenados por row.
    // Concatenar as listas na ordem das threads produz a ordenação global.
    bwt_.resize(static_cast<size_t>(N));

    ggql::ProgressTracker tracker_bwt(static_cast<size_t>(N), "FM-Index BWT", true);

    using SaPair = std::pair<int64_t, int64_t>;
    std::vector<std::vector<SaPair>> thr_samples(static_cast<size_t>(nt));
    const size_t est_per_thread =
        static_cast<size_t>(N / SA_SAMPLE) / static_cast<size_t>(nt) + 1000;
    for (auto& v : thr_samples) { v.reserve(est_per_thread); }

#ifdef _OPENMP
    #pragma omp parallel num_threads(nt)
    {
        const int tid = omp_get_thread_num();
        auto& ls = thr_samples[static_cast<size_t>(tid)];
        size_t local_bwt = 0;
    #pragma omp for schedule(static) nowait
        for (int64_t i = 0; i < N; ++i) {
            const int64_t sa_val = sa[i];
            const uint8_t m = (sa_val == 0) ? 0 : mapped[static_cast<size_t>(sa_val - 1)];
            bwt_[static_cast<size_t>(i)] = m;
            const bool is_regular    = (sa_val & (SA_SAMPLE - 1)) == 0;
            const bool is_sentinel   = (m == 0);
            if (is_regular || is_sentinel) { ls.push_back({i, sa_val}); }
            if (++local_bwt > 2 * 1024 * 1024) {
                tracker_bwt.add_progress(local_bwt);
                local_bwt = 0;
            }
        }
        if (local_bwt > 0) { tracker_bwt.add_progress(local_bwt); }
    }
#else
    size_t local_bwt = 0;
    for (int64_t i = 0; i < N; ++i) {
        const int64_t sa_val = sa[i];
        const uint8_t m = (sa_val == 0) ? 0 : mapped[static_cast<size_t>(sa_val - 1)];
        bwt_[static_cast<size_t>(i)] = m;
        const bool is_regular  = (sa_val & (SA_SAMPLE - 1)) == 0;
        const bool is_sentinel = (m == 0);
        if (is_regular || is_sentinel) { thr_samples[0].push_back({i, sa_val}); }
        if (++local_bwt > 2 * 1024 * 1024) {
            tracker_bwt.add_progress(local_bwt);
            local_bwt = 0;
        }
    }
    if (local_bwt > 0) { tracker_bwt.add_progress(local_bwt); }
#endif
    tracker_bwt.finish();

    // Libera SA e mapped — não são mais necessários.
    sa_buf.reset();
    { std::vector<uint8_t> tmp; mapped.swap(tmp); }

    // ── Bitmap + array compacto de amostras ───────────────────────────────────
    // sampled_bv_[row] = 1 se row tem amostra; sampled_vals_ guarda os SA values
    // compactados; rank support mapeia row → índice em vals.
    size_t total_samp = 0;
    for (const auto& v : thr_samples) { total_samp += v.size(); }

    sampled_bv_ = sdsl::bit_vector(static_cast<size_t>(N), 0);
    for (const auto& v : thr_samples) {
        for (const auto& [row, val] : v) {
            sampled_bv_[static_cast<size_t>(row)] = 1;
        }
    }

    // PERF-FM-4: fill paralelo de sampled_vals_ via prefix-sum de offsets por thread.
    // schedule(static) no BWT loop garante ordem global crescente — invariante do rank.
    sampled_vals_.resize(total_samp);
    {
        std::vector<size_t> thr_off(static_cast<size_t>(nt) + 1, 0);
        for (int t = 0; t < nt; ++t) {
            thr_off[static_cast<size_t>(t) + 1] =
                thr_off[static_cast<size_t>(t)] + thr_samples[static_cast<size_t>(t)].size();
        }
#ifdef _OPENMP
        #pragma omp parallel for schedule(static) num_threads(nt)
        for (int t = 0; t < nt; ++t) {
            size_t vi = thr_off[static_cast<size_t>(t)];
            for (const auto& [row, val] : thr_samples[static_cast<size_t>(t)]) {
                sampled_vals_[vi++] = val;
            }
        }
#else
        size_t vi = 0;
        for (const auto& v : thr_samples) {
            for (const auto& [row, val] : v) { sampled_vals_[vi++] = val; }
        }
#endif
    }
    sdsl::util::init_support(sampled_rank_, &sampled_bv_);

    // ── Array C ───────────────────────────────────────────────────────────────
    // freq[0..6] já populado por libsais em espaço mapeado.
    C_[0] = 0;
    for (int k = 1; k < 8; ++k) {
        C_[k] = C_[k - 1] + static_cast<uint64_t>(freq[k - 1]);
    }
    N_ = static_cast<uint64_t>(N);

    // ── Tabela Occ: 2 passes paralelos (PERF-FM-2) ──────────────────────────
    const size_t num_blocks = (static_cast<size_t>(N) + 63) / 64;
    blocks_.assign(num_blocks, OccBlock{});

    ggql::ProgressTracker tracker_occ(static_cast<size_t>(N), "FM-Index Occ", true);

    // Pass 1: máscaras (cada bloco é independente)
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(nt)
#endif
    for (size_t blk = 0; blk < num_blocks; ++blk) {
        const size_t base = blk * 64;
        const size_t end = std::min(base + 64, static_cast<size_t>(N));
        uint64_t masks[8] = {0};
        for (size_t i = base; i < end; ++i) {
            masks[bwt_[i]] |= (1ULL << (i - base));
        }
        blocks_[blk].mask = std::to_array(masks);
        tracker_occ.add_progress(end - base);
    }

    // Pass 2a: cada thread acumula contagens do seu range de blocos
    std::vector<std::array<uint64_t, 8>> thr_counts(
        static_cast<size_t>(nt), std::array<uint64_t, 8>{});
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(nt)
#endif
    for (int t = 0; t < nt; ++t) {
        const size_t lo = static_cast<size_t>(t) * num_blocks / static_cast<size_t>(nt);
        const size_t hi = (static_cast<size_t>(t) + 1) * num_blocks / static_cast<size_t>(nt);
        auto& tc = thr_counts[static_cast<size_t>(t)];
        for (size_t blk = lo; blk < hi; ++blk) {
            for (int c = 0; c < 8; ++c) {
                tc[c] += static_cast<uint64_t>(std::popcount(blocks_[blk].mask[c]));
            }
        }
    }

    // Pass 2b: prefix-sum serial → offset inicial por thread por símbolo
    std::vector<std::array<uint64_t, 8>> thr_starts(
        static_cast<size_t>(nt), std::array<uint64_t, 8>{});
    for (int t = 1; t < nt; ++t) {
        for (int c = 0; c < 8; ++c) {
            thr_starts[static_cast<size_t>(t)][c] =
                thr_starts[static_cast<size_t>(t - 1)][c] +
                thr_counts[static_cast<size_t>(t - 1)][c];
        }
    }

    // Pass 2c: cada thread preenche occ para seu range com running total local
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) num_threads(nt)
#endif
    for (int t = 0; t < nt; ++t) {
        const size_t lo = static_cast<size_t>(t) * num_blocks / static_cast<size_t>(nt);
        const size_t hi = (static_cast<size_t>(t) + 1) * num_blocks / static_cast<size_t>(nt);
        uint64_t cur[8];
        for (int c = 0; c < 8; ++c) { cur[c] = thr_starts[static_cast<size_t>(t)][c]; }
        for (size_t blk = lo; blk < hi; ++blk) {
            blocks_[blk].occ = std::to_array(cur);
            for (int c = 0; c < 8; ++c) {
                cur[c] += static_cast<uint64_t>(std::popcount(blocks_[blk].mask[c]));
            }
        }
    }

    tracker_occ.finish();

    const auto t2 = clock::now();
    cli::step_ok_end(
        "FM-Index",
        fmt::format("{} · amostras {} · blocos Occ {} · ≈ {}",
                    ggql::format_seconds(std::chrono::duration<double>(t2 - t0).count()),
                    cli::format_count(sampled_vals_.size()),
                    cli::format_count(blocks_.size()),
                    ggql::format_size(N_ + N_ / 8 + sampled_vals_.size() * 8 +
                                      blocks_.size() * 128)));
}

// ── compute_payload_hash ─────────────────────────────────────────────────────

uint64_t GfaSequenceIndex::compute_payload_hash() const noexcept {
    const uint64_t bv_words = (sampled_bv_.size() + 63) / 64;
    // BUG-FM-1: blocks_ incluído para detectar corrupção silenciosa em disco.
    ChecksumBuilder cb;
    cb.add(N_).add(C_).add(bwt_)
      .add(sampled_bv_.data(), bv_words * 8)
      .add(sampled_vals_);
    for (const auto& blk : blocks_) {
        cb.add(blk.occ);
    }
    return cb.finalize();
}

// ── save ─────────────────────────────────────────────────────────────────────

void GfaSequenceIndex::save(const std::string& dir) const {
    if (N_ == 0) { return; }

    std::ofstream out(dir + "/fm_index.bin", std::ios::binary);
    if (!out) {
        throw std::runtime_error("FM-Index: não foi possível abrir " + dir +
                                 "/fm_index.bin para escrita");
    }

    out.write(reinterpret_cast<const char*>(&kMagicV7), sizeof(kMagicV7));
    out.write(reinterpret_cast<const char*>(&N_), sizeof(N_));
    out.write(reinterpret_cast<const char*>(C_), sizeof(C_));
    out.write(reinterpret_cast<const char*>(bwt_.data()), static_cast<std::streamsize>(N_));

    const uint64_t bv_size  = sampled_bv_.size();
    const uint64_t bv_words = (bv_size + 63) / 64;
    out.write(reinterpret_cast<const char*>(&bv_size), sizeof(bv_size));
    out.write(reinterpret_cast<const char*>(sampled_bv_.data()),
              static_cast<std::streamsize>(bv_words * 8));

    const uint64_t nsamp = sampled_vals_.size();
    out.write(reinterpret_cast<const char*>(&nsamp), sizeof(nsamp));
    out.write(reinterpret_cast<const char*>(sampled_vals_.data()),
              static_cast<std::streamsize>(nsamp * sizeof(int64_t)));

    const uint64_t nblocks = blocks_.size();
    out.write(reinterpret_cast<const char*>(&nblocks), sizeof(nblocks));
    out.write(reinterpret_cast<const char*>(blocks_.data()),
              static_cast<std::streamsize>(nblocks * sizeof(OccBlock)));

    const uint64_t hash = compute_payload_hash();
    out.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    if (!out) {
        throw std::runtime_error("FM-Index: falha de I/O ao escrever fm_index.bin");
    }
}

// ── load ─────────────────────────────────────────────────────────────────────

void GfaSequenceIndex::load(const std::string& dir) {
    std::ifstream in(dir + "/fm_index.bin", std::ios::binary);
    if (!in) { return; }

    auto fail = [&](const char* msg) {
        N_ = 0;
        bwt_.clear();
        sampled_vals_.clear();
        blocks_.clear();
        throw std::runtime_error(std::string("FM-Index corrompido (") + msg + "): " +
                                 dir + "/fm_index.bin");
    };

    uint64_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (!in) { fail("header truncado"); }
    if (magic != kMagicV7) { N_ = 0; return; } // formato antigo — caller regenera

    in.read(reinterpret_cast<char*>(&N_), sizeof(N_));
    if (!in) { fail("N_ truncado"); }
    if (N_ == 0 || N_ > (64ULL << 30)) { fail("N_ fora de limites"); }

    in.read(reinterpret_cast<char*>(C_), sizeof(C_));
    if (!in) { fail("C_ truncado"); }

    bwt_.resize(N_);
    in.read(reinterpret_cast<char*>(bwt_.data()), static_cast<std::streamsize>(N_));
    if (!in) { fail("BWT truncado"); }

    uint64_t bv_size = 0;
    in.read(reinterpret_cast<char*>(&bv_size), sizeof(bv_size));
    if (!in) { fail("bv_size truncado"); }
    if (bv_size != N_) { fail("bv_size != N_"); }
    const uint64_t bv_words = (bv_size + 63) / 64;
    sampled_bv_ = sdsl::bit_vector(bv_size, 0);
    in.read(reinterpret_cast<char*>(sampled_bv_.data()),
            static_cast<std::streamsize>(bv_words * 8));
    if (!in) { fail("bitmap truncado"); }

    uint64_t nsamp = 0;
    in.read(reinterpret_cast<char*>(&nsamp), sizeof(nsamp));
    if (!in) { fail("nsamp truncado"); }
    if (nsamp > N_) { fail("nsamp fora de limites"); }
    sampled_vals_.resize(nsamp);
    in.read(reinterpret_cast<char*>(sampled_vals_.data()),
            static_cast<std::streamsize>(nsamp * sizeof(int64_t)));
    if (!in) { fail("sampled_vals truncado"); }

    sdsl::util::init_support(sampled_rank_, &sampled_bv_);

    uint64_t nblocks = 0;
    in.read(reinterpret_cast<char*>(&nblocks), sizeof(nblocks));
    if (!in) { fail("nblocks truncado"); }
    const uint64_t expected_blocks = (N_ + 63) / 64;
    if (nblocks != expected_blocks) { fail("nblocks inconsistente com N_"); }
    blocks_.resize(nblocks);
    in.read(reinterpret_cast<char*>(blocks_.data()),
            static_cast<std::streamsize>(nblocks * sizeof(OccBlock)));
    if (!in) { fail("blocks truncado"); }

    uint64_t stored_hash = 0;
    in.read(reinterpret_cast<char*>(&stored_hash), sizeof(stored_hash));
    if (!in) { fail("hash truncado"); }
    if (stored_hash != compute_payload_hash()) { fail("hash XXH64 divergente"); }
}

} // namespace ggql
