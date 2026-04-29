// test_storage_csr.cpp — Testes unitários de GfaCsrTopology.
// Verifica: build, graus, vizinhos, decode, find_tips, find_repeats.

#include "ggql/storage/topology.hpp"
#include "ggql/storage/topology_types.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>

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
    return EXIT_SUCCESS;
}
