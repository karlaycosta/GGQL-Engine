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
//             | crosses("<string>")          // p/ paths HPRC com '#', '/' etc.
//             | contains_kmer("<string>")
// <op>      ::= = | != | < | > | <= | >=
// <value>   ::= <integer> | "<string>"

#include <tao/pegtl.hpp>

#include <cassert>
#include <charconv>
#include <expected>
#include <string>
#include <utility>

#include "ast.hpp"

namespace ggql::grammar {

namespace peg = tao::pegtl;

// ═══════════════════════════════════════════════════════════════════════════════
// 1. REGRAS PEGTL
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Utilitários ──────────────────────────────────────────────────────────────

// B-10: comentários consumidos como whitespace.
//   line_comment: -- ... \n   (estilo SQL)
//   block_comment: /* ... */  (estilo C, sem aninhamento)
struct line_comment_body : peg::until<peg::sor<peg::eol, peg::eof>> {};
struct line_comment : peg::seq<TAO_PEGTL_STRING("--"), line_comment_body> {};

struct block_comment_body : peg::until<TAO_PEGTL_STRING("*/")> {};
struct block_comment : peg::seq<TAO_PEGTL_STRING("/*"), block_comment_body> {};

/// Espaço em branco opcional (espaços, tabs, newlines, comentários).
struct ws : peg::star<peg::sor<peg::space, line_comment, block_comment>> {};

/// Keyword: casa a string S somente se não for seguida por outro caractere de
/// identificador — evita que "FIND" case como prefixo de "FINDER".
template <typename S> struct kw : peg::seq<S, peg::not_at<peg::identifier_other>> {};

// ─── Palavras-chave ───────────────────────────────────────────────────────────

// Keywords case-insensitive (TAO_PEGTL_ISTRING vs STRING). Aceita FIND, find,
// Find, FiNd indistintamente. Decisão de design: queries são frequentemente
// escritas à mão por humanos; alinhar com SQL/Cypher. As normalizações em
// parse_entity e logical_op (abaixo) cuidam da comparação semantica.
struct kw_find : kw<TAO_PEGTL_ISTRING("FIND")> {};
struct kw_search : kw<TAO_PEGTL_ISTRING("SEARCH")> {};
struct kw_kmer : kw<TAO_PEGTL_ISTRING("KMER")> {};
struct kw_where : kw<TAO_PEGTL_ISTRING("WHERE")> {};
struct kw_return : kw<TAO_PEGTL_ISTRING("RETURN")> {};
struct kw_limit : kw<TAO_PEGTL_ISTRING("LIMIT")> {};
struct kw_offset : kw<TAO_PEGTL_ISTRING("OFFSET")> {};
struct kw_and : kw<TAO_PEGTL_ISTRING("AND")> {};
struct kw_or : kw<TAO_PEGTL_ISTRING("OR")> {};
struct kw_segment : kw<TAO_PEGTL_ISTRING("SEGMENT")> {};
struct kw_edge : kw<TAO_PEGTL_ISTRING("EDGE")> {};
struct kw_path : kw<TAO_PEGTL_ISTRING("PATH")> {};
struct kw_bubble : kw<TAO_PEGTL_ISTRING("BUBBLE")> {};
struct kw_tip : kw<TAO_PEGTL_ISTRING("TIP")> {};
struct kw_repeat : kw<TAO_PEGTL_ISTRING("REPEAT")> {};

/// Guarda: todas as palavras reservadas reunidas.
/// Usada em not_at<> para impedir que fields/return_fields casem keywords.
struct keyword_any : peg::sor<kw_find, kw_search, kw_kmer, kw_where, kw_return, kw_limit,
                              kw_offset, kw_and, kw_or,
                              kw_segment, kw_edge, kw_path, kw_bubble, kw_tip, kw_repeat> {};

// ─── Tipo de entidade ─────────────────────────────────────────────────────────

struct entity_type : peg::sor<kw_segment, kw_edge, kw_path, kw_bubble, kw_tip, kw_repeat> {};

// ─── Operadores relacionais ───────────────────────────────────────────────────
// A ordem importa no PEG: símbolos mais longos devem vir antes dos mais curtos.
// `<>` é sintaxe SQL-padrão para `!=` (suportado em SQL-92, Oracle, PostgreSQL).
// Sem o alternativo explícito, o PEG consumia apenas o `<` em `length <> N` e
// silenciosamente falhava o parse (run_count → 0 sem warning) — bug
// descoberto em Fase 1 do plano de comparação completa (2026-05-13).

struct op_sym : peg::sor<TAO_PEGTL_STRING("!="), TAO_PEGTL_STRING("<>"),
                         TAO_PEGTL_STRING(">="), TAO_PEGTL_STRING("<="),
                         TAO_PEGTL_STRING("="), TAO_PEGTL_STRING(">"), TAO_PEGTL_STRING("<")> {};

// ─── Fields ───────────────────────────────────────────────────────────────────

/// Identificador que não é palavra reservada — usado em condições WHERE
struct where_field : peg::seq<peg::not_at<keyword_any>, peg::identifier> {};

/// Identificador que não é palavra reservada — usado no RETURN
struct return_field : peg::seq<peg::not_at<keyword_any>, peg::identifier> {};

// ─── Valores ──────────────────────────────────────────────────────────────────

// B-6: inteiros e floats aceitam sinal negativo opcional.
struct opt_minus : peg::opt<peg::one<'-'>> {};

/// Literal float: [-]dígitos '.' dígitos  (ex: 3.5, -12.30)
struct float_value
    : peg::seq<opt_minus, peg::plus<peg::digit>, peg::one<'.'>, peg::plus<peg::digit>> {};

/// Literal int: [-]dígitos  (ex: 100, -1)
struct int_value : peg::seq<opt_minus, peg::plus<peg::digit>> {};

/// Conteúdo entre aspas duplas (sem as aspas)
struct str_content : peg::star<peg::not_one<'"'>> {};
struct str_value : peg::seq<peg::one<'"'>, str_content, peg::one<'"'>> {};

/// float_value antes de int_value: PEG ordered choice tenta float primeiro,
/// backtracks para int se não houver '.'.
struct value : peg::sor<float_value, int_value, str_value> {};

// ─── Chamadas de função ───────────────────────────────────────────────────────

/// Argumento (path name) de crosses() — duas formas:
///   1. identifier — crosses(HG002)             (legacy, só [A-Za-z_][A-Za-z0-9_]*)
///   2. string     — crosses("HG002#1#chr1")    (necessário para paths HPRC,
///                                               PanSN-spec sample#hap#chr)
struct fn_crosses_id_arg  : peg::identifier {};
struct fn_crosses_str_arg : peg::star<peg::not_one<'"'>> {};
struct fn_crosses
    : peg::seq<TAO_PEGTL_STRING("crosses"), ws, peg::one<'('>, ws,
               peg::sor<peg::seq<peg::one<'"'>, fn_crosses_str_arg, peg::one<'"'>>,
                        fn_crosses_id_arg>,
               ws, peg::one<')'>> {};

/// Conteúdo (k-mer) de contains_kmer()
struct fn_contains_arg : peg::star<peg::not_one<'"'>> {};
struct fn_contains : peg::seq<TAO_PEGTL_STRING("contains_kmer"), ws, peg::one<'('>, ws,
                              peg::one<'"'>, fn_contains_arg, peg::one<'"'>, ws, peg::one<')'>> {};

struct function_call : peg::sor<fn_crosses, fn_contains> {};

// ─── Condições ────────────────────────────────────────────────────────────────

/// <field> <op> <value>
struct condition_expr : peg::seq<where_field, ws, op_sym, ws, value> {};

// ─── Operador lógico ──────────────────────────────────────────────────────────

struct logical_op : peg::sor<kw_and, kw_or> {};

// B-5: parênteses em WHERE — `(<sub-where>)` permite precedência explícita.
//      Exemplo:   FIND PATH WHERE (a OR b) AND c
//      Resolução: sem parens, eval é left-to-right (a OR b) AND c == ok aqui;
//                 com parens, o evaluator agrupa explicitamente.
//      AST: GroupedExpr { unique_ptr<WhereClause> body }.
//
// Forward declaration: condition usa paren_expr e paren_expr (via paren_body)
// usa condition recursivamente. PEGTL aceita tipos incompletos enquanto não
// houver instanciação concreta (templates herdando de struct base).
struct condition;
struct paren_open  : peg::one<'('> {};
struct paren_close : peg::one<')'> {};
struct paren_body
    : peg::seq<condition, peg::star<peg::seq<ws, logical_op, ws, condition>>> {};
struct paren_expr  : peg::seq<paren_open, ws, paren_body, ws, paren_close> {};

/// paren_expr | function_call | condition_expr  — ordered choice (PEG)
struct condition : peg::sor<paren_expr, function_call, condition_expr> {};

// ─── Cláusula WHERE ───────────────────────────────────────────────────────────

struct where_clause
    : peg::seq<kw_where, ws, condition, peg::star<peg::seq<ws, logical_op, ws, condition>>> {};

// ─── Cláusula RETURN ──────────────────────────────────────────────────────────

struct comma_sep : peg::seq<ws, peg::one<','>, ws> {};

// B-8: RETURN * (todas as colunas) OU lista explícita de fields.
struct return_star : peg::one<'*'> {};
struct return_clause
    : peg::seq<kw_return, ws,
               peg::sor<return_star, peg::list<return_field, comma_sep>>> {};

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

// B-11: SEARCH KMER agora aceita LIMIT N [OFFSET M] simétrico a FIND.
struct search_stmt : peg::seq<kw_search, ws, kw_kmer, ws, peg::one<'"'>, kmer_arg, peg::one<'"'>,
                              peg::opt<peg::seq<ws, return_clause>>,
                              peg::opt<peg::seq<ws, limit_offset_clause>>> {};

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
    bool tmp_return_all = false;       // B-8
    std::string kmer_str;

    // B-5: stack de acumuladores salvos durante parsing de `(<sub-where>)`.
    // Cada paren_open empilha o estado externo; paren_close pop e wraps o
    // nível atual em um GroupedExpr no nível externo.
    struct ParenFrame {
        std::vector<ConditionExpr> conditions;
        std::vector<LogicalOp>     connectors;
    };
    std::vector<ParenFrame> paren_stack;

    // ── resultado final ───────────────────────────────────────────────────────
    GgqlQuery result;
};

// ═══════════════════════════════════════════════════════════════════════════════
// 3. AÇÕES
// ═══════════════════════════════════════════════════════════════════════════════

namespace detail {

// Comparação ASCII case-insensitive — necessária após B-7 (kw_*  passou a usar
// TAO_PEGTL_ISTRING). entity_type captura o texto literal do input (ex.
// "Segment" ou "segment"), que precisa virar enum sem depender da grafia.
[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') { ca = static_cast<char>(ca - 32); }
        if (cb >= 'a' && cb <= 'z') { cb = static_cast<char>(cb - 32); }
        if (ca != cb) { return false; }
    }
    return true;
}

[[nodiscard]] constexpr EntityType parse_entity(std::string_view s) noexcept {
    using enum EntityType;
    if (iequals(s, "SEGMENT")) { return SEGMENT; }
    if (iequals(s, "EDGE"))    { return EDGE; }
    if (iequals(s, "PATH"))    { return PATH; }
    if (iequals(s, "BUBBLE"))  { return BUBBLE; }
    if (iequals(s, "TIP"))     { return TIP; }
    if (iequals(s, "REPEAT"))  { return REPEAT; }
    std::unreachable(); // PEGTL `entity_type` rule garante input válido
}

[[nodiscard]] constexpr Operator parse_op(std::string_view s) noexcept {
    using enum Operator;
    if (s == "!=") { return NEQ; }
    if (s == "<>") { return NEQ; }   // SQL-92 alternativo para !=
    if (s == ">=") { return GEQ; }
    if (s == "<=") { return LEQ; }
    if (s == ">")  { return GT; }
    if (s == "<")  { return LT; }
    if (s == "=")  { return EQ; }
    std::unreachable(); // PEGTL `op_sym` rule garante input válido
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
// B-9: overflow lança parse_error em vez de saturar — caller de parse_ggql
// recebe a mensagem via std::expected (catch em parse_ggql converte exception
// → std::unexpected). Saturação silenciosa antes mascarava queries inválidas.
template <> struct action<float_value> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        double v = 0;
        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        if (ec == std::errc::result_out_of_range) {
            throw peg::parse_error("literal float fora do alcance representável", in);
        }
        s.tmp_value = v;
    }
};

// ── int_value ─────────────────────────────────────────────────────────────────
template <> struct action<int_value> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        int64_t v = 0;
        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        if (ec == std::errc::result_out_of_range) {
            throw peg::parse_error("literal inteiro fora do alcance int64_t", in);
        }
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

// ── fn_crosses_id_arg ─────────────────────────────────────────────────────────
template <> struct action<fn_crosses_id_arg> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.tmp_fn_arg = in.string_view();
    }
};

// ── fn_crosses_str_arg ────────────────────────────────────────────────────────
// in.string_view() já contém apenas o conteúdo (sem as aspas delimitadoras),
// pois a regra fn_crosses_str_arg é `star<not_one<'"'>>` consumida entre
// `one<'"'>` ... `one<'"'>`.
template <> struct action<fn_crosses_str_arg> {
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
        // Após B-7 (case-insensitive keywords) sv pode ser "and"/"And"/"AND".
        const auto sv = in.string_view();
        if (detail::iequals(sv, "AND")) {
            s.where_connectors.push_back(LogicalOp::AND);
        } else if (detail::iequals(sv, "OR")) {
            s.where_connectors.push_back(LogicalOp::OR);
        } else {
            std::unreachable(); // PEGTL `logical_op` rule garante AND/OR
        }
    }
};

// ── paren_open / paren_close (B-5) ────────────────────────────────────────────
//
// paren_open: empilha (where_conditions, where_connectors) e reseta os
// acumuladores. O conteúdo do paren popula novos acumuladores "do zero".
// paren_close: constrói WhereClause com os acumuladores atuais, restaura o
// frame anterior e empurra GroupedExpr no nível externo.
//
// Suporta aninhamento arbitrário via stack.
template <> struct action<paren_open> {
    template <typename In> static void apply(const In&, ParseState& s) {
        s.paren_stack.push_back({std::move(s.where_conditions),
                                 std::move(s.where_connectors)});
        s.where_conditions.clear();
        s.where_connectors.clear();
    }
};

template <> struct action<paren_close> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        if (s.paren_stack.empty()) [[unlikely]] {
            // Não deveria ocorrer: paren_expr garante open antes de close.
            throw peg::parse_error("paren_close sem paren_open correspondente", in);
        }
        auto inner = std::make_shared<WhereClause>();
        inner->conditions = std::move(s.where_conditions);
        inner->connectors = std::move(s.where_connectors);

        auto frame = std::move(s.paren_stack.back());
        s.paren_stack.pop_back();
        s.where_conditions = std::move(frame.conditions);
        s.where_connectors = std::move(frame.connectors);

        s.where_conditions.emplace_back(GroupedExpr{std::move(inner)});
    }
};

// ── return_field ──────────────────────────────────────────────────────────────
template <> struct action<return_field> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.return_fields.emplace_back(in.string_view());
    }
};

// ── return_star (B-8) ─────────────────────────────────────────────────────────
template <> struct action<return_star> {
    template <typename In> static void apply(const In&, ParseState& s) {
        s.tmp_return_all = true;
    }
};

// ── kmer_arg (SEARCH KMER "...") ──────────────────────────────────────────────
template <> struct action<kmer_arg> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        s.kmer_str = in.string_view();
    }
};

// ── limit_val ─────────────────────────────────────────────────────────────────
// B-9: overflow lança parse_error (alinhado com int/float).
template <> struct action<limit_val> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        uint32_t v = 0;
        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        if (ec == std::errc::result_out_of_range) {
            throw peg::parse_error("LIMIT fora do alcance uint32_t (~4.29B)", in);
        }
        s.tmp_limit = v;
    }
};

// ── offset_val ────────────────────────────────────────────────────────────────
template <> struct action<offset_val> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        uint32_t v = 0;
        const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
        if (ec == std::errc::result_out_of_range) {
            throw peg::parse_error("OFFSET fora do alcance uint32_t (~4.29B)", in);
        }
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
        fq.return_all = s.tmp_return_all;  // B-8
        fq.limit  = s.tmp_limit;
        fq.offset = s.tmp_offset;
        s.tmp_return_all = false;
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
        sq.return_all = s.tmp_return_all;  // B-8
        sq.limit  = s.tmp_limit;
        sq.offset = s.tmp_offset;  // B-11
        s.tmp_return_all = false;
        s.tmp_limit  = std::nullopt;
        s.tmp_offset = std::nullopt;
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

    // std::move obrigatório após B-5: ConditionExpr contém GroupedExpr com
    // unique_ptr (move-only), tornando GgqlQuery não-copiável.
    return std::move(state.result);
}

} // namespace ggql::grammar
