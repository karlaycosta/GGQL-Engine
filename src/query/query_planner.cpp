// query_planner.cpp — Backends de execução do QueryPlanner.
//
// Contém: construtor, execute_*, plan_*, format_table, project
//         e helpers detail:: (value_to_string, validate_query, colunas).
//
// Avaliação de condições WHERE: ver query_evaluator.cpp.

#include "ggql/query/query_planner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ggql {

// ── helpers internos ─────────────────────────────────────────────────────────

namespace detail {

static constexpr std::array<std::string_view, 3> kNodeColumns    = {"id", "in_degree", "out_degree"};
static constexpr std::array<std::string_view, 3> kBubbleColumns  = {"source", "sink", "branch_count"};
static constexpr std::array<std::string_view, 5> kSegmentColumns = {"id", "name", "length",
                                                                     "read_count", "depth"};

[[nodiscard]] static std::string value_to_string(const Value& v) {
    return std::visit(
        [](const auto& x) -> std::string {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, int64_t>) {
                return std::format("{}", x);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::format("{:.6g}", x);
            } else {
                return x;
            }
        },
        v);
}

[[nodiscard]] static std::optional<std::string>
validate_query(const FindQuery& q, std::span<const std::string_view> valid_columns) noexcept {
    auto is_numeric_value = [](const Value& v) -> bool {
        if (std::holds_alternative<int64_t>(v)) { return true; }
        if (std::holds_alternative<double>(v))  { return true; }
        if (const auto* s = std::get_if<std::string>(&v)) {
            double r = 0;
            const auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), r);
            return (ec == std::errc{} && ptr == s->data() + s->size());
        }
        return false;
    };

    if (q.where) {
        for (const auto& cond : q.where->conditions) {
            if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
                bool found = std::ranges::any_of(valid_columns,
                    [&](std::string_view col) { return cc->field == col; });
                if (!found) {
                    return "Erro de execução: campo desconhecido";
                }
                if (cc->field != "name" && !is_numeric_value(cc->value)) {
                    return "Erro de execução: tipo de dado incompatível para o campo numérico";
                }
            }
        }
    }
    for (const auto& f : q.return_fields) {
        bool found = std::ranges::any_of(valid_columns,
            [&](std::string_view col) { return f == col; });
        if (!found) {
            return "Erro de execução: campo desconhecido";
        }
    }
    return std::nullopt;
}

} // namespace detail

// ── Construtor ────────────────────────────────────────────────────────────────

QueryPlanner::QueryPlanner(const GfaCsrTopology*   topology,
                           const GfaMetadataStore* metadata,
                           const GfaSequenceIndex* seq_index)
    : topology_(topology), metadata_(metadata), seq_index_(seq_index) {}

// ── QueryResult::format_table ────────────────────────────────────────────────

std::string QueryResult::format_table() const {
    if (rows.empty())    { return "(0 linhas)\n"; }
    if (columns.empty()) { return ""; }

    std::vector<size_t> widths;
    widths.reserve(columns.size());
    for (const auto& col : columns) { widths.push_back(col.size()); }
    for (const auto& row : rows) {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (auto it = row.find(columns[i]); it != row.end()) {
                widths[i] = std::max(widths[i], detail::value_to_string(it->second).size());
            }
        }
    }

    std::string out;
    out.reserve(64 * (rows.size() + 2));

    out += ' ';
    for (size_t i = 0; i < columns.size(); ++i) {
        out += std::format("{:<{}}", columns[i], widths[i]);
        if (i + 1 < columns.size()) { out += " | "; }
    }
    out += '\n';

    out += '-';
    for (size_t i = 0; i < columns.size(); ++i) {
        out.append(widths[i], '-');
        if (i + 1 < columns.size()) { out += "-+-"; }
    }
    out += '\n';

    for (const auto& row : rows) {
        out += ' ';
        for (size_t i = 0; i < columns.size(); ++i) {
            auto it = row.find(columns[i]);
            const std::string val = (it != row.end()) ? detail::value_to_string(it->second) : "";
            out += std::format("{:<{}}", val, widths[i]);
            if (i + 1 < columns.size()) { out += " | "; }
        }
        out += '\n';
    }

    auto format_num = [](size_t v) {
        std::string s = std::to_string(v);
        int pos = static_cast<int>(s.length()) - 3;
        while (pos > 0) { s.insert(pos, "."); pos -= 3; }
        return s;
    };

    if (total_hits > rows.size()) {
        out += std::format("({} de {} linhas)\n", format_num(rows.size()), format_num(total_hits));
    } else {
        out += std::format("({} {})\n", format_num(rows.size()),
                           rows.size() == 1 ? "linha" : "linhas");
    }
    return out;
}

// ── execute ───────────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::execute(const GgqlQuery& query) const {
    return std::visit(
        [this](const auto& q) -> std::expected<QueryResult, std::string> {
            using T = std::decay_t<decltype(q)>;
            if constexpr (std::is_same_v<T, FindQuery>) {
                return execute_find(q);
            } else {
                return execute_search_kmer(q);
            }
        },
        query);
}

std::expected<QueryResult, std::string> QueryPlanner::execute_find(const FindQuery& q) const {
    switch (q.entity) {
        case EntityType::TIP:     return plan_tips(q);
        case EntityType::REPEAT:  return plan_repeats(q);
        case EntityType::BUBBLE:  return plan_bubbles(q);
        case EntityType::SEGMENT: return plan_segments(q);
        case EntityType::EDGE:    return std::unexpected{"FIND EDGE ainda não implementado (Phase 2)"};
        case EntityType::PATH:    return std::unexpected{"FIND PATH ainda não implementado (Phase 2)"};
    }
    return std::unexpected{"entidade desconhecida"};
}

std::expected<QueryResult, std::string>
QueryPlanner::execute_search_kmer(const SearchKmerQuery& q) const {
    bool all_valid = std::ranges::all_of(q.return_fields, [](const std::string& f) {
        return std::ranges::any_of(detail::kSegmentColumns,
            [&](std::string_view col) { return f == col; });
    });
    if (!all_valid) {
        return std::unexpected{"Erro de execução: campo desconhecido"};
    }

    if (!seq_index_ || !metadata_) {
        return std::unexpected{"SEARCH KMER requer FM-Index e MetadataStore instanciados"};
    }

    auto offsets = seq_index_->search_kmer(q.kmer);
    std::vector<uint64_t> ids;
    ids.reserve(offsets.size());
    for (uint64_t pos : offsets) {
        if (auto id = metadata_->segment_id_from_offset(pos)) {
            ids.push_back(*id);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    std::vector<std::string> return_fields;
    if (q.return_fields.empty()) {
        for (const auto f : detail::kSegmentColumns) { return_fields.emplace_back(f); }
    } else {
        return_fields = q.return_fields;
    }

    std::vector<RowFields> rows;
    rows.reserve(ids.size());
    for (uint64_t id : ids) {
        RowFields row;
        row["id"]     = static_cast<int64_t>(id);
        row["name"]   = std::string(metadata_->original_id(id));
        row["length"] = static_cast<int64_t>(metadata_->length(id));
        const int32_t rc = metadata_->read_count(id);
        row["read_count"] = (rc >= 0) ? Value{static_cast<int64_t>(rc)} : Value{std::string{"NULL"}};
        const float dp    = metadata_->depth(id);
        row["depth"]      = (dp >= 0.f) ? Value{static_cast<double>(dp)} : Value{std::string{"NULL"}};

        RowFields proj;
        for (const auto& field : return_fields) { proj[field] = row[field]; }
        rows.push_back(std::move(proj));
    }
    return QueryResult{std::move(return_fields), std::move(rows)};
}

// ── plan_tips ─────────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_tips(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND TIP requer GfaCsrTopology"}; }

    const auto node_ids = topology_->find_tips();
    std::vector<RowFields> rows;
    rows.reserve(node_ids.size());

    size_t total_hits = 0;
    for (const uint64_t nid : node_ids) {
        RowFields row;
        row["id"]         = static_cast<int64_t>(nid);
        row["in_degree"]  = static_cast<int64_t>(topology_->in_degree(nid));
        row["out_degree"] = static_cast<int64_t>(topology_->out_degree(nid));
        if (q.where && !eval_where(*q.where, row)) { continue; }
        ++total_hits;
        if (rows.size() < 10000) { rows.push_back(std::move(row)); }
    }
    return project(std::move(rows), q.return_fields, detail::kNodeColumns, total_hits);
}

// ── plan_repeats ──────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_repeats(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND REPEAT requer GfaCsrTopology"}; }

    const auto node_ids = topology_->find_repeats(2);
    std::vector<RowFields> rows;
    rows.reserve(node_ids.size());

    size_t total_hits = 0;
    for (const uint64_t nid : node_ids) {
        RowFields row;
        row["id"]         = static_cast<int64_t>(nid);
        row["in_degree"]  = static_cast<int64_t>(topology_->in_degree(nid));
        row["out_degree"] = static_cast<int64_t>(topology_->out_degree(nid));
        if (q.where && !eval_where(*q.where, row)) { continue; }
        ++total_hits;
        if (rows.size() < 10000) { rows.push_back(std::move(row)); }
    }
    return project(std::move(rows), q.return_fields, detail::kNodeColumns, total_hits);
}

// ── plan_bubbles ──────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_bubbles(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kBubbleColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND BUBBLE requer GfaCsrTopology"}; }

    const auto bubbles = topology_->find_bubbles();
    std::vector<RowFields> rows;
    rows.reserve(bubbles.size());

    size_t total_hits = 0;
    for (const auto& b : bubbles) {
        RowFields row;
        row["source"]       = static_cast<int64_t>(b.source);
        row["sink"]         = static_cast<int64_t>(b.sink);
        row["branch_count"] = static_cast<int64_t>(b.branches.size());
        if (q.where && !eval_where(*q.where, row)) { continue; }
        ++total_hits;
        if (rows.size() < 10000) { rows.push_back(std::move(row)); }
    }
    return project(std::move(rows), q.return_fields, detail::kBubbleColumns, total_hits);
}

// ── plan_segments ─────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_segments(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kSegmentColumns)) {
        return std::unexpected{*err};
    }
    if (!metadata_) { return std::unexpected{"FIND SEGMENT requer GfaMetadataStore instanciado"}; }

    std::vector<std::string> return_fields;
    if (q.return_fields.empty()) {
        for (const auto f : detail::kSegmentColumns) { return_fields.emplace_back(f); }
    } else {
        return_fields = q.return_fields;
    }

    // Pré-computa cache kmer → segment IDs para ContainsKmerCondition (O(K) em vez de O(N×K))
    KmerCache kmer_cache;
    if (q.where && seq_index_ && metadata_) {
        for (const auto& cond : q.where->conditions) {
            if (const auto* ck = std::get_if<ContainsKmerCondition>(&cond)) {
                auto& ids_set = kmer_cache[ck->kmer];
                for (uint64_t pos : seq_index_->search_kmer(ck->kmer)) {
                    if (auto sid = metadata_->segment_id_from_offset(pos)) {
                        ids_set.insert(*sid);
                    }
                }
            }
        }
    }
    const KmerCache* cache_ptr = kmer_cache.empty() ? nullptr : &kmer_cache;

    std::vector<RowFields> rows;
    size_t total_hits = 0;

    for (size_t id = 0; id < metadata_->size(); ++id) {
        RowFields row;
        row["id"]     = static_cast<int64_t>(id);
        row["name"]   = std::string(metadata_->original_id(id));
        row["length"] = static_cast<int64_t>(metadata_->length(id));
        const int32_t rc = metadata_->read_count(id);
        row["read_count"] = (rc >= 0) ? Value{static_cast<int64_t>(rc)} : Value{std::string{"NULL"}};
        const float dp    = metadata_->depth(id);
        row["depth"]      = (dp >= 0.f) ? Value{static_cast<double>(dp)} : Value{std::string{"NULL"}};

        if (q.where && !eval_where(*q.where, row, cache_ptr)) { continue; }

        ++total_hits;
        if (rows.size() < 10000) {
            RowFields proj;
            for (const auto& field : return_fields) { proj[field] = row[field]; }
            rows.push_back(std::move(proj));
        }
    }
    return QueryResult{std::move(return_fields), std::move(rows), total_hits};
}

// ── project ───────────────────────────────────────────────────────────────────

QueryResult QueryPlanner::project(std::vector<RowFields> rows,
                                   const std::vector<std::string>& return_fields,
                                   std::span<const std::string_view> all_columns,
                                   size_t total_hits) {
    if (return_fields.empty()) {
        std::vector<std::string> cols(all_columns.begin(), all_columns.end());
        return QueryResult{std::move(cols), std::move(rows), total_hits};
    }
    return QueryResult{return_fields, std::move(rows), total_hits};
}

} // namespace ggql
