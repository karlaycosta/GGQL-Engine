#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <tao/pegtl.hpp>

namespace peg = tao::pegtl;

// Helper: casa a string S somente se não for seguida por um caractere de identificador
template <typename S>
struct kw : peg::seq<S, peg::not_at<peg::identifier_other>> {};

// =========================================================
// 1. O ESTADO (Onde a AST será montada)
// =========================================================
struct Condition {
  std::string field;
  std::string op;
  std::string value;
};

struct GGQLState {
  std::string current_entity;
  std::vector<Condition> conditions;
  std::vector<std::string> return_fields;

  // Temporários durante o parse de uma condição
  std::string current_field;
  std::string current_op;
  std::string current_value;

  void flush_condition() {
    conditions.push_back({current_field, current_op, current_value});
    std::cout << "Condição extraída: " << current_field << " " << current_op
              << " " << current_value << "\n";
    current_field.clear();
    current_op.clear();
    current_value.clear();
  }
};

// =========================================================
// 2. A GRAMÁTICA (Tradução EBNF -> Templates PEGTL)
// =========================================================

// --- Utilitários Básicos ---
struct ws : peg::star<peg::space> {};     // Espaços em branco opcionais
struct identifier : peg::identifier {};   // [a-zA-Z_][a-zA-Z0-9_]*
struct number : peg::plus<peg::digit> {}; // Valores numéricos

// String entre aspas (ex: "ACG")
struct quoted_string
    : peg::seq<peg::one<'"'>, peg::star<peg::not_one<'"'>>, peg::one<'"'>> {};

// --- Palavras-chave (com verificação de borda de palavra) ---
struct kw_find : kw<TAO_PEGTL_STRING("FIND")> {};
struct kw_search : kw<TAO_PEGTL_STRING("SEARCH")> {};
struct kw_kmer : kw<TAO_PEGTL_STRING("KMER")> {};
struct kw_where : kw<TAO_PEGTL_STRING("WHERE")> {};
struct kw_return : kw<TAO_PEGTL_STRING("RETURN")> {};
struct kw_and : kw<TAO_PEGTL_STRING("AND")> {};
struct kw_or : kw<TAO_PEGTL_STRING("OR")> {};

// --- Entidades e Campos ---
// <entity> ::= "SEGMENT" | "EDGE" | "PATH" | "BUBBLE" | "TIP" | "REPEAT"
struct entity_type
    : peg::sor<kw<TAO_PEGTL_STRING("SEGMENT")>, kw<TAO_PEGTL_STRING("EDGE")>,
               kw<TAO_PEGTL_STRING("PATH")>, kw<TAO_PEGTL_STRING("BUBBLE")>,
               kw<TAO_PEGTL_STRING("TIP")>, kw<TAO_PEGTL_STRING("REPEAT")>> {};

// Guarda: impede que 'field' case com palavras reservadas
struct keyword_any
    : peg::sor<kw_find, kw_search, kw_kmer, kw_where, kw_return, kw_and, kw_or,
               kw<TAO_PEGTL_STRING("SEGMENT")>, kw<TAO_PEGTL_STRING("EDGE")>,
               kw<TAO_PEGTL_STRING("PATH")>, kw<TAO_PEGTL_STRING("BUBBLE")>,
               kw<TAO_PEGTL_STRING("TIP")>, kw<TAO_PEGTL_STRING("REPEAT")>> {};

// 'field' usado em condições WHERE; 'return_field' usado no RETURN (ações distintas)
struct field : peg::seq<peg::not_at<keyword_any>, identifier> {};
struct return_field : peg::seq<peg::not_at<keyword_any>, identifier> {};
struct value : peg::sor<number, quoted_string> {};

// --- Operadores ---
// ATENÇÃO: A ordem importa no PEG! Símbolos maiores ('!=', '>=') vêm antes dos
// menores.
struct op : peg::sor<TAO_PEGTL_STRING("!="), TAO_PEGTL_STRING(">="),
                     TAO_PEGTL_STRING("<="), TAO_PEGTL_STRING("="),
                     TAO_PEGTL_STRING(">"), TAO_PEGTL_STRING("<")> {};

// --- Cláusulas de Função e Condição ---
// <function_call> ::= "crosses(" <identifier> ")" | "contains_kmer(" <string>
// ")"
struct fn_crosses : peg::seq<TAO_PEGTL_STRING("crosses"), ws, peg::one<'('>, ws,
                             identifier, ws, peg::one<')'>> {};

struct fn_contains
    : peg::seq<TAO_PEGTL_STRING("contains_kmer"), ws, peg::one<'('>, ws,
               quoted_string, ws, peg::one<')'>> {};

struct function_call : peg::sor<fn_crosses, fn_contains> {};

// <condition> ::= <field> <operator> <value> | <function_call>
struct condition_expr : peg::seq<field, ws, op, ws, value> {};
struct condition : peg::sor<function_call, condition_expr> {};

// --- Cláusulas Principais ---
// <where_clause> ::= "WHERE" <condition> { ( "AND" | "OR" ) <condition> }
struct logical_op : peg::sor<kw_and, kw_or> {};
struct where_clause : peg::seq<kw_where, ws, condition,
                               peg::star<ws, logical_op, ws, condition>> {};

// <return_clause> ::= "RETURN" <field> { "," <field> }
// O PEGTL possui peg::list que simplifica listas separadas por vírgula!
struct comma_sep : peg::seq<ws, peg::one<','>, ws> {};
struct return_clause : peg::seq<kw_return, ws, peg::list<return_field, comma_sep>> {};

// --- Declarações de Topo (Statements) ---
// <find_stmt> ::= "FIND" <entity> [ <where_clause> ] [ <return_clause> ]
struct find_stmt
    : peg::seq<kw_find, ws, entity_type, ws, peg::opt<where_clause>, ws,
               peg::opt<return_clause>> {};

// <search_stmt> ::= "SEARCH KMER" <string> [ <return_clause> ]
struct search_stmt : peg::seq<kw_search, ws, kw_kmer, ws, quoted_string, ws,
                              peg::opt<return_clause>> {};

// A Regra Raiz: <query> ::= <find_stmt> | <search_stmt>
struct query : peg::seq<ws, peg::sor<find_stmt, search_stmt>, ws, peg::eof> {};

// =========================================================
// 3. AS AÇÕES (Conectando a gramática com a memória)
// =========================================================

template <typename Rule> struct action : peg::nothing<Rule> {};

// Captura a entidade (ex: TIP, BUBBLE)
template <> struct action<entity_type> {
  template <typename ActionInput>
  static void apply(const ActionInput &in, GGQLState &state) {
    state.current_entity = in.string();
    std::cout << "Entidade detectada: " << state.current_entity << "\n";
  }
};

// Campo de uma condição WHERE
template <> struct action<field> {
  template <typename ActionInput>
  static void apply(const ActionInput &in, GGQLState &state) {
    state.current_field = in.string();
  }
};

template <> struct action<op> {
  template <typename ActionInput>
  static void apply(const ActionInput &in, GGQLState &state) {
    state.current_op = in.string();
  }
};

template <> struct action<value> {
  template <typename ActionInput>
  static void apply(const ActionInput &in, GGQLState &state) {
    state.current_value = in.string();
  }
};

// Quando uma condição inteira termina, é acumulada no estado
template <> struct action<condition_expr> {
  template <typename ActionInput>
  static void apply(const ActionInput & /*in*/, GGQLState &state) {
    state.flush_condition();
  }
};

// Campos do RETURN acumulados separadamente (não sobrescrevem current_field)
template <> struct action<return_field> {
  template <typename ActionInput>
  static void apply(const ActionInput &in, GGQLState &state) {
    state.return_fields.push_back(in.string());
  }
};

// =========================================================
// 4. TESTANDO O PARSER
// =========================================================

int main() {
  // A string exata do seu exemplo EBNF
  std::string query_str = "FIND TIP \n WHERE length < 200 \n RETURN id, length";

  std::cout << "Analisando: " << query_str << "\n\n";

  peg::memory_input in(query_str, "entrada_usuario");
  GGQLState state;

  try {
    bool sucesso = peg::parse<query, action>(in, state);
    if (sucesso) {
      std::cout << "\n[OK] Parse concluído com sucesso!\n";
      std::cout << "  Entidade : " << state.current_entity << "\n";
      std::cout << "  Condições (" << state.conditions.size() << "):\n";
      for (const auto &c : state.conditions)
        std::cout << "    " << c.field << " " << c.op << " " << c.value << "\n";
      std::cout << "  RETURN   (" << state.return_fields.size() << "): ";
      for (const auto &f : state.return_fields)
        std::cout << f << " ";
      std::cout << "\n";
    } else {
      std::cerr << "\n[ERRO] Falha de sintaxe na query.\n";
    }
  } catch (const peg::parse_error &e) {
    std::cerr << "Erro fatal: " << e.what() << '\n';
  }

  return 0;
}