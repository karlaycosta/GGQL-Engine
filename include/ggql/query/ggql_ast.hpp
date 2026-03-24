#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ggql {

// ─── Enumerações ──────────────────────────────────────────────────────────────

enum class EntityType { SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT };
enum class Operator   { EQ, NEQ, GT, LT, GEQ, LEQ };
enum class LogicalOp  { AND, OR };

// ─── Value (número inteiro ou string entre aspas) ─────────────────────────────

using Value = std::variant<int64_t, std::string>;

// ─── Expressões de condição (cláusula WHERE) ──────────────────────────────────

/// <field> <op> <value>  — ex.: length < 200, depth >= 3, name = "chr1"
struct CompareCondition {
    std::string field;
    Operator    op;
    Value       value;
};

/// crosses(<path_name>) — segmentos que cruzam o caminho especificado
struct CrossesCondition {
    std::string path_name;
};

/// contains_kmer("<seq>") — segmentos que contêm o k-mer
struct ContainsKmerCondition {
    std::string kmer;
};

using ConditionExpr = std::variant<CompareCondition,
                                   CrossesCondition,
                                   ContainsKmerCondition>;

// ─── Cláusula WHERE ───────────────────────────────────────────────────────────

struct WhereClause {
    std::vector<ConditionExpr> conditions;
    /// connectors[i] é o operador lógico entre conditions[i] e conditions[i+1].
    /// Invariante: connectors.size() == conditions.size() - 1
    std::vector<LogicalOp>     connectors;
};

// ─── Queries ──────────────────────────────────────────────────────────────────

/// FIND <entity> [WHERE ...] [RETURN ...]
struct FindQuery {
    EntityType               entity = EntityType::SEGMENT;
    std::optional<WhereClause> where;
    std::vector<std::string> return_fields;
};

/// SEARCH KMER "<seq>" [RETURN ...]
struct SearchKmerQuery {
    std::string              kmer;
    std::vector<std::string> return_fields;
};

using GgqlQuery = std::variant<FindQuery, SearchKmerQuery>;

// ─── Utilitários de conversão para string ─────────────────────────────────────

[[nodiscard]] constexpr std::string_view entity_to_string(EntityType e) noexcept {
    switch (e) {
        case EntityType::SEGMENT: return "SEGMENT";
        case EntityType::EDGE:    return "EDGE";
        case EntityType::PATH:    return "PATH";
        case EntityType::BUBBLE:  return "BUBBLE";
        case EntityType::TIP:     return "TIP";
        case EntityType::REPEAT:  return "REPEAT";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view op_to_string(Operator o) noexcept {
    switch (o) {
        case Operator::EQ:  return "=";
        case Operator::NEQ: return "!=";
        case Operator::GT:  return ">";
        case Operator::LT:  return "<";
        case Operator::GEQ: return ">=";
        case Operator::LEQ: return "<=";
    }
    return "?";
}

[[nodiscard]] constexpr std::string_view logical_op_to_string(LogicalOp l) noexcept {
    return l == LogicalOp::AND ? "AND" : "OR";
}

} // namespace ggql
