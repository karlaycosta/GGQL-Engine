// test_ingestor_parse.cpp — testa parse_w_line e parse_p_line via ingestão
// de GFAs mínimos com W-lines e P-lines.

#include "ggql/ingest/gfa_ingestor.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// GFA 1.1 com W-line válida — 1 haplótipo de 2 nós
static void test_w_line_valid() {
    const std::string gfa = "/tmp/ggql_parse_w.gfa";
    const std::string idx = "/tmp/ggql_parse_w_idx/";
    {
        std::ofstream f(gfa);
        f << "H\tVN:Z:1.1\n"
          << "S\tA\tACGT\n"
          << "S\tB\tTGCA\n"
          << "L\tA\t+\tB\t+\t0M\n"
          << "W\tsample\t0\tchr1\t0\t8\t>A>B\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 2, false, idx);
    assert(res.has_value());
    assert(res->node_count == 2);
    assert(res->path_count == 1);

    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// GFA 1.0 com P-line válida
static void test_p_line_valid() {
    const std::string gfa = "/tmp/ggql_parse_p.gfa";
    const std::string idx = "/tmp/ggql_parse_p_idx/";
    {
        std::ofstream f(gfa);
        f << "H\tVN:Z:1.0\n"
          << "S\tX\tAAAA\n"
          << "S\tY\tCCCC\n"
          << "L\tX\t+\tY\t+\t0M\n"
          << "P\thap1\tX+,Y+\t0M,0M\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 2, false, idx);
    assert(res.has_value());
    assert(res->node_count == 2);
    assert(res->path_count == 1);

    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// W-line com segmento desconhecido — deve ser ignorado sem crash
static void test_w_line_unknown_segment() {
    const std::string gfa = "/tmp/ggql_parse_wunk.gfa";
    const std::string idx = "/tmp/ggql_parse_wunk_idx/";
    {
        std::ofstream f(gfa);
        f << "S\tA\tACGT\n"
          << "W\tsample\t0\tchr1\t0\t4\t>A>UNKNOWN\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    // Deve completar sem crash; path pode ter 1 nó válido ou 0 dependendo do impl
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 2, false, idx);
    assert(res.has_value());
    assert(res->node_count == 1);

    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// P-line com orientação '-' (nó reverso)
static void test_p_line_reverse_orient() {
    const std::string gfa = "/tmp/ggql_parse_prev.gfa";
    const std::string idx = "/tmp/ggql_parse_prev_idx/";
    {
        std::ofstream f(gfa);
        f << "S\tM\tAAAA\n"
          << "S\tN\tTTTT\n"
          << "L\tM\t+\tN\t-\t0M\n"
          << "P\thap2\tM+,N-\t0M\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(gfa, 1, 2.0, false, false, true, false, 2, false, idx);
    assert(res.has_value());
    assert(res->node_count == 2);
    assert(res->path_count == 1);

    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

int main() {
    test_w_line_valid();
    test_p_line_valid();
    test_w_line_unknown_segment();
    test_p_line_reverse_orient();
    return EXIT_SUCCESS;
}
