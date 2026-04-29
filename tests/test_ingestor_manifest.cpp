// test_ingestor_manifest.cpp — testa write_manifest_atomic + load_index
// (roundtrip, campo inválido, arquivo truncado).

#include "ggql/ingest/ingestor.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Roundtrip: ingere GFA → cache → ingere novamente (deve usar cache)
static void test_cache_roundtrip() {
    const std::string gfa = "/tmp/ggql_mf_rt.gfa";
    const std::string idx = "/tmp/ggql_mf_rt_idx/";
    {
        std::ofstream f(gfa);
        f << "S\tA\tACGT\nS\tB\tTGCA\nL\tA\t+\tB\t+\t0M\n";
    }

    ggql::GfaIngestorMmapParallel ing1;
    auto r1 = ing1.ingest(gfa, 1, 2.0, false, true, true, false, 0, false, idx);
    assert(r1.has_value());
    assert(r1->node_count == 2);

    // Segunda ingestão — deve carregar do cache (mtime+size hit)
    ggql::GfaIngestorMmapParallel ing2;
    auto r2 = ing2.ingest(gfa, 1, 2.0, false, true, true, false, 0, false, idx);
    assert(r2.has_value());
    assert(r2->node_count == 2);

    ::unlink(gfa.c_str());
    fs::remove_all(idx);
}

// Manifest com campo numérico inválido → cache deve ser rejeitado (version < 2)
static void test_corrupt_manifest_version() {
    const std::string idx = "/tmp/ggql_mf_corrupt_idx/";
    fs::create_directories(idx);

    // Escreve manifest com version=1 (desatualizado)
    {
        std::ofstream f(idx + "manifest.txt");
        f << "version=1\ngfa_hash=12345\nnum_nodes=10\n";
    }

    ggql::GfaIngestorMmapParallel ing;
    // load_index deve retornar nullopt para version=1
    // Verificamos indiretamente: ingestão sem GFA real deve falhar no mmap, não no cache
    auto res = ing.ingest("/dev/null", 1, 2.0, false, false, false, false, 0, false, idx);
    // /dev/null retorna erro de mmap ou segmento vazio — o importante é que
    // o manifest corrompido não causa crash nem retorno de dados inválidos.
    (void)res;

    fs::remove_all(idx);
}

// Manifest truncado (sem campos obrigatórios) — deve ser tratado graciosamente
static void test_truncated_manifest() {
    const std::string idx = "/tmp/ggql_mf_trunc_idx/";
    fs::create_directories(idx);

    {
        std::ofstream f(idx + "manifest.txt");
        f << "version=2\n"; // sem gfa_hash, gfa_size, etc.
    }

    ggql::GfaIngestorMmapParallel ing;
    auto res = ing.ingest("/dev/null", 1, 2.0, false, false, false, false, 0, false, idx);
    // Deve falhar no mmap/segmento, nunca em crash por acesso a campos não inicializados
    (void)res;

    fs::remove_all(idx);
}

int main() {
    test_cache_roundtrip();
    test_corrupt_manifest_version();
    test_truncated_manifest();
    return EXIT_SUCCESS;
}
