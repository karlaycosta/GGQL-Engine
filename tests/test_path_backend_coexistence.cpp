// test_path_backend_coexistence.cpp — Valida que PathStorage e GbwtBackend
// produzem resultados identicos para os mesmos GbwtGroups via API
// path_nodes() da GfaCsrTopology.
//
// Cobertura:
//   1. build com PathBackendKind::Gbwt funciona end-to-end
//   2. build com PathBackendKind::PathStorage (default) funciona
//   3. parity: mesmos dados → mesmos path_nodes em ambos backends
//   4. shard vazio + shard com dados misturados em ambos backends

#include "ggql/ingest/gbwt_group.hpp"
#include "ggql/ingest/profile.hpp"
#include "ggql/storage/topology.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

using ggql::GfaCsrTopology;
using ggql::PathBackendKind;

static ggql::gbwt::vector_type
encoded_seq(std::initializer_list<std::pair<uint64_t, uint64_t>> nodes) {
    ggql::gbwt::vector_type out;
    out.reserve(nodes.size());
    for (auto [nid, orient] : nodes) {
        out.push_back(static_cast<ggql::gbwt::node_type>(
            ggql::gbwt_encode_node(nid, orient)));
    }
    return out;
}

// Helper: monta dois shards de exemplo. Os mesmos dados sao usados em
// ambos backends para o parity test.
static std::vector<GfaCsrTopology::GbwtGroup> make_sample_groups() {
    std::vector<GfaCsrTopology::GbwtGroup> groups;

    GfaCsrTopology::GbwtGroup g0;
    g0.in_ram_seqs.push_back(encoded_seq({{1, 0}, {2, 0}, {3, 1}, {4, 0}}));
    g0.in_ram_seqs.push_back(encoded_seq({{5, 0}, {6, 1}}));
    g0.path_names = {"shard0_pa", "shard0_pb"};
    groups.push_back(std::move(g0));

    GfaCsrTopology::GbwtGroup g1;
    g1.in_ram_seqs.push_back(encoded_seq({{10, 0}, {11, 0}, {12, 0}}));
    g1.path_names = {"shard1_pc"};
    groups.push_back(std::move(g1));

    return groups;
}

static void test_build_with_gbwt_backend() {
    GfaCsrTopology topo;
    auto t = topo.build_path_storage_parallel(
        make_sample_groups(), /*node_width=*/8, /*num_threads=*/1,
        PathBackendKind::Gbwt);
    (void)t;

    assert(topo.path_count() == 3);
    assert(topo.has_path_storage_backend());
    assert(topo.path_name(0) == "shard0_pa");
    assert(topo.path_name(1) == "shard0_pb");
    assert(topo.path_name(2) == "shard1_pc");

    std::vector<uint64_t> buf;

    topo.path_nodes(0, buf);
    assert((buf == std::vector<uint64_t>{1, 2, 3, 4}));

    topo.path_nodes(1, buf);
    assert((buf == std::vector<uint64_t>{5, 6}));

    topo.path_nodes(2, buf);
    assert((buf == std::vector<uint64_t>{10, 11, 12}));
}

static void test_build_with_pathstorage_backend() {
    GfaCsrTopology topo;
    (void)topo.build_path_storage_parallel(
        make_sample_groups(), /*node_width=*/8, /*num_threads=*/2,
        PathBackendKind::PathStorage);

    assert(topo.path_count() == 3);

    std::vector<uint64_t> buf;
    topo.path_nodes(0, buf);
    assert((buf == std::vector<uint64_t>{1, 2, 3, 4}));
    topo.path_nodes(2, buf);
    assert((buf == std::vector<uint64_t>{10, 11, 12}));
}

// ── Parity test: mesmos dados → mesmos path_nodes ─────────────────────────
//
// Critico para garantir que callers (build_path_stats, query_planner) podem
// trocar de backend transparentemente sem mudanca observavel.
static void test_parity_pathstorage_vs_gbwt() {
    GfaCsrTopology topo_ps;
    (void)topo_ps.build_path_storage_parallel(
        make_sample_groups(), 8, 1, PathBackendKind::PathStorage);

    GfaCsrTopology topo_gb;
    (void)topo_gb.build_path_storage_parallel(
        make_sample_groups(), 8, 1, PathBackendKind::Gbwt);

    assert(topo_ps.path_count() == topo_gb.path_count());

    std::vector<uint64_t> buf_ps;
    std::vector<uint64_t> buf_gb;
    for (size_t pid = 0; pid < topo_ps.path_count(); ++pid) {
        topo_ps.path_nodes(pid, buf_ps);
        topo_gb.path_nodes(pid, buf_gb);
        assert(buf_ps == buf_gb && "parity mismatch entre PathStorage e Gbwt");

        // Nomes tambem devem bater
        assert(topo_ps.path_name(pid) == topo_gb.path_name(pid));
    }
}

static void test_gbwt_with_empty_shard() {
    std::vector<GfaCsrTopology::GbwtGroup> groups;

    GfaCsrTopology::GbwtGroup g0;  // shard vazio (no paths)
    groups.push_back(std::move(g0));

    GfaCsrTopology::GbwtGroup g1;
    g1.in_ram_seqs.push_back(encoded_seq({{42, 0}, {43, 1}}));
    g1.path_names = {"only"};
    groups.push_back(std::move(g1));

    GfaCsrTopology topo;
    (void)topo.build_path_storage_parallel(
        std::move(groups), 8, 1, PathBackendKind::Gbwt);

    assert(topo.path_count() == 1);
    std::vector<uint64_t> buf;
    topo.path_nodes(0, buf);
    assert((buf == std::vector<uint64_t>{42, 43}));
}

int main() {
    test_build_with_gbwt_backend();
    test_build_with_pathstorage_backend();
    test_parity_pathstorage_vs_gbwt();
    test_gbwt_with_empty_shard();
    return EXIT_SUCCESS;
}
