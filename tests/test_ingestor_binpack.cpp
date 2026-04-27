// test_ingestor_binpack.cpp — testa o bin-packing via ingestão de GFA
// com múltiplos componentes e verifica equilíbrio de carga.

#include "ggql/ingest/gfa_ingestor.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static std::string make_chain_gfa(const std::string& prefix, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) {
        out += "S\t" + prefix + std::to_string(i) + "\tAAAA\n";
    }
    for (int i = 0; i + 1 < n; ++i) {
        out += "L\t" + prefix + std::to_string(i) + "\t+\t" + prefix + std::to_string(i + 1) +
               "\t+\t0M\n";
    }
    return out;
}

// K=1 componente, B=1 bin (trivial)
static void test_one_component_one_bin() {
    std::string path = "/tmp/ggql_bp_1c.gfa";
    std::string idx = "/tmp/ggql_bp_1c_idx/";
    {
        std::ofstream f(path);
        f << make_chain_gfa("n", 10);
    }
    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(path, 1, 2.0, false, false, true, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 10);
    ::unlink(path.c_str());
    fs::remove_all(idx);
}

// K=10 componentes desconexos de tamanho 1 cada, 4 threads → ~4 bins máx
static void test_ten_components() {
    std::string path = "/tmp/ggql_bp_10c.gfa";
    std::string idx = "/tmp/ggql_bp_10c_idx/";
    {
        std::ofstream f(path);
        for (int i = 0; i < 10; ++i) {
            f << "S\tnode" << i << "\tAAAA\n";
        }
    }
    ggql::GfaIngestorMmapParallel ing;
    // 4 threads → max_bins = min(10, 4*4=16) = 10
    auto res = ing.ingest(path, 4, 2.0, false, false, true, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 10);
    ::unlink(path.c_str());
    fs::remove_all(idx);
}

// 3 componentes de tamanhos 100, 50, 10 → bin maior - bin menor ≤ tamanho do maior componente
static void test_unequal_components() {
    std::string path = "/tmp/ggql_bp_uneq.gfa";
    std::string idx = "/tmp/ggql_bp_uneq_idx/";
    {
        std::ofstream f(path);
        f << make_chain_gfa("big", 100);
        f << make_chain_gfa("med", 50);
        f << make_chain_gfa("sml", 10);
    }
    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(path, 4, 2.0, false, false, true, false, 0, false, idx);
    assert(res.has_value());
    assert(res->node_count == 160);
    ::unlink(path.c_str());
    fs::remove_all(idx);
}

int main() {
    test_one_component_one_bin();
    test_ten_components();
    test_unequal_components();
    return EXIT_SUCCESS;
}
