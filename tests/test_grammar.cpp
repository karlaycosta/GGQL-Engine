// test_grammar.cpp — Testes unitários da gramática GGQL (parse_ggql).
//
// Cobre o parser puro, sem planner/topology/.idx. Validação:
//   A. Statements básicos: FIND <entity> + SEARCH KMER
//   B. Operadores relacionais (=, !=, <, >, <=, >=)
//   C. Tipos de valor (int, float, string)
//   D. AND/OR — 2-condição, 3+ mistos
//   E. Function calls (crosses, contains_kmer)
//   F. RETURN — single/multi/ausente
//   G. LIMIT / OFFSET
//   H. Erros sintáticos (keyword/op/entity/fn desconhecidos)
//   I. Identificadores reservados em WHERE/RETURN
//   J. Case-sensitivity das keywords
//   K. Overflow numérico (saturação silenciosa — L-10 do queries_status)
//   L. Whitespace (espaços extras, newlines)

#include "ggql/query/grammar.hpp"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

using namespace ggql;
using namespace ggql::grammar;

// ─── Infra mínima ─────────────────────────────────────────────────────────────

static int n_pass = 0, n_fail = 0;

static void check(bool cond, const char* msg) {
    if (cond) { std::printf("[ OK ] %s\n", msg); ++n_pass; }
    else      { std::printf("[FAIL] %s\n", msg); ++n_fail; }
}

// Helper: parseia e retorna FindQuery (assume sucesso); caller checa atributos.
static const FindQuery* parse_find(std::string_view q) {
    static thread_local std::optional<GgqlQuery> last;
    auto r = parse_ggql(q);
    if (!r) {
        last.reset();
        return nullptr;
    }
    last.emplace(std::move(*r));
    return std::get_if<FindQuery>(&*last);
}

static const SearchKmerQuery* parse_search(std::string_view q) {
    static thread_local std::optional<GgqlQuery> last;
    auto r = parse_ggql(q);
    if (!r) {
        last.reset();
        return nullptr;
    }
    last.emplace(std::move(*r));
    return std::get_if<SearchKmerQuery>(&*last);
}

static bool parses_ok(std::string_view q) {
    return parse_ggql(q).has_value();
}

// ─── A. Statements básicos ────────────────────────────────────────────────────

static void test_a_basic_statements() {
    std::printf("\n── A. Statements básicos ──\n");
    using enum EntityType;
    struct Case { std::string_view q; EntityType e; const char* msg; };
    constexpr Case cases[] = {
        {"FIND SEGMENT",  SEGMENT, "FIND SEGMENT"},
        {"FIND EDGE",     EDGE,    "FIND EDGE"},
        {"FIND PATH",     PATH,    "FIND PATH"},
        {"FIND BUBBLE",   BUBBLE,  "FIND BUBBLE"},
        {"FIND TIP",      TIP,     "FIND TIP"},
        {"FIND REPEAT",   REPEAT,  "FIND REPEAT"},
    };
    for (const auto& c : cases) {
        auto* fq = parse_find(c.q);
        check(fq != nullptr && fq->entity == c.e, c.msg);
    }
    auto* sq = parse_search(R"(SEARCH KMER "ACGT")");
    check(sq != nullptr && sq->kmer == "ACGT",       "SEARCH KMER \"ACGT\"");
}

// ─── B. Operadores relacionais ────────────────────────────────────────────────

static void test_b_operators() {
    std::printf("\n── B. Operadores relacionais ──\n");
    struct Case { std::string_view q; Operator op; const char* msg; };
    constexpr Case cases[] = {
        {"FIND SEGMENT WHERE length = 100",  Operator::EQ,  "WHERE x = 100"},
        {"FIND SEGMENT WHERE length != 100", Operator::NEQ, "WHERE x != 100"},
        {"FIND SEGMENT WHERE length < 100",  Operator::LT,  "WHERE x < 100"},
        {"FIND SEGMENT WHERE length > 100",  Operator::GT,  "WHERE x > 100"},
        {"FIND SEGMENT WHERE length <= 100", Operator::LEQ, "WHERE x <= 100"},
        {"FIND SEGMENT WHERE length >= 100", Operator::GEQ, "WHERE x >= 100"},
    };
    for (const auto& c : cases) {
        auto* fq = parse_find(c.q);
        if (!fq || !fq->where || fq->where->conditions.size() != 1) {
            check(false, c.msg);
            continue;
        }
        auto* cc = std::get_if<CompareCondition>(&fq->where->conditions[0]);
        check(cc != nullptr && cc->op == c.op, c.msg);
    }
}

// ─── C. Tipos de valor ────────────────────────────────────────────────────────

static void test_c_value_types() {
    std::printf("\n── C. Tipos de valor ──\n");

    auto* fq_i = parse_find("FIND SEGMENT WHERE length = 12345");
    {
        auto* cc = (fq_i && fq_i->where) ? std::get_if<CompareCondition>(&fq_i->where->conditions[0]) : nullptr;
        const auto* iv = cc ? std::get_if<int64_t>(&cc->value) : nullptr;
        check(iv && *iv == 12345,                                "valor int64");
    }

    auto* fq_f = parse_find("FIND SEGMENT WHERE depth >= 2.5");
    {
        auto* cc = (fq_f && fq_f->where) ? std::get_if<CompareCondition>(&fq_f->where->conditions[0]) : nullptr;
        const auto* dv = cc ? std::get_if<double>(&cc->value) : nullptr;
        check(dv && *dv == 2.5,                                  "valor double");
    }

    auto* fq_s = parse_find(R"(FIND SEGMENT WHERE name = "HG002")");
    {
        auto* cc = (fq_s && fq_s->where) ? std::get_if<CompareCondition>(&fq_s->where->conditions[0]) : nullptr;
        const auto* sv = cc ? std::get_if<std::string>(&cc->value) : nullptr;
        check(sv && *sv == "HG002",                              "valor string");
    }

    // String com chars especiais
    auto* fq_s2 = parse_find(R"(FIND PATH WHERE name = "HG002#1#chr1")");
    {
        auto* cc = (fq_s2 && fq_s2->where) ? std::get_if<CompareCondition>(&fq_s2->where->conditions[0]) : nullptr;
        const auto* sv = cc ? std::get_if<std::string>(&cc->value) : nullptr;
        check(sv && *sv == "HG002#1#chr1",                       "string com '#'");
    }

    // String vazia
    auto* fq_s3 = parse_find(R"(FIND PATH WHERE name = "")");
    {
        auto* cc = (fq_s3 && fq_s3->where) ? std::get_if<CompareCondition>(&fq_s3->where->conditions[0]) : nullptr;
        const auto* sv = cc ? std::get_if<std::string>(&cc->value) : nullptr;
        check(sv && sv->empty(),                                 "string vazia");
    }
}

// ─── D. AND/OR (incluindo 3+ condições mistas) ────────────────────────────────

static void test_d_and_or() {
    std::printf("\n── D. AND/OR ──\n");

    auto* fq_and = parse_find("FIND SEGMENT WHERE length > 100 AND length < 1000");
    check(fq_and && fq_and->where &&
          fq_and->where->conditions.size() == 2 &&
          fq_and->where->connectors.size() == 1 &&
          fq_and->where->connectors[0] == LogicalOp::AND,
          "AND simples — 2 cond + 1 connector AND");

    auto* fq_or = parse_find("FIND SEGMENT WHERE length > 100 OR length < 10");
    check(fq_or && fq_or->where &&
          fq_or->where->conditions.size() == 2 &&
          fq_or->where->connectors[0] == LogicalOp::OR,
          "OR simples");

    // 3 condições mistas
    auto* fq_mix = parse_find(
        "FIND SEGMENT WHERE length > 100 AND length < 1000 OR depth >= 2.0");
    check(fq_mix && fq_mix->where &&
          fq_mix->where->conditions.size() == 3 &&
          fq_mix->where->connectors.size() == 2 &&
          fq_mix->where->connectors[0] == LogicalOp::AND &&
          fq_mix->where->connectors[1] == LogicalOp::OR,
          "3 condições — AND + OR (esquerda-para-direita)");

    // 4 condições com OR/AND/OR
    auto* fq_4 = parse_find(
        R"(FIND SEGMENT WHERE name = "a" OR name = "b" AND length > 0 OR depth = 1)");
    check(fq_4 && fq_4->where &&
          fq_4->where->conditions.size() == 4 &&
          fq_4->where->connectors.size() == 3,
          "4 condições — OR/AND/OR");

    // is_valid() — invariante interno (connectors == conditions - 1)
    check(fq_4 && fq_4->where && fq_4->where->is_valid(),
          "is_valid() em WHERE com 4 conditions");
}

// ─── E. Function calls ────────────────────────────────────────────────────────

static void test_e_function_calls() {
    std::printf("\n── E. crosses() / contains_kmer() ──\n");

    // crosses() forma legacy (identifier)
    auto* fq_id = parse_find("FIND PATH WHERE crosses(HG002)");
    {
        auto* cc = (fq_id && fq_id->where) ? std::get_if<CrossesCondition>(&fq_id->where->conditions[0]) : nullptr;
        check(cc && cc->path_name == "HG002",                    "crosses(HG002) legacy id");
    }

    // crosses() forma string (B-2)
    auto* fq_str = parse_find(R"(FIND PATH WHERE crosses("HG002#1#chr1"))");
    {
        auto* cc = (fq_str && fq_str->where) ? std::get_if<CrossesCondition>(&fq_str->where->conditions[0]) : nullptr;
        check(cc && cc->path_name == "HG002#1#chr1",             "crosses(\"...\") com '#'");
    }

    // crosses() string vazia
    auto* fq_emp = parse_find(R"(FIND PATH WHERE crosses(""))");
    {
        auto* cc = (fq_emp && fq_emp->where) ? std::get_if<CrossesCondition>(&fq_emp->where->conditions[0]) : nullptr;
        check(cc && cc->path_name.empty(),                       "crosses(\"\") string vazia");
    }

    // contains_kmer() simples
    auto* fq_k = parse_find(R"(FIND SEGMENT WHERE contains_kmer("ACGT"))");
    {
        auto* cc = (fq_k && fq_k->where) ? std::get_if<ContainsKmerCondition>(&fq_k->where->conditions[0]) : nullptr;
        check(cc && cc->kmer == "ACGT",                          "contains_kmer(\"ACGT\")");
    }

    // contains_kmer() longo
    auto* fq_kl = parse_find(R"(FIND SEGMENT WHERE contains_kmer("ACGTACGTACGTACGTACGT"))");
    {
        auto* cc = (fq_kl && fq_kl->where) ? std::get_if<ContainsKmerCondition>(&fq_kl->where->conditions[0]) : nullptr;
        check(cc && cc->kmer.size() == 20,                       "contains_kmer 20-mer");
    }

    // Combinado: contains_kmer + crosses + AND
    auto* fq_combo = parse_find(
        R"(FIND PATH WHERE contains_kmer("ACG") AND crosses("HG002"))");
    check(fq_combo && fq_combo->where &&
          fq_combo->where->conditions.size() == 2 &&
          std::holds_alternative<ContainsKmerCondition>(fq_combo->where->conditions[0]) &&
          std::holds_alternative<CrossesCondition>(fq_combo->where->conditions[1]),
          "fn calls combinados via AND");
}

// ─── F. RETURN ────────────────────────────────────────────────────────────────

static void test_f_return() {
    std::printf("\n── F. RETURN ──\n");

    auto* fq_none = parse_find("FIND SEGMENT");
    check(fq_none && fq_none->return_fields.empty(),             "sem RETURN — vetor vazio");

    auto* fq_one = parse_find("FIND SEGMENT RETURN id");
    check(fq_one && fq_one->return_fields.size() == 1 &&
          fq_one->return_fields[0] == "id",                      "RETURN id");

    auto* fq_many = parse_find("FIND SEGMENT RETURN id, name, length");
    check(fq_many && fq_many->return_fields.size() == 3 &&
          fq_many->return_fields[0] == "id" &&
          fq_many->return_fields[1] == "name" &&
          fq_many->return_fields[2] == "length",                 "RETURN id, name, length");
}

// ─── G. LIMIT / OFFSET ────────────────────────────────────────────────────────

static void test_g_limit_offset() {
    std::printf("\n── G. LIMIT / OFFSET ──\n");

    auto* fq_l = parse_find("FIND SEGMENT LIMIT 100");
    check(fq_l && fq_l->limit && *fq_l->limit == 100 && !fq_l->offset,
          "LIMIT 100 sem OFFSET");

    auto* fq_lo = parse_find("FIND SEGMENT LIMIT 100 OFFSET 50");
    check(fq_lo && fq_lo->limit && *fq_lo->limit == 100 &&
          fq_lo->offset && *fq_lo->offset == 50,
          "LIMIT 100 OFFSET 50");

    // OFFSET sem LIMIT — gramática rejeita
    check(!parses_ok("FIND SEGMENT OFFSET 50"),                  "OFFSET sem LIMIT — rejeitado");

    // SEARCH KMER aceita LIMIT
    auto* sq = parse_search(R"(SEARCH KMER "ACGT" LIMIT 5)");
    check(sq && sq->limit && *sq->limit == 5,                    "SEARCH KMER LIMIT 5");

    // B-11: SEARCH KMER agora aceita OFFSET (simétrico ao FIND)
    auto* sq_off = parse_search(R"(SEARCH KMER "ACGT" LIMIT 5 OFFSET 2)");
    check(sq_off && sq_off->limit && *sq_off->limit == 5 &&
          sq_off->offset && *sq_off->offset == 2,
          "SEARCH KMER LIMIT 5 OFFSET 2 (B-11)");

    // SEARCH KMER OFFSET sem LIMIT também rejeita (mesmo invariante do FIND)
    check(!parses_ok(R"(SEARCH KMER "ACGT" OFFSET 2)"),
          "SEARCH KMER OFFSET sem LIMIT — rejeitado");
}

// ─── H. Erros sintáticos ──────────────────────────────────────────────────────

static void test_h_syntax_errors() {
    std::printf("\n── H. Erros sintáticos ──\n");

    check(!parses_ok("LIST SEGMENT"),                            "keyword inválida (LIST)");
    check(!parses_ok("FIND FOOBAR"),                             "entity inválida (FOOBAR)");
    check(!parses_ok("FIND SEGMENT WHERE length ~= 100"),        "operador inválido (~=)");
    check(!parses_ok("FIND SEGMENT WHERE name = HG002"),         "string sem aspas");
    check(!parses_ok(R"(FIND SEGMENT WHERE name = "unterminated)"),
                                                                 "string sem aspa de fechamento");
    check(!parses_ok("FIND SEGMENT WHERE foobar(HG002)"),        "função desconhecida");
    check(!parses_ok("FIND SEGMENT extra_garbage"),              "lixo após query");
    check(!parses_ok(""),                                        "string vazia");
    check(!parses_ok("   "),                                     "só whitespace");
}

// ─── I. Identificadores reservados ────────────────────────────────────────────

static void test_i_reserved_identifiers() {
    std::printf("\n── I. Identificadores reservados ──\n");

    // Keyword EXATA (uppercase) como field é rejeitada por not_at<keyword_any>
    check(!parses_ok("FIND SEGMENT WHERE FIND > 0"),             "FIND como field — rejeitado");
    check(!parses_ok("FIND SEGMENT WHERE WHERE > 0"),            "WHERE como field — rejeitado");
    check(!parses_ok("FIND SEGMENT RETURN FIND, name"),          "FIND em RETURN — rejeitado");
    check(!parses_ok("FIND SEGMENT RETURN AND"),                 "AND em RETURN — rejeitado");

    // Após B-7 (case-insensitive), 'find' minúsculo TAMBÉM é keyword reservada
    // → rejeitada como field name.
    check(!parses_ok("FIND SEGMENT WHERE find > 0"),
          "'find' (minúsculo) — rejeitado após B-7 (era aceito antes)");
}

// ─── J. Case-insensitive (B-7) ────────────────────────────────────────────────

static void test_j_case_insensitive() {
    std::printf("\n── J. Case-insensitive (B-7) ──\n");

    // Após B-7: keywords usam TAO_PEGTL_ISTRING — qualquer grafia funciona.
    check( parses_ok("find segment"),                            "'find segment' minúsculo — aceito");
    check( parses_ok("FIND segment"),                            "FIND + entity minúscula — aceito");
    check( parses_ok("Find Segment"),                            "TitleCase — aceito");
    check( parses_ok("FIND SEGMENT"),                            "UPPERCASE canônico — aceito");
    check( parses_ok("fInD sEgMeNt"),                            "MiXeD CaSe — aceito");

    // AND/OR também case-insensitive
    check( parses_ok("FIND SEGMENT WHERE length > 0 and depth = 1"),
                                                                 "'and' minúsculo — aceito");

    // Entity capturado normaliza corretamente (verifica enum)
    auto* fq = parse_find("find segment where length > 0");
    check(fq && fq->entity == EntityType::SEGMENT,               "enum EntityType correto após case-insensitive parse");
}

// ─── K. Overflow numérico (B-9: erro explícito, não mais saturação) ─────────

static void test_k_overflow() {
    std::printf("\n── K. Overflow numérico (B-9) ──\n");

    // Inteiro > INT64_MAX → ERRO de parse (antes saturava silenciosamente)
    check(!parses_ok("FIND SEGMENT WHERE length > 99999999999999999999"),
          "int overflow → erro de parse (antes saturava)");

    // Limite exato — passa intacto
    auto* fq_max = parse_find("FIND SEGMENT WHERE length > 9223372036854775807");
    if (fq_max && fq_max->where && !fq_max->where->conditions.empty()) {
        auto* cc = std::get_if<CompareCondition>(&fq_max->where->conditions[0]);
        const auto* iv = cc ? std::get_if<int64_t>(&cc->value) : nullptr;
        check(iv && *iv == 9223372036854775807LL,
              "INT64_MAX exato — preservado");
    } else {
        check(false, "INT64_MAX exato — preservado");
    }

    // INT64_MAX + 1 também explode
    check(!parses_ok("FIND SEGMENT WHERE length = 9223372036854775808"),
          "INT64_MAX + 1 → erro");

    // LIMIT > UINT32_MAX → erro
    check(!parses_ok("FIND SEGMENT LIMIT 99999999999"),
          "LIMIT overflow → erro");

    // OFFSET > UINT32_MAX → erro
    check(!parses_ok("FIND SEGMENT LIMIT 1 OFFSET 99999999999"),
          "OFFSET overflow → erro");
}

// ─── L. Whitespace ────────────────────────────────────────────────────────────

static void test_l_whitespace() {
    std::printf("\n── L. Whitespace ──\n");

    check(parses_ok("FIND   SEGMENT   WHERE   length>100"),     "espaços extras");
    check(parses_ok("FIND SEGMENT\nWHERE length > 100"),         "newlines entre cláusulas");
    check(parses_ok("FIND SEGMENT\tWHERE\tlength\t>\t100"),      "tabs como separador");
    check(parses_ok("  FIND SEGMENT  "),                         "leading/trailing whitespace");
    check(parses_ok("FIND SEGMENT WHERE length>100"),            "sem espaços ao redor de >");
}

// ─── M. Inteiros negativos (B-6) ──────────────────────────────────────────────

static void test_m_negative_numbers() {
    std::printf("\n── M. Inteiros/floats negativos (B-6) ──\n");

    auto* fq_n1 = parse_find("FIND SEGMENT WHERE length > -1");
    if (fq_n1 && fq_n1->where && !fq_n1->where->conditions.empty()) {
        auto* cc = std::get_if<CompareCondition>(&fq_n1->where->conditions[0]);
        const auto* iv = cc ? std::get_if<int64_t>(&cc->value) : nullptr;
        check(iv && *iv == -1, "WHERE x > -1 (negativo)");
    } else {
        check(false, "WHERE x > -1 (negativo)");
    }

    auto* fq_n2 = parse_find("FIND SEGMENT WHERE depth = -3.14");
    if (fq_n2 && fq_n2->where && !fq_n2->where->conditions.empty()) {
        auto* cc = std::get_if<CompareCondition>(&fq_n2->where->conditions[0]);
        const auto* dv = cc ? std::get_if<double>(&cc->value) : nullptr;
        check(dv && *dv == -3.14, "WHERE x = -3.14 (float negativo)");
    } else {
        check(false, "WHERE x = -3.14 (float negativo)");
    }

    // INT64_MIN (-9223372036854775808) — passa
    auto* fq_min = parse_find("FIND SEGMENT WHERE length = -9223372036854775808");
    if (fq_min && fq_min->where && !fq_min->where->conditions.empty()) {
        auto* cc = std::get_if<CompareCondition>(&fq_min->where->conditions[0]);
        const auto* iv = cc ? std::get_if<int64_t>(&cc->value) : nullptr;
        check(iv && *iv == std::numeric_limits<int64_t>::min(),
              "INT64_MIN exato — preservado");
    } else {
        check(false, "INT64_MIN exato — preservado");
    }

    // LIMIT/OFFSET continuam unsigned — negativos rejeitados
    check(!parses_ok("FIND SEGMENT LIMIT -1"), "LIMIT negativo — rejeitado");
}

// ─── N. RETURN * (B-8) ────────────────────────────────────────────────────────

static void test_n_return_star() {
    std::printf("\n── N. RETURN * (B-8) ──\n");

    auto* fq = parse_find("FIND SEGMENT RETURN *");
    check(fq && fq->return_all && fq->return_fields.empty(),
          "RETURN * — return_all=true, return_fields vazio");

    auto* fq_combo = parse_find("FIND SEGMENT WHERE length > 100 RETURN * LIMIT 10");
    check(fq_combo && fq_combo->return_all &&
          fq_combo->limit && *fq_combo->limit == 10,
          "RETURN * combinado com WHERE e LIMIT");

    auto* sq = parse_search(R"(SEARCH KMER "ACGT" RETURN *)");
    check(sq && sq->return_all,
          "SEARCH KMER RETURN *");

    // Sem RETURN — return_all false (default)
    auto* fq_none = parse_find("FIND SEGMENT");
    check(fq_none && !fq_none->return_all && fq_none->return_fields.empty(),
          "Sem RETURN — return_all=false (default)");

    // RETURN id — return_all false, return_fields=[id]
    auto* fq_one = parse_find("FIND SEGMENT RETURN id");
    check(fq_one && !fq_one->return_all && fq_one->return_fields.size() == 1,
          "RETURN id — return_all=false");
}

// ─── O. Comentários em queries (B-10) ────────────────────────────────────────

static void test_o_comments() {
    std::printf("\n── O. Comentários (B-10) ──\n");

    // Line comment SQL-style (--) entre cláusulas
    check(parses_ok("FIND SEGMENT -- comentário\n WHERE length > 0"),
          "comentário -- entre cláusulas");

    // Line comment no fim
    check(parses_ok("FIND SEGMENT -- foo bar"),
          "comentário -- no fim");

    // Block comment /* */
    check(parses_ok("FIND /* tudo bem */ SEGMENT WHERE length > 0"),
          "comentário /* */ inline");

    // Block comment multi-linha
    check(parses_ok("FIND SEGMENT\n/*\nlinha 1\nlinha 2\n*/\nWHERE length > 0"),
          "comentário /* */ multi-linha");

    // Múltiplos comentários
    check(parses_ok("-- header\nFIND /* mid */ SEGMENT -- trailing"),
          "múltiplos comentários");

    // Não confundir `--` em strings com comentário
    check(parses_ok(R"(FIND SEGMENT WHERE name = "a--b")"),
          "'--' dentro de string — não é comentário");

    // Block comment não fechado → erro (por causa do until)
    check(!parses_ok("FIND SEGMENT /* nunca fecha"),
          "/* sem */ — erro");
}

// ─── P. Parens em WHERE (B-5) ────────────────────────────────────────────────

static void test_p_parens() {
    std::printf("\n── P. Parens em WHERE (B-5) ──\n");

    // Parens triviais — single condition entre parens
    auto* fq_t = parse_find("FIND SEGMENT WHERE (length > 100)");
    if (fq_t && fq_t->where && fq_t->where->conditions.size() == 1) {
        auto* g = std::get_if<GroupedExpr>(&fq_t->where->conditions[0]);
        check(g != nullptr && g->body && g->body->conditions.size() == 1,
              "(single) — produz GroupedExpr com 1 cond");
    } else {
        check(false, "(single) — produz GroupedExpr com 1 cond");
    }

    // (a OR b) AND c — clássico de precedência
    auto* fq_p = parse_find(
        R"(FIND SEGMENT WHERE (name = "a" OR name = "b") AND length > 100)");
    if (fq_p && fq_p->where && fq_p->where->conditions.size() == 2) {
        auto* g = std::get_if<GroupedExpr>(&fq_p->where->conditions[0]);
        auto* c = std::get_if<CompareCondition>(&fq_p->where->conditions[1]);
        check(g && c && g->body && g->body->conditions.size() == 2 &&
              g->body->connectors.size() == 1 &&
              g->body->connectors[0] == LogicalOp::OR &&
              fq_p->where->connectors.size() == 1 &&
              fq_p->where->connectors[0] == LogicalOp::AND,
              "(a OR b) AND c — Group(2) + AND + cond");
    } else {
        check(false, "(a OR b) AND c — Group(2) + AND + cond");
    }

    // a AND (b OR c)
    auto* fq_p2 = parse_find(
        R"(FIND SEGMENT WHERE length > 100 AND (name = "x" OR name = "y"))");
    if (fq_p2 && fq_p2->where && fq_p2->where->conditions.size() == 2) {
        auto* g = std::get_if<GroupedExpr>(&fq_p2->where->conditions[1]);
        check(g && g->body && g->body->conditions.size() == 2,
              "a AND (b OR c) — cond + AND + Group(2)");
    } else {
        check(false, "a AND (b OR c) — cond + AND + Group(2)");
    }

    // Parens aninhados
    auto* fq_n = parse_find("FIND SEGMENT WHERE ((length > 100))");
    if (fq_n && fq_n->where && fq_n->where->conditions.size() == 1) {
        auto* g_outer = std::get_if<GroupedExpr>(&fq_n->where->conditions[0]);
        check(g_outer && g_outer->body && g_outer->body->conditions.size() == 1 &&
              std::holds_alternative<GroupedExpr>(g_outer->body->conditions[0]),
              "((cond)) — Group dentro de Group (aninhamento)");
    } else {
        check(false, "((cond)) — Group dentro de Group");
    }

    // (a AND b) OR (c AND d) — dois grupos no top-level
    auto* fq_2g = parse_find(
        "FIND SEGMENT WHERE (length > 0 AND length < 10) OR (length > 100 AND length < 200)");
    if (fq_2g && fq_2g->where && fq_2g->where->conditions.size() == 2) {
        auto* g1 = std::get_if<GroupedExpr>(&fq_2g->where->conditions[0]);
        auto* g2 = std::get_if<GroupedExpr>(&fq_2g->where->conditions[1]);
        check(g1 && g2 &&
              fq_2g->where->connectors.size() == 1 &&
              fq_2g->where->connectors[0] == LogicalOp::OR,
              "(a AND b) OR (c AND d) — 2 grupos + OR");
    } else {
        check(false, "(a AND b) OR (c AND d) — 2 grupos + OR");
    }

    // Parens com fn call
    auto* fq_fn = parse_find(
        R"(FIND PATH WHERE (crosses("HG002") OR crosses("HG003")) AND length > 0)");
    check(fq_fn && fq_fn->where && fq_fn->where->conditions.size() == 2,
          "parens com fn calls (crosses)");

    // Erros: paren não fechado / não aberto
    check(!parses_ok("FIND SEGMENT WHERE (length > 100"),
          "( sem ) — erro");
    check(!parses_ok("FIND SEGMENT WHERE length > 100)"),
          ") sem ( — erro");
    check(!parses_ok("FIND SEGMENT WHERE ()"),
          "() vazio — erro (precisa de pelo menos 1 cond)");
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main() {
    test_a_basic_statements();
    test_b_operators();
    test_c_value_types();
    test_d_and_or();
    test_e_function_calls();
    test_f_return();
    test_g_limit_offset();
    test_h_syntax_errors();
    test_i_reserved_identifiers();
    test_j_case_insensitive();
    test_k_overflow();
    test_l_whitespace();
    test_m_negative_numbers();
    test_n_return_star();
    test_o_comments();
    test_p_parens();

    std::printf("\n=== %d pass · %d fail ===\n", n_pass, n_fail);
    return n_fail == 0 ? 0 : 1;
}
