// query_planner.cpp — Backends de execução do QueryPlanner.
//
// Contém: construtor, execute_*, plan_*, format_table e helpers detail::.
// Avaliação de condições WHERE: ver query_evaluator.cpp.
//
// Layout de QueryResult (colunar row-major):
//   flat[row * ncols + col]  →  valor da célula (row, col)
//   Elimina um unordered_map por linha — ganho O(N) para datasets > 100K nós.

#include "ggql/query/query_planner.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace ggql {

// ── Constantes de paginação ───────────────────────────────────────────────────
// kSafetyHardLimit: aborta queries sem LIMIT explícito antes de alocar memória ilimitada.
// kMaxTotalHitsCount: para de incrementar total_hits para evitar varreduras de ~370ms no HPRC.
static constexpr uint32_t kSafetyHardLimit   = 100'000;
static constexpr size_t   kMaxTotalHitsCount = 1'000'000;

// ── helpers internos ─────────────────────────────────────────────────────────

namespace detail {

static constexpr std::array<std::string_view, 3> kNodeColumns    = {"id", "in_degree", "out_degree"};
static constexpr std::array<std::string_view, 5> kBubbleColumns  = {
    "source", "sink", "branch_count", "internal_node_count", "has_cycles"};
static constexpr std::array<std::string_view, 5> kSegmentColumns = {"id", "name", "length",
                                                                     "read_count", "depth"};
static constexpr std::array<std::string_view, 3> kEdgeColumns    = {"from_id", "to_id", "to_orient"};
static constexpr std::array<std::string_view, 4> kPathColumns    = {"id", "name", "node_count", "length"};

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

// Campos string conhecidos — aceitam valores não-numéricos em condições WHERE.
static constexpr std::array<std::string_view, 2> kStringFields = {"name", "to_orient"};

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

    auto is_string_field = [](std::string_view field) {
        return std::ranges::any_of(kStringFields, [&](auto f) { return field == f; });
    };

    if (q.where) {
        for (const auto& cond : q.where->conditions) {
            if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
                bool found = std::ranges::any_of(valid_columns,
                    [&](std::string_view col) { return cc->field == col; });
                if (!found) {
                    return "Erro de execução: campo desconhecido";
                }
                if (!is_string_field(cc->field) && !is_numeric_value(cc->value)) {
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

// Resolve lista de campos de retorno: se vazia, expande para todos.
[[nodiscard]] static std::vector<std::string>
resolve_return_fields(const std::vector<std::string>& req,
                      std::span<const std::string_view> all_cols) {
    if (req.empty()) {
        std::vector<std::string> cols;
        cols.reserve(all_cols.size());
        for (auto sv : all_cols) { cols.emplace_back(sv); }
        return cols;
    }
    return req;
}

// Tenta extrair um intervalo [lo, hi] de comprimento de uma WhereClause.
// Retorna nullopt se o WHERE tiver OR ou nenhuma condição sobre "length".
// O índice secundário só é seguro quando todos os conectores são AND —
// um OR poderia qualificar nós fora do range de comprimento.
[[nodiscard]] static std::optional<std::pair<uint32_t, uint32_t>>
extract_length_range(const WhereClause& wc) noexcept {
    for (auto op : wc.connectors) {
        if (op == LogicalOp::OR) { return std::nullopt; }
    }

    uint32_t lo = 0;
    uint32_t hi = std::numeric_limits<uint32_t>::max();
    bool found = false;

    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CompareCondition>(&cond);
        if (!cc || cc->field != "length") { continue; }

        // Converte Value para uint32_t; ignora condições com string não-numérica
        int64_t v = 0;
        if (const auto* i = std::get_if<int64_t>(&cc->value)) {
            v = *i;
        } else if (const auto* d = std::get_if<double>(&cc->value)) {
            v = static_cast<int64_t>(*d);
        } else {
            continue;
        }

        const uint32_t uv = (v <= 0) ? 0u
                          : (v > int64_t(std::numeric_limits<uint32_t>::max()))
                            ? std::numeric_limits<uint32_t>::max()
                            : static_cast<uint32_t>(v);

        switch (cc->op) {
            case Operator::EQ:
                lo = std::max(lo, uv);
                hi = std::min(hi, uv);
                found = true;
                break;
            case Operator::GT:
                if (uv == std::numeric_limits<uint32_t>::max()) { return std::nullopt; }
                lo = std::max(lo, uv + 1u);
                found = true;
                break;
            case Operator::GEQ:
                lo = std::max(lo, uv);
                found = true;
                break;
            case Operator::LT:
                if (uv == 0) { return std::nullopt; } // length < 0 — impossível
                hi = std::min(hi, uv - 1u);
                found = true;
                break;
            case Operator::LEQ:
                hi = std::min(hi, uv);
                found = true;
                break;
            case Operator::NEQ:
                break; // != não reduz o range de forma simples
        }
    }

    if (!found || lo > hi) { return std::nullopt; }
    return std::make_pair(lo, hi);
}

// Tenta extrair um intervalo [lo, hi] de depth de uma WhereClause.
// Retorna nullopt se o WHERE tiver OR ou nenhuma condição sobre "depth".
[[nodiscard]] static std::optional<std::pair<float, float>>
extract_depth_range(const WhereClause& wc) noexcept {
    for (auto op : wc.connectors) {
        if (op == LogicalOp::OR) { return std::nullopt; }
    }

    float lo = 0.f;
    float hi = std::numeric_limits<float>::max();
    bool found = false;

    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CompareCondition>(&cond);
        if (!cc || cc->field != "depth") { continue; }

        float fv = 0.f;
        if (const auto* i = std::get_if<int64_t>(&cc->value)) {
            fv = static_cast<float>(*i);
        } else if (const auto* d = std::get_if<double>(&cc->value)) {
            fv = static_cast<float>(*d);
        } else {
            continue;
        }

        switch (cc->op) {
            case Operator::EQ:
                lo = std::max(lo, fv);
                hi = std::min(hi, fv);
                found = true;
                break;
            case Operator::GT:
                lo = std::max(lo, std::nextafter(fv, std::numeric_limits<float>::max()));
                found = true;
                break;
            case Operator::GEQ:
                lo = std::max(lo, fv);
                found = true;
                break;
            case Operator::LT:
                hi = std::min(hi, std::nextafter(fv, -std::numeric_limits<float>::max()));
                found = true;
                break;
            case Operator::LEQ:
                hi = std::min(hi, fv);
                found = true;
                break;
            case Operator::NEQ:
                break;
        }
    }

    if (!found || lo > hi) { return std::nullopt; }
    return std::make_pair(lo, hi);
}

// Retorna o valor exato de uma condição "name = X" em contexto AND-only.
// Retorna nullopt se houver OR, se não houver condição EQ sobre "name",
// ou se o predicado for NEQ/GT/etc.
[[nodiscard]] static std::optional<std::string_view>
extract_name_eq(const WhereClause& wc) noexcept {
    for (auto op : wc.connectors) {
        if (op == LogicalOp::OR) { return std::nullopt; }
    }
    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CompareCondition>(&cond);
        if (!cc || cc->field != "name" || cc->op != Operator::EQ) { continue; }
        const auto* s = std::get_if<std::string>(&cc->value);
        if (s) { return std::string_view{*s}; }
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
    if (row_count == 0) { return "(0 linhas)\n"; }
    if (columns.empty())  { return ""; }

    const size_t ncols = columns.size();

    // Calcula larguras máximas por coluna
    std::vector<size_t> widths(ncols);
    for (size_t c = 0; c < ncols; ++c) { widths[c] = columns[c].size(); }
    for (size_t r = 0; r < row_count; ++r) {
        for (size_t c = 0; c < ncols; ++c) {
            widths[c] = std::max(widths[c],
                detail::value_to_string(flat[r * ncols + c]).size());
        }
    }

    std::string out;
    out.reserve(64 * (row_count + 2));

    // Cabeçalho
    out += ' ';
    for (size_t c = 0; c < ncols; ++c) {
        out += std::format("{:<{}}", columns[c], widths[c]);
        if (c + 1 < ncols) { out += " | "; }
    }
    out += '\n';

    // Separador
    out += '-';
    for (size_t c = 0; c < ncols; ++c) {
        out.append(widths[c], '-');
        if (c + 1 < ncols) { out += "-+-"; }
    }
    out += '\n';

    // Linhas
    for (size_t r = 0; r < row_count; ++r) {
        out += ' ';
        for (size_t c = 0; c < ncols; ++c) {
            out += std::format("{:<{}}",
                detail::value_to_string(flat[r * ncols + c]), widths[c]);
            if (c + 1 < ncols) { out += " | "; }
        }
        out += '\n';
    }

    // Resumo de contagem
    auto format_num = [](size_t v) {
        std::string s = std::to_string(v);
        int pos = static_cast<int>(s.length()) - 3;
        while (pos > 0) { s.insert(pos, "."); pos -= 3; }
        return s;
    };

    if (safety_limit_hit) {
        out += std::format("({} linhas mostradas — sem LIMIT explícito, limite de segurança atingido; "
                           "adicione LIMIT N ou um WHERE mais seletivo)\n",
                           format_num(row_count));
    } else if (total_hits_truncated) {
        out += std::format("({} de mais de {} resultados — contagem interrompida; "
                           "use WHERE mais seletivo ou LIMIT N)\n",
                           format_num(row_count), format_num(kMaxTotalHitsCount));
    } else if (total_hits > row_count) {
        out += std::format("({} de {} linhas)\n",
                           format_num(row_count), format_num(total_hits));
    } else {
        out += std::format("({} {})\n",
                           format_num(row_count), row_count == 1 ? "linha" : "linhas");
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
        case EntityType::EDGE:    return plan_edges(q);
        case EntityType::PATH:    return plan_paths(q);
    }
    return std::unexpected{"entidade desconhecida"};
}

// ── execute_search_kmer ───────────────────────────────────────────────────────

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

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kSegmentColumns);
    const size_t ncols = cols.size();

    const uint32_t take_n = q.limit.value_or(static_cast<uint32_t>(ids.size()));

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(ids.size(), size_t{take_n}) * ncols);

    for (uint64_t id : ids) {
        ++result.total_hits;
        if (result.row_count >= take_n) { continue; }

        const int64_t len    = metadata_->length(id);
        const int32_t rc_raw = metadata_->read_count(id);
        const float   dp_raw = metadata_->depth(id);

        for (const auto& col : cols) {
            if (col == "id")          { result.flat.emplace_back(int64_t(id)); }
            else if (col == "name")   { result.flat.emplace_back(std::string(metadata_->original_id(id))); }
            else if (col == "length") { result.flat.emplace_back(int64_t(len)); }
            else if (col == "read_count") {
                result.flat.push_back(rc_raw >= 0 ? Value{int64_t(rc_raw)} : Value{std::string{"NULL"}});
            }
            else if (col == "depth") {
                result.flat.push_back(dp_raw >= 0.f ? Value{double(dp_raw)} : Value{std::string{"NULL"}});
            }
        }
        ++result.row_count;
    }
    result.total_hits = ids.size();  // conjunto já é pequeno — contagem exata
    return result;
}

// ── plan_tips ─────────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_tips(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND TIP requer GfaCsrTopology"}; }

    const auto node_ids = topology_->find_tips();
    auto cols = detail::resolve_return_fields(q.return_fields, detail::kNodeColumns);
    const size_t ncols = cols.size();

    const uint32_t take_n      = q.limit.value_or(kSafetyHardLimit);
    const bool     has_explicit = q.limit.has_value();
    const uint32_t skip_n      = q.offset.value_or(0);
    uint32_t       pag_skipped = 0;

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(node_ids.size(), size_t{take_n}) * ncols);

    for (const uint64_t nid : node_ids) {
        const int64_t in_d  = static_cast<int64_t>(topology_->in_degree(nid));
        const int64_t out_d = static_cast<int64_t>(topology_->out_degree(nid));

        if (q.where) {
            RowFields row;
            row.insert("id",         int64_t(nid));
            row.insert("in_degree",  in_d);
            row.insert("out_degree", out_d);
            if (!eval_where(*q.where, row)) { continue; }
        }

        ++result.total_hits;
        if (result.total_hits > kMaxTotalHitsCount) { result.total_hits_truncated = true; break; }
        if (!has_explicit && result.total_hits > kSafetyHardLimit) { result.safety_limit_hit = true; break; }
        if (pag_skipped < skip_n) { ++pag_skipped; continue; }
        if (result.row_count >= take_n) { continue; }
        for (const auto& col : cols) {
            if (col == "id")              { result.flat.emplace_back(int64_t(nid)); }
            else if (col == "in_degree")  { result.flat.emplace_back(in_d); }
            else if (col == "out_degree") { result.flat.emplace_back(out_d); }
        }
        ++result.row_count;
    }
    return result;
}

// ── plan_repeats ──────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_repeats(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND REPEAT requer GfaCsrTopology"}; }

    const auto node_ids = topology_->find_repeats(2);
    auto cols = detail::resolve_return_fields(q.return_fields, detail::kNodeColumns);
    const size_t ncols = cols.size();

    const uint32_t take_n      = q.limit.value_or(kSafetyHardLimit);
    const bool     has_explicit = q.limit.has_value();
    const uint32_t skip_n      = q.offset.value_or(0);
    uint32_t       pag_skipped = 0;

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(node_ids.size(), size_t{take_n}) * ncols);

    for (const uint64_t nid : node_ids) {
        const int64_t in_d  = static_cast<int64_t>(topology_->in_degree(nid));
        const int64_t out_d = static_cast<int64_t>(topology_->out_degree(nid));

        if (q.where) {
            RowFields row;
            row.insert("id",         int64_t(nid));
            row.insert("in_degree",  in_d);
            row.insert("out_degree", out_d);
            if (!eval_where(*q.where, row)) { continue; }
        }

        ++result.total_hits;
        if (result.total_hits > kMaxTotalHitsCount) { result.total_hits_truncated = true; break; }
        if (!has_explicit && result.total_hits > kSafetyHardLimit) { result.safety_limit_hit = true; break; }
        if (pag_skipped < skip_n) { ++pag_skipped; continue; }
        if (result.row_count >= take_n) { continue; }
        for (const auto& col : cols) {
            if (col == "id")              { result.flat.emplace_back(int64_t(nid)); }
            else if (col == "in_degree")  { result.flat.emplace_back(in_d); }
            else if (col == "out_degree") { result.flat.emplace_back(out_d); }
        }
        ++result.row_count;
    }
    return result;
}

// ── plan_bubbles ──────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_bubbles(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kBubbleColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND BUBBLE requer GfaCsrTopology"}; }
    if (!topology_->has_snarls()) {
        return std::unexpected{
            "FIND BUBBLE: snarls.bin ausente — execute o ingest novamente para "
            "habilitar a query (snarls são pré-computados)."};
    }

    const auto snarls = topology_->snarls();
    auto cols = detail::resolve_return_fields(q.return_fields, detail::kBubbleColumns);
    const size_t ncols = cols.size();

    const uint32_t take_n      = q.limit.value_or(kSafetyHardLimit);
    const bool     has_explicit = q.limit.has_value();
    const uint32_t skip_n      = q.offset.value_or(0);
    uint32_t       pag_skipped = 0;

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(snarls.size(), size_t{take_n}) * ncols);

    for (const auto& s : snarls) {
        const int64_t src = static_cast<int64_t>(s.source);
        const int64_t snk = static_cast<int64_t>(s.sink);
        const int64_t bc  = static_cast<int64_t>(s.branch_count);
        const int64_t inc = static_cast<int64_t>(s.internal_node_count);
        const int64_t hc  = static_cast<int64_t>(s.has_cycles);

        if (q.where) {
            RowFields row;
            row.insert("source",              src);
            row.insert("sink",                snk);
            row.insert("branch_count",        bc);
            row.insert("internal_node_count", inc);
            row.insert("has_cycles",          hc);
            if (!eval_where(*q.where, row)) { continue; }
        }

        ++result.total_hits;
        if (result.total_hits > kMaxTotalHitsCount) { result.total_hits_truncated = true; break; }
        if (!has_explicit && result.total_hits > kSafetyHardLimit) { result.safety_limit_hit = true; break; }
        if (pag_skipped < skip_n) { ++pag_skipped; continue; }
        if (result.row_count >= take_n) { continue; }
        for (const auto& col : cols) {
            if (col == "source")                   { result.flat.emplace_back(src); }
            else if (col == "sink")                 { result.flat.emplace_back(snk); }
            else if (col == "branch_count")         { result.flat.emplace_back(bc);  }
            else if (col == "internal_node_count")  { result.flat.emplace_back(inc); }
            else if (col == "has_cycles")           { result.flat.emplace_back(hc);  }
        }
        ++result.row_count;
    }
    return result;
}

// ── plan_segments ─────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_segments(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kSegmentColumns)) {
        return std::unexpected{*err};
    }
    if (!metadata_) { return std::unexpected{"FIND SEGMENT requer GfaMetadataStore instanciado"}; }

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kSegmentColumns);
    const size_t ncols = cols.size();

    // KmerCache — pré-computa kmer → segment IDs para ContainsKmerCondition
    KmerCache kmer_cache;
    if (q.where && seq_index_) {
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

    // Analisa quais campos o WHERE realmente usa para evitar computar Values desnecessários.
    // Ex.: WHERE length > X não precisa de "depth" ou "read_count" — evita N alocações de
    // std::string{"NULL"} por linha em datasets sem essas tags (CHM13 não tem dp:f:).
    bool needs_name = std::ranges::any_of(cols, [](const auto& f) { return f == "name"; });
    bool where_depth = false, where_rc = false;
    if (q.where) {
        for (const auto& cond : q.where->conditions) {
            if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
                if (cc->field == "name")       needs_name  = true;
                if (cc->field == "depth")      where_depth = true;
                if (cc->field == "read_count") where_rc    = true;
            }
        }
    }

    // Campos opcionais no resultado (fora do loop — constantes por query)
    const bool ret_rc = std::ranges::any_of(cols, [](const auto& c) { return c == "read_count"; });
    const bool ret_dp = std::ranges::any_of(cols, [](const auto& c) { return c == "depth"; });

    const uint32_t take_n      = q.limit.value_or(kSafetyHardLimit);
    const bool     has_explicit = q.limit.has_value();
    const uint32_t skip_n      = q.offset.value_or(0);
    uint32_t       pag_skipped = 0;

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(metadata_->size(), size_t{take_n}) * ncols);

    // Lambda que avalia WHERE e empurra o row para o resultado.
    // Usado tanto no caminho rápido (índice) como no full-scan.
    // Flags safety_limit_hit/total_hits_truncated devem ser verificadas no outer loop.
    auto process_id = [&](size_t id) {
        const int64_t len = static_cast<int64_t>(metadata_->length(id));

        if (q.where) {
            const int32_t rc_raw = where_rc    ? metadata_->read_count(id) : -1;
            const float   dp_raw = where_depth ? metadata_->depth(id)      : -1.f;
            RowFields row;
            row.insert("id",     int64_t(id));
            row.insert("length", len);
            if (where_rc)    row.insert("read_count",
                rc_raw >= 0 ? Value{int64_t(rc_raw)} : Value{std::string{"NULL"}});
            if (where_depth) row.insert("depth",
                dp_raw >= 0.f ? Value{double(dp_raw)} : Value{std::string{"NULL"}});
            if (needs_name)  row.insert("name", std::string(metadata_->original_id(id)));
            if (!eval_where(*q.where, row, cache_ptr)) { return; }
        }

        ++result.total_hits;
        if (result.total_hits > kMaxTotalHitsCount) { result.total_hits_truncated = true; return; }
        if (!has_explicit && result.total_hits > kSafetyHardLimit) { result.safety_limit_hit = true; return; }
        if (pag_skipped < skip_n) { ++pag_skipped; return; }
        if (result.row_count >= take_n) { return; }

        const int32_t rc_out = ret_rc ? metadata_->read_count(id) : -1;
        const float   dp_out = ret_dp ? metadata_->depth(id)      : -1.f;
        for (const auto& col : cols) {
            if (col == "id")             { result.flat.emplace_back(int64_t(id)); }
            else if (col == "length")    { result.flat.emplace_back(len); }
            else if (col == "name")      { result.flat.emplace_back(std::string(metadata_->original_id(id))); }
            else if (col == "read_count"){ result.flat.push_back(rc_out >= 0 ? Value{int64_t(rc_out)} : Value{std::string{"NULL"}}); }
            else if (col == "depth")     { result.flat.push_back(dp_out >= 0.f ? Value{double(dp_out)} : Value{std::string{"NULL"}}); }
        }
        ++result.row_count;
    };

    auto should_stop = [&]() {
        return result.safety_limit_hit || result.total_hits_truncated;
    };

    // ── Caminho rápido 1: índice de nome (O(1) — mais seletivo) ─────────────
    // Usado quando WHERE tem "name = X" em contexto AND-only. O índice entrega
    // no máximo 1 candidato; os predicados restantes são avaliados por process_id.
    if (q.where && metadata_->has_name_index()) {
        if (auto name = detail::extract_name_eq(*q.where)) {
            if (auto id = metadata_->lookup_name(*name)) {
                process_id(*id);
            }
            return result;
        }
    }

    // ── Caminho rápido 2: índice de comprimento (O(log N + K)) ───────────────
    // Usado quando WHERE tem condições de length sem OR. Predicados restantes
    // (depth, name, kmer…) são avaliados por process_id sobre cada candidato.
    if (q.where && metadata_->has_length_index()) {
        if (auto range = detail::extract_length_range(*q.where)) {
            for (const uint32_t cid : metadata_->ids_in_length_range(range->first, range->second)) {
                process_id(cid);
                if (should_stop()) break;
            }
            return result;
        }
    }

    // ── Caminho rápido 3: índice de depth (O(log N + K)) ─────────────────────
    // Acionado quando não há range de length utilizável (ex.: WHERE depth >= X).
    // Nós sem depth (depth=-1) são excluídos do índice — NULL não satisfaz
    // predicados numéricos, então a exclusão é semanticamente correta.
    if (q.where && metadata_->has_depth_index()) {
        if (auto range = detail::extract_depth_range(*q.where)) {
            for (const uint32_t cid : metadata_->ids_in_depth_range(range->first, range->second)) {
                process_id(cid);
                if (should_stop()) break;
            }
            return result;
        }
    }

    // ── Full scan ─────────────────────────────────────────────────────────────
    for (size_t id = 0; id < metadata_->size() && !should_stop(); ++id) {
        process_id(id);
    }
    return result;
}

// ── plan_edges ────────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_edges(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kEdgeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND EDGE requer GfaCsrTopology"}; }

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kEdgeColumns);
    const size_t ncols = cols.size();

    // Pre-extrai filtro de orientação EQ para evitar std::string{char} por aresta.
    // orient_eq: o char esperado se o WHERE tiver "to_orient = X" em EQ.
    // where_only_orient: true quando to_orient é a única condição — não precisa
    //   construir RowFields para arestas que passaram na comparação de char.
    std::optional<char> orient_eq;
    bool where_only_orient = false;
    if (q.where) {
        size_t orient_cond_count = 0;
        for (const auto& cond : q.where->conditions) {
            if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
                if (cc->field == "to_orient" && cc->op == Operator::EQ) {
                    if (const auto* s = std::get_if<std::string>(&cc->value); s && !s->empty()) {
                        orient_eq = (*s)[0];
                    }
                    ++orient_cond_count;
                }
            }
        }
        where_only_orient = (orient_cond_count == q.where->conditions.size());
    }

    const uint32_t take_n      = q.limit.value_or(kSafetyHardLimit);
    const bool     has_explicit = q.limit.has_value();
    const uint32_t skip_n      = q.offset.value_or(0);
    uint32_t       pag_skipped = 0;

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(topology_->num_edges(), size_t{take_n}) * ncols);

    bool stop = false;
    for (uint64_t from = 0; from < topology_->num_nodes() && !stop; ++from) {
        for (const uint32_t enc : topology_->out_neighbors(from)) {
            const auto [to_id, to_orient] = topology_->decode(enc);

            // Comparação de char — sem alocação, elimina a maioria das arestas antes de
            // construir o RowFields.
            if (orient_eq && to_orient != *orient_eq) { continue; }

            if (q.where && !where_only_orient) {
                RowFields row;
                row.insert("from_id",   int64_t(from));
                row.insert("to_id",     int64_t(to_id));
                row.insert("to_orient", std::string{to_orient});
                if (!eval_where(*q.where, row)) { continue; }
            }

            ++result.total_hits;
            if (result.total_hits > kMaxTotalHitsCount) { result.total_hits_truncated = true; stop = true; break; }
            if (!has_explicit && result.total_hits > kSafetyHardLimit) { result.safety_limit_hit = true; stop = true; break; }
            if (pag_skipped < skip_n) { ++pag_skipped; continue; }
            if (result.row_count >= take_n) { continue; }
            for (const auto& col : cols) {
                if (col == "from_id")        { result.flat.emplace_back(int64_t(from)); }
                else if (col == "to_id")     { result.flat.emplace_back(int64_t(to_id)); }
                else if (col == "to_orient") { result.flat.emplace_back(std::string{to_orient}); }
            }
            ++result.row_count;
        }
    }
    return result;
}

// ── plan_paths ────────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_paths(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kPathColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) { return std::unexpected{"FIND PATH requer GfaCsrTopology"}; }

    const size_t count = topology_->path_count();
    if (count == 0) {
        return std::unexpected{
            "FIND PATH: nenhum path indexado — o GFA não contém linhas W ou P"};
    }

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kPathColumns);
    const size_t ncols = cols.size();

    const uint32_t take_n      = q.limit.value_or(kSafetyHardLimit);
    const bool     has_explicit = q.limit.has_value();
    const uint32_t skip_n      = q.offset.value_or(0);
    uint32_t       pag_skipped = 0;

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(count, size_t{take_n}) * ncols);

    const bool use_stats = topology_->has_path_stats();

    for (size_t pid = 0; pid < count; ++pid) {
        int64_t nc = 0;
        int64_t total_length = 0;

        if (use_stats) {
            nc           = static_cast<int64_t>(topology_->path_node_count(pid));
            total_length = topology_->path_bp_length(pid);
        } else {
            const auto nodes = topology_->path_nodes(pid);
            nc = static_cast<int64_t>(nodes.size());
            if (metadata_) {
                for (const uint64_t nid : nodes) {
                    total_length += static_cast<int64_t>(metadata_->length(nid));
                }
            }
        }

        const std::string_view pname = topology_->path_name(pid);

        if (q.where) {
            RowFields row;
            row.insert("id",         int64_t(pid));
            row.insert("name",       std::string(pname));
            row.insert("node_count", nc);
            row.insert("length",     total_length);
            if (!eval_where(*q.where, row)) { continue; }
        }

        ++result.total_hits;
        if (result.total_hits > kMaxTotalHitsCount) { result.total_hits_truncated = true; break; }
        if (!has_explicit && result.total_hits > kSafetyHardLimit) { result.safety_limit_hit = true; break; }
        if (pag_skipped < skip_n) { ++pag_skipped; continue; }
        if (result.row_count >= take_n) { continue; }
        for (const auto& col : cols) {
            if (col == "id")              { result.flat.emplace_back(int64_t(pid)); }
            else if (col == "name")       { result.flat.emplace_back(std::string(pname)); }
            else if (col == "node_count") { result.flat.emplace_back(nc); }
            else if (col == "length")     { result.flat.emplace_back(total_length); }
        }
        ++result.row_count;
    }
    return result;
}

} // namespace ggql
