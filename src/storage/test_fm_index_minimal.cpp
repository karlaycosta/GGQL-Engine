// Reproducer mínimo para o bug do FM-Index observado no oráculo.
// Constrói FM-Index em RAM sobre texto pequeno conhecido e valida search_kmer.
// Uso: ./build/test_fm_index_minimal
#include "ggql/storage/gfa_fm_index.hpp"

#include <cassert>
#include <fmt/core.h>
#include <fmt/color.h>
#include <string>
#include <vector>

int main() {
    // Texto multi-segmento com sentinelas: 3 segmentos pequenos.
    // seg 0: 'ACGTACGTACGT' @ 0..12  ($ em 12)
    // seg 1: 'TTTTGGGGAAAA' @ 13..25 ($ em 25)
    // seg 2: 'ACGTAAAAGGGG' @ 26..38 ($ em 38)
    const std::string s0 = "ACGTACGTACGT";
    const std::string s1 = "TTTTGGGGAAAA";
    const std::string s2 = "ACGTAAAAGGGG";

    std::vector<char> text;
    for (char c : s0) text.push_back(c); text.push_back('$');
    const size_t off1 = text.size();
    for (char c : s1) text.push_back(c); text.push_back('$');
    const size_t off2 = text.size();
    for (char c : s2) text.push_back(c); text.push_back('$');

    fmt::println("Texto: {} bytes (N={})", text.size(), text.size());
    fmt::println("  seg0 @ 0..{} : '{}'", s0.size(), s0);
    fmt::println("  seg1 @ {}..+{} : '{}'", off1, s1.size(), s1);
    fmt::println("  seg2 @ {}..+{} : '{}'", off2, s2.size(), s2);
    fmt::println("Concatenado: '{}'", std::string(text.begin(), text.end()));

    ggql::GfaSequenceIndex idx;
    idx.build(text, 1);

    // Ground truth: suffix array e BWT manualmente
    const int64_t N = static_cast<int64_t>(text.size());
    std::vector<int64_t> sa_ref(N);
    for (int64_t i = 0; i < N; ++i) sa_ref[i] = i;
    std::sort(sa_ref.begin(), sa_ref.end(), [&](int64_t a, int64_t b){
        for (int64_t k = 0; a+k < N && b+k < N; ++k) {
            if (text[a+k] != text[b+k]) return text[a+k] < text[b+k];
        }
        return (N - a) < (N - b);
    });
    fmt::println("\nSA (referência): tamanho {}", sa_ref.size());
    for (int64_t i = 0; i < N; ++i) {
        std::string suf(text.begin() + sa_ref[i], text.end());
        if (suf.size() > 20) suf = suf.substr(0, 20) + "...";
        fmt::println("  row {:2} SA={:2}  BWT='{}'  suffix='{}'",
                     i, sa_ref[i],
                     sa_ref[i] == 0 ? '$' : text[sa_ref[i] - 1],
                     suf);
    }

    auto check = [&](const std::string& k, const std::vector<uint64_t>& expected) {
        auto hits = idx.search_kmer(k);
        std::sort(hits.begin(), hits.end());
        std::vector<uint64_t> sorted_expected = expected;
        std::sort(sorted_expected.begin(), sorted_expected.end());
        bool ok = (hits == sorted_expected);

        fmt::print("  search('{}') = [", k);
        for (size_t i = 0; i < hits.size(); ++i) {
            if (i) fmt::print(", ");
            fmt::print("{}", hits[i]);
        }
        fmt::print("]  esperado=[");
        for (size_t i = 0; i < sorted_expected.size(); ++i) {
            if (i) fmt::print(", ");
            fmt::print("{}", sorted_expected[i]);
        }
        fmt::println("]  {}", ok ? "OK" : "FAIL");

        if (!ok) {
            for (uint64_t h : hits) {
                std::string at(&text[h], k.size());
                fmt::println("      hit={} content='{}' match={}",
                             h, at, at == k);
            }
        }
        return ok;
    };

    bool all_ok = true;
    // 'ACGT' aparece em pos 0, 4, 8 (seg0) e 26 (seg2)
    all_ok &= check("ACGT", {0, 4, 8, 26});
    // 'TTTT' aparece em pos 13
    all_ok &= check("TTTT", {13});
    // 'GGGG' aparece em pos 17 e 34
    all_ok &= check("GGGG", {17, 34});
    // 'AAAA' aparece em pos 21 e 30
    all_ok &= check("AAAA", {21, 30});
    // 'CGTA' aparece em pos 1, 5 (seg0) e 27 (seg2)
    all_ok &= check("CGTA", {1, 5, 27});
    // k-mer atravessando fronteira não deve ser encontrado (tem '$')
    all_ok &= check("ACGT$", {}); // rejeitado pela validação
    // k-mer único em cada segmento
    all_ok &= check("GTACGTACGT", {2});

    fmt::println("\nResultado: {}", all_ok ? "TODOS OK" : "FALHAS DETECTADAS");
    return all_ok ? 0 : 1;
}
