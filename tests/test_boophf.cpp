// test_boophf.cpp — Baseline test suite for include/mphf/BooPHF.h.
// Cobre: bijeção, save/load round-trip, determinismo de bijeção entre threads,
// fastmode on/off, e edge cases (n=0, n=1).
//
// Não-coberto (fora do escopo de correção):
//   - latência de lookup, build time, RAM peak — ver bench_boophf.cpp.
//   - branch fastmode-overflow (BooPHF.h:402–406): difícil forçar
//     deterministicamente; depende de hash patológico.

#include "mphf/BooPHF.h"

#include <cassert>
#include <climits>
#include <cstdint>
#include <print>
#include <random>
#include <set>
#include <sstream>
#include <vector>

// splitmix64 — hasher determinístico independente de libs externas.
struct U64Hasher {
    [[nodiscard]] uint64_t operator()(uint64_t key, uint64_t seed = 0) const noexcept {
        uint64_t z = key + seed + 0x9E3779B97F4A7C15ULL;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

using mphf_t = boomphf::mphf<uint64_t, U64Hasher>;

// keys únicas e determinísticas por seed.
static std::vector<uint64_t> make_keys(size_t n, uint64_t seed = 42) {
    std::set<uint64_t> uniq;
    std::mt19937_64 rng(seed);
    while (uniq.size() < n) {
        uniq.insert(rng());
    }
    return {uniq.begin(), uniq.end()};
}

// ── 1. Edge cases ────────────────────────────────────────────────────────────

static void test_n0() {
    std::vector<uint64_t> empty;
    mphf_t m(0, empty, 1, 2.0, 0.0);
    assert(m.nbKeys() == 0);
    // ctor com n==0 retorna cedo, _built fica false → lookup retorna sentinela.
    assert(m.lookup(42) == ULLONG_MAX);
}

static void test_n1() {
    auto keys = make_keys(1);
    mphf_t m(1, keys, 1, 2.0, 0.0);
    assert(m.nbKeys() == 1);
    auto idx = m.lookup(keys[0]);
    assert(idx == 0); // bijeção com n=1 → único índice possível.
}

// ── 2. Bijeção ───────────────────────────────────────────────────────────────

static void verify_bijection(const mphf_t& m, const std::vector<uint64_t>& keys) {
    const size_t n = keys.size();
    std::vector<bool> seen(n, false);
    for (uint64_t k : keys) {
        uint64_t idx = m.lookup(k);
        assert(idx < n && "lookup retornou índice fora de [0, n)");
        assert(!seen[idx] && "lookup colidiu — não é bijeção");
        seen[idx] = true;
    }
    for (bool b : seen) {
        assert(b && "algum índice em [0, n) ficou sem cobertura");
    }
}

static void test_bijection_grid() {
    for (size_t n : {64UZ, 1000UZ, 100'000UZ}) {
        for (double gamma : {1.0, 2.0, 5.0}) {
            for (int t : {1, 4}) {
                for (float fm : {0.0F, 0.03F}) {
                    auto keys = make_keys(n, n + static_cast<uint64_t>(gamma * 100));
                    mphf_t m(n, keys, t, gamma, fm);
                    verify_bijection(m, keys);
                }
            }
        }
    }
}

// ── 3. Save/load round-trip ─────────────────────────────────────────────────

static void test_save_load_roundtrip() {
    for (size_t n : {1UZ, 64UZ, 1000UZ, 100'000UZ}) {
        auto keys = make_keys(n);
        mphf_t orig(n, keys, 2, 2.0, 0.03F);

        // capturar mapping original key→idx para comparar pós-load.
        std::vector<uint64_t> mapping(n);
        for (size_t i = 0; i < n; ++i) {
            mapping[i] = orig.lookup(keys[i]);
        }

        std::stringstream buf(std::ios::in | std::ios::out | std::ios::binary);
        orig.save(buf);

        mphf_t loaded;
        loaded.load(buf);

        assert(loaded.nbKeys() == n);
        for (size_t i = 0; i < n; ++i) {
            assert(loaded.lookup(keys[i]) == mapping[i]);
        }
    }
}

// ── 4. Determinismo entre nº de threads ──────────────────────────────────────
//
// Mesmo input, threads diferentes → bijeção válida em ambos. Os mappings
// específicos (key→idx) podem diferir devido à ordem de inserção paralela
// no _final_hash — verificamos só que ambos cobrem [0, n) sem colisão.

static void test_threads_determinism() {
    auto keys = make_keys(10'000);
    mphf_t m1(keys.size(), keys, 1, 2.0, 0.03F);
    mphf_t m4(keys.size(), keys, 4, 2.0, 0.03F);
    verify_bijection(m1, keys);
    verify_bijection(m4, keys);
}

// ── 5. Fastmode on/off ──────────────────────────────────────────────────────

static void test_fastmode_variants() {
    auto keys = make_keys(50'000);
    for (float fm : {0.0F, 0.03F, 0.10F}) {
        mphf_t m(keys.size(), keys, 4, 2.0, fm);
        verify_bijection(m, keys);
    }
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    test_n0();
    std::print("  ✓ n=0\n");

    test_n1();
    std::print("  ✓ n=1\n");

    test_bijection_grid();
    std::print("  ✓ bijection grid (n × gamma × threads × fastmode)\n");

    test_save_load_roundtrip();
    std::print("  ✓ save/load round-trip\n");

    test_threads_determinism();
    std::print("  ✓ thread-count determinism\n");

    test_fastmode_variants();
    std::print("  ✓ fastmode variants\n");

    std::print("test_boophf: all checks passed\n");
    return 0;
}
