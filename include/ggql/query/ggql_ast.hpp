#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>   // std::to_underlying, std::unreachable (C++23)
#include <variant>
#include <vector>

namespace ggql {

// ─── Enumerações ──────────────────────────────────────────────────────────────

enum class EntityType { SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT };
enum class Operator { EQ, NEQ, GT, LT, GEQ, LEQ };
enum class LogicalOp { AND, OR };

// ─── Value (número inteiro ou string entre aspas) ─────────────────────────────

using Value = std::variant<int64_t, double, std::string>;

// ─── Expressões de condição (cláusula WHERE) ──────────────────────────────────

/// <field> <op> <value>  — ex.: length < 200, depth >= 3, name = "chr1"
struct CompareCondition {
    std::string field;
    Operator op;
    Value value;
};

/// crosses(<path_name>) — segmentos que cruzam o caminho especificado
struct CrossesCondition {
    std::string path_name;
};

/// contains_kmer("<seq>") — segmentos que contêm o k-mer
struct ContainsKmerCondition {
    std::string kmer;
};

using ConditionExpr = std::variant<CompareCondition, CrossesCondition, ContainsKmerCondition>;

// ─── Cláusula WHERE ───────────────────────────────────────────────────────────

struct WhereClause {
    std::vector<ConditionExpr> conditions;
    /// connectors[i] é o operador lógico entre conditions[i] e conditions[i+1].
    /// Invariante: connectors.size() == conditions.size() - 1
    std::vector<LogicalOp> connectors;

    /// Verifica o invariante de tamanho em tempo de execução (debug builds).
    [[nodiscard]] bool is_valid() const noexcept {
        return conditions.empty() || connectors.size() + 1 == conditions.size();
    }
};

// ─── Queries ──────────────────────────────────────────────────────────────────

/// FIND <entity> [WHERE ...] [RETURN ...]
struct FindQuery {
    EntityType entity = EntityType::SEGMENT;
    std::optional<WhereClause> where;
    std::vector<std::string> return_fields;
};

/// SEARCH KMER "<seq>" [RETURN ...]
struct SearchKmerQuery {
    std::string kmer;
    std::vector<std::string> return_fields;
};

using GgqlQuery = std::variant<FindQuery, SearchKmerQuery>;

// ─── Utilitários de conversão para string ─────────────────────────────────────

// Enums são contíguos a partir de 0 — garantido por static_assert abaixo.
static_assert(std::to_underlying(EntityType::REPEAT) == 5,
    "EntityType deve ser contíguo 0..5");
static_assert(std::to_underlying(Operator::LEQ) == 5,
    "Operator deve ser contíguo 0..5");

constexpr std::array<std::string_view, 6> k_entity_names {
    "SEGMENT", "EDGE", "PATH", "BUBBLE", "TIP", "REPEAT"
};

constexpr std::array<std::string_view, 6> k_op_names {
    "=", "!=", ">", "<", ">=", "<="
};

[[nodiscard("Use o nome retornado para exibição ou serialização")]]
constexpr std::string_view entity_to_string(EntityType e) noexcept {
    return k_entity_names[std::to_underlying(e)];
}

[[nodiscard("Use o operador retornado para exibição ou serialização")]]
constexpr std::string_view op_to_string(Operator o) noexcept {
    return k_op_names[std::to_underlying(o)];
}

[[nodiscard("Use o operador lógico retornado para exibição ou serialização")]]
constexpr std::string_view logical_op_to_string(LogicalOp l) noexcept {
    switch (l) {
        case LogicalOp::AND: return "AND";
        case LogicalOp::OR:  return "OR";
    }
    std::unreachable();
}

} // namespace ggql
