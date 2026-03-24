// ggql_parser_dev.cpp — Driver de desenvolvimento/teste do parser GGQL
//
// Compilar (via CMake):  cmake --build build --target ggql_parser_dev
// Executar:              ./build/ggql_parser_dev

#include <ggql/query/ggql_grammar.hpp>

#include <iostream>
#include <string_view>

using namespace ggql;
using namespace ggql::grammar;

// ─── Dump helpers ────────────────────────────────────────────────────────────

static void dump_value(const Value& v) {
    std::visit([](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, int64_t>)
            std::cout << x;
        else
            std::cout << '"' << x << '"';
    }, v);
}

static void dump_condition(const ConditionExpr& cond) {
    std::visit([](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, CompareCondition>) {
            std::cout << c.field << ' ' << op_to_string(c.op) << ' ';
            dump_value(c.value);
        } else if constexpr (std::is_same_v<T, CrossesCondition>) {
            std::cout << "crosses(" << c.path_name << ')';
        } else if constexpr (std::is_same_v<T, ContainsKmerCondition>) {
            std::cout << "contains_kmer(\"" << c.kmer << "\")";
        }
    }, cond);
}

static void dump_query(const GgqlQuery& q) {
    std::visit([](const auto& qr) {
        using T = std::decay_t<decltype(qr)>;

        if constexpr (std::is_same_v<T, FindQuery>) {
            std::cout << "  FindQuery {\n";
            std::cout << "    entity  : " << entity_to_string(qr.entity) << '\n';

            if (qr.where) {
                std::cout << "    WHERE:\n";
                const auto& wc = *qr.where;
                for (size_t i = 0; i < wc.conditions.size(); ++i) {
                    if (i > 0)
                        std::cout << "      "
                                  << logical_op_to_string(wc.connectors[i - 1])
                                  << '\n';
                    std::cout << "      ";
                    dump_condition(wc.conditions[i]);
                    std::cout << '\n';
                }
            }

            if (!qr.return_fields.empty()) {
                std::cout << "    RETURN  :";
                for (const auto& f : qr.return_fields)
                    std::cout << ' ' << f;
                std::cout << '\n';
            }
            std::cout << "  }\n";

        } else {
            std::cout << "  SearchKmerQuery {\n";
            std::cout << "    kmer    : \"" << qr.kmer << "\"\n";
            if (!qr.return_fields.empty()) {
                std::cout << "    RETURN  :";
                for (const auto& f : qr.return_fields)
                    std::cout << ' ' << f;
                std::cout << '\n';
            }
            std::cout << "  }\n";
        }
    }, q);
}

// ─── Runner de caso de teste ──────────────────────────────────────────────────

static int g_pass = 0, g_fail = 0;

static void test(std::string_view label, std::string_view input,
                 bool expect_ok = true) {
    std::cout << "── " << label << " ──\n";
    std::cout << "  input : \"" << input << "\"\n";

    auto result = parse_ggql(input);

    if (result && expect_ok) {
        std::cout << "  [OK]\n";
        dump_query(*result);
        ++g_pass;
    } else if (!result && !expect_ok) {
        std::cout << "  [OK — rejeição esperada]: " << result.error() << '\n';
        ++g_pass;
    } else if (result && !expect_ok) {
        std::cout << "  [FALHA — deveria ter rejeitado]\n";
        dump_query(*result);
        ++g_fail;
    } else {
        std::cout << "  [FALHA]: " << result.error() << '\n';
        ++g_fail;
    }
    std::cout << '\n';
}

// ─── Suite de testes ──────────────────────────────────────────────────────────

int main() {
    // ── casos válidos ─────────────────────────────────────────────────────────
    test("FIND TIP mínimo",
         "FIND TIP");

    test("FIND BUBBLE mínimo",
         "FIND BUBBLE");

    test("FIND TIP WHERE simples",
         "FIND TIP WHERE length < 200");

    test("FIND TIP WHERE + RETURN",
         "FIND TIP\nWHERE length < 200\nRETURN id, length");

    test("FIND BUBBLE WHERE AND + RETURN",
         "FIND BUBBLE WHERE depth > 3 AND length >= 1000 RETURN id, depth");

    test("FIND BUBBLE WHERE OR",
         "FIND BUBBLE WHERE depth < 2 OR depth > 10 RETURN id");

    test("FIND SEGMENT crosses()",
         "FIND SEGMENT WHERE crosses(path_chr1) RETURN id");

    test("FIND SEGMENT contains_kmer()",
         "FIND SEGMENT WHERE contains_kmer(\"ACGT\") RETURN id, sequence");

    test("FIND SEGMENT valor string",
         "FIND SEGMENT WHERE name = \"chr1\" RETURN id, name");

    test("FIND REPEAT básico",
         "FIND REPEAT");

    test("FIND REPEAT WHERE GEQ + RETURN",
         "FIND REPEAT WHERE count >= 5 RETURN id, count");

    test("FIND EDGE básico",
         "FIND EDGE");

    test("FIND PATH básico",
         "FIND PATH");

    test("FIND SEGMENT operador !=",
         "FIND SEGMENT WHERE depth != 0 RETURN id");

    test("SEARCH KMER mínimo",
         "SEARCH KMER \"ACGTACGT\"");

    test("SEARCH KMER com RETURN",
         "SEARCH KMER \"ACGT\" RETURN id, position");

    test("Espaços extras e newlines",
         "  FIND   TIP  \n  WHERE  length  <=  500  \n  RETURN   id  ,  length  ");

    // ── casos inválidos (rejeição esperada) ──────────────────────────────────
    test("SELECT SQL inválido",       "SELECT * FROM segments",       false);
    test("FIND sem entidade",         "FIND",                         false);
    test("FIND entidade inválida",    "FIND GRAPH",                   false);
    test("SEARCH sem KMER",          "SEARCH \"ACGT\"",              false);
    test("WHERE sem condição",        "FIND TIP WHERE",               false);
    test("Query vazia",               "",                             false);

    // ── sumário ───────────────────────────────────────────────────────────────
    std::cout << "════════════════════════════════════════\n";
    std::cout << "  Resultado: " << g_pass << " OK  |  " << g_fail << " FALHA\n";
    return g_fail == 0 ? 0 : 1;
}
