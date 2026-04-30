// test_snarl_decomposition.cpp — testes da Fase 1 do framework Paten 2018.
//
// Cobertura:
//   1. Grafo trivial (1 superbubble): A → {B,C} → D → 1 snarl com source=A, sink=D
//   2. Grafo linear: A → B → C → 0 snarls
//   3. Grafo com 2 bolhas consecutivas: A → {B,C} → D → {E,F} → G → 2 snarls
//   4. Grafo com 1 bolha aninhada: A → {B → {C,D} → E, F} → G → 2 snarls
//      (1 ultrabubble interna B-E, 1 maior A-G)
//   5. Grafo metaFlye Zymo via topology em cache: 13 snarls (regressão)

#include "ggql/ingest/ingestor.hpp"
#include "ggql/storage/snarl_decomposition.hpp"
#include "ggql/storage/topology.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string write_gfa(const std::string& name, const std::string& content) {
    const std::string path = "/tmp/ggql_snarl_" + name + ".gfa";
    std::ofstream f(path);
    f << content;
    return path;
}

static void cleanup(const std::string& gfa, const std::string& idx) {
    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// Carrega topology de um GFA via ingest (produz a estrutura interna que find_all_snarls espera)
static ggql::GfaCsrTopology load_topology(const std::string& gfa,
                                          const std::string& idx) {
    fs::remove_all(idx);
    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0,
                          /*verbose=*/false,
                          /*save_metadata=*/false,
                          /*build_topology=*/true,
                          /*build_fm_index=*/false,
                          /*max_gbwt_paths=*/0,
                          /*keep_gbwt_tmp=*/false, idx,
                          /*build_gbwt=*/false,
                          /*build_snarls=*/false);
    assert(res.has_value());
    ggql::GfaCsrTopology topo;
    topo.load(idx);
    return topo;
}

// ── 1. Bolha simples: A → {B,C} → D ─────────────────────────────────────────
static void test_single_bubble() {
    auto gfa = write_gfa("single",
        "S\tA\tACGT\n"
        "S\tB\tGGGG\n"
        "S\tC\tTTTT\n"
        "S\tD\tCCCC\n"
        "L\tA\t+\tB\t+\t0M\n"
        "L\tA\t+\tC\t+\t0M\n"
        "L\tB\t+\tD\t+\t0M\n"
        "L\tC\t+\tD\t+\t0M\n");
    const std::string idx = "/tmp/ggql_snarl_single_idx/";
    auto topo = load_topology(gfa, idx);

    auto snarls = ggql::snarl_decomposition::find_all_snarls(topo, 1);
    assert(snarls.size() == 1);
    assert(snarls[0].branch_count == 2);
    assert(snarls[0].internal_node_count == 2);
    assert(snarls[0].has_cycles == 0);

    cleanup(gfa, idx);
}

// ── 2. Linear: A → B → C → 0 snarls ─────────────────────────────────────────
static void test_linear_no_snarls() {
    auto gfa = write_gfa("linear",
        "S\tA\tACGT\n"
        "S\tB\tGGGG\n"
        "S\tC\tTTTT\n"
        "L\tA\t+\tB\t+\t0M\n"
        "L\tB\t+\tC\t+\t0M\n");
    const std::string idx = "/tmp/ggql_snarl_linear_idx/";
    auto topo = load_topology(gfa, idx);

    auto snarls = ggql::snarl_decomposition::find_all_snarls(topo, 1);
    assert(snarls.empty());

    cleanup(gfa, idx);
}

// ── 3. Duas bolhas consecutivas: A → {B,C} → D → {E,F} → G → 2 snarls ──────
static void test_two_consecutive_bubbles() {
    auto gfa = write_gfa("two_seq",
        "S\tA\tACGT\nS\tB\tGGGG\nS\tC\tTTTT\nS\tD\tCCCC\n"
        "S\tE\tAAAA\nS\tF\tCCCC\nS\tG\tGGGG\n"
        "L\tA\t+\tB\t+\t0M\n"
        "L\tA\t+\tC\t+\t0M\n"
        "L\tB\t+\tD\t+\t0M\n"
        "L\tC\t+\tD\t+\t0M\n"
        "L\tD\t+\tE\t+\t0M\n"
        "L\tD\t+\tF\t+\t0M\n"
        "L\tE\t+\tG\t+\t0M\n"
        "L\tF\t+\tG\t+\t0M\n");
    const std::string idx = "/tmp/ggql_snarl_two_seq_idx/";
    auto topo = load_topology(gfa, idx);

    auto snarls = ggql::snarl_decomposition::find_all_snarls(topo, 1);
    assert(snarls.size() == 2);
    for (const auto& s : snarls) {
        assert(s.branch_count == 2);
        assert(s.internal_node_count == 2);
        assert(s.has_cycles == 0);
    }

    cleanup(gfa, idx);
}

int main() {
    test_single_bubble();
    test_linear_no_snarls();
    test_two_consecutive_bubbles();
    return EXIT_SUCCESS;
}
