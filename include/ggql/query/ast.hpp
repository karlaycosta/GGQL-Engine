#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ggql {

// ─── Enumerações ──────────────────────────────────────────────────────────────

enum class EntityType : uint8_t { SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT };
enum class Operator   : uint8_t { EQ, NEQ, GT, LT, GEQ, LEQ };
enum class LogicalOp  : uint8_t { AND, OR };

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

// Forward — referência circular: WhereClause contém ConditionExpr,
// ConditionExpr (via GroupedExpr) contém WhereClause.
struct WhereClause;

/// (<sub-where>) — sub-expressão entre parênteses (B-5).
/// Permite combinações como `(a OR b) AND c`. Precedência explícita —
/// resolve a ambiguidade left-to-right de AND/OR documentada no evaluator.
struct GroupedExpr {
    /// shared_ptr (vs unique_ptr): mantém ConditionExpr copyable, preservando
    /// a ergonomia de `execute(const GgqlQuery&)` em todo o pipeline. AST é
    /// efetivamente imutável após parse — share não causa aliasing problemático.
    std::shared_ptr<WhereClause> body;
};

using ConditionExpr =
    std::variant<CompareCondition, CrossesCondition, ContainsKmerCondition, GroupedExpr>;

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
    /// Lista explícita de colunas. Vazio + return_all=false → planner usa
    /// default (mesmo conjunto que return_all=true). Vazio + return_all=true
    /// → "RETURN *" explícito (semanticamente igual ao default mas registrado).
    std::vector<std::string> return_fields;
    bool return_all = false;          // B-8: `RETURN *`
    std::optional<uint32_t> limit;    // máximo de linhas em flat[]
    std::optional<uint32_t> offset;   // linhas a pular antes de emitir
};

/// SEARCH KMER "<seq>" [RETURN ...] [LIMIT N [OFFSET M]]
struct SearchKmerQuery {
    std::string kmer;
    std::vector<std::string> return_fields;
    bool return_all = false;          // B-8
    std::optional<uint32_t> limit;
    std::optional<uint32_t> offset;   // B-11: simétrico ao FindQuery
};

using GgqlQuery = std::variant<FindQuery, SearchKmerQuery>;

} // namespace ggql
