// test_storage_fm_index.cpp — Testes unitários de GfaSequenceIndex (FM-Index).
// Verifica: build, is_loaded, search_kmer (encontrado/não encontrado), save/load,
// count_kmer (regressão A: DoS-proof contagem sem materializar offsets),
// search_kmer com max_results (regressão A: cap de hits para k-mers de alto
// recall).

#include "ggql/storage/fm_index.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

static void test_default_not_loaded() {
    ggql::GfaSequenceIndex idx;
    assert(!idx.is_loaded());
}

static void test_build_single_segment() {
    // "ACGT$" — um segmento com sentinela
    std::vector<char> seq = {'A', 'C', 'G', 'T', '$'};
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);
    assert(idx.is_loaded());
}

static void test_search_kmer_found() {
    std::vector<char> seq = {'A', 'C', 'G', 'T', '$'};
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    auto h1 = idx.search_kmer("ACG");
    assert(h1.size() == 1 && h1[0] == 0);

    auto h2 = idx.search_kmer("CGT");
    assert(h2.size() == 1 && h2[0] == 1);

    auto h3 = idx.search_kmer("GTA");  // não existe
    assert(h3.empty());
}

static void test_search_kmer_invalid_chars() {
    std::vector<char> seq = {'A', 'C', 'G', 'T', '$'};
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    assert(idx.search_kmer("XYZ").empty());
    assert(idx.search_kmer("ACG$").empty());   // kmer com '$'
    assert(idx.search_kmer("ACGTTTTTTT").empty()); // mais longo que a seq
}

static void test_search_kmer_two_segments() {
    // "ACGT$AAAA$" — dois segmentos
    // Posições: 0=A 1=C 2=G 3=T 4=$ 5=A 6=A 7=A 8=A 9=$
    std::vector<char> seq = {'A','C','G','T','$','A','A','A','A','$'};
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    // "CGT" aparece apenas no primeiro segmento, offset 1
    auto h1 = idx.search_kmer("CGT");
    assert(h1.size() == 1 && h1[0] == 1);

    // "AAA" aparece duas vezes em "AAAA": offsets 5 e 6
    auto h2 = idx.search_kmer("AAA");
    assert(h2.size() == 2);
    for (uint64_t h : h2) { assert(h == 5 || h == 6); }

    // "ACG" só no primeiro segmento
    auto h3 = idx.search_kmer("ACG");
    assert(h3.size() == 1 && h3[0] == 0);
}

static void test_save_load_roundtrip() {
    const std::string dir = "/tmp/ggql_test_fmidx/";
    std::filesystem::create_directories(dir);

    std::vector<char> seq = {'A','C','G','T','$','A','A','A','A','$'};

    ggql::GfaSequenceIndex idx1;
    idx1.build(seq, 1);
    auto save_r = idx1.save(dir);
    assert(save_r.has_value());

    ggql::GfaSequenceIndex idx2;
    auto load_r = idx2.load(dir);
    assert(load_r.has_value());
    assert(idx2.is_loaded());

    // Resultados idênticos após reload
    auto h1a = idx1.search_kmer("CGT");
    auto h2a = idx2.search_kmer("CGT");
    std::sort(h1a.begin(), h1a.end());
    std::sort(h2a.begin(), h2a.end());
    assert(h1a == h2a);

    auto h1b = idx1.search_kmer("AAA");
    auto h2b = idx2.search_kmer("AAA");
    std::sort(h1b.begin(), h1b.end());
    std::sort(h2b.begin(), h2b.end());
    assert(h1b == h2b);
    assert(h1b.size() == 2);

    std::filesystem::remove_all(dir);
}

static void test_hash_consistent() {
    std::vector<char> seq = {'A','C','G','T','$'};
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);
    auto h1 = idx.compute_payload_hash();
    auto h2 = idx.compute_payload_hash();
    assert(h1 == h2);
    assert(h1 != 0);
}

// ── Regressão A: count_kmer + max_results contra DoS por k-mer curto ────────
//
// Cenário: k-mer "A" em pangenoma resulta em ~25% × N matches. Em HPRC isso é
// ~850M offsets, ~6.8 GB de RAM no reserve(). Os testes abaixo provam:
//   1) count_kmer retorna o count exato sem materializar offsets
//   2) search_kmer(kmer, max_results=K) corta exatamente em K
//   3) search_kmer(kmer, max_results=0) preserva semântica anterior
//   4) count_kmer rejeita inválidos (mesmo critério de search_kmer)

// Constrói "AAAA...A$ACGT$" — N_A 'A's seguidos de um seg ACGT como controle.
static std::vector<char> make_a_heavy_seq(size_t n_a) {
    std::vector<char> seq;
    seq.reserve(n_a + 6);
    seq.assign(n_a, 'A');
    seq.push_back('$');
    seq.push_back('A'); seq.push_back('C'); seq.push_back('G'); seq.push_back('T');
    seq.push_back('$');
    return seq;
}

static void test_count_kmer_high_recall_no_alloc() {
    // 100K A's no primeiro segmento + 1 A no segmento "ACGT". Total = 100001.
    constexpr size_t N_A = 100'000;
    auto seq = make_a_heavy_seq(N_A);

    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    // count_kmer faz só backward search — O(1) RAM, O(|kmer|) tempo. Mesmo
    // se N_A fosse 850M (HPRC), esta linha consumiria <1 µs e zero alocações.
    const uint64_t total_a = idx.count_kmer("A");
    assert(total_a == N_A + 1);

    // K-mers curtos repetitivos são exatamente o caso adversarial:
    const uint64_t aa_count = idx.count_kmer("AA");
    assert(aa_count == N_A - 1);   // (N_A-1) AA's contíguos, A solto não conta

    // K-mer ausente
    assert(idx.count_kmer("CCCC") == 0);

    // K-mer único (controle)
    assert(idx.count_kmer("CGT") == 1);
}

static void test_count_kmer_invalid_returns_zero() {
    auto seq = make_a_heavy_seq(100);
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    assert(idx.count_kmer("") == 0);
    assert(idx.count_kmer("X") == 0);                // char inválido
    assert(idx.count_kmer("AC$") == 0);              // contém sentinel
    // Múltiplos chars inválidos não devem fazer search "rolar" pelo array C
    assert(idx.count_kmer("@@@@") == 0);
}

static void test_search_kmer_max_results_caps_exactly() {
    constexpr size_t N_A = 10'000;
    auto seq = make_a_heavy_seq(N_A);

    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    // count_kmer reporta o universo completo
    const uint64_t total = idx.count_kmer("A");
    assert(total == N_A + 1);

    // max_results=10 deve cortar exatamente em 10 — sem alocar 10K offsets
    auto cap10 = idx.search_kmer("A", 10);
    assert(cap10.size() == 10);

    // max_results=1 — caso degenerado (caller que só quer "existe?")
    auto cap1 = idx.search_kmer("A", 1);
    assert(cap1.size() == 1);

    // max_results=N_A+10 (acima do total) — deve devolver tudo, sem extras
    auto cap_over = idx.search_kmer("A", N_A + 10);
    assert(cap_over.size() == N_A + 1);

    // max_results igual ao total — borda exata
    auto cap_exact = idx.search_kmer("A", N_A + 1);
    assert(cap_exact.size() == N_A + 1);
}

static void test_search_kmer_max_results_zero_is_unbounded() {
    // Garante que max_results=0 (default) preserva semântica pré-correção:
    // retorna todos os hits.
    constexpr size_t N_A = 5'000;
    auto seq = make_a_heavy_seq(N_A);

    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    auto unbounded = idx.search_kmer("A");          // default
    auto explicit_zero = idx.search_kmer("A", 0);   // explícito
    assert(unbounded.size() == N_A + 1);
    assert(explicit_zero.size() == N_A + 1);
    assert(unbounded.size() == explicit_zero.size());
}

static void test_search_kmer_max_results_no_match_unaffected() {
    // max_results não pode introduzir falsos positivos: k-mer que não existe
    // deve continuar retornando vazio independentemente do cap.
    auto seq = make_a_heavy_seq(100);
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    assert(idx.search_kmer("CCCC", 100).empty());
    assert(idx.search_kmer("CCCC", 1).empty());
    assert(idx.search_kmer("X", 50).empty());       // inválido
}

static void test_count_search_consistency() {
    // Para todo k-mer válido: count_kmer == search_kmer().size() quando
    // max_results=0. Verifica que a refatoração compartilhou backward_search
    // corretamente — sem divergências numéricas entre as duas APIs.
    auto seq = make_a_heavy_seq(500);
    ggql::GfaSequenceIndex idx;
    idx.build(seq, 1);

    for (const char* k : {"A", "AA", "AAA", "ACGT", "CGT", "GT", "T", "CCC"}) {
        const auto search_n = idx.search_kmer(k).size();
        const auto count_n  = idx.count_kmer(k);
        assert(search_n == count_n);
    }
}

int main() {
    test_default_not_loaded();
    test_build_single_segment();
    test_search_kmer_found();
    test_search_kmer_invalid_chars();
    test_search_kmer_two_segments();
    test_save_load_roundtrip();
    test_hash_consistent();

    // Regressão A — DoS por k-mer curto / count vs locate
    test_count_kmer_high_recall_no_alloc();
    test_count_kmer_invalid_returns_zero();
    test_search_kmer_max_results_caps_exactly();
    test_search_kmer_max_results_zero_is_unbounded();
    test_search_kmer_max_results_no_match_unaffected();
    test_count_search_consistency();
    return EXIT_SUCCESS;
}
