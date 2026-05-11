// run_queries.cpp — Utilitário de validação Tier A / 1.bis.
//
// Carrega um .idx pré-existente (gerado por bench_gbwt) e imprime contagens
// das queries em formato TSV "query\tcount" para diff fácil contra ground
// truth. Sem REPL, sem FFI — usa API C++ direta.
//
// Uso:
//   run_queries <cache_dir.idx>                  → 6 queries canônicas (Tier A)
//   run_queries <cache_dir.idx> "<query>"        → 1 query custom (Tier 1.bis)
//   run_queries <cache_dir.idx> --batch          → lê queries de stdin
//                                                  (uma por linha; comentários '#' OK)

#include "ggql/query/grammar.hpp"
#include "ggql/query/query_planner.hpp"
#include "ggql/storage/topology.hpp"
#include "ggql/storage/fm_index.hpp"
#include "ggql/ingest/metadata_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

using namespace ggql;

static size_t run_count(const QueryPlanner& planner, const std::string& q) {
    auto ast = grammar::parse_ggql(q);
    if (!ast) { return 0; }
    auto res = planner.execute(*ast);
    if (!res) { return 0; }
    return res->total_hits;
}

// R3 (2026-05-11): formata as linhas retornadas em TSV (uma linha por row,
// colunas separadas por \t). Imprime header com nomes das colunas em '#'.
// Útil para validar CONTEÚDO de RETURN, não só counts.
static int run_rows(const QueryPlanner& planner, const std::string& q) {
    auto ast = grammar::parse_ggql(q);
    if (!ast) {
        std::fprintf(stderr, "parse error\n");
        return 1;
    }
    auto res = planner.execute(*ast);
    if (!res) {
        std::fprintf(stderr, "exec error: %s\n", res.error().c_str());
        return 1;
    }
    const auto& r = *res;
    if (!r.columns.empty()) {
        std::printf("#");
        for (size_t i = 0; i < r.columns.size(); ++i) {
            std::printf("%s%s", i ? "\t" : "", r.columns[i].c_str());
        }
        std::printf("\n");
    }
    for (size_t row = 0; row < r.row_count; ++row) {
        for (size_t col = 0; col < r.columns.size(); ++col) {
            const auto& v = r.flat[row * r.columns.size() + col];
            std::printf("%s", col ? "\t" : "");
            std::visit([](const auto& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    std::printf("%s", val.c_str());
                } else if constexpr (std::is_same_v<T, double>) {
                    std::printf("%.6g", val);
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    std::printf("%lld", static_cast<long long>(val));
                }
            }, v);
        }
        std::printf("\n");
    }
    return 0;
}

// Aplica LIMIT default se a query não especificar — força total_hits global.
static std::string ensure_limit(std::string q) {
    constexpr std::string_view kHas = "LIMIT";
    auto upper = q;
    for (auto& c : upper) {
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); }
    }
    if (upper.find(kHas) == std::string::npos) {
        q += " LIMIT 100000000";
    }
    return q;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "Uso:\n"
            "  %s <idx>                    # 6 queries canônicas\n"
            "  %s <idx> \"<query>\"          # 1 query custom (só count)\n"
            "  %s <idx> --batch            # lê queries de stdin (1 por linha)\n"
            "  %s <idx> --rows \"<query>\"   # imprime linhas em TSV (R3)\n",
            argv[0], argv[0], argv[0], argv[0]);
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

    // ── Modo batch: lê queries de stdin ───────────────────────────────────────
    if (argc == 3 && std::string_view(argv[2]) == "--batch") {
        std::string line;
        while (std::getline(std::cin, line)) {
            // pula vazias e comentários
            auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos) { continue; }
            if (line[first] == '#')         { continue; }
            std::string q = ensure_limit(line.substr(first));
            std::printf("%s\t%zu\n", line.c_str(), run_count(planner, q));
        }
        return 0;
    }

    // ── Modo --rows: <idx> --rows "<query>" ──────────────────────────────────
    if (argc == 4 && std::string_view(argv[2]) == "--rows") {
        std::string q = ensure_limit(argv[3]);
        return run_rows(planner, q);
    }

    // ── Modo single query: <idx> "<query>" ───────────────────────────────────
    if (argc >= 3) {
        std::string q = ensure_limit(argv[2]);
        std::printf("%zu\n", run_count(planner, q));
        return 0;
    }

    // ── Modo default: 6 queries canônicas em TSV ─────────────────────────────
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
