// test_topology_pstore_build.cpp — Testa build_path_storage_parallel (Fase 6).
// Valida construção end-to-end de path_stores_ a partir de GbwtGroups in-RAM,
// sem passar pelo gBWT, e verifica que path_nodes() entrega os valores
// decodificados esperados.

#include "ggql/ingest/gbwt_group.hpp"
#include "ggql/storage/topology.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <vector>

static ggql::gbwt::vector_type encoded_seq(std::initializer_list<std::pair<uint64_t, uint64_t>> nodes) {
    ggql::gbwt::vector_type out;
    out.reserve(nodes.size());
    for (auto [nid, orient] : nodes) {
        out.push_back(static_cast<ggql::gbwt::node_type>(ggql::gbwt_encode_node(nid, orient)));
    }
    return out;
}

static void test_build_two_shards() {
    using ggql::GfaCsrTopology;

    // Shard 0: 2 paths.
    GfaCsrTopology::GbwtGroup g0;
    g0.in_ram_seqs.push_back(encoded_seq({{1, 0}, {2, 0}, {3, 0}}));
    g0.in_ram_seqs.push_back(encoded_seq({{4, 0}, {5, 1}}));
    g0.path_names = {"shard0_pathA", "shard0_pathB"};

    // Shard 1: 1 path.
    GfaCsrTopology::GbwtGroup g1;
    g1.in_ram_seqs.push_back(encoded_seq({{10, 0}}));
    g1.path_names = {"shard1_pathC"};

    std::vector<GfaCsrTopology::GbwtGroup> groups;
    groups.push_back(std::move(g0));
    groups.push_back(std::move(g1));

    GfaCsrTopology topo;
    auto t = topo.build_path_storage_parallel(std::move(groups), /*node_width=*/8, /*num_threads=*/2);
    assert(t.total_nodes == 6);

    assert(topo.path_count() == 3);
    assert(topo.has_path_storage_backend());
    assert(topo.path_name(0) == "shard0_pathA");
    assert(topo.path_name(1) == "shard0_pathB");
    assert(topo.path_name(2) == "shard1_pathC");

    std::vector<uint64_t> buf;

    topo.path_nodes(0, buf);
    assert((buf == std::vector<uint64_t>{1, 2, 3}));

    topo.path_nodes(1, buf);
    assert((buf == std::vector<uint64_t>{4, 5}));

    topo.path_nodes(2, buf);
    assert((buf == std::vector<uint64_t>{10}));
}

static void test_build_with_empty_group() {
    using ggql::GfaCsrTopology;

    // Shard 0 vazio (nenhum path), shard 1 com 1 path.
    GfaCsrTopology::GbwtGroup g0;  // sem seqs nem nomes
    GfaCsrTopology::GbwtGroup g1;
    g1.in_ram_seqs.push_back(encoded_seq({{42, 0}, {43, 1}}));
    g1.path_names = {"only"};

    std::vector<GfaCsrTopology::GbwtGroup> groups;
    groups.push_back(std::move(g0));
    groups.push_back(std::move(g1));

    GfaCsrTopology topo;
    (void)topo.build_path_storage_parallel(std::move(groups), 8, 1);

    assert(topo.path_count() == 1);
    std::vector<uint64_t> buf;
    topo.path_nodes(0, buf);
    assert((buf == std::vector<uint64_t>{42, 43}));
}

static void test_build_idempotent_overrides_previous_state() {
    using ggql::GfaCsrTopology;

    GfaCsrTopology topo;

    // 1ª chamada
    GfaCsrTopology::GbwtGroup g_a;
    g_a.in_ram_seqs.push_back(encoded_seq({{1, 0}}));
    g_a.path_names = {"first"};
    std::vector<GfaCsrTopology::GbwtGroup> groups_a;
    groups_a.push_back(std::move(g_a));
    (void)topo.build_path_storage_parallel(std::move(groups_a), 8, 1);
    assert(topo.path_count() == 1);

    // 2ª chamada com dataset totalmente diferente — deve sobrescrever, não acumular.
    GfaCsrTopology::GbwtGroup g_b;
    g_b.in_ram_seqs.push_back(encoded_seq({{99, 0}, {100, 0}}));
    g_b.path_names = {"second"};
    std::vector<GfaCsrTopology::GbwtGroup> groups_b;
    groups_b.push_back(std::move(g_b));
    (void)topo.build_path_storage_parallel(std::move(groups_b), 8, 1);
    assert(topo.path_count() == 1);
    assert(topo.path_name(0) == "second");

    std::vector<uint64_t> buf;
    topo.path_nodes(0, buf);
    assert((buf == std::vector<uint64_t>{99, 100}));
}

int main() {
    test_build_two_shards();
    test_build_with_empty_group();
    test_build_idempotent_overrides_previous_state();
    return EXIT_SUCCESS;
}
