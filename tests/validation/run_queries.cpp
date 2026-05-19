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
#include <vector>

using namespace ggql;

// 2026-05-13: emite diagnóstico no stderr em parse/exec error (Fase 1
// descobriu que silenciar `nullopt` mascarava bugs de parser — ex.: bug
// `<>` retornava 0 sem warning). Fase 2+ exigem honestidade do batch.
static size_t run_count(const QueryPlanner& planner, const std::string& q) {
    auto ast = grammar::parse_ggql(q);
    if (!ast) {
        std::fprintf(stderr, "[parse error] %s: %s\n",
                     q.c_str(), ast.error().c_str());
        return 0;
    }
    auto res = planner.execute(*ast);
    if (!res) {
        std::fprintf(stderr, "[plan error] %s: %s\n",
                     q.c_str(), res.error().c_str());
        return 0;
    }
    return res->total_hits;
}

// R3 (2026-05-11): formata as linhas retornadas em TSV (uma linha por row,
// colunas separadas por \t). Imprime header com nomes das colunas em '#'.
// Útil para validar CONTEÚDO de RETURN, não só counts.
static int run_rows(const QueryPlanner& planner, const std::string& q) {
    auto ast = grammar::parse_ggql(q);
    if (!ast) {
        std::fprintf(stderr, "[parse error] %s: %s\n",
                     q.c_str(), ast.error().c_str());
        return 1;
    }
    auto res = planner.execute(*ast);
    if (!res) {
        std::fprintf(stderr, "[plan error] %s: %s\n",
                     q.c_str(), res.error().c_str());
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
        // I-7 (auditoria DoD §11.6, 2026-05-15): pós-fix Opção A do I-1,
        // SEARCH KMER propaga q.limit × 16 ao FM-Index SEM teto. Injetar
        // LIMIT 100000000 aqui → 1,6 bi offsets → DoS por k-mer curto
        // (ex.: `SEARCH KMER "A"` k=1, centenas de M de occ).
        //
        // LIMIT 500000 escolhido empiricamente: (1) ≥ 286.696 (maior
        // contagem de segs distintos da suite — F07 `SEARCH KMER "ACGT"`
        // em CHM13-464), então o Paginator não trunca o count; (2)
        // 500000 × 16 = 8M offsets como teto efetivo → `SEARCH KMER "A"`
        // completa em ~25-30 s (bounded), não os 4 min/13 GiB pré-I-1.
        // FIND/EDGE/etc. mantêm 100M (count global é bounded por
        // #segs ≈ 1,35M).
        if (upper.find("SEARCH KMER") != std::string::npos) {
            q += " LIMIT 500000";
        } else {
            q += " LIMIT 100000000";
        }
    }
    return q;
}

// #1 (perf, 2026-05-19): o FM-Index só é consumido por `SEARCH KMER` e
// `WHERE contains_kmer(...)`. Carregá-lo incondicionalmente dominava a
// latência one-shot em pangenomas grandes (GRCh38: ~8,5 s ≈ 100% load do
// .idx, perdendo até para `gfatools stat`). "KMER" (case-insensitive) só
// ocorre nesses dois construtos da grammar GGQL; um falso-positivo apenas
// recarrega à toa — nunca afeta corretude.
[[nodiscard]] static bool query_needs_fm(std::string_view q) noexcept {
    static constexpr std::string_view kK = "KMER";
    if (q.size() < kK.size()) { return false; }
    for (size_t i = 0; i + kK.size() <= q.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < kK.size(); ++j) {
            char c = q[i + j];
            if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); }
            if (c != kK[j]) { match = false; break; }
        }
        if (match) { return true; }
    }
    return false;
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

    // ── Determina modo + coleta queries ANTES de carregar (p/ #1) ────────────
    enum class Mode { Default6, Single, Rows, Batch };
    Mode mode = Mode::Default6;
    std::string single_q;             // Single / Rows
    std::vector<std::string> batch;   // Batch (buffer p/ decidir need_fm)
    if (argc == 3 && std::string_view(argv[2]) == "--batch") {
        mode = Mode::Batch;
        for (std::string line; std::getline(std::cin, line); ) {
            batch.push_back(std::move(line));
        }
    } else if (argc == 4 && std::string_view(argv[2]) == "--rows") {
        mode = Mode::Rows;
        single_q = argv[3];
    } else if (argc >= 3) {
        mode = Mode::Single;
        single_q = argv[2];
    }

    // #1: FM-Index só carrega se alguma query usa SEARCH KMER/contains_kmer.
    // Default6 (SEGMENT/EDGE/TIP/REPEAT/BUBBLE/PATH) nunca usa.
    bool need_fm = false;
    if (mode == Mode::Single || mode == Mode::Rows) {
        need_fm = query_needs_fm(single_q);
    } else if (mode == Mode::Batch) {
        for (const auto& l : batch) {
            if (query_needs_fm(l)) { need_fm = true; break; }
        }
    }

    GfaCsrTopology topo;
    GfaMetadataStore meta;
    GfaSequenceIndex fm;
    bool topo_ok = false, fm_ok = false;
    // I-2: metadata é OBRIGATÓRIO (passado direto ao planner). Falha aqui
    // mascararia corrupção, retornando counts errados sem warning.
    try {
        meta.load(idx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[load error] meta: %s\n", e.what());
        return 1;
    }
    // I-2: topology é opcional (planner cai em fallback se ausente).
    // Diferencia falha real (warning) de "perfil sem topologia" (silencioso).
    try {
        topo.load(idx);
        topo_ok = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[load warning] topo: %s (queries TIP/EDGE/PATH/BUBBLE/REPEAT indisponíveis)\n",
                     e.what());
    }
    // I-2: FM-Index é opcional + retorna std::expected. Diferencia
    // arquivo ausente (silencioso) de corrupção (warning preservado).
    // #1: só tenta carregar se a(s) query(ies) realmente precisam.
    if (need_fm) {
        if (auto r = fm.load(idx); !r) {
            std::fprintf(stderr, "[load warning] fm_index: %s (SEARCH KMER e WHERE contains_kmer indisponíveis)\n",
                         r.error().c_str());
        } else {
            fm_ok = fm.is_loaded();
        }
    }

    QueryPlanner planner(topo_ok ? &topo : nullptr, &meta, fm_ok ? &fm : nullptr);

    // ── Modo batch ───────────────────────────────────────────────────────────
    if (mode == Mode::Batch) {
        for (const auto& line : batch) {
            // pula vazias e comentários
            auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos) { continue; }
            if (line[first] == '#')         { continue; }
            std::string q = ensure_limit(line.substr(first));
            std::printf("%s\t%zu\n", line.c_str(), run_count(planner, q));
        }
        return 0;
    }

    // ── Modo --rows ──────────────────────────────────────────────────────────
    if (mode == Mode::Rows) {
        return run_rows(planner, ensure_limit(single_q));
    }

    // ── Modo single query ────────────────────────────────────────────────────
    if (mode == Mode::Single) {
        std::printf("%zu\n", run_count(planner, ensure_limit(single_q)));
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
