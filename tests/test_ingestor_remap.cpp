// test_ingestor_remap.cpp — testa build_remap_from_pairs via ingestão de GFAs
// com topologia conhecida (componentes desconexos, grafo trivial, grafo linear).

#include "ggql/ingest/gfa_ingestor.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static std::string write_gfa(const std::string& name, const std::string& content) {
    std::string path = "/tmp/ggql_remap_" + name + ".gfa";
    std::ofstream f(path);
    f << content;
    return path;
}

static void cleanup(const std::string& gfa, const std::string& idx) {
    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// Dois segmentos desconexos → 2 componentes
static void test_two_disconnected() {
    auto gfa = write_gfa("disc", "S\tA\tACGT\nS\tB\tTTTT\n");
    std::string idx = "/tmp/ggql_remap_disc_idx/";

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 2);

    cleanup(gfa, idx);
}

// Grafo linear: A-B-C → 1 componente
static void test_linear_chain() {
    auto gfa = write_gfa("chain",
        "S\tA\tACGT\n"
        "S\tB\tGGGG\n"
        "S\tC\tTTTT\n"
        "L\tA\t+\tB\t+\t0M\n"
        "L\tB\t+\tC\t+\t0M\n");
    std::string idx = "/tmp/ggql_remap_chain_idx/";

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 3);
    assert(res->link_count == 2);

    cleanup(gfa, idx);
}

// Grafo com 1 nó, sem arestas
static void test_single_node() {
    auto gfa = write_gfa("single", "S\tonly\tAAAA\n");
    std::string idx = "/tmp/ggql_remap_single_idx/";

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, false, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 1);

    cleanup(gfa, idx);
}

// Dois componentes com 3 nós cada
static void test_two_components() {
    auto gfa = write_gfa("twocomp",
        "S\tA1\tAAAA\nS\tA2\tAAAA\nS\tA3\tAAAA\n"
        "S\tB1\tTTTT\nS\tB2\tTTTT\nS\tB3\tTTTT\n"
        "L\tA1\t+\tA2\t+\t0M\nL\tA2\t+\tA3\t+\t0M\n"
        "L\tB1\t+\tB2\t+\t0M\nL\tB2\t+\tB3\t+\t0M\n");
    std::string idx = "/tmp/ggql_remap_twocomp_idx/";

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 6);
    assert(res->link_count == 4);

    cleanup(gfa, idx);
}

int main() {
    test_two_disconnected();
    test_linear_chain();
    test_single_node();
    test_two_components();
    return EXIT_SUCCESS;
}
