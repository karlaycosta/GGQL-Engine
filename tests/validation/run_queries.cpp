// run_queries.cpp — Utilitário de validação Tier A.
//
// Carrega um .idx pré-existente (gerado por bench_gbwt) e imprime contagens
// das queries canônicas em formato TSV "query\tcount" para diff fácil contra
// ground truth. Sem REPL, sem FFI — usa API C++ direta.
//
// Uso:
//   run_queries <cache_dir.idx>
//
// Saída esperada (stdout, TSV):
//   FIND_SEGMENT     <count>
//   FIND_EDGE        <count>
//   FIND_TIP         <count>
//   FIND_REPEAT      <count>
//   FIND_BUBBLE      <count>
//   FIND_PATH        <count>

#include "ggql/query/grammar.hpp"
#include "ggql/query/query_planner.hpp"
#include "ggql/storage/topology.hpp"
#include "ggql/storage/fm_index.hpp"
#include "ggql/ingest/metadata_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ggql;

static size_t run_count(const QueryPlanner& planner, const std::string& q) {
    auto ast = grammar::parse_ggql(q);
    if (!ast) { return 0; }
    auto res = planner.execute(*ast);
    if (!res) { return 0; }
    return res->total_hits;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Uso: %s <cache_dir.idx>\n", argv[0]);
        return 2;
    }
    const std::string idx = argv[1];

    GfaCsrTopology topo;
    GfaMetadataStore meta;
    GfaSequenceIndex fm;
    bool topo_ok = false, fm_ok = false;
    try { topo.load(idx); topo_ok = true; } catch (...) {}
    try { meta.load(idx); } catch (...) {}
    try { fm.load(idx);   fm_ok = true; } catch (...) {}

    QueryPlanner planner(topo_ok ? &topo : nullptr, &meta, fm_ok ? &fm : nullptr);

    // LIMIT alto para forçar planner a contar TUDO (não só max)
    constexpr const char* kBigLimit = "LIMIT 100000000";

    auto emit = [&](const char* name, const std::string& q) {
        std::printf("%s\t%zu\n", name, run_count(planner, q));
    };

    emit("FIND_SEGMENT", std::string("FIND SEGMENT ") + kBigLimit);
    emit("FIND_EDGE",    std::string("FIND EDGE ") + kBigLimit);
    emit("FIND_TIP",     std::string("FIND TIP ") + kBigLimit);
    emit("FIND_REPEAT",  std::string("FIND REPEAT ") + kBigLimit);
    emit("FIND_BUBBLE",  std::string("FIND BUBBLE ") + kBigLimit);
    emit("FIND_PATH",    std::string("FIND PATH ") + kBigLimit);

    return 0;
}
