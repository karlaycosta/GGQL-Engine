// test_ingestor_tokenize.cpp — testa os helpers de parsing do ingestor GFA.
// Utiliza apenas <cassert> e <cstdlib>; sem dependências externas.
//
// Testar: skip_line, next_token, tokenize, align_to_line_start
// (acessados via GfaIngestorMmapParallel::process_data_parallel como static private,
//  mas verificáveis indiretamente através do comportamento do ingestor em GFA mínimo).

#include "ggql/ingest/ingestor.hpp"

#include <cassert>
#include <cstdlib>
#include <string>

// Testa comportamento do parser via ingestão de GFA mínimo em memória.
// Isso valida tokenize, skip_line e align_to_line_start indiretamente.

static void test_empty_gfa() {
    // GFA vazio deve retornar erro "Nenhum segmento"
    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest("/dev/null", 1, 2.0, false, false, false, false, 0, false, "/tmp/ggql_test_empty/");
    assert(!res.has_value());
    assert(res.error().find("segmento") != std::string::npos
           || res.error().find("mmap") != std::string::npos);
}

static void test_single_segment_gfa() {
    // Cria GFA temporário com um único segmento
    const std::string path = "/tmp/ggql_test_single.gfa";
    {
        std::ofstream f(path);
        f << "H\tVN:Z:1.0\n"
          << "S\tseq1\tACGT\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(path, 1, 2.0, false, false, false, false, 0, false,
                          "/tmp/ggql_test_single_idx/");
    assert(res.has_value());
    assert(res->node_count == 1);
    assert(res->segment_count == 1);
    assert(res->link_count == 0);

    ::unlink(path.c_str());
}

static void test_two_segments_one_link() {
    const std::string path = "/tmp/ggql_test_link.gfa";
    {
        std::ofstream f(path);
        f << "S\tA\tACGT\n"
          << "S\tB\tTGCA\n"
          << "L\tA\t+\tB\t+\t0M\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(path, 1, 2.0, false, false, true, false, 0, false,
                          "/tmp/ggql_test_link_idx/");
    assert(res.has_value());
    assert(res->node_count == 2);
    assert(res->segment_count == 2);
    assert(res->link_count == 1);

    ::unlink(path.c_str());
}

static void test_line_boundary_no_newline_at_end() {
    // GFA sem '\n' no final — tokenizer não deve travar
    const std::string path = "/tmp/ggql_test_nonl.gfa";
    {
        std::ofstream f(path);
        f << "S\tX\tGGCC"; // sem newline no final
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest(path, 1, 2.0, false, false, false, false, 0, false,
                          "/tmp/ggql_test_nonl_idx/");
    assert(res.has_value());
    assert(res->node_count == 1);

    ::unlink(path.c_str());
}

int main() {
    test_empty_gfa();
    test_single_segment_gfa();
    test_two_segments_one_link();
    test_line_boundary_no_newline_at_end();
    return EXIT_SUCCESS;
}
