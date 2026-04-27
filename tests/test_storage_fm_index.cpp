// test_storage_fm_index.cpp — Testes unitários de GfaSequenceIndex (FM-Index).
// Verifica: build, is_loaded, search_kmer (encontrado/não encontrado), save/load.

#include "ggql/storage/gfa_fm_index.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
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
    idx1.save(dir);

    ggql::GfaSequenceIndex idx2;
    idx2.load(dir);
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

int main() {
    test_default_not_loaded();
    test_build_single_segment();
    test_search_kmer_found();
    test_search_kmer_invalid_chars();
    test_search_kmer_two_segments();
    test_save_load_roundtrip();
    test_hash_consistent();
    return EXIT_SUCCESS;
}
