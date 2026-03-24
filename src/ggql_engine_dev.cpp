// ggql_engine_dev.cpp — Driver de integração: parser + planner + topologia + SQLite
//
// Constrói uma topologia de teste em memória, um banco SQLite temporário com
// metadados de segmentos, e executa queries GGQL completas validando cada resultado.
//
// Compilar:  cmake --build build --target ggql_engine_dev -j 4
// Executar:  ./build/ggql_engine_dev

#include <ggql/query/ggql_grammar.hpp>
#include <ggql/query/query_planner.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string_view>

using namespace ggql;
using namespace ggql::grammar;

// ─── Utilitários de teste ─────────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void pass(std::string_view label) {
    std::cout << "  [OK]  " << label << '\n';
    ++g_pass;
}

static void fail(std::string_view label, std::string_view detail = "") {
    std::cout << "  [FAIL] " << label;
    if (!detail.empty()) std::cout << " — " << detail;
    std::cout << '\n';
    ++g_fail;
}

#define CHECK(label, cond) \
    do { if (cond) pass(label); else fail(label, "condição falsa"); } while(false)

#define CHECK_EQ(label, a, b) \
    do { if ((a) == (b)) pass(label); \
         else fail(label, std::to_string(a) + " != " + std::to_string(b)); } while(false)

// ─── Topologia de teste ───────────────────────────────────────────────────────
//
// Grafo de 5 nós:
//
//   0 → 1 → 2 → 3     (caminho principal)
//           ↘   ↑
//            4 ─┘     (ramo alternativo: cria bolha entre 1 e 3)
//
// Estrutura esperada:
//   Tips   : {0, 3}    (0 tem in_degree=0; 3 tem out_degree=0)
//   Repeats: {1}       (out_degree=2: para 2 e para 4)
//   Bubbles: source=1, sink=3, branches=[2, 4]

static GfaCsrTopology build_test_topology() {
    std::vector<LinkRecord> links = {
        LinkRecord::make(0, '+', 1, '+'),
        LinkRecord::make(1, '+', 2, '+'),
        LinkRecord::make(1, '+', 4, '+'),
        LinkRecord::make(2, '+', 3, '+'),
        LinkRecord::make(4, '+', 3, '+'),
    };
    GfaCsrTopology topo;
    topo.build(5, std::move(links), 1);
    return topo;
}

// ─── Banco SQLite de teste ────────────────────────────────────────────────────
//
// Cria um arquivo temporário com a tabela `segments` do schema da
// GfaMetadataStore e insere 6 segmentos de teste.

static std::string create_test_db() {
    const std::string path = "/tmp/ggql_test_engine.sqlite";
    std::filesystem::remove(path); // garante banco limpo

    sqlite3* db = nullptr;
    sqlite3_open(path.c_str(), &db);
    assert(db);

    const char* ddl = R"(
        PRAGMA journal_mode = OFF;
        PRAGMA synchronous  = OFF;
        CREATE TABLE IF NOT EXISTS segments (
            internal_id  INTEGER PRIMARY KEY,
            original_id  TEXT    NOT NULL,
            length       INTEGER NOT NULL,
            read_count   INTEGER,
            depth        REAL
        );
        CREATE INDEX IF NOT EXISTS idx_segments_original_id
            ON segments(original_id);
    )";
    char* errmsg = nullptr;
    sqlite3_exec(db, ddl, nullptr, nullptr, &errmsg);
    sqlite3_free(errmsg);

    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db,
        "INSERT INTO segments (internal_id, original_id, length, read_count, depth)"
        " VALUES (?, ?, ?, ?, ?)",
        -1, &stmt, nullptr);

    // internal_id, name, length, read_count, depth
    const struct { int64_t id; const char* name; int len; int rc; double depth; }
    segs[] = {
        {0, "ctg001", 500,  10, 3.5},
        {1, "ctg002", 1200, 25, 8.1},
        {2, "ctg003", 300,   5, 1.2},
        {3, "ctg004", 900,  18, 6.0},
        {4, "ctg005", 150,   2, 0.8},
        {5, "ctg006", 2000, 40, 12.3},
    };

    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    for (const auto& s : segs) {
        sqlite3_reset(stmt);
        sqlite3_bind_int64(stmt, 1, s.id);
        sqlite3_bind_text(stmt, 2, s.name, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt,  3, s.len);
        sqlite3_bind_int(stmt,  4, s.rc);
        sqlite3_bind_double(stmt, 5, s.depth);
        sqlite3_step(stmt);
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return path;
}

// ─── Runner de query ──────────────────────────────────────────────────────────

static std::optional<QueryResult> run(const QueryPlanner& planner,
                                       std::string_view    ggql_str) {
    auto parsed = parse_ggql(ggql_str);
    if (!parsed) {
        std::cerr << "  Erro de parse: " << parsed.error() << '\n';
        return {};
    }
    auto result = planner.execute(*parsed);
    if (!result) {
        std::cerr << "  Erro de execução: " << result.error() << '\n';
        return {};
    }
    return *result;
}

// ─── Suíte de testes ──────────────────────────────────────────────────────────

static void test_tips(const QueryPlanner& planner) {
    std::cout << "\n── FIND TIP ──\n";

    // FIND TIP → deve retornar nós 0 e 3
    {
        auto r = run(planner, "FIND TIP");
        CHECK("FIND TIP retorna 2 nós", r && r->rows.size() == 2);
        if (r) std::cout << r->format_table();
    }

    // FIND TIP WHERE in_degree = 0 RETURN id → apenas nó 0
    {
        auto r = run(planner, "FIND TIP WHERE in_degree = 0 RETURN id");
        CHECK("TIP WHERE in_degree=0: 1 resultado", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            const auto& row = r->rows[0];
            CHECK("TIP WHERE in_degree=0: id correto",
                  std::get<int64_t>(row.at("id")) == 0);
            std::cout << r->format_table();
        }
    }

    // FIND TIP WHERE out_degree = 0 RETURN id → apenas nó 3
    {
        auto r = run(planner, "FIND TIP WHERE out_degree = 0 RETURN id");
        CHECK("TIP WHERE out_degree=0: 1 resultado", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            CHECK("TIP WHERE out_degree=0: id=3",
                  std::get<int64_t>(r->rows[0].at("id")) == 3);
        }
    }

    // FIND TIP WHERE in_degree > 5 → 0 resultados (nenhum tip com in>5)
    {
        auto r = run(planner, "FIND TIP WHERE in_degree > 5");
        CHECK("TIP WHERE in_degree>5: 0 resultados", r && r->rows.empty());
    }
}

static void test_repeats(const QueryPlanner& planner) {
    std::cout << "\n── FIND REPEAT ──\n";

    // FIND REPEAT → nó 1 (out_degree=2)
    {
        auto r = run(planner, "FIND REPEAT");
        CHECK("FIND REPEAT retorna 1 nó", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            CHECK("REPEAT: id=1",
                  std::get<int64_t>(r->rows[0].at("id")) == 1);
            std::cout << r->format_table();
        }
    }

    // FIND REPEAT WHERE out_degree >= 2 RETURN id, out_degree
    {
        auto r = run(planner,
            "FIND REPEAT WHERE out_degree >= 2 RETURN id, out_degree");
        CHECK("REPEAT WHERE out_degree>=2: 1 resultado", r && r->rows.size() == 1);
        if (r) std::cout << r->format_table();
    }

    // FIND REPEAT WHERE out_degree >= 3 → 0 resultados
    {
        auto r = run(planner, "FIND REPEAT WHERE out_degree >= 3");
        CHECK("REPEAT WHERE out_degree>=3: 0 resultados", r && r->rows.empty());
    }
}

static void test_bubbles(const QueryPlanner& planner) {
    std::cout << "\n── FIND BUBBLE ──\n";

    // FIND BUBBLE → 1 bolha (source=1, sink=3)
    {
        auto r = run(planner, "FIND BUBBLE");
        CHECK("FIND BUBBLE retorna >= 1 bolha", r && !r->rows.empty());
        if (r) std::cout << r->format_table();
    }

    // FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count
    {
        auto r = run(planner,
            "FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count");
        CHECK("BUBBLE WHERE branch_count>=2: >= 1", r && !r->rows.empty());
        if (r && !r->rows.empty()) {
            CHECK("BUBBLE: source=1",
                  std::get<int64_t>(r->rows[0].at("source")) == 1);
            CHECK("BUBBLE: sink=3",
                  std::get<int64_t>(r->rows[0].at("sink")) == 3);
            std::cout << r->format_table();
        }
    }

    // FIND BUBBLE WHERE branch_count > 100 → 0 resultados
    {
        auto r = run(planner, "FIND BUBBLE WHERE branch_count > 100");
        CHECK("BUBBLE WHERE branch_count>100: 0", r && r->rows.empty());
    }
}

static void test_segments(const QueryPlanner& planner) {
    std::cout << "\n── FIND SEGMENT ──\n";

    // FIND SEGMENT → 6 segmentos
    {
        auto r = run(planner, "FIND SEGMENT");
        CHECK("FIND SEGMENT retorna 6", r && r->rows.size() == 6);
        if (r) std::cout << r->format_table();
    }

    // FIND SEGMENT WHERE length > 1000 RETURN id, name, length
    {
        auto r = run(planner,
            "FIND SEGMENT WHERE length > 1000 RETURN id, name, length");
        // ctg002 (1200) e ctg006 (2000) → 2 resultados
        CHECK("SEGMENT WHERE length>1000: 2 resultados", r && r->rows.size() == 2);
        if (r) std::cout << r->format_table();
    }

    // FIND SEGMENT WHERE length >= 500 AND length <= 1200 RETURN id, name, length
    {
        auto r = run(planner,
            "FIND SEGMENT WHERE length >= 500 AND length <= 1200 RETURN id, name, length");
        // ctg001 (500), ctg002 (1200), ctg004 (900) → 3 resultados
        CHECK("SEGMENT WHERE 500<=length<=1200: 3 resultados", r && r->rows.size() == 3);
        if (r) std::cout << r->format_table();
    }

    // FIND SEGMENT WHERE name = "ctg003" RETURN id, name, length
    {
        auto r = run(planner,
            R"(FIND SEGMENT WHERE name = "ctg003" RETURN id, name, length)");
        CHECK("SEGMENT WHERE name=ctg003: 1 resultado", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            CHECK("SEGMENT ctg003: length=300",
                  std::get<int64_t>(r->rows[0].at("length")) == 300);
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE length <= 200 RETURN id, name
    {
        auto r = run(planner,
            "FIND SEGMENT WHERE length <= 200 RETURN id, name");
        // ctg005 (150) → 1 resultado
        CHECK("SEGMENT WHERE length<=200: 1 resultado", r && r->rows.size() == 1);
        if (r) std::cout << r->format_table();
    }

    // FIND SEGMENT WHERE length > 9999 → 0 resultados
    {
        auto r = run(planner, "FIND SEGMENT WHERE length > 9999");
        CHECK("SEGMENT WHERE length>9999: 0 resultados", r && r->rows.empty());
    }

    // OR: FIND SEGMENT WHERE length < 200 OR length > 1500 RETURN id, name, length
    {
        auto r = run(planner,
            "FIND SEGMENT WHERE length < 200 OR length > 1500 RETURN id, name, length");
        // ctg005 (150) + ctg006 (2000) → 2 resultados
        CHECK("SEGMENT WHERE length<200 OR >1500: 2 resultados", r && r->rows.size() == 2);
        if (r) std::cout << r->format_table();
    }
}

static void test_error_cases(const QueryPlanner& planner) {
    std::cout << "\n── Casos de erro esperados ──\n";

    // SEARCH KMER → ainda não implementado
    {
        auto parsed = parse_ggql(R"(SEARCH KMER "ACGT")");
        CHECK("SEARCH KMER parseia", parsed.has_value());
        if (parsed) {
            auto r = planner.execute(*parsed);
            CHECK("SEARCH KMER retorna erro esperado", !r.has_value());
            if (!r) std::cout << "  erro: " << r.error() << '\n';
        }
    }

    // FIND EDGE → ainda não implementado
    {
        auto r = run(planner, "FIND EDGE");
        CHECK("FIND EDGE retorna erro esperado", !r.has_value());
    }

    // FIND PATH → ainda não implementado
    {
        auto r = run(planner, "FIND PATH");
        CHECK("FIND PATH retorna erro esperado", !r.has_value());
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "═══════════════════════════════════════════════════\n";
    std::cout << " ggql_engine_dev — Integração Parser + QueryPlanner\n";
    std::cout << "═══════════════════════════════════════════════════\n";

    // ── Monta topologia ───────────────────────────────────────────────────────
    std::cout << "\nConstruindo topologia de teste (5 nós, 5 arestas)...\n";
    const GfaCsrTopology topo = build_test_topology();
    std::cout << "  nós   : " << topo.num_nodes() << '\n';
    std::cout << "  arestas: " << topo.num_edges() << '\n';

    // ── Monta banco SQLite ────────────────────────────────────────────────────
    std::cout << "\nCriando banco SQLite de teste (/tmp/)...\n";
    const std::string db_path = create_test_db();
    std::cout << "  banco : " << db_path << '\n';

    // ── Cria QueryPlanner com ambos os backends ───────────────────────────────
    const QueryPlanner planner{&topo, db_path};

    // ── Suíte de testes ───────────────────────────────────────────────────────
    test_tips(planner);
    test_repeats(planner);
    test_bubbles(planner);
    test_segments(planner);
    test_error_cases(planner);

    // ── Limpeza ───────────────────────────────────────────────────────────────
    std::filesystem::remove(db_path);

    // ── Sumário ───────────────────────────────────────────────────────────────
    std::cout << "\n═══════════════════════════════════════════════════\n";
    std::cout << "  Resultado: " << g_pass << " OK  |  " << g_fail << " FALHA\n";
    return g_fail == 0 ? 0 : 1;
}
