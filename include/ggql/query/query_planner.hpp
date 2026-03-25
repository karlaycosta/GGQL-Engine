#pragma once

// query_planner.hpp — Recebe a AST GgqlQuery e a executa contra os backends:
//
//   EntityType   │ Backend                       │ Estado
//   ─────────────┼───────────────────────────────┼─────────────────
//   TIP          │ GfaCsrTopology::find_tips()   │ ✅
//   REPEAT       │ GfaCsrTopology::find_repeats() │ ✅
//   BUBBLE       │ GfaCsrTopology::find_bubbles() │ ✅
//   SEGMENT      │ SQLite (gfa_metadata_store)   │ ✅
//   EDGE         │ (não implementado)             │ ❌ Phase 2
//   PATH         │ (não implementado)             │ ❌ Phase 2
//   SEARCH KMER  │ FM-Index (não implementado)   │ ❌ Phase 2
//
// ── Estratégia de pushdown SQL para FIND SEGMENT ─────────────────────────────
//
//   • Se TODAS as condições WHERE são CompareCondition em campos conhecidos
//     (id, name, length, read_count, depth):
//       → Tradução completa para SQL com parentesização correta para
//         preservar a avaliação GGQL esquerda-para-direita.
//
//   • Se alguma condição não é traduzível (CrossesCondition,
//     ContainsKmerCondition, campo desconhecido):
//       → Varredura completa da tabela + filtragem em memória (correto, mas
//         potencialmente lento em tabelas de 85M linhas).
//         TODO Phase 2: pushdown parcial com AND para condições mistas.
//
// ── Avaliação de WHERE para entidades topológicas ────────────────────────────
//
//   Os campos disponíveis por entidade são:
//     TIP / REPEAT   : id, in_degree, out_degree
//     BUBBLE         : source, sink, branch_count
//   Se uma condição referencia um campo inexistente na entidade, o predicado
//   retorna false e a linha é excluída (comportamento conservador).

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <ggql/storage/gfa_csr_topology.hpp>
#include <iomanip>
#include <optional>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ggql_ast.hpp"

namespace ggql {

// ─── Linha de resultado ───────────────────────────────────────────────────────

using RowFields = std::unordered_map<std::string, Value>;

// ─── Resultado de uma query ───────────────────────────────────────────────────

struct QueryResult {
    std::vector<std::string> columns; ///< Colunas na ordem de exibição
    std::vector<RowFields> rows;

    /// Formata como tabela ASCII alinhada (útil para REPL / debug).
    [[nodiscard]] std::string format_table() const;
};

// ─── QueryPlanner ─────────────────────────────────────────────────────────────

class QueryPlanner {
  public:
    /// @param topology  Ponteiro não-proprietário para a topologia CSR.
    ///                  Pode ser null se apenas queries SEGMENT forem usadas.
    /// @param db_path   Caminho para o SQLite de metadados.
    ///                  Vazio roda sem banco — FIND SEGMENT retornará erro.
    explicit QueryPlanner(const GfaCsrTopology* topology = nullptr, std::string_view db_path = "");
    ~QueryPlanner();

    // Não copiável (gerencia sqlite3* raw)
    QueryPlanner(const QueryPlanner&) = delete;
    QueryPlanner& operator=(const QueryPlanner&) = delete;

    /// Executa uma GgqlQuery parseada e retorna o resultado tabular.
    [[nodiscard]] std::expected<QueryResult, std::string> execute(const GgqlQuery& query) const;

  private:
    // ── dispatchers ──────────────────────────────────────────────────────────

    [[nodiscard]] std::expected<QueryResult, std::string> execute_find(const FindQuery& q) const;

    [[nodiscard]] std::expected<QueryResult, std::string>
    execute_search_kmer(const SearchKmerQuery& q) const;

    // ── backends por entidade ─────────────────────────────────────────────────

    [[nodiscard]] std::expected<QueryResult, std::string> plan_tips(const FindQuery& q) const;

    [[nodiscard]] std::expected<QueryResult, std::string> plan_repeats(const FindQuery& q) const;

    [[nodiscard]] std::expected<QueryResult, std::string> plan_bubbles(const FindQuery& q) const;

    [[nodiscard]] std::expected<QueryResult, std::string> plan_segments(const FindQuery& q) const;

    // ── avaliação de WHERE ────────────────────────────────────────────────────

    [[nodiscard]] static bool eval_condition(const ConditionExpr& cond,
                                             const RowFields& row) noexcept;

    [[nodiscard]] static bool eval_where(const WhereClause& wc, const RowFields& row) noexcept;

    // ── helpers ───────────────────────────────────────────────────────────────

    [[nodiscard]] static bool compare_values(const Value& lhs, Operator op,
                                             const Value& rhs) noexcept;

    [[nodiscard]] static QueryResult project(std::vector<RowFields> rows,
                                             const std::vector<std::string>& return_fields,
                                             const std::vector<std::string>& all_columns);

    // ── estado ────────────────────────────────────────────────────────────────

    const GfaCsrTopology* topology_ = nullptr;
    sqlite3* db_ = nullptr;
};

// ═══════════════════════════════════════════════════════════════════════════════
// Implementações inline
// ═══════════════════════════════════════════════════════════════════════════════

namespace detail {

[[nodiscard]] inline std::string value_to_string(const Value& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, int64_t>)
                return std::to_string(x);
            else
                return x;
        },
        v);
}

/// Mapeamento de nome de campo GGQL → coluna SQLite (vazio = campo inválido).
[[nodiscard]] inline std::string sql_column_for(std::string_view field) noexcept {
    if (field == "id")
        return "internal_id";
    if (field == "name")
        return "original_id";
    if (field == "length")
        return "length";
    if (field == "read_count")
        return "read_count";
    if (field == "depth")
        return "depth";
    return {};
}

/// Verifica se todas as condições WHERE são traduzíveis para SQL.
[[nodiscard]] inline bool all_sql_translatable(const WhereClause& wc) noexcept {
    for (const auto& cond : wc.conditions) {
        if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
            if (sql_column_for(cc->field).empty())
                return false;
        } else {
            return false; // CrossesCondition / ContainsKmerCondition
        }
    }
    return true;
}

const std::vector<std::string> kNodeColumns = {"id", "in_degree", "out_degree"};
const std::vector<std::string> kBubbleColumns = {"source", "sink", "branch_count"};
const std::vector<std::string> kSegmentColumns = {"id", "name", "length", "read_count", "depth"};

} // namespace detail

// ── QueryResult::format_table ────────────────────────────────────────────────

inline std::string QueryResult::format_table() const {
    if (rows.empty())
        return "(0 linhas)\n";
    if (columns.empty())
        return "";

    // 1. Calcula largura máxima de cada coluna
    std::vector<size_t> widths;
    widths.reserve(columns.size());
    for (const auto& col : columns)
        widths.push_back(col.size());

    for (const auto& row : rows) {
        for (size_t i = 0; i < columns.size(); ++i) {
            auto it = row.find(columns[i]);
            if (it != row.end())
                widths[i] = std::max(widths[i], detail::value_to_string(it->second).size());
        }
    }

    std::ostringstream out;

    // 2. Cabeçalho
    out << ' ';
    for (size_t i = 0; i < columns.size(); ++i) {
        out << std::left << std::setw(static_cast<int>(widths[i])) << columns[i];
        if (i + 1 < columns.size())
            out << " | ";
    }
    out << '\n';

    // 3. Linha separadora
    out << '-';
    for (size_t i = 0; i < columns.size(); ++i) {
        out << std::string(widths[i], '-');
        if (i + 1 < columns.size())
            out << "-+-";
    }
    out << '\n';

    // 4. Linhas de dados
    for (const auto& row : rows) {
        out << ' ';
        for (size_t i = 0; i < columns.size(); ++i) {
            auto it = row.find(columns[i]);
            const std::string val = it != row.end() ? detail::value_to_string(it->second) : "";
            out << std::left << std::setw(static_cast<int>(widths[i])) << val;
            if (i + 1 < columns.size())
                out << " | ";
        }
        out << '\n';
    }
    out << '(' << rows.size() << " linha" << (rows.size() == 1 ? "" : "s") << ")\n";

    return out.str();
}

// ── Construtor / Destrutor ────────────────────────────────────────────────────

inline QueryPlanner::QueryPlanner(const GfaCsrTopology* topology, std::string_view db_path)
    : topology_(topology) {
    if (!db_path.empty()) {
        const std::string path{db_path};
        const int rc = ::sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) {
            // Falha ao abrir → db_ permanece null; plan_segments() retornará erro
            if (db_) {
                ::sqlite3_close(db_);
                db_ = nullptr;
            }
        }
    }
}

inline QueryPlanner::~QueryPlanner() {
    if (db_) {
        ::sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ── execute ───────────────────────────────────────────────────────────────────

inline std::expected<QueryResult, std::string> QueryPlanner::execute(const GgqlQuery& query) const {
    return std::visit(
        [this](const auto& q) -> std::expected<QueryResult, std::string> {
            using T = std::decay_t<decltype(q)>;
            if constexpr (std::is_same_v<T, FindQuery>)
                return execute_find(q);
            else
                return execute_search_kmer(q);
        },
        query);
}

inline std::expected<QueryResult, std::string>
QueryPlanner::execute_find(const FindQuery& q) const {
    switch (q.entity) {
        case EntityType::TIP:
            return plan_tips(q);
        case EntityType::REPEAT:
            return plan_repeats(q);
        case EntityType::BUBBLE:
            return plan_bubbles(q);
        case EntityType::SEGMENT:
            return plan_segments(q);
        case EntityType::EDGE:
            return std::unexpected{"FIND EDGE ainda não implementado (Phase 2)"};
        case EntityType::PATH:
            return std::unexpected{"FIND PATH ainda não implementado (Phase 2)"};
    }
    return std::unexpected{"entidade desconhecida"};
}

inline std::expected<QueryResult, std::string>
QueryPlanner::execute_search_kmer(const SearchKmerQuery&) const {
    return std::unexpected{"SEARCH KMER requer FM-Index (não implementado — Phase 2)"};
}

// ── plan_tips ─────────────────────────────────────────────────────────────────

inline std::expected<QueryResult, std::string> QueryPlanner::plan_tips(const FindQuery& q) const {
    if (!topology_)
        return std::unexpected{"FIND TIP requer GfaCsrTopology"};

    const auto node_ids = topology_->find_tips();
    std::vector<RowFields> rows;
    rows.reserve(node_ids.size());

    for (const uint64_t nid : node_ids) {
        RowFields row;
        row["id"] = static_cast<int64_t>(nid);
        row["in_degree"] = static_cast<int64_t>(topology_->in_degree(nid));
        row["out_degree"] = static_cast<int64_t>(topology_->out_degree(nid));

        if (q.where && !eval_where(*q.where, row))
            continue;
        rows.push_back(std::move(row));
    }

    return project(std::move(rows), q.return_fields, detail::kNodeColumns);
}

// ── plan_repeats ──────────────────────────────────────────────────────────────

inline std::expected<QueryResult, std::string>
QueryPlanner::plan_repeats(const FindQuery& q) const {
    if (!topology_)
        return std::unexpected{"FIND REPEAT requer GfaCsrTopology"};

    // TODO Phase 2: extraia min_degree do WHERE (ex: out_degree >= N)
    //              e passe para find_repeats(N) para early-pruning.
    const auto node_ids = topology_->find_repeats(2);
    std::vector<RowFields> rows;
    rows.reserve(node_ids.size());

    for (const uint64_t nid : node_ids) {
        RowFields row;
        row["id"] = static_cast<int64_t>(nid);
        row["in_degree"] = static_cast<int64_t>(topology_->in_degree(nid));
        row["out_degree"] = static_cast<int64_t>(topology_->out_degree(nid));

        if (q.where && !eval_where(*q.where, row))
            continue;
        rows.push_back(std::move(row));
    }

    return project(std::move(rows), q.return_fields, detail::kNodeColumns);
}

// ── plan_bubbles ──────────────────────────────────────────────────────────────

inline std::expected<QueryResult, std::string>
QueryPlanner::plan_bubbles(const FindQuery& q) const {
    if (!topology_)
        return std::unexpected{"FIND BUBBLE requer GfaCsrTopology"};

    const auto bubbles = topology_->find_bubbles();
    std::vector<RowFields> rows;
    rows.reserve(bubbles.size());

    for (const auto& b : bubbles) {
        RowFields row;
        row["source"] = static_cast<int64_t>(b.source);
        row["sink"] = static_cast<int64_t>(b.sink);
        row["branch_count"] = static_cast<int64_t>(b.branches.size());

        if (q.where && !eval_where(*q.where, row))
            continue;
        rows.push_back(std::move(row));
    }

    return project(std::move(rows), q.return_fields, detail::kBubbleColumns);
}

// ── plan_segments ─────────────────────────────────────────────────────────────

inline std::expected<QueryResult, std::string>
QueryPlanner::plan_segments(const FindQuery& q) const {
    if (!db_)
        return std::unexpected{"FIND SEGMENT requer banco SQLite (passe db_path ao QueryPlanner)"};

    // ── 1. Colunas a selecionar (respeitando RETURN) ───────────────────────────

    const auto& rf = q.return_fields;
    const auto& sel_ggql = rf.empty() ? detail::kSegmentColumns : rf;

    // Mapeia nomes GGQL → SQL; campos inválidos são silenciosamente ignorados
    std::vector<std::pair<std::string, std::string>> sel_cols; // {ggql, sql}
    for (const auto& f : sel_ggql) {
        const auto sql_col = detail::sql_column_for(f);
        if (!sql_col.empty())
            sel_cols.push_back({f, sql_col});
    }
    if (sel_cols.empty())
        return std::unexpected{"Nenhum campo SEGMENT válido no RETURN"};

    // ── 2. Monta SELECT ───────────────────────────────────────────────────────

    std::string sql = "SELECT ";
    for (size_t i = 0; i < sel_cols.size(); ++i) {
        sql += sel_cols[i].second;
        if (i + 1 < sel_cols.size())
            sql += ", ";
    }
    sql += " FROM segments";

    // ── 3. Monta WHERE (pushdown vs. memória) ─────────────────────────────────
    //
    // Estratégia: se TODAS as condições são CompareCondition em campos
    // conhecidos → tradução completa para SQL com parentesização correta.
    // Caso contrário → varredura completa + filtragem em memória.

    std::vector<const CompareCondition*> bind_params; // na ordem dos '?'
    bool memory_filter = false;

    if (q.where && !q.where->conditions.empty()) {
        if (detail::all_sql_translatable(*q.where)) {
            // ── Tradução completa (garante semântica esquerda-para-direita) ──
            // Gera expressão com parênteses explícitos:
            //   cond1 AND cond2 OR cond3 → ((cond1 AND cond2) OR cond3)
            std::string expr;

            {
                const auto* cc0 = std::get_if<CompareCondition>(&q.where->conditions[0]);
                expr = detail::sql_column_for(cc0->field) + " " +
                       std::string{op_to_string(cc0->op)} + " ?";
                bind_params.push_back(cc0);
            }

            for (size_t i = 0; i < q.where->connectors.size(); ++i) {
                const auto* ccN = std::get_if<CompareCondition>(&q.where->conditions[i + 1]);
                const std::string rhs = detail::sql_column_for(ccN->field) + " " +
                                        std::string{op_to_string(ccN->op)} + " ?";
                const char* logic = q.where->connectors[i] == LogicalOp::AND ? " AND " : " OR ";
                expr = "(" + expr + logic + rhs + ")";
                bind_params.push_back(ccN);
            }

            sql += " WHERE " + expr;
        } else {
            // ── Fallback: varredura completa + filtro em memória ─────────────
            // TODO Phase 2: pushdown parcial para condições AND-only mistas.
            memory_filter = true;
        }
    }

    // ── 4. Prepara statement ──────────────────────────────────────────────────

    sqlite3_stmt* stmt = nullptr;
    int rc = ::sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected{std::string{"sqlite3_prepare: "} + ::sqlite3_errmsg(db_)};
    }

    // ── 5. Bind dos parâmetros (somente no caminho de pushdown) ───────────────

    for (int idx = 0; idx < static_cast<int>(bind_params.size()); ++idx) {
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, int64_t>)
                    ::sqlite3_bind_int64(stmt, idx + 1, v);
                else
                    ::sqlite3_bind_text(stmt, idx + 1, v.c_str(), -1, SQLITE_STATIC);
            },
            bind_params[static_cast<size_t>(idx)]->value);
    }

    // ── 6. Coleta linhas ──────────────────────────────────────────────────────

    std::vector<RowFields> rows;
    const int total_sql_cols = static_cast<int>(sel_cols.size());

    while ((rc = ::sqlite3_step(stmt)) == SQLITE_ROW) {
        RowFields row;
        for (int col = 0; col < total_sql_cols; ++col) {
            const auto& [ggql_name, _] = sel_cols[static_cast<size_t>(col)];
            const int col_type = ::sqlite3_column_type(stmt, col);

            if (col_type == SQLITE_INTEGER) {
                row[ggql_name] = static_cast<int64_t>(::sqlite3_column_int64(stmt, col));
            } else if (col_type == SQLITE_FLOAT) {
                // depth é REAL; representa como string para caber em Value
                row[ggql_name] = std::to_string(::sqlite3_column_double(stmt, col));
            } else if (col_type == SQLITE_TEXT) {
                const auto* txt = reinterpret_cast<const char*>(::sqlite3_column_text(stmt, col));
                row[ggql_name] = txt ? std::string{txt} : std::string{};
            } else {
                row[ggql_name] = std::string{"NULL"};
            }
        }

        // No caminho de memória, aplica WHERE completo
        if (memory_filter && q.where && !eval_where(*q.where, row))
            continue;

        rows.push_back(std::move(row));
    }

    ::sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return std::unexpected{std::string{"sqlite3_step: "} + ::sqlite3_errmsg(db_)};
    }

    // Colunas de resultado na ordem de RETURN (ou ordem padrão)
    std::vector<std::string> result_cols;
    result_cols.reserve(sel_cols.size());
    for (const auto& [g, _] : sel_cols)
        result_cols.push_back(g);

    return QueryResult{std::move(result_cols), std::move(rows)};
}

// ── eval_condition ────────────────────────────────────────────────────────────

inline bool QueryPlanner::eval_condition(const ConditionExpr& cond, const RowFields& row) noexcept {
    return std::visit(
        [&](const auto& c) -> bool {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, CompareCondition>) {
                auto it = row.find(c.field);
                // Campo ausente nesta entidade → predicado não aplicável → false
                if (it == row.end())
                    return false;
                return compare_values(it->second, c.op, c.value);

            } else if constexpr (std::is_same_v<T, CrossesCondition>) {
                // Requer dados de caminho — não disponível para entidades topológicas
                // TODO Phase 2: integrar PathIndex
                return false;

            } else {
                // ContainsKmerCondition: requer FM-Index
                // TODO Phase 2: integrar FM-Index
                return false;
            }
        },
        cond);
}

// ── eval_where ────────────────────────────────────────────────────────────────

inline bool QueryPlanner::eval_where(const WhereClause& wc, const RowFields& row) noexcept {
    if (wc.conditions.empty())
        return true;

    bool result = eval_condition(wc.conditions[0], row);

    // Avaliação esquerda-para-direita (sem precedência de operadores)
    for (size_t i = 0; i < wc.connectors.size(); ++i) {
        const bool rhs = eval_condition(wc.conditions[i + 1], row);
        if (wc.connectors[i] == LogicalOp::AND)
            result &= rhs;
        else
            result |= rhs;
    }

    return result;
}

// ── compare_values ────────────────────────────────────────────────────────────

inline bool QueryPlanner::compare_values(const Value& lhs, Operator op, const Value& rhs) noexcept {
    // Caso 1: ambos int64_t — comparação numérica
    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
        const int64_t l = std::get<int64_t>(lhs);
        const int64_t r = std::get<int64_t>(rhs);
        switch (op) {
            case Operator::EQ:
                return l == r;
            case Operator::NEQ:
                return l != r;
            case Operator::GT:
                return l > r;
            case Operator::LT:
                return l < r;
            case Operator::GEQ:
                return l >= r;
            case Operator::LEQ:
                return l <= r;
        }
    }

    // Caso 2: ambos string — comparação lexicográfica
    if (std::holds_alternative<std::string>(lhs) && std::holds_alternative<std::string>(rhs)) {
        const auto& l = std::get<std::string>(lhs);
        const auto& r = std::get<std::string>(rhs);
        switch (op) {
            case Operator::EQ:
                return l == r;
            case Operator::NEQ:
                return l != r;
            case Operator::GT:
                return l > r;
            case Operator::LT:
                return l < r;
            case Operator::GEQ:
                return l >= r;
            case Operator::LEQ:
                return l <= r;
        }
    }

    // Caso 3: int64_t do lado esquerdo, string do direito
    // (típico para campos como in_degree comparados a literais inteiros no AST)
    if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<std::string>(rhs)) {
        const auto& s = std::get<std::string>(rhs);
        int64_t r = 0;
        const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), r);
        if (ec != std::errc{} || ptr != s.data() + s.size())
            return false; // string não é inteiro válido
        const int64_t l = std::get<int64_t>(lhs);
        switch (op) {
            case Operator::EQ:
                return l == r;
            case Operator::NEQ:
                return l != r;
            case Operator::GT:
                return l > r;
            case Operator::LT:
                return l < r;
            case Operator::GEQ:
                return l >= r;
            case Operator::LEQ:
                return l <= r;
        }
    }

    // Tipos incompatíveis (string vs. int sem conversão possível)
    return op == Operator::NEQ;
}

// ── project ───────────────────────────────────────────────────────────────────

inline QueryResult QueryPlanner::project(std::vector<RowFields> rows,
                                         const std::vector<std::string>& return_fields,
                                         const std::vector<std::string>& all_columns) {
    const auto& cols = return_fields.empty() ? all_columns : return_fields;
    return QueryResult{cols, std::move(rows)};
}

} // namespace ggql
