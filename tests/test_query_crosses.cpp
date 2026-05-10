// test_query_crosses.cpp — Validação end-to-end do `crosses(<path>)` real.
//
// Pré-requisito: $GGQL_TEST_IDX (default: ../data/chm13_subset_100w.gfa.idx)
// deve ser um cache .idx com path_index (ingest com build_gbwt=true).
//
// Cobre:
//   - Forma com aspas:    crosses("HG002#1#chr1")    ← B-2
//   - Forma legacy:       crosses(simple_id)         ← retrocompat
//   - Path inexistente   → 0 hits (não erro)
//   - crosses(self)      → ≥1 hit (path crossa a si mesmo)
//   - Combinado com AND  → integração com short-circuit do eval_where

#include "ggql/query/grammar.hpp"
#include "ggql/query/query_planner.hpp"
#include "ggql/storage/topology.hpp"
#include "ggql/ingest/metadata_store.hpp"

#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>

using namespace ggql;

static int n_pass = 0, n_fail = 0;

static void check(bool cond, const char* msg) {
    if (cond) { std::printf("[ OK ] %s\n", msg); ++n_pass; }
    else      { std::printf("[FAIL] %s\n", msg); ++n_fail; }
}

static QueryResult run(const QueryPlanner& planner, const std::string& q_str) {
    auto ast = grammar::parse_ggql(q_str);
    if (!ast) {
        std::printf("  [parse error] %s\n", ast.error().c_str());
        return {};
    }
    auto res = planner.execute(*ast);
    if (!res) {
        std::printf("  [exec error] %s\n", res.error().c_str());
        return {};
    }
    return std::move(*res);
}

int main() {
    const char* idx = std::getenv("GGQL_TEST_IDX");
    if (!idx) { idx = "../data/chm13_subset_100w.gfa.idx"; }

    GfaCsrTopology topo;
    GfaMetadataStore meta;
    try {
        topo.load(idx);
        meta.load(idx);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_query_crosses: falha carregando .idx (%s): %s\n", idx, e.what());
        return 2;
    }

    std::printf("Loaded: %zu paths, %zu nodes (idx=%s)\n",
                topo.path_count(), topo.num_nodes(), idx);
    if (topo.path_count() == 0) {
        std::fprintf(stderr, "test_query_crosses: índice não tem paths — esperado FIND PATH falhar\n");
        return 2;
    }

    QueryPlanner planner(&topo, &meta, nullptr);

    // ── 1. Descobre um path name real (LIMIT 1) ───────────────────────────────
    auto r0 = run(planner, "FIND PATH RETURN id, name LIMIT 1");
    check(r0.row_count == 1,                    "FIND PATH LIMIT 1 → 1 linha");
    if (r0.row_count != 1) { return 1; }
    const int64_t target_pid = std::get<int64_t>(r0.flat[0]);
    const std::string target_name = std::get<std::string>(r0.flat[1]);
    std::printf("  target: pid=%ld name=\"%s\"\n", (long)target_pid, target_name.c_str());

    // ── 2. crosses("<target>") com aspas — deve retornar ≥1 hit (o próprio) ───
    {
        std::string q = "FIND PATH WHERE crosses(\"" + target_name + "\") RETURN id, name";
        auto r = run(planner, q);
        std::printf("  crosses(\"%s\") hits=%zu\n", target_name.c_str(), r.row_count);
        check(r.row_count >= 1,                 "crosses(self) >= 1 (path crossa a si mesmo)");

        bool found_self = false;
        const size_t ncols = r.columns.size();
        for (size_t i = 0; i < r.row_count; ++i) {
            if (std::get<int64_t>(r.flat[i * ncols + 0]) == target_pid) {
                found_self = true; break;
            }
        }
        check(found_self,                       "target_pid aparece no resultado de crosses(self)");
    }

    // ── 3. crosses("__inexistente__") — 0 hits, sem erro ─────────────────────
    {
        auto r = run(planner,
            "FIND PATH WHERE crosses(\"__path_que_nao_existe_xyz__\") RETURN id");
        check(r.row_count == 0,                 "crosses(<inexistente>) == 0 hits (sem erro)");
    }

    // ── 4. Forma legacy (identifier sem aspas) — retrocompat ─────────────────
    {
        auto is_legacy_safe = [](const std::string& s) {
            if (s.empty()) return false;
            if (!std::isalpha(static_cast<unsigned char>(s[0])) && s[0] != '_') return false;
            for (char c : s) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
            }
            return true;
        };
        if (is_legacy_safe(target_name)) {
            std::string q = "FIND PATH WHERE crosses(" + target_name + ") RETURN id";
            auto r = run(planner, q);
            check(r.row_count >= 1,             "crosses(legacy_id) sem aspas — retrocompat");
        } else {
            std::printf("  skip retrocompat: target_name=\"%s\" tem chars não-id\n",
                        target_name.c_str());
        }
    }

    // ── 5. Combinado: crosses() AND length > 0 ───────────────────────────────
    {
        std::string q = "FIND PATH WHERE crosses(\"" + target_name +
                        "\") AND length > 0 RETURN id";
        auto r = run(planner, q);
        check(r.row_count >= 1,                 "crosses() AND length > 0 retorna hits");
    }

    // ── 6. Combinado: length impossível AND crosses(...) — short-circuit ─────
    //     length > 999999999999 é falso para todo path real → AND curto-circuita
    //     e crosses() não precisa ser avaliado. Verifica apenas a corretude
    //     do resultado (0 hits) — o ganho do short-circuit é interno.
    {
        std::string q = "FIND PATH WHERE length > 999999999999 AND crosses(\"" +
                        target_name + "\") RETURN id";
        auto r = run(planner, q);
        check(r.row_count == 0,                 "length>>0 AND crosses(...) == 0 (corretude do AND)");
    }

    std::printf("\n=== %d pass · %d fail ===\n", n_pass, n_fail);
    return n_fail == 0 ? 0 : 1;
}
