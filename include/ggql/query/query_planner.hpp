#pragma once

// query_planner.hpp — Interface pública do QueryPlanner.
//
// Recebe AST GgqlQuery e executa contra os backends:
//
//   EntityType   │ Backend                                       │ Estado
//   ─────────────┼───────────────────────────────────────────────┼──────
//   TIP          │ GfaCsrTopology::find_tips()                   │ ✅
//   REPEAT       │ GfaCsrTopology::find_repeats()                │ ✅
//   BUBBLE       │ GfaCsrTopology::snarls() (pré-computado)      │ ✅
//   SEGMENT      │ GfaMetadataStore (binário .idx)               │ ✅
//   EDGE         │ GfaCsrTopology::out_neighbors()               │ ✅
//   PATH         │ path_stats / path_nodes() (fallback)          │ ✅
//   SEARCH KMER  │ FM-Index                                      │ ✅
//
// Implementação: src/query/query_planner.cpp + query_evaluator.cpp

#include <array>
#include <expected>
#include <generator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast.hpp"

namespace ggql {

// Forward declarations — tipos completos necessários apenas em
// query_planner.cpp e em callers que instanciem essas classes (esses
// devem incluir os headers correspondentes diretamente).
class GfaCsrTopology;
class GfaMetadataStore;
class GfaSequenceIndex;

// ─── RowFields ────────────────────────────────────────────────────────────────
//
// Mapa inline sem alocação de heap — usado exclusivamente para avaliação de
// cláusulas WHERE dentro de plan_*(). Máximo de 6 campos (SEGMENT tem 5).
// As chaves são string_view apontando para literais estáticos ("id", "name"…).

struct RowFields {
    static constexpr size_t kMaxFields = 6;
    std::array<std::string_view, kMaxFields> keys{};
    std::array<Value,            kMaxFields> vals{};
    size_t                                   count = 0;

    void insert(std::string_view k, Value v) noexcept {
        if (count >= kMaxFields) [[unlikely]] { return; }
        keys[count] = k;
        vals[count] = std::move(v);
        ++count;
    }

    [[nodiscard]] const Value* find(std::string_view key) const noexcept {
        for (size_t i = 0; i < count; ++i) {
            if (keys[i] == key) { return &vals[i]; }
        }
        return nullptr;
    }
};

// ─── Tipos de resultado ───────────────────────────────────────────────────────

/// Cache pré-computado: kmer → conjunto de segment IDs que o contêm.
using KmerCache = std::unordered_map<std::string, std::unordered_set<uint64_t>>;

/// Cache pré-computado: path_name (target de `crosses(...)`) → conjunto de
/// node IDs visitados por aquele path. Construído uma vez por query em
/// plan_paths quando há CrossesCondition no WHERE; entradas com path_name
/// inexistente não são inseridas (eval retorna false naturalmente).
using CrossesCache = std::unordered_map<std::string, std::unordered_set<uint64_t>>;

// Resultado de uma consulta em layout colunar row-major.
// flat[row * columns.size() + col] dá o valor da célula (row, col).
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<Value>       flat;
    size_t                   row_count  = 0;
    size_t                   total_hits = 0;
    bool   total_hits_truncated = false;  // true se total_hits parou em kMaxTotalHitsCount
    bool   safety_limit_hit     = false;  // true se sem LIMIT explícito e > kSafetyHardLimit
    bool   cancelled            = false;  // Round 3.F: scan abortado via cancel_token cooperativo

    /// Formata como tabela ASCII alinhada (útil para REPL / debug).
    [[nodiscard]] std::string format_table() const;
};

// ─── QueryPlanner ─────────────────────────────────────────────────────────────

class QueryPlanner {
  public:
    /// Ponteiros não-proprietários. Podem ser null se o backend não for necessário.
    explicit QueryPlanner(const GfaCsrTopology*   topology  = nullptr,
                          const GfaMetadataStore* metadata  = nullptr,
                          const GfaSequenceIndex* seq_index = nullptr) noexcept;
    ~QueryPlanner() = default;

    QueryPlanner(const QueryPlanner&)                     = delete;
    QueryPlanner& operator=(const QueryPlanner&)          = delete;
    QueryPlanner(QueryPlanner&&) noexcept                 = default;
    QueryPlanner& operator=(QueryPlanner&&) noexcept      = default;

    [[nodiscard]] std::expected<QueryResult, std::string> execute(const GgqlQuery& query) const;

    // C7 / Fase 7+ (2026-05-22): cursor-style streaming. Mantém estado de
    // iteração entre páginas — não re-parseia/re-planeja a query nem repete
    // o "skip" interno (que tornava execute_streaming O(N²) cumulativo).
    //
    // ESCOPO ATUAL (C7.1): apenas FIND SEGMENT em modo full-scan. Caminhos
    // rápidos (name/length/depth index) e demais backends (EDGE/PATH/TIP/
    // REPEAT/BUBBLE/SEARCH KMER) retornam std::unexpected("cursor: backend
    // não suportado — use execute() ou execute_streaming()"). Refactor
    // incremental por backend nas fases C7.2+.
    //
    // Uso típico (C++):
    //   auto cursor = planner.execute_cursor(parsed_ast);
    //   if (!cursor) return cursor.error();
    //   while (true) {
    //       auto page = (*cursor)->next(1024);
    //       if (!page) return page.error();
    //       if (page->row_count == 0) break;  // fim
    //       process(*page);
    //   }
    [[nodiscard]] std::expected<std::unique_ptr<class QueryCursor>, std::string>
        execute_cursor(const GgqlQuery& query) const;

    // C7: público para uso por generators de cursor que vivem em
    // namespace anônimo no .cpp. Não-API estável — consumers externos
    // devem usar execute() / execute_cursor().
    [[nodiscard]] bool eval_where(const WhereClause& wc, const RowFields& row,
                                  const KmerCache* kmer_cache = nullptr,
                                  const CrossesCache* crosses_cache = nullptr) const noexcept;

  private:
    [[nodiscard]] std::expected<QueryResult, std::string> execute_find(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> execute_search_kmer(const SearchKmerQuery& q) const;

    [[nodiscard]] std::expected<QueryResult, std::string> plan_tips(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_repeats(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string>
        plan_node_query(const FindQuery& q, std::span<const uint64_t> node_ids) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_bubbles(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_segments(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_edges(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_paths(const FindQuery& q) const;

    [[nodiscard]] bool eval_condition(const ConditionExpr& cond, const RowFields& row,
                                      const KmerCache* kmer_cache = nullptr,
                                      const CrossesCache* crosses_cache = nullptr) const noexcept;

    [[nodiscard]] static bool compare_values(const Value& lhs, Operator op,
                                             const Value& rhs) noexcept;

    const GfaCsrTopology*   topology_  = nullptr;
    const GfaMetadataStore* metadata_  = nullptr;
    const GfaSequenceIndex* seq_index_ = nullptr;

};

// ─── QueryCursor (C7 / 2026-05-22) ────────────────────────────────────────────
//
// Cursor stateful para streaming verdadeiro (true O(N) cumulativo, sem
// re-parse e sem re-execução com offset crescente). Implementado via
// `std::generator<Value>` por baixo do capô — cada `co_yield` emite uma
// célula (Value), o cursor agrupa em linhas baseado em ncols.
//
// ESCOPO ATUAL (C7.1): apenas FIND SEGMENT full-scan. Backends adicionais
// virão em C7.2+.
//
// Lifetime: o cursor mantém referência ao QueryPlanner (não-proprietário) —
// destruir o planner antes de exaurir o cursor é UB. O Cursor é movível mas
// não copiável (generator não é copiável).
class QueryCursor {
public:
    // Construção via QueryPlanner::execute_cursor(). Caller não constrói
    // diretamente em uso normal.
    QueryCursor(std::generator<Value> gen,
                std::vector<std::string> columns,
                size_t ncols);
    ~QueryCursor();

    QueryCursor(const QueryCursor&)            = delete;
    QueryCursor& operator=(const QueryCursor&) = delete;
    QueryCursor(QueryCursor&&) noexcept;
    QueryCursor& operator=(QueryCursor&&) noexcept;

    // Materializa até `page_size` linhas. Retorna QueryResult com
    // row_count <= page_size; row_count < page_size indica fim do stream
    // (próxima chamada também retorna 0 linhas).
    [[nodiscard]] std::expected<QueryResult, std::string>
        next(uint32_t page_size);

    // True se o generator está esgotado (próxima next() retorna 0 linhas).
    [[nodiscard]] bool exhausted() const noexcept;

    // Total cumulativo de linhas emitidas desde construção do cursor.
    [[nodiscard]] uint64_t total_emitted() const noexcept;

    // Esquema da query (colunas) — constante por cursor.
    [[nodiscard]] std::span<const std::string> columns() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace ggql
