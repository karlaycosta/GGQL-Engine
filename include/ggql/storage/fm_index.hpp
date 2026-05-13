#pragma once

// FM-Index sobre sequências de segmentos GFA.
//
// Design (V8 — bitmap-sampled locate, hash cobre mask+occ):
//   - Pré-mapeamento paralelo: input ASCII → alfabeto denso 0..6 antes de libsais.
//   - libsais64_omp: SA completo temporário; BWT e amostras em scan paralelo.
//   - SA allocation: make_unique_for_overwrite — evita 8N bytes de zero-init.
//   - Text-position sampling (SA[i] % r == 0 OU BWT[i]=='$').
//   - r = SA_SAMPLE = 32 (potência de 2).
//   - get_occ(): popcount em chunk de 64 bits — O(1) por chamada.
//   - locate(): bitmap O(1) via sdsl::bit_vector + rank_support_v.
//   - OccBlock: 128 bytes = occ[8] (64 B) seguido de mask[8] (64 B);
//     `get_occ(c)` toca 2 cache lines (uma por campo).
//   - MAP[256] em fm_index.cpp: lookup table constexpr — O(1), zero branches.
//
// Complexidade (N = soma das sequências, ~N/4 amostras com SA_SAMPLE=32):
//   Build RAM pico : ~12N bytes durante libsais64 (input N + mapped N +
//                    SA 8N + buffers internos ~2-3N). Cai para ~3,4N após
//                    sa_buf.reset(): bwt N + sampled_bv N/8 + vals ~N/4 +
//                    blocks 2N.
//   fm_index.bin   : N (BWT) + N/8 (bitmap) + ~N/4 (vals) + 2N (Occ)
//                    = ~3,4N total.
//   Busca locate   : O(r) com bitmap O(1) e get_occ O(1) via popcount.
//
// Implementação: src/storage/fm_index.cpp
// (inclui os headers pesados — libsais64, omp, chrono, fstream, fmt)

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sdsl/bit_vectors.hpp>
#include <sdsl/rank_support_v.hpp>

namespace ggql {

class GfaSequenceIndex {
  public:
    // SA_SAMPLE=32: amostra 1 em cada 32 posições do SA.
    // locate() usa bitmap + rank O(1) via sdsl em vez de lower_bound O(log S).
    static constexpr int64_t SA_SAMPLE = 32;

    GfaSequenceIndex() = default;

    // Constrói o índice a partir das sequências concatenadas de segmentos GFA.
    // seq_chars deve conter sentinelas '$' (0x24) separando cada sequência.
    void build(std::span<const char> seq_chars, int num_threads = 1);

    // Backward search sobre BWT. Retorna offsets (0-based) na string concatenada.
    // Retorna vazio para k-mers com '$', caracteres fora de {A,C,G,N,T} ou N_==0.
    //
    // max_results: limite duro de offsets materializados (0 = ilimitado).
    //   Use sempre que k-mer pode ter alto recall (ex.: k=1 em pangenoma
    //   causa ~25% × N matches → centenas de milhões de locate() + alocação
    //   gigante). Para apenas contar sem localizar, use count_kmer().
    [[nodiscard]] std::vector<uint64_t>
    search_kmer(std::string_view kmer, size_t max_results = 0) const;

    // Apenas conta ocorrências (R - L + 1 do backward search), sem chamar
    // locate() para cada hit. O(|k-mer|) — milhões de vezes mais barato
    // que search_kmer quando o objetivo é só "quantos hits?".
    // Retorna 0 para k-mers inválidos (mesmo critério de search_kmer).
    [[nodiscard]] uint64_t count_kmer(std::string_view kmer) const noexcept;

    // Hash XXH64 sobre N_, C_, bwt_, bitmap, sampled_vals_ e blocos
    // (mask + occ — V8). Cobertura total ≈ 3,4N bytes.
    [[nodiscard]] uint64_t compute_payload_hash() const noexcept;

    // Serialização em disco. Retorna mensagem de erro em falha de I/O.
    [[nodiscard]] std::expected<void, std::string>
    save(const std::string& dir) const;

    // Deserialização. Convenção:
    //   arquivo ausente → sucesso, N_=0 (caller checa is_loaded())
    //   magic obsoleto  → sucesso, N_=0 (caller regenera)
    //   corrompido      → std::unexpected com diagnóstico
    [[nodiscard]] std::expected<void, std::string>
    load(const std::string& dir);

    [[nodiscard]] bool is_loaded() const noexcept { return N_ > 0; }

    // Layout V8 em disco (fm_index.bin):
    //   magic (8B)=FMIDX8 | N_ (8B) | C_[8] (64B)
    //   bwt_ (N B)
    //   bv_size (8B) | sampled_bv_ raw (ceil(N/64)*8 B)
    //   nsamp (8B) | sampled_vals_ (nsamp*8 B)
    //   nblocks (8B) | blocks_ (nblocks * 128 B, cada bloco = occ[8] (64 B)
    //                            seguido de mask[8] (64 B); AoS por bloco)
    //   xxh64_hash (8B) — cobre BWT + bitmap + vals + blocks (mask+occ)
    // Caches V≤7 são rejeitados pelo load() — caller regenera.
    static constexpr uint64_t kMagicV8 = 0x464D49445838ULL; // "FMIDX8"

  private:
    // OccBlock = 128 B / bloco BWT de 64 posições.
    // Layout: occ[8] (64 B = 1 cache line, contagens acumuladas) seguido
    // de mask[8] (64 B = 1 cache line, bitmask por símbolo). get_occ(c)
    // toca ambas as cache lines: occ[c] para a base + popcount(mask[c]
    // & lomask) para o delta dentro do bloco.
    struct OccBlock {
        std::array<uint64_t, 8> occ;  // contagem acumulada até o bloco
        std::array<uint64_t, 8> mask; // bitmask 64 posições por símbolo
    };

    static constexpr uint64_t kInvalidPos = UINT64_MAX;

    // Mapeamento ASCII → símbolo denso [0..6]. Implementado no .cpp.
    // Permanece privado — callers usam search_kmer() como API pública.
    [[nodiscard]] static uint8_t map_char(char c) noexcept;

    [[nodiscard]] uint64_t get_occ(uint8_t c, int64_t idx) const noexcept;
    [[nodiscard]] std::pair<uint64_t, uint64_t>
    get_occ_pair(uint8_t c, int64_t idxL, int64_t idxR) const noexcept;
    [[nodiscard]] uint64_t locate(int64_t i) const noexcept;

    // Backward search interno — retorna {L, R} com L > R sinalizando "0 hits"
    // (k-mer vazio, com '$', char inválido, ou simplesmente não encontrado).
    // Compartilhado por count_kmer() e search_kmer().
    struct BwtRange { int64_t L; int64_t R; };
    [[nodiscard]] BwtRange backward_search(std::string_view kmer) const noexcept;

    uint64_t N_ = 0;
    uint64_t C_[8] = {0};
    std::vector<uint8_t> bwt_;
    sdsl::bit_vector sampled_bv_;          // N bits: 1 se row tem amostra
    sdsl::rank_support_v<1> sampled_rank_; // rank support para O(1)
    std::vector<int64_t> sampled_vals_;    // valores SA compactados
    std::vector<OccBlock> blocks_;
};

} // namespace ggql
