#pragma once

// FM-Index sobre sequências de segmentos GFA.
//
// Design (v5 — bitmap-sampled locate):
//   - Pré-mapeamento paralelo: input ASCII → alfabeto denso 0..6 antes de libsais.
//   - libsais64_omp: SA completo temporário; BWT e amostras em scan paralelo.
//   - SA allocation: make_unique_for_overwrite — evita 8N bytes de zero-init.
//   - Text-position sampling (SA[i] % r == 0 OU BWT[i]=='$').
//   - r = SA_SAMPLE = 32 (potência de 2).
//   - get_occ(): popcount em chunk de 64 bits — O(1) por chamada.
//   - locate(): bitmap O(1) via sdsl::bit_vector + rank_support_v.
//   - OccBlock{occ,mask}: AoS — 128 bytes contíguos.
//   - CHAR_MAP[256]: lookup table constexpr — O(1), zero branches.
//
// Complexidade:
//   Build RAM pico : 11N bytes (seq N + mapped N + SA 8N + BWT N)
//   fm_index.bin   : N (BWT) + N/8 (bitmap) + S×8 (vals) + (N/64)×128 (Occ)
//   Busca locate   : O(r) com bitmap O(1) e get_occ O(1) via popcount
//
// Implementação: src/storage/gfa_fm_index.cpp
// (inclui os headers pesados — libsais64, omp, chrono, fstream, fmt)

#include <array>
#include <cstdint>
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
    void build(const std::vector<char>& seq_chars, int num_threads = 1);

    // Backward search sobre BWT. Retorna offsets (0-based) na string concatenada.
    // Retorna vazio para k-mers com '$', caracteres fora de {A,C,G,N,T} ou N_==0.
    [[nodiscard]] std::vector<uint64_t> search_kmer(std::string_view kmer) const;

    // Hash XXH64 sobre N_, C_, bwt_, bitmap, sampled_vals_ e occ fields.
    [[nodiscard]] uint64_t compute_payload_hash() const noexcept;

    // Serialização em disco (lança std::runtime_error em falha de I/O).
    void save(const std::string& dir) const;

    // Deserialização. Arquivo ausente: silencioso (N_=0). Arquivo corrompido:
    // lança std::runtime_error — o chamador decide rebuild vs. abort.
    void load(const std::string& dir);

    [[nodiscard]] bool is_loaded() const noexcept { return N_ > 0; }

    // Layout v7 em disco (fm_index.bin):
    //   magic (8B)=FMIDX7 | N_ (8B) | C_[8] (64B)
    //   bwt_ (N B)
    //   bv_size (8B) | sampled_bv_ raw (ceil(N/64)*8 B)
    //   nsamp (8B) | sampled_vals_ (nsamp*8 B)
    //   nblocks (8B) | blocks_ (nblocks*128 B, AoS: occ[8]+mask[8] interleaved)
    //   xxh64_hash (8B)
    static constexpr uint64_t kMagicV7 = 0x464D49445837ULL; // "FMIDX7"

  private:
    // AoS de 128 bytes = 2 cache lines (occ e mask sempre carregados juntos).
    struct OccBlock {
        std::array<uint64_t, 8> occ;  // contagem acumulada até o bloco
        std::array<uint64_t, 8> mask; // bitmask 64 posições por símbolo
    };

    static constexpr uint64_t kInvalidPos = UINT64_MAX;

    // Mapeamento ASCII → símbolo denso [0..6]. Implementado no .cpp.
    // Permanece privado — callers usam search_kmer() como API pública.
    static uint8_t map_char(char c) noexcept;

    [[nodiscard]] uint64_t get_occ(uint8_t c, int64_t idx) const noexcept;
    [[nodiscard]] std::pair<uint64_t, uint64_t>
    get_occ_pair(uint8_t c, int64_t idxL, int64_t idxR) const noexcept;
    [[nodiscard]] uint64_t locate(int64_t i) const noexcept;

    uint64_t N_ = 0;
    uint64_t C_[8] = {0};
    std::vector<uint8_t> bwt_;
    sdsl::bit_vector sampled_bv_;          // N bits: 1 se row tem amostra
    sdsl::rank_support_v<1> sampled_rank_; // rank support para O(1)
    std::vector<int64_t> sampled_vals_;    // valores SA compactados
    std::vector<OccBlock> blocks_;
};

} // namespace ggql
