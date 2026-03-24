// #include <iostream>
// #include <string>
// #include <string_view>
// #include <tao/pegtl.hpp>
// #include <vector>

// namespace peg = tao::pegtl;

// // =========================================================
// // 1. O ESTADO (Onde construímos a AST do GGQL)
// // =========================================================
// struct QueryState {
//   std::string_view current_alias;
//   std::string_view current_label;

//   // Na prática, aqui você usaria a sua QueryArena para instanciar os nós reais
//   void print_parsed_node() const {
//     std::cout << "Nó capturado -> Alias: '" << current_alias << "', Label: '"
//               << current_label << "'\n";
//   }
// };

// // =========================================================
// // 2. A GRAMÁTICA (Definida como Tipos C++ via Templates)
// // =========================================================

// // Ignora espaços em branco arbitrários
// struct ws : peg::star<peg::space> {};

// // Identificadores (ex: 'n', 'Kmer', 'Segment') -> [a-zA-Z_][a-zA-Z0-9_]*
// struct identifier : peg::identifier {};

// // Estrutura do Nó: '(' alias? ':' label ')'
// struct node_alias : identifier {};
// struct node_label : identifier {};

// struct node_pattern
//     : peg::seq<peg::one<'('>, ws, peg::opt<node_alias>, ws, // Alias é opcional
//                peg::one<':'>, ws, node_label,
//                ws, // Label é obrigatória neste exemplo
//                peg::one<')'>> {};

// // Palavra-chave MATCH
// struct match_kw : peg::string<'M', 'A', 'T', 'C', 'H'> {};

// // Cláusula MATCH inteira
// struct match_clause : peg::seq<match_kw, peg::plus<peg::space>, node_pattern> {
// };

// // A Regra Raiz: Espaços opcionais, a cláusula e o fim do arquivo/string
// struct ggql_query : peg::seq<ws, match_clause, ws, peg::eof> {};

// // =========================================================
// // 3. AS AÇÕES (O que fazer quando uma regra bater)
// // =========================================================

// // Template base vazio: por padrão, não fazemos nada
// template <typename Rule> struct action : peg::nothing<Rule> {};

// // Especialização: O que fazer quando o parser encontrar um 'node_alias'
// template <> struct action<node_alias> {
//   template <typename ActionInput>
//   static void apply(const ActionInput &in, QueryState &state) {
//     // in.string_view() retorna exatamenta a fatia da string original
//     // (zero-copy!)
//     state.current_alias = in.string_view();
//   }
// };

// // Especialização: O que fazer quando encontrar um 'node_label'
// template <> struct action<node_label> {
//   template <typename ActionInput>
//   static void apply(const ActionInput &in, QueryState &state) {
//     state.current_label = in.string_view();
//   }
// };

// // Especialização: Quando o padrão inteiro do nó fechar ')'
// template <> struct action<node_pattern> {
//   template <typename ActionInput>
//   static void apply(const ActionInput & /*in*/, QueryState &state) {
//     // Aqui, instanciamos o ASTNode na nossa Arena e adicionamos à árvore
//     state.print_parsed_node();
//   }
// };

// // =========================================================
// // 4. EXECUÇÃO DO MOTOR
// // =========================================================

// int main() {
//   std::string input_query = "MATCH ( n : Kmer )";

//   // Prepara o input de memória (sem copiar a string)
//   peg::memory_input in(input_query, "query_usuario");

//   // Inicia o estado da AST
//   QueryState state;

//   try {
//     // Inicia o parser com a regra raiz 'ggql_query', associando as ações e o
//     // estado
//     bool success = peg::parse<ggql_query, action>(in, state);

//     if (success) {
//       std::cout << "Parse do GGQL concluído com sucesso e sem alocações!\n";
//     } else {
//       std::cerr << "Erro de sintaxe na query.\n";
//     }
//   } catch (const peg::parse_error &e) {
//     // O PEGTL lança exceções detalhadas se você configurar regras com 'must<>'
//     std::cerr << "Erro fatal de parse: " << e.what() << '\n';
//   }

//   return 0;
// }