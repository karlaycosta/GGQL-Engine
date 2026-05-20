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

    // ── 9. Destroi engine ──────────────────────────────────────────────────────
    ggql_engine_destroy(eng);

    std::puts("test_ffi_query: todos os testes passaram.");
    return 0;
}
