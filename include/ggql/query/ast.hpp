#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
    /// Estado válido: WHERE vazio (sem conditions e sem connectors) OU
    /// connectors.size() == conditions.size() - 1 (cada connector liga
    /// duas conditions consecutivas).
    [[nodiscard]] bool is_valid() const noexcept {
        if (conditions.empty()) {
            return connectors.empty(); // sem conditions → sem connectors
        }
        return connectors.size() + 1 == conditions.size();
    }
};

// ─── Queries ──────────────────────────────────────────────────────────────────

/// FIND <entity> [WHERE ...] [RETURN ...] [LIMIT N [OFFSET M]]
struct FindQuery {
    EntityType entity = EntityType::SEGMENT;
    std::optional<WhereClause> where;
    std::vector<std::string> return_fields;
    std::optional<uint32_t> limit;   // máximo de linhas em flat[]
    std::optional<uint32_t> offset;  // linhas a pular antes de emitir
};

/// SEARCH KMER "<seq>" [RETURN ...] [LIMIT N]
struct SearchKmerQuery {
    std::string kmer;
    std::vector<std::string> return_fields;
    std::optional<uint32_t> limit;
};

using GgqlQuery = std::variant<FindQuery, SearchKmerQuery>;

} // namespace ggql
