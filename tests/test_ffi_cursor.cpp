// test_ffi_cursor.cpp — Testes da FFI cursor stateful (C7.1).
//
// Cobre:
//   - ggql_engine_execute_cursor + ggql_cursor_next + destroy
//   - Paginação manual (3 chamadas next com page_size=2 cobrem todas as linhas)
//   - exhausted() + total_emitted() consistentes
//   - Backend não suportado (FIND EDGE) → NULL + last_error
//   - cursor sobrevive a múltiplas chamadas next sem perder estado
//
// Requer: chm13_subset_sm.gfa.idx em $GGQL_TEST_IDX ou
//         ../data/chm13_subset_sm.gfa.idx

#include "ffi/ffi.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* get_idx() {
    const char* env = std::getenv("GGQL_TEST_IDX");
    if (env && *env) { return env; }
    return "../data/chm13_subset_sm.gfa.idx";
}

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FALHA: %s\n", msg);
        std::exit(1);
    }
}

int main() {
    const char* idx = get_idx();
    GgqlEngineHandle eng = ggql_engine_load(idx);
    check(eng != nullptr, "engine_load deve funcionar");

    // ── 1. Backend não suportado em C7.1 → NULL ─────────────────────────────
    {
        GgqlCursorHandle c = ggql_engine_execute_cursor(eng, "FIND EDGE");
        check(c == nullptr, "cursor: FIND EDGE deve retornar NULL (C7.1 só SEGMENT)");
        const char* err = ggql_engine_last_error(eng);
        check(err && std::strstr(err, "backend") != nullptr,
              "last_error deve mencionar backend não suportado");
    }
    {
        GgqlCursorHandle c = ggql_engine_execute_cursor(eng, "SEARCH KMER \"AA\" RETURN id");
        check(c == nullptr, "cursor: SEARCH KMER deve retornar NULL (C7.1 só FIND SEGMENT)");
    }

    // ── 2. Cursor FIND SEGMENT full-scan + paginação manual ──────────────────
    // Threshold alto pra garantir poucos rows (chm13_subset_sm: ~131k segs,
    // poucos com length > 100k).
    {
        GgqlCursorHandle c = ggql_engine_execute_cursor(
            eng, "FIND SEGMENT WHERE length > 5000 RETURN id, length");
        check(c != nullptr, "cursor: FIND SEGMENT deve funcionar");
        check(!ggql_cursor_exhausted(c), "cursor recém-criado não está exhausted");
        check(ggql_cursor_total_emitted(c) == 0, "cursor recém-criado: 0 emitted");

        int64_t total_rows = 0;
        int call = 0;
        while (true) {
            GgqlResultHandle page = ggql_cursor_next(c, 2);
            check(page != nullptr, "cursor_next deve retornar handle válido");
            const int64_t rows = ggql_result_row_count(page);
            const int64_t cols = ggql_result_col_count(page);
            if (rows == 0) {
                ggql_result_free(page);
                break;  // fim do stream
            }
            check(cols == 2, "cada página tem ncols=2 (id, length)");
            check(rows <= 2, "page_size=2 respeitado");
            // Sanity: id é int, length é int
            check(ggql_result_cell_type(page, 0, 0) == GGQL_TYPE_INT, "col 0 = INT");
            check(ggql_result_cell_type(page, 0, 1) == GGQL_TYPE_INT, "col 1 = INT");
            total_rows += rows;
            ggql_result_free(page);
            ++call;
            check(call < 100000, "loop não deve passar de 100k iterações (sanity)");
        }
        check(ggql_cursor_exhausted(c), "após esgotar, exhausted=true");
        check(static_cast<int64_t>(ggql_cursor_total_emitted(c)) == total_rows,
              "total_emitted bate com soma das páginas");
        check(total_rows > 0,
              "chm13_subset_sm deve ter pelo menos 1 segment com length > 100000");
        std::fprintf(stderr,
                     "INFO: cursor full-scan emitiu %lld rows em %d chamadas\n",
                     (long long)total_rows, call);

        // Próxima next após esgotar deve retornar 0 linhas (handle não-NULL)
        GgqlResultHandle empty = ggql_cursor_next(c, 100);
        check(empty != nullptr, "next pós-esgotamento devolve handle (vazio)");
        check(ggql_result_row_count(empty) == 0, "row_count=0 pós-esgotamento");
        ggql_result_free(empty);

        ggql_cursor_destroy(c);
    }

    // ── 3. cursor + paridade com execute() ──────────────────────────────────
    // Cursor e execute() devem produzir o mesmo total de linhas para a mesma query.
    {
        const char* q = "FIND SEGMENT WHERE length > 5000 RETURN id";

        // execute() com limit grande
        GgqlResultHandle r = ggql_engine_execute(eng, q, 1'000'000, 0);
        check(r != nullptr, "paridade: execute deve funcionar");
        const int64_t exec_rows = ggql_result_row_count(r);
        ggql_result_free(r);

        // cursor: itera até esgotar
        GgqlCursorHandle c = ggql_engine_execute_cursor(eng, q);
        check(c != nullptr, "paridade: cursor deve funcionar");
        int64_t cursor_rows = 0;
        while (true) {
            GgqlResultHandle p = ggql_cursor_next(c, 100);
            const int64_t n = ggql_result_row_count(p);
            cursor_rows += n;
            ggql_result_free(p);
            if (n == 0) { break; }
        }
        ggql_cursor_destroy(c);

        check(cursor_rows == exec_rows,
              "paridade: cursor e execute produzem o mesmo total de linhas");
    }

    // ── 4. cursor_next em handle NULL é seguro (sem crash) ───────────────────
    {
        check(ggql_cursor_next(nullptr, 100) == nullptr, "cursor_next(NULL) → NULL");
        check(ggql_cursor_total_emitted(nullptr) == 0, "total_emitted(NULL) = 0");
        check(ggql_cursor_exhausted(nullptr) == true, "exhausted(NULL) = true");
        ggql_cursor_destroy(nullptr);  // não deve crashar
    }

    ggql_engine_destroy(eng);
    std::puts("test_ffi_cursor: todos os testes passaram.");
    return 0;
}
