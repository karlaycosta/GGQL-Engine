// test_path_storage.cpp — Testes unitários de ggql::gbwt::PathStorage.
// Valida: build via factory e builder, extract/view consistency, save/load
// roundtrip + integridade (magic, version, checksum, invariants).

#include "ggql/gbwt/path_storage.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

using ggql::PathStorage;
using ggql::PathStorageBuilder;
using node_t = PathStorage::node_type;

static void test_empty_default() {
    PathStorage ps;
    assert(ps.num_paths() == 0);
    assert(ps.total_nodes() == 0);
}

static void test_builder_empty_finish() {
    PathStorageBuilder b;
    auto ps = b.finish();
    assert(ps.num_paths() == 0);
    assert(ps.total_nodes() == 0);
}

static void test_single_path() {
    PathStorageBuilder b;
    std::vector<node_t> p = {1, 2, 3, 4, 5};
    b.insert(p);
    auto ps = b.finish();

    assert(ps.num_paths() == 1);
    assert(ps.total_nodes() == 5);
    assert(ps.path_size(0) == 5);

    std::vector<node_t> out;
    ps.extract_into(0, out);
    assert(out == p);

    auto v = ps.nodes_view(0);
    assert(v.size() == 5);
    assert(std::equal(v.begin(), v.end(), p.begin()));
}

static void test_many_paths_consistency() {
    std::vector<std::vector<node_t>> paths = {
        {10, 20, 30},
        {},                     // path vazio
        {42},                   // path de 1 nó
        {5, 5, 5, 5, 5, 5},     // repetições
        {100, 200, 300, 400},
    };
    auto copy = paths;
    auto ps = PathStorage::from_paths(std::move(paths));

    assert(ps.num_paths() == 5);
    assert(ps.total_nodes() == 3 + 0 + 1 + 6 + 4);

    for (std::size_t i = 0; i < ps.num_paths(); ++i) {
        // path_size O(1) bate com extract
        assert(ps.path_size(i) == copy[i].size());

        // extract_into devolve cópia idêntica
        std::vector<node_t> out;
        ps.extract_into(i, out);
        assert(out == copy[i]);

        // nodes_view (zero-copy) bate com extract
        auto view = ps.nodes_view(i);
        assert(view.size() == copy[i].size());
        assert(std::equal(view.begin(), view.end(), copy[i].begin()));
    }
}

static void test_extract_into_overwrites_buffer() {
    PathStorageBuilder b;
    b.insert(std::vector<node_t>{1, 2});
    b.insert(std::vector<node_t>{9, 9, 9, 9});
    auto ps = b.finish();

    std::vector<node_t> buf = {77, 77, 77, 77, 77, 77, 77, 77};  // pré-populado
    ps.extract_into(0, buf);
    assert(buf == (std::vector<node_t>{1, 2}));

    ps.extract_into(1, buf);
    assert(buf == (std::vector<node_t>{9, 9, 9, 9}));
}

static void test_builder_finish_twice_throws() {
    PathStorageBuilder b;
    b.insert(std::vector<node_t>{1});
    (void)b.finish();
    bool threw = false;
    try {
        (void)b.finish();
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);
}

static void test_builder_insert_after_finish_throws() {
    PathStorageBuilder b;
    (void)b.finish();
    bool threw = false;
    try {
        b.insert(std::vector<node_t>{1});
    } catch (const std::logic_error&) {
        threw = true;
    }
    assert(threw);
}

static void test_save_load_roundtrip() {
    std::vector<std::vector<node_t>> paths = {
        {1, 2, 3},
        {7, 8, 9, 10},
        {},
        {1234567890U, 4000000000U},  // borderline alto em uint32_t (max ~4.29B)
    };
    auto copy = paths;
    auto original = PathStorage::from_paths(std::move(paths));

    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    original.save(ss);

    auto loaded = PathStorage::load(ss);
    assert(loaded.num_paths() == original.num_paths());
    assert(loaded.total_nodes() == original.total_nodes());

    for (std::size_t i = 0; i < loaded.num_paths(); ++i) {
        std::vector<node_t> out;
        loaded.extract_into(i, out);
        assert(out == copy[i]);
    }
}

static void test_save_load_empty_roundtrip() {
    PathStorage empty;
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    empty.save(ss);
    auto loaded = PathStorage::load(ss);
    assert(loaded.num_paths() == 0);
    assert(loaded.total_nodes() == 0);
}

static void test_load_bad_magic_throws() {
    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    const char garbage[] = "BAD_MAGIC_AND_LOTS_MORE_BYTES_FOLLOWING_FOR_SAFE_READS_____";
    ss.write(garbage, sizeof(garbage));
    bool threw = false;
    try {
        (void)PathStorage::load(ss);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

static void test_load_corrupt_payload_throws() {
    PathStorageBuilder b;
    b.insert(std::vector<node_t>{1, 2, 3});
    b.insert(std::vector<node_t>{4, 5, 6});
    auto ps = b.finish();

    std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
    ps.save(ss);

    // Corrompe um byte no meio do payload (após header de 32B).
    auto buf = ss.str();
    assert(buf.size() > 40);
    buf[40] ^= 0xFF;

    std::stringstream corrupted(buf, std::ios::in | std::ios::out | std::ios::binary);
    bool threw = false;
    try {
        (void)PathStorage::load(corrupted);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

static void test_large_10k_paths() {
    constexpr std::size_t N = 10000;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<std::size_t> len_dist(1, 50);

    PathStorageBuilder b;
    b.reserve(N, N * 25);
    std::vector<std::vector<node_t>> ground_truth;
    ground_truth.reserve(N);

    for (std::size_t i = 0; i < N; ++i) {
        const auto len = len_dist(rng);
        std::vector<node_t> p;
        p.reserve(len);
        for (std::size_t k = 0; k < len; ++k) {
            p.push_back(static_cast<node_t>(rng() & 0xFFFFFF));
        }
        b.insert(p);
        ground_truth.push_back(std::move(p));
    }
    auto ps = b.finish();
    assert(ps.num_paths() == N);

    // Spot-check 100 paths aleatórios via view.
    std::uniform_int_distribution<std::size_t> idx_dist(0, N - 1);
    for (int s = 0; s < 100; ++s) {
        const auto i = idx_dist(rng);
        auto v = ps.nodes_view(i);
        assert(v.size() == ground_truth[i].size());
        assert(std::equal(v.begin(), v.end(), ground_truth[i].begin()));
    }
}

int main() {
    test_empty_default();
    test_builder_empty_finish();
    test_single_path();
    test_many_paths_consistency();
    test_extract_into_overwrites_buffer();
    test_builder_finish_twice_throws();
    test_builder_insert_after_finish_throws();
    test_save_load_roundtrip();
    test_save_load_empty_roundtrip();
    test_load_bad_magic_throws();
    test_load_corrupt_payload_throws();
    test_large_10k_paths();
    return EXIT_SUCCESS;
}
