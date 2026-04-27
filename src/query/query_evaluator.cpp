// query_evaluator.cpp — Avaliação de condições WHERE do QueryPlanner.
//
// Contém: eval_condition, eval_where, compare_values.
// Separado de query_planner.cpp para isolar a lógica de predicados
// e facilitar testes unitários independentes dos backends de entidade.

#include "ggql/query/query_planner.hpp"

#include <charconv>
#include <optional>
#include <utility> // std::unreachable

namespace ggql {

// ── compare_values ────────────────────────────────────────────────────────────

bool QueryPlanner::compare_values(const Value& lhs, Operator op, const Value& rhs) noexcept {
    auto cmp_dbl = [](double l, Operator o, double r) -> bool {
        switch (o) {
            case Operator::EQ:  return l == r;
            case Operator::NEQ: return l != r;
            case Operator::GT:  return l > r;
            case Operator::LT:  return l < r;
            case Operator::GEQ: return l >= r;
            case Operator::LEQ: return l <= r;
        }
        std::unreachable();
    };

    auto as_double = [](const Value& v) -> std::optional<double> {
        if (const auto* i = std::get_if<int64_t>(&v)) { return static_cast<double>(*i); }
        if (const auto* d = std::get_if<double>(&v))  { return *d; }
        if (const auto* s = std::get_if<std::string>(&v)) {
            double r = 0;
            const auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), r);
            if (ec == std::errc{} && ptr == s->data() + s->size()) { return r; }
        }
        return std::nullopt;
    };

    // Caso 1: ambos int64_t — comparação inteira exata
    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
        return cmp_dbl(static_cast<double>(std::get<int64_t>(lhs)), op,
                       static_cast<double>(std::get<int64_t>(rhs)));
    }

    // Caso 2: pelo menos um double — promoção numérica
    if (std::holds_alternative<double>(lhs) || std::holds_alternative<double>(rhs)) {
        auto ld = as_double(lhs);
        auto rd = as_double(rhs);
        if (ld && rd) { return cmp_dbl(*ld, op, *rd); }
        return op == Operator::NEQ;
    }

    // Caso 3: ambos string — comparação lexicográfica
    if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
        const auto& l = std::get<std::string>(lhs);
        const auto& r = std::get<std::string>(rhs);
        switch (op) {
            case Operator::EQ:  return l == r;
            case Operator::NEQ: return l != r;
            case Operator::GT:  return l > r;
            case Operator::LT:  return l < r;
            case Operator::GEQ: return l >= r;
            case Operator::LEQ: return l <= r;
        }
        std::unreachable();
    }

    // Caso 4: int64_t vs string — tenta parse numérico
    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<std::string>(rhs)) {
        auto ld = as_double(lhs);
        auto rd = as_double(rhs);
        if (ld && rd) { return cmp_dbl(*ld, op, *rd); }
        return false;
    }

    return op == Operator::NEQ; // tipos incompatíveis
}

// ── eval_condition ────────────────────────────────────────────────────────────

bool QueryPlanner::eval_condition(const ConditionExpr& cond, const RowFields& row,
                                   const KmerCache* cache) const noexcept {
    return std::visit(
        [&](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, CompareCondition>) {
                auto it = row.find(c.field);
                if (it == row.end()) { return false; } // campo ausente → predicado não aplicável
                return compare_values(it->second, c.op, c.value);

            } else if constexpr (std::is_same_v<T, CrossesCondition>) {
                // TODO Phase 2: integrar PathIndex
                return false;

            } else {
                // ContainsKmerCondition
                auto it_id = row.find("id");
                if (it_id == row.end()) { return false; }
                const uint64_t current_id =
                    static_cast<uint64_t>(std::get<int64_t>(it_id->second));

                // Caminho rápido: cache pré-computado O(1)
                if (cache) {
                    auto it_cache = cache->find(c.kmer);
                    if (it_cache == cache->end()) { return false; }
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

bool QueryPlanner::eval_where(const WhereClause& wc, const RowFields& row,
                               const KmerCache* cache) const noexcept {
    if (wc.conditions.empty()) { return true; }

    bool result = eval_condition(wc.conditions[0], row, cache);

    // Avaliação esquerda-para-direita (sem precedência de operadores)
    for (size_t i = 0; i < wc.connectors.size(); ++i) {
        const bool rhs = eval_condition(wc.conditions[i + 1], row, cache);
        if (wc.connectors[i] == LogicalOp::AND) {
            result &= rhs;
        } else {
            result |= rhs;
        }
    }
    return result;
}

} // namespace ggql
