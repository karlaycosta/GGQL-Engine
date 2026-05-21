// test_ffi_query.cpp — Testes de integração da API FFI de query (C API).
//
// Requer: chm13_subset_sm.gfa.idx em $GGQL_TEST_IDX ou ../data/chm13_subset_sm.gfa.idx
//
// Cobre:
//   - ggql_engine_load / ggql_engine_destroy
//   - ggql_engine_execute: limit/offset via parâmetro
//   - Metadados: row_count, col_count, total_hits, col_name
//   - Acessores de célula: cell_type, cell_int, cell_string
//   - ggql_result_free
//   - Erro de parse via ggql_engine_last_error

#include "ffi/ffi.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

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

    // ── 1. Carrega engine ──────────────────────────────────────────────────────
    GgqlEngineHandle eng = ggql_engine_load(idx);
    check(eng != nullptr, "ggql_engine_load deve retornar handle válido");

    // ── 2. Resultado com limit=3, offset=0 ────────────────────────────────────
    GgqlResultHandle r1 = ggql_engine_execute(
        eng,
        "FIND SEGMENT WHERE length > 10000 RETURN id, name, length",
        3, 0
    );
    check(r1 != nullptr, "execute: WHERE length > 10000 LIMIT 3 deve ter resultado");
    check(ggql_result_row_count(r1) == 3,       "row_count deve ser 3");
    check(ggql_result_col_count(r1) == 3,       "col_count deve ser 3");
    check(ggql_result_total_hits(r1) >= 3,      "total_hits >= 3");
    check(!ggql_result_safety_limit_hit(r1),    "safety_limit_hit deve ser false");

    // Verifica nomes de colunas
    check(std::strcmp(ggql_result_col_name(r1, 0), "id")     == 0, "col 0 = id");
    check(std::strcmp(ggql_result_col_name(r1, 1), "name")   == 0, "col 1 = name");
    check(std::strcmp(ggql_result_col_name(r1, 2), "length") == 0, "col 2 = length");

    // Verifica tipos de célula na primeira linha
    check(ggql_result_cell_type(r1, 0, 0) == GGQL_TYPE_INT,    "célula (0,0) deve ser INT");
    check(ggql_result_cell_type(r1, 0, 1) == GGQL_TYPE_STRING, "célula (0,1) deve ser STRING");
    check(ggql_result_cell_type(r1, 0, 2) == GGQL_TYPE_INT,    "célula (0,2) deve ser INT");

    // Comprimento deve ser > 10000
    const int64_t length0 = ggql_result_cell_int(r1, 0, 2);
    check(length0 > 10000, "length na linha 0 deve ser > 10000");

    ggql_result_free(r1);

    // ── 3. Paginação: offset=1 deve pular a primeira linha ────────────────────
    GgqlResultHandle r_p1 = ggql_engine_execute(
        eng,
        "FIND SEGMENT WHERE length > 10000 RETURN id, name, length",
        2, 0
    );
    GgqlResultHandle r_p2 = ggql_engine_execute(
        eng,
        "FIND SEGMENT WHERE length > 10000 RETURN id, name, length",
        2, 1
    );
    check(r_p1 != nullptr && r_p2 != nullptr, "execuções de paginação devem ter resultado");
    check(ggql_result_row_count(r_p1) == 2, "página 1: row_count == 2");
    check(ggql_result_row_count(r_p2) == 2, "página 2: row_count == 2");

    // A linha 0 de r_p2 deve ser a linha 1 de r_p1 (id e name iguais)
    const int64_t id_p1_row1 = ggql_result_cell_int(r_p1, 1, 0);
    const int64_t id_p2_row0 = ggql_result_cell_int(r_p2, 0, 0);
    check(id_p1_row1 == id_p2_row0, "offset shift: id da 2ª linha p1 == 1ª linha p2");

    ggql_result_free(r_p1);
    ggql_result_free(r_p2);

    // ── 4. default limit (0) deve usar kFfiDefaultLimit = 100 ─────────────────
    GgqlResultHandle r_def = ggql_engine_execute(
        eng,
        "FIND SEGMENT RETURN id, name",
        0, 0
    );
    check(r_def != nullptr, "execute com limit=0 deve ter resultado");
    const int64_t n_def = ggql_result_row_count(r_def);
    check(n_def <= 100, "default limit: row_count deve ser <= 100");
    check(ggql_result_total_hits(r_def) > 100, "total_hits deve ser > 100 para SM");
    ggql_result_free(r_def);

    // ── 5. SEARCH KMER com limit via parâmetro ────────────────────────────────
    GgqlResultHandle r_kmer = ggql_engine_execute(
        eng,
        "SEARCH KMER \"ATGCATGC\" RETURN id, name",
        5, 0
    );
    check(r_kmer != nullptr, "SEARCH KMER deve ter resultado");
    check(ggql_result_row_count(r_kmer) <= 5, "SEARCH KMER: row_count <= 5");
    check(ggql_result_col_count(r_kmer) == 2,  "SEARCH KMER: 2 colunas");
    ggql_result_free(r_kmer);

    // ── 6. Query inválida deve retornar NULL e preencher last_error ───────────
    GgqlResultHandle r_bad = ggql_engine_execute(eng, "FIND XYZZY RETURN id", 10, 0);
    check(r_bad == nullptr, "query inválida deve retornar NULL");
    const char* err = ggql_engine_last_error(eng);
    check(err != nullptr && *err != '\0', "last_error deve ser preenchido após erro de parse");

    // ── 7. Limites de célula fora do range ────────────────────────────────────
    GgqlResultHandle r_bound = ggql_engine_execute(
        eng, "FIND SEGMENT WHERE length > 10000 RETURN id", 1, 0);
    check(r_bound != nullptr, "execute para teste de limites");
    check(ggql_result_cell_type(r_bound, 999, 0)  == GGQL_TYPE_NULL, "row OOB → NULL");
    check(ggql_result_cell_type(r_bound, 0,   999) == GGQL_TYPE_NULL, "col OOB → NULL");
    check(ggql_result_cell_int(r_bound, 999, 0)   == 0,               "cell_int OOB → 0");
    check(ggql_result_cell_string(r_bound, 0, 999) == nullptr,        "cell_string OOB → NULL");
    check(ggql_result_col_name(r_bound, 999) == nullptr,              "col_name OOB → NULL");
    ggql_result_free(r_bound);

    // ── 8. result_free(NULL) não deve craShar ─────────────────────────────────
    ggql_result_free(nullptr);

    // ── 8.bis. §3.bis — ggql_engine_load agora carrega topology ──────────────
    //         Antes: FIND PATH retornava "FIND PATH requer GfaCsrTopology".
    //         Depois: deve executar normalmente.
    {
        GgqlResultHandle r = ggql_engine_execute(
            eng, "FIND PATH RETURN id, name", 3, 0);
        check(r != nullptr,
              "FIND PATH via FFI (§3.bis: topology carregado em ggql_engine_load)");
        if (r) {
            check(ggql_result_row_count(r) <= 3, "FIND PATH LIMIT 3 row_count");
            ggql_result_free(r);
        }
    }

    // ── 8.ter. case-insensitive end-to-end via FFI (B-7) ─────────────────────
    {
        GgqlResultHandle r = ggql_engine_execute(
            eng, "find segment limit 1", 1, 0);
        check(r != nullptr && ggql_result_row_count(r) == 1,
              "case-insensitive 'find segment' via FFI (B-7)");
        if (r) { ggql_result_free(r); }
    }

    // ── 10. Round 2.D: ggql_engine_validate parse-only ───────────────────────
    {
        GgqlParseResult pr{};
        // queries válidas
        check(ggql_engine_validate(eng, "FIND SEGMENT", &pr) == 0 && pr.valid,
              "validate: FIND SEGMENT");
        check(ggql_engine_validate(eng,
              "FIND SEGMENT WHERE length > 1000 AND depth >= 3 RETURN id, name",
              &pr) == 0 && pr.valid, "validate: composite WHERE");
        check(ggql_engine_validate(eng, "SEARCH KMER \"ACGT\" RETURN id", &pr) == 0
              && pr.valid, "validate: SEARCH KMER");
        check(ggql_engine_validate(eng, "FIND BUBBLE RETURN source, sink", &pr) == 0
              && pr.valid, "validate: FIND BUBBLE");
        check(ggql_engine_validate(eng, "FIND SEGMENT WHERE length <> 100", &pr) == 0
              && pr.valid, "validate: SQL-92 <>");
        // queries inválidas
        check(ggql_engine_validate(eng, "FIND XYZZY", &pr) == 1 && !pr.valid &&
              pr.error_message != nullptr && *pr.error_message != '\0',
              "validate: FIND XYZZY rejeitado com mensagem");
        check(ggql_engine_validate(eng, "FIND SEGMENT WHERE length >", &pr) == 1
              && !pr.valid, "validate: WHERE quebrado");
        check(ggql_engine_validate(eng, "NOT A QUERY AT ALL", &pr) == 1 && !pr.valid,
              "validate: lixo total");
        // args inválidos
        check(ggql_engine_validate(eng, nullptr, &pr) == -1, "validate: query NULL");
        check(ggql_engine_validate(eng, "FIND SEGMENT", nullptr) == -1, "validate: out NULL");
    }

    // ── 11. Round 2.E: list_entities ──────────────────────────────────────────
    {
        int32_t n = 0;
        const char* const* ents = ggql_engine_list_entities(&n);
        check(ents != nullptr && n == 6, "list_entities: 6 entidades");
        check(std::strcmp(ents[0], "SEGMENT") == 0, "list_entities[0] = SEGMENT");
        check(std::strcmp(ents[1], "EDGE")    == 0, "list_entities[1] = EDGE");
        check(std::strcmp(ents[2], "PATH")    == 0, "list_entities[2] = PATH");
        check(std::strcmp(ents[3], "BUBBLE")  == 0, "list_entities[3] = BUBBLE");
        check(std::strcmp(ents[4], "TIP")     == 0, "list_entities[4] = TIP");
        check(std::strcmp(ents[5], "REPEAT")  == 0, "list_entities[5] = REPEAT");
    }

    // ── 12. Round 2.E: describe_entity ────────────────────────────────────────
    {
        int32_t n = 0;
        const char* const* cols;

        cols = ggql_engine_describe_entity("SEGMENT", &n);
        check(cols && n == 5 && std::strcmp(cols[0], "id") == 0 &&
              std::strcmp(cols[1], "name") == 0 &&
              std::strcmp(cols[4], "depth") == 0, "describe SEGMENT");

        cols = ggql_engine_describe_entity("EDGE", &n);
        check(cols && n == 3 && std::strcmp(cols[0], "from_id") == 0,
              "describe EDGE");

        cols = ggql_engine_describe_entity("PATH", &n);
        check(cols && n == 4 && std::strcmp(cols[2], "node_count") == 0,
              "describe PATH");

        cols = ggql_engine_describe_entity("BUBBLE", &n);
        check(cols && n == 5 && std::strcmp(cols[0], "source") == 0 &&
              std::strcmp(cols[4], "has_cycles") == 0, "describe BUBBLE");

        cols = ggql_engine_describe_entity("TIP", &n);
        check(cols && n == 3 && std::strcmp(cols[1], "in_degree") == 0,
              "describe TIP");

        cols = ggql_engine_describe_entity("REPEAT", &n);
        check(cols && n == 3, "describe REPEAT (mesma shape de TIP)");

        // case-insensitive
        cols = ggql_engine_describe_entity("segment", &n);
        check(cols && n == 5, "describe case-insensitive (segment)");

        // entidade desconhecida → NULL, count = 0
        cols = ggql_engine_describe_entity("XYZZY", &n);
        check(cols == nullptr && n == 0, "describe XYZZY → NULL");

        // arg NULL
        cols = ggql_engine_describe_entity(nullptr, &n);
        check(cols == nullptr, "describe NULL arg → NULL");
    }

    // ── 13. Round 1.B: ggql_engine_capabilities ──────────────────────────────
    {
        GgqlEngineCapabilities caps{};
        check(ggql_engine_capabilities(eng, &caps) == 0, "capabilities rc=0");
        check(caps.has_metadata, "caps: has_metadata=true");
        check(caps.has_topology, "caps: has_topology=true (§3.bis)");
        check(caps.has_fm_index, "caps: has_fm_index=true (SM tem fm_index.bin)");
        check(caps.segment_count > 0, "caps: segment_count > 0");
        check(caps.edge_count    > 0, "caps: edge_count > 0");
        // args inválidos
        check(ggql_engine_capabilities(nullptr, &caps) == -1, "caps engine NULL");
        check(ggql_engine_capabilities(eng, nullptr) == -1, "caps out NULL");
    }

    // ── 14. FIND BUBBLE via FFI — regressão do bug do move ctor (commit 5d9c8a5) ─
    //   Pré-fix: optional<GfaCsrTopology>.emplace(std::move(topo)) descartava
    //   snarls_ → FIND BUBBLE retornava 0 silencioso. Garantir > 0.
    {
        GgqlResultHandle r = ggql_engine_execute(eng, "FIND BUBBLE", 1, 0);
        check(r != nullptr, "FIND BUBBLE via FFI deve executar");
        check(ggql_result_total_hits(r) > 0,
              "FIND BUBBLE total_hits > 0 (regressão move ctor — bug fix 5d9c8a5)");
        ggql_result_free(r);
    }

    // ── 15. Round 3.F: ggql_engine_cancel ─────────────────────────────────────
    {
        // engine NULL é no-op (não deve crashar)
        ggql_engine_cancel(nullptr);

        // Cancel armado antes de execute: o execute reseta o flag no início
        // (ffi.cpp linha ~684), então a query roda completa. Apenas garante
        // que cancel não corrompe o engine.
        ggql_engine_cancel(eng);
        GgqlResultHandle r0 = ggql_engine_execute(
            eng, "FIND SEGMENT RETURN id, name, length", 5, 0);
        check(r0 != nullptr, "execute pós-cancel armado deve funcionar");
        check(ggql_result_row_count(r0) == 5,
              "execute pós-cancel armado: flag foi resetado, query roda normal");
        ggql_result_free(r0);

        // Baseline sem cancel concorrente: SEARCH KMER "AA" varre muitas
        // posições em chm13_subset_sm. Capturar hits para comparar com cancel.
        // (Mantemos cap default — total_hits pode truncar em 1M, mas o ponto
        // do teste é comparar relativo.)
        GgqlResultHandle baseline = ggql_engine_execute(
            eng, "SEARCH KMER \"AA\" RETURN id", 100, 0);
        check(baseline != nullptr, "SEARCH KMER baseline deve executar");
        const int64_t baseline_hits = ggql_result_total_hits(baseline);
        ggql_result_free(baseline);

        // Cancel concorrente: thread dispara cancel após pequeno delay.
        // O check é cooperativo (Paginator::advance), então a query aborta
        // a partir do próximo hit observado pelo planner. Resultado é
        // VÁLIDO (handle não-NULL) mas potencialmente parcial.
        std::atomic<bool> cancel_fired{false};
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            ggql_engine_cancel(eng);
            cancel_fired.store(true);
        });
        GgqlResultHandle rc = ggql_engine_execute(
            eng, "SEARCH KMER \"AA\" RETURN id", 100, 0);
        canceller.join();
        check(cancel_fired.load(), "cancel thread executou");
        check(rc != nullptr, "execute cancelado retorna handle válido (parcial)");
        // total_hits parcial deve ser <= baseline (cooperativo: pode terminar
        // antes do cancel chegar se a query for muito rápida; aceitar igualdade).
        const int64_t cancel_hits = ggql_result_total_hits(rc);
        check(cancel_hits <= baseline_hits,
              "cancel: total_hits parcial <= baseline");
        ggql_result_free(rc);
    }

    // ── 16. Round 3.G: ggql_engine_set_max_total_hits ─────────────────────────
    {
        // engine NULL é no-op
        ggql_engine_set_max_total_hits(nullptr, 100);

        // Cap baixo trunca SEARCH KMER (Round 1.#4: count estrutural não
        // trunca; este cap morde KMER e queries WHERE complexas).
        // Paginator::advance incrementa total_hits ANTES de checar > cap, então
        // o valor final ao parar é cap+1.
        ggql_engine_set_max_total_hits(eng, 5);
        GgqlResultHandle r_cap = ggql_engine_execute(
            eng, "SEARCH KMER \"AA\" RETURN id", 100, 0);
        check(r_cap != nullptr, "SEARCH KMER com cap=5 deve executar");
        check(ggql_result_total_hits(r_cap) <= 6,
              "cap=5: total_hits <= cap+1 = 6");
        check(ggql_result_total_truncated(r_cap),
              "cap=5: total_truncated = true");
        ggql_result_free(r_cap);

        // cap=0 restaura o default
        ggql_engine_set_max_total_hits(eng, 0);
        GgqlResultHandle r_def = ggql_engine_execute(
            eng, "SEARCH KMER \"AA\" RETURN id", 100, 0);
        check(r_def != nullptr, "SEARCH KMER pós-reset deve executar");
        check(ggql_result_total_hits(r_def) > 6,
              "cap=0: total_hits > 6 (default ~1M, não trunca em 6)");
        ggql_result_free(r_def);
    }

    // ── 17. Round 3.H: ggql_engine_execute_streaming ──────────────────────────
    {
        // Args inválidos → -1
        check(ggql_engine_execute_streaming(nullptr, "FIND SEGMENT", 10,
                  [](GgqlResultHandle, int32_t, int64_t, void*) { return true; },
                  nullptr) == -1,
              "streaming: engine NULL → -1");
        check(ggql_engine_execute_streaming(eng, nullptr, 10,
                  [](GgqlResultHandle, int32_t, int64_t, void*) { return true; },
                  nullptr) == -1,
              "streaming: query NULL → -1");
        check(ggql_engine_execute_streaming(eng, "FIND SEGMENT", 10,
                  nullptr, nullptr) == -1,
              "streaming: cb NULL → -1");

        // Total esperado: FIND SEGMENT WHERE length > 10000 retorna N linhas.
        // Streaming page_size=10 deve emitir exatamente N (re-executa em
        // páginas até esgotar; o "natural end" é página com row_count <
        // page_size).
        const char* q = "FIND SEGMENT WHERE length > 10000 RETURN id, name, length";
        GgqlResultHandle full = ggql_engine_execute(eng, q, 1'000'000, 0);
        check(full != nullptr, "streaming: baseline execute deve funcionar");
        const int64_t expected = ggql_result_row_count(full);
        ggql_result_free(full);
        check(expected > 0,
              "streaming: baseline tem linhas (necessário para o teste fazer sentido)");

        struct CountState { int64_t rows = 0; int64_t last_global = -1; };
        CountState st;
        auto counter = [](GgqlResultHandle r, int32_t row_in_page,
                          int64_t global_row, void* ud) -> bool {
            auto* s = static_cast<CountState*>(ud);
            ++s->rows;
            // global_row deve ser monotônico (cada página soma row_in_page+offset)
            if (global_row != s->last_global + 1) {
                std::fprintf(stderr, "streaming: global_row não-monotônico: "
                             "esperado %lld, recebido %lld\n",
                             (long long)(s->last_global + 1),
                             (long long)global_row);
                std::exit(1);
            }
            s->last_global = global_row;
            // Acessar célula só para garantir que o handle ainda é válido
            (void)ggql_result_cell_int(r, row_in_page, 0);
            return true;
        };
        const int64_t emitted = ggql_engine_execute_streaming(eng, q, 10,
                                                              counter, &st);
        check(emitted == expected,
              "streaming: total emitido == row_count do execute completo");
        check(st.rows == expected,
              "streaming: callback contou tudo");

        // Callback que para na 3ª linha. Contrato (ffi.cpp:513-519): a linha
        // cujo cb retornou false NÃO conta em `total_emitted`. Logo cb é
        // chamado 3x mas o retorno é 2 (as duas que retornaram true).
        struct StopState { int64_t rows = 0; };
        StopState sst;
        auto stopper = [](GgqlResultHandle, int32_t, int64_t global_row,
                          void* ud) -> bool {
            auto* s = static_cast<StopState*>(ud);
            ++s->rows;
            return global_row < 2;  // true em 0,1 → false em 2 (para no 3º call)
        };
        const int64_t partial = ggql_engine_execute_streaming(eng, q, 10,
                                                              stopper, &sst);
        check(partial == 2,
              "streaming: cb false na 3ª linha → retorno = 2 (linha do false não conta)");
        check(sst.rows == 3, "streaming: callback chamado 3 vezes antes de stop");

        // page_size=0 usa default 1024
        st = {};
        const int64_t emitted_def = ggql_engine_execute_streaming(eng, q, 0,
                                                                  counter, &st);
        check(emitted_def == expected,
              "streaming: page_size=0 usa default e emite tudo");
    }

    // ── 9. Destroi engine ──────────────────────────────────────────────────────
    ggql_engine_destroy(eng);

    std::puts("test_ffi_query: todos os testes passaram.");
    return 0;
}
