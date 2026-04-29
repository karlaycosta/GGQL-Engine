#pragma once

// grammar.hpp — Gramática PEGTL para GGQL + ações + API pública
//
// Sintaxe suportada:
//   FIND   <entity> [WHERE <cond> [AND|OR <cond> ...]] [RETURN <field>, ...]
//   SEARCH KMER "<seq>"              [RETURN <field>, ...]
//
// <entity>  ::= SEGMENT | EDGE | PATH | BUBBLE | TIP | REPEAT
// <cond>    ::= <field> <op> <value>
//             | crosses(<identifier>)
//             | contains_kmer("<string>")
// <op>      ::= = | != | < | > | <= | >=
// <value>   ::= <integer> | "<string>"

#include <tao/pegtl.hpp>

#include <cassert>
#include <charconv>
#include <expected>
#include <string>

#include "ast.hpp"

namespace ggql::grammar {

namespace peg = tao::pegtl;

// ═══════════════════════════════════════════════════════════════════════════════
// 1. REGRAS PEGTL
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Utilitários ──────────────────────────────────────────────────────────────

/// Espaço em branco opcional (espaços, tabs, newlines)
struct ws : peg::star<peg::space> {};

/// Keyword: casa a string S somente se não for seguida por outro caractere de
/// identificador — evita que "FIND" case como prefixo de "FINDER".
template <typename S> struct kw : peg::seq<S, peg::not_at<peg::identifier_other>> {};

// ─── Palavras-chave ───────────────────────────────────────────────────────────

struct kw_find : kw<TAO_PEGTL_STRING("FIND")> {};
struct kw_search : kw<TAO_PEGTL_STRING("SEARCH")> {};
struct kw_kmer : kw<TAO_PEGTL_STRING("KMER")> {};
struct kw_where : kw<TAO_PEGTL_STRING("WHERE")> {};
struct kw_return : kw<TAO_PEGTL_STRING("RETURN")> {};
struct kw_limit : kw<TAO_PEGTL_STRING("LIMIT")> {};
struct kw_offset : kw<TAO_PEGTL_STRING("OFFSET")> {};
struct kw_and : kw<TAO_PEGTL_STRING("AND")> {};
struct kw_or : kw<TAO_PEGTL_STRING("OR")> {};
struct kw_segment : kw<TAO_PEGTL_STRING("SEGMENT")> {};
struct kw_edge : kw<TAO_PEGTL_STRING("EDGE")> {};
struct kw_path : kw<TAO_PEGTL_STRING("PATH")> {};
struct kw_bubble : kw<TAO_PEGTL_STRING("BUBBLE")> {};
struct kw_tip : kw<TAO_PEGTL_STRING("TIP")> {};
struct kw_repeat : kw<TAO_PEGTL_STRING("REPEAT")> {};

/// Guarda: todas as palavras reservadas reunidas.
/// Usada em not_at<> para impedir que fields/return_fields casem keywords.
struct keyword_any : peg::sor<kw_find, kw_search, kw_kmer, kw_where, kw_return, kw_limit,
                              kw_offset, kw_and, kw_or,
                              kw_segment, kw_edge, kw_path, kw_bubble, kw_tip, kw_repeat> {};

// ─── Tipo de entidade ─────────────────────────────────────────────────────────

struct entity_type : peg::sor<kw_segment, kw_edge, kw_path, kw_bubble, kw_tip, kw_repeat> {};

// ─── Operadores relacionais ───────────────────────────────────────────────────
// A ordem importa no PEG: símbolos mais longos devem vir antes dos mais curtos.

struct op_sym : peg::sor<TAO_PEGTL_STRING("!="), TAO_PEGTL_STRING(">="), TAO_PEGTL_STRING("<="),
                         TAO_PEGTL_STRING("="), TAO_PEGTL_STRING(">"), TAO_PEGTL_STRING("<")> {};

// ─── Fields ───────────────────────────────────────────────────────────────────

/// Identificador que não é palavra reservada — usado em condições WHERE
struct where_field : peg::seq<peg::not_at<keyword_any>, peg::identifier> {};

/// Identificador que não é palavra reservada — usado no RETURN
struct return_field : peg::seq<peg::not_at<keyword_any>, peg::identifier> {};

// ─── Valores ──────────────────────────────────────────────────────────────────

/// Literal float: dígitos '.' dígitos  (ex: 3.5, 12.30)
struct float_value : peg::seq<peg::plus<peg::digit>, peg::one<'.'>, peg::plus<peg::digit>> {};

struct int_value : peg::plus<peg::digit> {};

/// Conteúdo entre aspas duplas (sem as aspas)
struct str_content : peg::star<peg::not_one<'"'>> {};
struct str_value : peg::seq<peg::one<'"'>, str_content, peg::one<'"'>> {};

/// float_value antes de int_value: PEG ordered choice tenta float primeiro,
/// backtracks para int se não houver '.'.
struct value : peg::sor<float_value, int_value, str_value> {};

// ─── Chamadas de função ───────────────────────────────────────────────────────

/// Argumento (path name) de crosses()
struct fn_crosses_arg : peg::identifier {};
struct fn_crosses : peg::seq<TAO_PEGTL_STRING("crosses"), ws, peg::one<'('>, ws, fn_crosses_arg, ws,
                             peg::one<')'>> {};

/// Conteúdo (k-mer) de contains_kmer()
struct fn_contains_arg : peg::star<peg::not_one<'"'>> {};
struct fn_contains : peg::seq<TAO_PEGTL_STRING("contains_kmer"), ws, peg::one<'('>, ws,
                              peg::one<'"'>, fn_contains_arg, peg::one<'"'>, ws, peg::one<')'>> {};

struct function_call : peg::sor<fn_crosses, fn_contains> {};

// ─── Condições ────────────────────────────────────────────────────────────────

/// <field> <op> <value>
struct condition_expr : peg::seq<where_field, ws, op_sym, ws, value> {};

/// function_call | condition_expr
/// function_call é tentada primeiro (ordered choice)
struct condition : peg::sor<function_call, condition_expr> {};

// ─── Operador lógico ──────────────────────────────────────────────────────────

struct logical_op : peg::sor<kw_and, kw_or> {};

// ─── Cláusula WHERE ───────────────────────────────────────────────────────────

struct where_clause
    : peg::seq<kw_where, ws, condition, peg::star<peg::seq<ws, logical_op, ws, condition>>> {};

// ─── Cláusula RETURN ──────────────────────────────────────────────────────────

struct comma_sep : peg::seq<ws, peg::one<','>, ws> {};
struct return_clause : peg::seq<kw_return, ws, peg::list<return_field, comma_sep>> {};

// ─── Cláusulas LIMIT / OFFSET ─────────────────────────────────────────────────

struct limit_val  : peg::plus<peg::digit> {};
struct offset_val : peg::plus<peg::digit> {};
struct limit_clause  : peg::seq<kw_limit,  ws, limit_val>  {};
struct offset_clause : peg::seq<kw_offset, ws, offset_val> {};
/// LIMIT N [OFFSET M] — OFFSET sem LIMIT é rejeitado no parse
struct limit_offset_clause
    : peg::seq<limit_clause, peg::opt<peg::seq<ws, offset_clause>>> {};

// ─── Statements ───────────────────────────────────────────────────────────────

struct find_stmt : peg::seq<kw_find, ws, entity_type,
                            peg::opt<peg::seq<ws, where_clause>>,
                            peg::opt<peg::seq<ws, return_clause>>,
                            peg::opt<peg::seq<ws, limit_offset_clause>>> {};

/// Conteúdo do k-mer em SEARCH KMER "..."
struct kmer_arg : peg::star<peg::not_one<'"'>> {};

struct search_stmt : peg::seq<kw_search, ws, kw_kmer, ws, peg::one<'"'>, kmer_arg, peg::one<'"'>,
                              peg::opt<peg::seq<ws, return_clause>>,
                              peg::opt<peg::seq<ws, limit_clause>>> {};

/// Regra raiz — uma única query, sem conteúdo residual
struct query : peg::seq<ws, peg::sor<find_stmt, search_stmt>, ws, peg::eof> {};

// ═══════════════════════════════════════════════════════════════════════════════
// 2. ESTADO DE PARSE
// ═══════════════════════════════════════════════════════════════════════════════

struct ParseState {
    // ── temporários (válidos apenas durante o parse de uma cláusula) ──────────
    // string_view é seguro: o input permanece válido durante todo o parse.
    std::string_view tmp_entity_sv;
    std::string_view tmp_field;
    std::string_view tmp_op_sv;
    // Valor temporário de uma condição WHERE: inteiro, double ou string (zero-copy).
    std::variant<int64_t, double, std::string_view> tmp_value{int64_t{0}};
    std::string_view tmp_fn_arg;
    std::optional<uint32_t> tmp_limit;
    std::optional<uint32_t> tmp_offset;

    // ── acumuladores ─────────────────────────────────────────────────────────
    std::vector<ConditionExpr> where_conditions;
    std::vector<LogicalOp> where_connectors;
    std::vector<std::string> return_fields;
    std::string kmer_str;

    // ── resultado final ───────────────────────────────────────────────────────
    GgqlQuery result;
};

// ═══════════════════════════════════════════════════════════════════════════════
// 3. AÇÕES
// ═══════════════════════════════════════════════════════════════════════════════

namespace detail {

[[nodiscard]] constexpr EntityType parse_entity(std::string_view s) noexcept {
    using enum EntityType;
    if (s == "SEGMENT") { return SEGMENT;
}
    if (s == "EDGE") {    return EDGE;
}
    if (s == "PATH") {    return PATH;
}
    if (s == "BUBBLE") {  return BUBBLE;
}
    if (s == "TIP") {     return TIP;
}
    /* REPEAT */        return REPEAT;
}

[[nodiscard]] constexpr Operator parse_op(std::string_view s) noexcept {
    using enum Operator;
    if (s == "!=") { return NEQ;
}
    if (s == ">=") { return GEQ;
}
    if (s == "<=") { return LEQ;
}
    if (s == ">") {  return GT;
}
    if (s == "<") {  return LT;
}
    /* = */        return EQ;
}

} // namespace detail

// Template base — sem ação por padrão
template <typename Rule> struct action : peg::nothing<Rule> {};

// ── entity_type ───────────────────────────────────────────────────────────────
template <> struct action<entity_type> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_entity_sv = in.string_view();
    }
};

// ── where_field ───────────────────────────────────────────────────────────────
template <> struct action<where_field> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_field = in.string_view();
    }
};

// ── op_sym ────────────────────────────────────────────────────────────────────
template <> struct action<op_sym> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_op_sv = in.string_view();
    }
};

// ── float_value ───────────────────────────────────────────────────────────────
template <> struct action<float_value> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        double v = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        s.tmp_value = v;
    }
};

// ── int_value ─────────────────────────────────────────────────────────────────
template <> struct action<int_value> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        int64_t v = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        s.tmp_value = v;
    }
};

// ── str_content (conteúdo de valor string em condições WHERE) ─────────────────
template <> struct action<str_content> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_value = in.string_view();
    }
};

// ── condition_expr: <field> <op> <value> ─────────────────────────────────────
template <> struct action<condition_expr> {
    template <typename In> static void apply(const In&, ParseState& s) {
        // Converte o variant interno (string_view) para o Value do AST (string).
        Value ast_value = std::visit(
            []<typename T>(const T& v) -> Value {
                if constexpr (std::is_same_v<T, std::string_view>) {
                    return Value{std::string{v}};
                } else {
                    return Value{v};
}
            },
            s.tmp_value);
        s.where_conditions.push_back(
            CompareCondition{std::string{s.tmp_field}, detail::parse_op(s.tmp_op_sv),
                             std::move(ast_value)});
    }
};

// ── fn_crosses_arg ────────────────────────────────────────────────────────────
template <> struct action<fn_crosses_arg> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_fn_arg = in.string_view();
    }
};

// ── fn_crosses: empurra CrossesCondition ao completar ─────────────────────────
template <> struct action<fn_crosses> {
    template <typename In> static void apply(const In&, ParseState& s) {
        s.where_conditions.push_back(CrossesCondition{std::string{s.tmp_fn_arg}});
        s.tmp_fn_arg = {};
    }
};

// ── fn_contains_arg ───────────────────────────────────────────────────────────
template <> struct action<fn_contains_arg> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_fn_arg = in.string_view();
    }
};

// ── fn_contains: empurra ContainsKmerCondition ao completar ───────────────────
template <> struct action<fn_contains> {
    template <typename In> static void apply(const In&, ParseState& s) {
        s.where_conditions.push_back(ContainsKmerCondition{std::string{s.tmp_fn_arg}});
        s.tmp_fn_arg = {};
    }
};

// ── logical_op ────────────────────────────────────────────────────────────────
template <> struct action<logical_op> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        if (sv == "AND") {      s.where_connectors.push_back(LogicalOp::AND);
        } else if (sv == "OR") {  s.where_connectors.push_back(LogicalOp::OR);
        } else {                  std::unreachable();
}
    }
};

// ── return_field ──────────────────────────────────────────────────────────────
template <> struct action<return_field> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.return_fields.emplace_back(in.string_view());
    }
};

// ── kmer_arg (SEARCH KMER "...") ──────────────────────────────────────────────
template <> struct action<kmer_arg> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.kmer_str = in.string_view();
    }
};

// ── limit_val ─────────────────────────────────────────────────────────────────
template <> struct action<limit_val> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        uint32_t v = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        s.tmp_limit = v;
    }
};

// ── offset_val ────────────────────────────────────────────────────────────────
template <> struct action<offset_val> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        uint32_t v = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        s.tmp_offset = v;
    }
};

// ── find_stmt: monta o FindQuery final ────────────────────────────────────────
template <> struct action<find_stmt> {
    template <typename In> static void apply(const In&, ParseState& s) {
        FindQuery fq;
        fq.entity = detail::parse_entity(s.tmp_entity_sv);

        if (!s.where_conditions.empty()) {
            WhereClause wc;
            wc.conditions = std::move(s.where_conditions);
            wc.connectors = std::move(s.where_connectors);
            assert(wc.is_valid() && "WHERE: connectors.size() deve ser conditions.size() - 1");
            fq.where = std::move(wc);
        }

        fq.return_fields = std::move(s.return_fields);
        fq.limit  = s.tmp_limit;
        fq.offset = s.tmp_offset;
        s.tmp_limit  = std::nullopt;
        s.tmp_offset = std::nullopt;
        s.result = std::move(fq);
    }
};

// ── search_stmt: monta o SearchKmerQuery final ────────────────────────────────
template <> struct action<search_stmt> {
    template <typename In> static void apply(const In&, ParseState& s) {
        SearchKmerQuery sq;
        sq.kmer = std::move(s.kmer_str);
        sq.return_fields = std::move(s.return_fields);
        sq.limit = s.tmp_limit;
        s.tmp_limit = std::nullopt;
        s.result = std::move(sq);
    }
};

// ═══════════════════════════════════════════════════════════════════════════════
// 4. API PÚBLICA
// ═══════════════════════════════════════════════════════════════════════════════

/// Analisa uma query GGQL.
/// Retorna o AST tipado em caso de sucesso ou uma mensagem de erro.
[[nodiscard]] inline std::expected<GgqlQuery, std::string> parse_ggql(std::string_view input) {
    peg::memory_input in(input.data(), input.size(), "ggql_input");
    ParseState state;

    try {
        if (!peg::parse<query, action>(in, state)) {
            return std::unexpected{"Erro de sintaxe na query GGQL"};
}
    } catch (const peg::parse_error& e) {
        return std::unexpected{std::string{e.what()}};
    }

    return state.result;
}

} // namespace ggql::grammar
