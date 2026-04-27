#pragma once

// query_planner.hpp — Interface pública do QueryPlanner.
//
// Recebe AST GgqlQuery e executa contra os backends:
//
//   EntityType   │ Backend                            │ Estado
//   ─────────────┼────────────────────────────────────┼──────────────
//   TIP          │ GfaCsrTopology::find_tips()        │ ✅
//   REPEAT       │ GfaCsrTopology::find_repeats()     │ ✅
//   BUBBLE       │ GfaCsrTopology::find_bubbles()     │ ✅
//   SEGMENT      │ GfaMetadataStore (binário .idx)    │ ✅
//   EDGE         │ (não implementado)                 │ ❌ Phase 2
//   PATH         │ (não implementado)                 │ ❌ Phase 2
//   SEARCH KMER  │ FM-Index                           │ ✅
//
// Implementação: src/query/query_planner.cpp + query_evaluator.cpp

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ggql/ingest/gfa_metadata_store.hpp>
#include <ggql/storage/gfa_fm_index.hpp>
#include <ggql/storage/gfa_topology.hpp>
#include "ggql_ast.hpp"

namespace ggql {

// ─── Tipos de resultado ───────────────────────────────────────────────────────

using RowFields = std::unordered_map<std::string, Value>;

/// Cache pré-computado: kmer → conjunto de segment IDs que o contêm.
using KmerCache = std::unordered_map<std::string, std::unordered_set<uint64_t>>;

struct QueryResult {
    std::vector<std::string> columns;
    std::vector<RowFields>   rows;
    size_t                   total_hits = 0;

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

    QueryPlanner(const QueryPlanner&)            = delete;
    QueryPlanner& operator=(const QueryPlanner&) = delete;

    [[nodiscard]] std::expected<QueryResult, std::string> execute(const GgqlQuery& query) const;

  private:
    [[nodiscard]] std::expected<QueryResult, std::string> execute_find(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> execute_search_kmer(const SearchKmerQuery& q) const;

    [[nodiscard]] std::expected<QueryResult, std::string> plan_tips(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_repeats(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_bubbles(const FindQuery& q) const;
    [[nodiscard]] std::expected<QueryResult, std::string> plan_segments(const FindQuery& q) const;

    [[nodiscard]] bool eval_condition(const ConditionExpr& cond, const RowFields& row,
                                      const KmerCache* cache = nullptr) const noexcept;
    [[nodiscard]] bool eval_where(const WhereClause& wc, const RowFields& row,
                                  const KmerCache* cache = nullptr) const noexcept;

    [[nodiscard]] static bool compare_values(const Value& lhs, Operator op,
                                             const Value& rhs) noexcept;
    [[nodiscard]] static QueryResult project(std::vector<RowFields> rows,
                                             const std::vector<std::string>& return_fields,
                                             std::span<const std::string_view> all_columns,
                                             size_t total_hits = 0);

    const GfaCsrTopology*   topology_  = nullptr;
    const GfaMetadataStore* metadata_  = nullptr;
    const GfaSequenceIndex* seq_index_ = nullptr;
};

} // namespace ggql
