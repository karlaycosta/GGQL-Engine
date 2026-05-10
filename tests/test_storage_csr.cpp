// test_storage_csr.cpp — Testes unitários de GfaCsrTopology.
// Verifica: build, graus, vizinhos, decode, find_tips, find_repeats.
// + Simetria E_fwd↔E_bwd (B-V2 repro — sessão K)

#include "ggql/storage/topology.hpp"
#include "ggql/storage/topology_types.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

// Grafo em diamante:
//   0 → 1 (+/+)
//   0 → 2 (+/+)
//   1 → 3 (+/+)
//   2 → 3 (+/+)
//
// Graus:  nó 0: in=0 out=2   nó 1: in=1 out=1
//         nó 2: in=1 out=1   nó 3: in=2 out=0

static ggql::GfaCsrTopology build_diamond() {
    using LR = ggql::LinkRecord;
    std::vector<LR> links = {
        LR::make(0, '+', 1, '+'),
        LR::make(0, '+', 2, '+'),
        LR::make(1, '+', 3, '+'),
        LR::make(2, '+', 3, '+'),
    };
    ggql::GfaCsrTopology topo;
    topo.build(4, std::move(links), 1);
    return topo;
}

static void test_dimensions() {
    auto topo = build_diamond();
    assert(topo.num_nodes() == 4);
    assert(topo.num_edges() == 4);
}

static void test_degrees() {
    auto topo = build_diamond();
    assert(topo.out_degree(0) == 2);
    assert(topo.in_degree(0)  == 0);
    assert(topo.out_degree(1) == 1);
    assert(topo.in_degree(1)  == 1);
    assert(topo.out_degree(2) == 1);
    assert(topo.in_degree(2)  == 1);
    assert(topo.out_degree(3) == 0);
    assert(topo.in_degree(3)  == 2);
}

static void test_out_of_bounds_degree() {
    auto topo = build_diamond();
    assert(topo.out_degree(99) == 0);
    assert(topo.in_degree(99)  == 0);
}

static void test_out_neighbors() {
    auto topo = build_diamond();
    auto nbrs = topo.out_neighbors(0);
    assert(nbrs.size() == 2);
    for (auto enc : nbrs) {
        auto ref = ggql::GfaCsrTopology::decode(enc);
        assert(ref.orient == '+');
        assert(ref.node_id == 1 || ref.node_id == 2);
    }
    assert(topo.out_neighbors(3).empty());
    assert(topo.in_neighbors(0).empty());
}

static void test_in_neighbors() {
    auto topo = build_diamond();
    auto nbrs = topo.in_neighbors(3);
    assert(nbrs.size() == 2);
    for (auto enc : nbrs) {
        auto ref = ggql::GfaCsrTopology::decode(enc);
        assert(ref.orient == '+');
        assert(ref.node_id == 1 || ref.node_id == 2);
    }
}

static void test_find_tips() {
    auto topo = build_diamond();
    auto tips = topo.find_tips();
    assert(tips.size() == 2);
    std::sort(tips.begin(), tips.end());
    assert(tips[0] == 0);
    assert(tips[1] == 3);
}

static void test_find_repeats_min2() {
    auto topo = build_diamond();
    auto reps = topo.find_repeats(2);
    assert(reps.size() == 1);
    assert(reps[0] == 0);
}

static void test_find_repeats_min1() {
    auto topo = build_diamond();
    auto reps = topo.find_repeats(1);
    assert(reps.size() == 3);
    std::sort(reps.begin(), reps.end());
    assert(reps[0] == 0);
    assert(reps[1] == 1);
    assert(reps[2] == 2);
}

static void test_decode_forward() {
    uint32_t enc = (5u << 1) | 0u;
    auto ref = ggql::GfaCsrTopology::decode(enc);
    assert(ref.node_id == 5);
    assert(ref.orient == '+');
}

static void test_decode_reverse() {
    uint32_t enc = (7u << 1) | 1u;
    auto ref = ggql::GfaCsrTopology::decode(enc);
    assert(ref.node_id == 7);
    assert(ref.orient == '-');
}

// ─── B-V2 repro: simetria E_fwd ↔ E_bwd ──────────────────────────────────────
//
// Para cada link u→v inserido, deve valer:
//   v ∈ {decoded(e).node_id for e in out_neighbors(u)}    (E_fwd contém v)
//   u ∈ {decoded(e).node_id for e in in_neighbors(v)}     (E_bwd contém u)
//
// Em chm13_subset_100w (8 componentes, 4M nós), 99.99% dos links violam isso.
// Estes testes localizam o ponto exato onde a simetria quebra.

// Verifica simetria global: para cada nó u, cada out_neighbor v_id, valida
// que u aparece em in_neighbors(v_id). Imprime as primeiras N divergências
// para diagnóstico. Retorna número total de pares assimétricos.
static size_t check_symmetry(const ggql::GfaCsrTopology& topo, const char* label) {
    size_t total_links = 0;
    size_t asymmetric = 0;
    constexpr size_t kMaxReport = 5;
    size_t reported = 0;
    for (uint64_t u = 0; u < topo.num_nodes(); ++u) {
        const auto out_u = topo.out_neighbors(u);
        for (uint32_t enc_v : out_u) {
            ++total_links;
            const auto v = ggql::GfaCsrTopology::decode(enc_v).node_id;
            const auto in_v = topo.in_neighbors(v);
            bool found = false;
            for (uint32_t enc_u : in_v) {
                if (ggql::GfaCsrTopology::decode(enc_u).node_id == u) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ++asymmetric;
                if (reported < kMaxReport) {
                    std::fprintf(stderr,
                        "  [%s] asym: out_neighbors(%lu) tem %lu, mas %lu não está em in_neighbors(%lu) (size=%zu)\n",
                        label, (unsigned long)u, (unsigned long)v, (unsigned long)u,
                        (unsigned long)v, in_v.size());
                    ++reported;
                }
            }
        }
    }
    std::fprintf(stderr, "  [%s] total_links=%zu asymmetric=%zu\n",
                 label, total_links, asymmetric);
    return asymmetric;
}

// ── Caso 1: diamante (1 componente, 4 nós) — baseline ─────────────────────
static void test_symmetry_diamond() {
    auto topo = build_diamond();
    const size_t asym = check_symmetry(topo, "diamond");
    assert(asym == 0 && "diamante deve ter simetria E_fwd↔E_bwd perfeita");
}

// Constrói grafo com 2 componentes desconexos, cada um um path linear.
//   Componente A: 0 → 1 → 2 → ... → 49
//   Componente B: 50 → 51 → 52 → ... → 99
// Total: 100 nós, 98 edges, 2 componentes
static ggql::GfaCsrTopology build_two_components(size_t comp_size = 50) {
    using LR = ggql::LinkRecord;
    std::vector<LR> links;
    links.reserve(2 * (comp_size - 1));
    for (uint64_t i = 0; i + 1 < comp_size; ++i) {
        links.push_back(LR::make(i, '+', i + 1, '+'));
    }
    for (uint64_t i = comp_size; i + 1 < 2 * comp_size; ++i) {
        links.push_back(LR::make(i, '+', i + 1, '+'));
    }
    ggql::GfaCsrTopology topo;
    topo.build(2 * comp_size, std::move(links), 1);
    return topo;
}

// ── Caso 2: 2 componentes, single-thread (num_threads=1) ──────────────────
static void test_symmetry_two_components_serial() {
    auto topo = build_two_components(50);
    assert(topo.num_nodes() == 100);
    assert(topo.num_edges() == 98);
    const size_t asym = check_symmetry(topo, "2comp-serial");
    assert(asym == 0 && "2 componentes (serial) deve ter simetria perfeita");
}

// ── Caso 3: 2 componentes, paralelo (num_threads=4) ───────────────────────
static void test_symmetry_two_components_parallel() {
    using LR = ggql::LinkRecord;
    std::vector<LR> links;
    constexpr size_t comp_size = 50;
    for (uint64_t i = 0; i + 1 < comp_size; ++i) {
        links.push_back(LR::make(i, '+', i + 1, '+'));
    }
    for (uint64_t i = comp_size; i + 1 < 2 * comp_size; ++i) {
        links.push_back(LR::make(i, '+', i + 1, '+'));
    }
    ggql::GfaCsrTopology topo;
    topo.build(2 * comp_size, std::move(links), 4);  // ← 4 threads
    const size_t asym = check_symmetry(topo, "2comp-parallel-4t");
    assert(asym == 0 && "2 componentes (4 threads) deve ter simetria perfeita");
}

// ── Caso 4: escala maior (~5k nós, 100 componentes) ───────────────────────
//          Reprodução mais próxima da topologia chm13_100w (multi-comp).
static void test_symmetry_many_components() {
    using LR = ggql::LinkRecord;
    constexpr size_t n_comps = 100;
    constexpr size_t comp_size = 50;
    std::vector<LR> links;
    links.reserve(n_comps * (comp_size - 1));
    for (size_t c = 0; c < n_comps; ++c) {
        const uint64_t base = c * comp_size;
        for (uint64_t i = 0; i + 1 < comp_size; ++i) {
            links.push_back(LR::make(base + i, '+', base + i + 1, '+'));
        }
    }
    ggql::GfaCsrTopology topo;
    topo.build(n_comps * comp_size, std::move(links), 8);
    const size_t asym = check_symmetry(topo, "100comp-8t");
    assert(asym == 0 && "100 componentes (8 threads) deve ter simetria perfeita");
}

// ── Caso 5: orientações mistas (+/-/-/-) ──────────────────────────────────
//          Reprodução de pangenoma com walks invertidos (HPRC tem orient='-').
static void test_symmetry_mixed_orientations() {
    using LR = ggql::LinkRecord;
    std::vector<LR> links = {
        LR::make(0, '+', 1, '+'),
        LR::make(1, '+', 2, '-'),  // → reverse complement
        LR::make(2, '-', 3, '+'),
        LR::make(3, '+', 4, '-'),
        LR::make(4, '-', 5, '-'),
        // Componente B
        LR::make(10, '+', 11, '-'),
        LR::make(11, '-', 12, '+'),
    };
    ggql::GfaCsrTopology topo;
    topo.build(20, std::move(links), 4);
    const size_t asym = check_symmetry(topo, "mixed-orient");
    assert(asym == 0 && "orientações mistas devem ter simetria perfeita");
}

int main() {
    test_dimensions();
    test_degrees();
    test_out_of_bounds_degree();
    test_out_neighbors();
    test_in_neighbors();
    test_find_tips();
    test_find_repeats_min2();
    test_find_repeats_min1();
    test_decode_forward();
    test_decode_reverse();
    // B-V2 repro (sessão K)
    test_symmetry_diamond();
    test_symmetry_two_components_serial();
    test_symmetry_two_components_parallel();
    test_symmetry_many_components();
    test_symmetry_mixed_orientations();
    return EXIT_SUCCESS;
}
