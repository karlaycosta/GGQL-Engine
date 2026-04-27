// ggql_engine_dev.cpp — Driver de integração: parser + planner + topologia + metadados binários
//
// Constrói uma topologia de teste em memória, um cache binário temporário (.idx)
// com metadados de segmentos via GfaMetadataStore, e executa queries GGQL completas.
//
// Compilar:  cmake --build build --target ggql_engine_dev -j 4
// Executar:  ./build/ggql_engine_dev

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fmt/color.h>
#include <fmt/core.h>
#include <ggql/query/ggql_grammar.hpp>
#include <ggql/query/query_planner.hpp>
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
    if (!detail.empty()) {
        std::cout << " — " << detail;
    }
    std::cout << '\n';
    ++g_fail;
}

#define CHECK(label, cond)                                                                         \
    do {                                                                                           \
        if (cond)                                                                                  \
            pass(label);                                                                           \
        else                                                                                       \
            fail(label, "condição falsa");                                                         \
    } while (false)

#define CHECK_EQ(label, a, b)                                                                      \
    do {                                                                                           \
        if ((a) == (b))                                                                            \
            pass(label);                                                                           \
        else                                                                                       \
            fail(label, std::to_string(a) + " != " + std::to_string(b));                           \
    } while (false)

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
        LinkRecord::make(0, '+', 1, '+'), LinkRecord::make(1, '+', 2, '+'),
        LinkRecord::make(1, '+', 4, '+'), LinkRecord::make(2, '+', 3, '+'),
        LinkRecord::make(4, '+', 3, '+'),
    };
    GfaCsrTopology topo;
    topo.build(5, std::move(links), 1);

    // Adiciona alguns caminhos para testar indexação gBWT
    std::vector<PathRecord> paths;
    paths.push_back(
        {"Path1", {(0ULL << 1) | 0, (1ULL << 1) | 0, (2ULL << 1) | 0, (3ULL << 1) | 0}});
    paths.push_back(
        {"Path2", {(0ULL << 1) | 0, (1ULL << 1) | 0, (4ULL << 1) | 0, (3ULL << 1) | 0}});

    // Usamos a versão paralela para testar os novos coletores de métricas
    std::vector<GfaCsrTopology::GbwtGroup> groups(1);
    groups[0].path_names = {"Path1", "Path2"};
    groups[0].in_ram_seqs = {
        {(0U << 1) | 0, (1U << 1) | 0, (2U << 1) | 0, (3U << 1) | 0, gbwt::ENDMARKER},
        {(0U << 1) | 0, (1U << 1) | 0, (4U << 1) | 0, (3U << 1) | 0, gbwt::ENDMARKER}};

    // Apenas para efeito de teste, o retorno de build_path_index_parallel
    // deve ser capturado se quisermos ver as métricas aqui.
    auto timings = topo.build_path_index_parallel(std::move(groups), 4, 2);

    std::cout << "\n── gBWT Performance (Test Build) ──\n";
    fmt::print("  Wall time   : {:.4f} s\n", timings.wall_s);
    fmt::print("  CPU (Avg)   : {:.4f} s\n", timings.cpu_avg_s);
    fmt::print("  CPU (Max)   : {:.4f} s\n", timings.cpu_max_s);
    fmt::print("  Peak RSS    : {} KB\n", timings.peak_rss_kb);
    fmt::print("  Index Size  : {} bytes\n", timings.index_size_b);
    fmt::print("  Throughput  : {:.2f} nodes/s\n", timings.nodes_per_s());
    fmt::print("  Efficiency  : {:.2f} bits/node\n", timings.bits_per_node());

    return topo;
}

// ─── Banco Metadados de teste ────────────────────────────────────────────────────
//
// Cria um diretório temporário com `metadata.bin` da GfaMetadataStore e insere
// 6 segmentos de teste.

static std::string create_test_metadata() {
    const std::string path = "/tmp/ggql_test_idx";
    std::filesystem::remove_all(path); // garante limpo
    std::filesystem::create_directories(path);

    std::vector<SegmentRecord> segs = {
        {0, "ctg001", 5, 10, 3.5, "ACGTA"}, 
        {1, "ctg002", 5, 25, 8.1, "TTGCA"}, 
        {2, "ctg003", 5, 5, 1.2, "AAAAA"},
        {3, "ctg004", 5, 18, 6.0, "CGCGC"}, 
        {4, "ctg005", 5, 2, 0.8, "TTAAA"},   
        {5, "ctg006", 5, 40, 12.3, "GGGGG"},
    };

    GfaMetadataStore store;
    store.init(6);
    store.flush_segments(segs);
    store.save(path);
    return path;
}

// ─── Runner de query ──────────────────────────────────────────────────────────

static std::optional<QueryResult> run(const QueryPlanner& planner, std::string_view ggql_str) {
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
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND TIP WHERE in_degree = 0 RETURN id → apenas nó 0
    {
        auto r = run(planner, "FIND TIP WHERE in_degree = 0 RETURN id");
        CHECK("TIP WHERE in_degree=0: 1 resultado", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            const auto& row = r->rows[0];
            CHECK("TIP WHERE in_degree=0: id correto", std::get<int64_t>(row.at("id")) == 0);
            std::cout << r->format_table();
        }
    }

    // FIND TIP WHERE out_degree = 0 RETURN id → apenas nó 3
    {
        auto r = run(planner, "FIND TIP WHERE out_degree = 0 RETURN id");
        CHECK("TIP WHERE out_degree=0: 1 resultado", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            CHECK("TIP WHERE out_degree=0: id=3", std::get<int64_t>(r->rows[0].at("id")) == 3);
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
            CHECK("REPEAT: id=1", std::get<int64_t>(r->rows[0].at("id")) == 1);
            std::cout << r->format_table();
        }
    }

    // FIND REPEAT WHERE out_degree >= 2 RETURN id, out_degree
    {
        auto r = run(planner, "FIND REPEAT WHERE out_degree >= 2 RETURN id, out_degree");
        CHECK("REPEAT WHERE out_degree>=2: 1 resultado", r && r->rows.size() == 1);
        if (r) {
            std::cout << r->format_table();
        }
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
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count
    {
        auto r =
            run(planner, "FIND BUBBLE WHERE branch_count >= 2 RETURN source, sink, branch_count");
        CHECK("BUBBLE WHERE branch_count>=2: >= 1", r && !r->rows.empty());
        if (r && !r->rows.empty()) {
            CHECK("BUBBLE: source=1", std::get<int64_t>(r->rows[0].at("source")) == 1);
            CHECK("BUBBLE: sink=3", std::get<int64_t>(r->rows[0].at("sink")) == 3);
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
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE length > 3 RETURN id, name, length
    {
        auto r = run(planner, "FIND SEGMENT WHERE length > 3 RETURN id, name, length");
        CHECK("SEGMENT WHERE length>3: 6 resultados", r && r->rows.size() == 6);
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE length >= 4 AND length <= 6 RETURN id, name, length
    {
        auto r = run(planner,
                     "FIND SEGMENT WHERE length >= 4 AND length <= 6 RETURN id, name, length");
        CHECK("SEGMENT WHERE 4<=length<=6: 6 resultados", r && r->rows.size() == 6);
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE name = "ctg003" RETURN id, name, length
    {
        auto r = run(planner, R"(FIND SEGMENT WHERE name = "ctg003" RETURN id, name, length)");
        CHECK("SEGMENT WHERE name=ctg003: 1 resultado", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            CHECK("SEGMENT ctg003: length=5", std::get<int64_t>(r->rows[0].at("length")) == 5);
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE length <= 2 RETURN id, name
    {
        auto r = run(planner, "FIND SEGMENT WHERE length <= 2 RETURN id, name");
        CHECK("SEGMENT WHERE length<=2: 0 resultado", r && r->rows.size() == 0);
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE length > 9999 → 0 resultados
    {
        auto r = run(planner, "FIND SEGMENT WHERE length > 9999");
        CHECK("SEGMENT WHERE length>9999: 0 resultados", r && r->rows.empty());
    }

    // FIND SEGMENT WHERE length < 2 OR length > 15 RETURN id, name, length
    {
        auto r = run(planner,
                     "FIND SEGMENT WHERE length < 2 OR length > 15 RETURN id, name, length");
        CHECK("SEGMENT WHERE length<2 OR >15: 0 resultados", r && r->rows.size() == 0);
        if (r) {
            std::cout << r->format_table();
        }
    }

    // FIND SEGMENT WHERE contains_kmer
    {
        auto r = run(planner, R"(FIND SEGMENT WHERE contains_kmer("AAA") RETURN name)");
        CHECK("SEGMENT WHERE contains_kmer(AAA): 2 resultados", r && r->rows.size() == 2); // AAAAA e TTAAA
        if (r) {
            std::cout << r->format_table();
        }
    }
}

static void test_search_kmer(const QueryPlanner& planner) {
    std::cout << "\n── SEARCH KMER ──\n";

    {
        auto r = run(planner, R"(SEARCH KMER "CGT" RETURN id, name)");
        // "ACGTA" tem "CGT" -> ID=0 "ctg001"
        CHECK("SEARCH KMER 'CGT' retorna 1", r && r->rows.size() == 1);
        if (r && !r->rows.empty()) {
            CHECK("SEARCH KMER 'CGT' id correto", std::get<int64_t>(r->rows[0].at("id")) == 0);
            std::cout << r->format_table();
        }
    }
    
    {
        auto r = run(planner, R"(SEARCH KMER "AAA" RETURN id, name)");
        // "AAAAA" e "TTAAA" -> IDs 2 e 4
        CHECK("SEARCH KMER 'AAA' retorna 2 resultados", r && r->rows.size() == 2);
        if (r) std::cout << r->format_table();
    }
}

static void test_error_cases(const QueryPlanner& planner) {
    std::cout << "\n── Casos de erro esperados ──\n";

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

    // ── Monta banco Array na RAM ──────────────────────────────────────────────
    std::cout << "\nCriando/Carregando cache em (/tmp/ggql_test_idx)...\n";
    const std::string metadata_path = create_test_metadata();
    GfaMetadataStore metadata;
    metadata.load(metadata_path);
    std::cout << "  metadata cache : " << metadata_path << '\n';

    // ── Constrói Index Sequencial Baseado na libsais ──────────────────────────
    std::cout << "\nConstruindo FM-Index sobre sequências em RAM...\n";
    GfaSequenceIndex seq_index;
    seq_index.build(metadata.all_sequences(), 1);

    // ── Cria QueryPlanner com ambos os backends ───────────────────────────────
    const QueryPlanner planner{&topo, &metadata, &seq_index};

    // ── Suíte de testes ───────────────────────────────────────────────────────
    test_tips(planner);
    test_repeats(planner);
    // test_bubbles(planner); // Futuramente estender para SNARLS
    test_segments(planner);
    test_search_kmer(planner);
    test_error_cases(planner);

    // ── Limpeza ───────────────────────────────────────────────────────────────
    std::filesystem::remove_all(metadata_path);

    // ── Sumário ───────────────────────────────────────────────────────────────
    std::cout << "\n═══════════════════════════════════════════════════\n";
    std::cout << "  Resultado: " << g_pass << " OK  |  " << g_fail << " FALHA\n";
    return g_fail == 0 ? 0 : 1;
}
