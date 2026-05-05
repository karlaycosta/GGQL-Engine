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

// Resultado de uma consulta em layout colunar row-major.
// flat[row * columns.size() + col] dá o valor da célula (row, col).
struct QueryResult {
    std::vector<std::string> columns;
    std::vector<Value>       flat;
    size_t                   row_count  = 0;
    size_t                   total_hits = 0;
    bool   total_hits_truncated = false;  // true se total_hits parou em kMaxTotalHitsCount
    bool   safety_limit_hit     = false;  // true se sem LIMIT explícito e > kSafetyHardLimit

    /// Formata como tabela ASCII alinhada (útil para REPL / debug).
    [[nodiscard]] std::string format_table() const;
};

// ─── QueryPlanner ─────────────────────────────────────────────────────────────

class QueryPlanner {
  public:
    /// Ponteiros não-proprietários. Podem ser null se o backend não for necessário.
    explicit QueryPlanner(const GfaCsrTopology*   topology  = nullptr,
                          const GfaMetadataStore* metadata  = nullptr,
                          const GfaSequenceIndex* seq_index = nullptr);
    ~QueryPlanner() = default;

    QueryPlanner(const QueryPlanner&)                     = delete;
    QueryPlanner& operator=(const QueryPlanner&)          = delete;
    QueryPlanner(QueryPlanner&&) noexcept                 = default;
    QueryPlanner& operator=(QueryPlanner&&) noexcept      = default;

    [[nodiscard]] std::expected<QueryResult, std::string> execute(const GgqlQuery& query) const;

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
                                      const KmerCache* cache = nullptr) const noexcept;
    [[nodiscard]] bool eval_where(const WhereClause& wc, const RowFields& row,
                                  const KmerCache* cache = nullptr) const noexcept;

    [[nodiscard]] static bool compare_values(const Value& lhs, Operator op,
                                             const Value& rhs) noexcept;

    const GfaCsrTopology*   topology_  = nullptr;
    const GfaMetadataStore* metadata_  = nullptr;
    const GfaSequenceIndex* seq_index_ = nullptr;
};

} // namespace ggql
