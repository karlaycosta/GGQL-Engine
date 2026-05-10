// query_evaluator.cpp — Avaliação de condições WHERE do QueryPlanner.
//
// Contém: eval_condition, eval_where, compare_values.
// Separado de query_planner.cpp para isolar a lógica de predicados
// e facilitar testes unitários independentes dos backends de entidade.

#include "ggql/query/query_planner.hpp"

#include "ggql/ingest/metadata_store.hpp"
#include "ggql/storage/fm_index.hpp"
#include "ggql/storage/topology.hpp"

#include <charconv>
#include <optional>
#include <utility> // std::unreachable
#include <vector>

namespace ggql {

// ── compare_values ────────────────────────────────────────────────────────────

namespace {

// Helper genérico: compara dois T usando o Operator. Especializado para
// int64_t (exato), double (promoção numérica) e std::string (lexicográfico).
template <typename T>
[[nodiscard]] constexpr bool cmp_op(const T& l, Operator o, const T& r) noexcept {
    switch (o) {
        case Operator::EQ:  return l == r;
        case Operator::NEQ: return l != r;
        case Operator::GT:  return l > r;
        case Operator::LT:  return l < r;
        case Operator::GEQ: return l >= r;
        case Operator::LEQ: return l <= r;
    }
    std::unreachable();
}

[[nodiscard]] inline std::optional<double> as_double(const Value& v) noexcept {
    if (const auto* i = std::get_if<int64_t>(&v)) { return static_cast<double>(*i); }
    if (const auto* d = std::get_if<double>(&v))  { return *d; }
    if (const auto* s = std::get_if<std::string>(&v)) {
        double r = 0;
        const auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), r);
        if (ec == std::errc{} && ptr == s->data() + s->size()) { return r; }
    }
    return std::nullopt;
}

} // anonymous namespace

bool QueryPlanner::compare_values(const Value& lhs, Operator op, const Value& rhs) noexcept {
    // Caso 1: ambos int64_t — comparação inteira exata (preserva precisão
    // para valores > 2^53 que perderiam bits via promoção a double).
    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
        return cmp_op<int64_t>(std::get<int64_t>(lhs), op, std::get<int64_t>(rhs));
    }

    // Caso 2: pelo menos um double — promoção numérica via as_double.
    if (std::holds_alternative<double>(lhs) || std::holds_alternative<double>(rhs)) {
        auto ld = as_double(lhs);
        auto rd = as_double(rhs);
        if (ld && rd) { return cmp_op<double>(*ld, op, *rd); }
        return op == Operator::NEQ; // tipos incompatíveis = SQL UNKNOWN
    }

    // Caso 3: ambos string — comparação lexicográfica.
    if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
        return cmp_op<std::string>(std::get<std::string>(lhs), op, std::get<std::string>(rhs));
    }

    // Caso 4: int64_t × string ou string × int64_t (simétrico) — tenta parse
    // numérico de ambos. Fallback consistente com Caso 2 (op == NEQ).
    const bool lhs_is_int = std::holds_alternative<int64_t>(lhs);
    const bool rhs_is_int = std::holds_alternative<int64_t>(rhs);
    const bool lhs_is_str = std::holds_alternative<std::string>(lhs);
    const bool rhs_is_str = std::holds_alternative<std::string>(rhs);
    if ((lhs_is_int && rhs_is_str) || (lhs_is_str && rhs_is_int)) {
        auto ld = as_double(lhs);
        auto rd = as_double(rhs);
        if (ld && rd) { return cmp_op<double>(*ld, op, *rd); }
        return op == Operator::NEQ;
    }

    return op == Operator::NEQ; // tipos totalmente incompatíveis
}

// ── eval_condition ────────────────────────────────────────────────────────────

bool QueryPlanner::eval_condition(const ConditionExpr& cond, const RowFields& row,
                                   const KmerCache* kmer_cache,
                                   const CrossesCache* crosses_cache) const noexcept {
    return std::visit(
        [&](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, CompareCondition>) {
                const Value* v = row.find(c.field);
                if (!v) { return false; }
                return compare_values(*v, c.op, c.value);

            } else if constexpr (std::is_same_v<T, GroupedExpr>) {
                // B-5: sub-WhereClause aninhada — recursão direta.
                if (!c.body) { return false; }
                return eval_where(*c.body, row, kmer_cache, crosses_cache);

            } else if constexpr (std::is_same_v<T, CrossesCondition>) {
                // Pré-condição: cache pré-computado pelo plan_paths.
                // Sem cache (caller que não construiu — ex: outras entidades) →
                // semantic equivalente ao stub antigo (zero hits).
                if (!crosses_cache || !topology_) { return false; }

                const auto it = crosses_cache->find(c.path_name);
                if (it == crosses_cache->end()) {
                    // Path target inexistente OU não foi pré-cacheado.
                    return false;
                }
                const auto& target_nodes = it->second;

                // path_id do candidato (path da row atual).
                const Value* id_val = row.find("id");
                if (!id_val) { return false; }
                const auto* id_int = std::get_if<int64_t>(id_val);
                if (!id_int) { return false; }
                const uint64_t candidate_pid = static_cast<uint64_t>(*id_int);

                // Buffer thread_local reusado entre chamadas — evita alocação
                // por candidato em loops com milhares de paths. path_nodes()
                // pode lançar bad_alloc; convertemos em "false" para preservar
                // noexcept no eval_condition.
                thread_local std::vector<uint64_t> candidate_nodes;
                candidate_nodes.clear();
                try {
                    topology_->path_nodes(candidate_pid, candidate_nodes);
                } catch (...) {
                    return false;
                }

                // Early exit no primeiro nó compartilhado.
                for (const uint64_t nid : candidate_nodes) {
                    if (target_nodes.contains(nid)) { return true; }
                }
                return false;

            } else {
                // ContainsKmerCondition
                const Value* id_val = row.find("id");
                if (!id_val) { return false; }
                // get_if (não-throwing) em vez de get<T> — eval_condition é
                // noexcept; se o caller construir row com tipo errado em "id",
                // get<int64_t> chamaria std::terminate.
                const auto* id_int = std::get_if<int64_t>(id_val);
                if (!id_int) { return false; }
                const uint64_t current_id = static_cast<uint64_t>(*id_int);

                // Caminho rápido: cache pré-computado O(1)
                if (kmer_cache) {
                    auto it_cache = kmer_cache->find(c.kmer);
                    if (it_cache == kmer_cache->end()) { return false; }
                    return it_cache->second.contains(current_id);
                }

                // Fallback: busca direta O(K + hits) — para entidades não-SEGMENT
                if (!seq_index_ || !metadata_) { return false; }
                for (uint64_t pos : seq_index_->search_kmer(c.kmer)) {
                    if (auto sid = metadata_->segment_id_from_offset(pos);
                        sid && *sid == current_id) {
                        return true;
                    }
                }
                return false;
            }
        },
        cond);
}

// ── eval_where ────────────────────────────────────────────────────────────────

// Semântica: avaliação estritamente esquerda-para-direita, sem precedência
// AND > OR (diferente de SQL/C). Em GGQL:
//   `A OR B AND C` é avaliado como `(A OR B) AND C`
// Para WHERE com 1-2 conditions (caso comum), nunca expõe diferença. Para
// queries complexas com mix AND/OR, usuário deve agrupar mentalmente da
// esquerda. Implementar precedência exigiria parser hierárquico (AST de
// expressão) na grammar — fora do escopo atual.
bool QueryPlanner::eval_where(const WhereClause& wc, const RowFields& row,
                               const KmerCache* kmer_cache,
                               const CrossesCache* crosses_cache) const noexcept {
    if (wc.conditions.empty()) { return true; }

    bool result = eval_condition(wc.conditions[0], row, kmer_cache, crosses_cache);

    // Avaliação esquerda-para-direita com short-circuit por operador.
    for (size_t i = 0; i < wc.connectors.size(); ++i) {
        if (wc.connectors[i] == LogicalOp::AND) {
            if (!result) { break; }               // false AND anything = false
            result = eval_condition(wc.conditions[i + 1], row, kmer_cache, crosses_cache);
        } else {
            if (result)  { break; }               // true OR anything = true
            result = eval_condition(wc.conditions[i + 1], row, kmer_cache, crosses_cache);
        }
    }
    return result;
}

} // namespace ggql
