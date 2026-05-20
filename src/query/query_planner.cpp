// query_planner.cpp — Backends de execução do QueryPlanner.
//
// Contém: construtor, execute_*, plan_*, format_table e helpers detail::.
// Avaliação de condições WHERE: ver query_evaluator.cpp.
//
// Layout de QueryResult (colunar row-major):
//   flat[row * ncols + col]  →  valor da célula (row, col)
//   Elimina um unordered_map por linha — ganho O(N) para datasets > 100K nós.

#include "ggql/query/query_planner.hpp"

#include "ggql/ingest/metadata_store.hpp"
#include "ggql/query/entity_columns.hpp"
#include "ggql/query/query_context.hpp"
#include "ggql/storage/fm_index.hpp"
#include "ggql/storage/topology.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>      // std::getenv, std::strtoull (kMaxTotalHitsCount env override)
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace ggql {

// ── Constantes de paginação ───────────────────────────────────────────────────
// kSafetyHardLimit: aborta queries sem LIMIT explícito antes de alocar memória ilimitada.
// kMaxTotalHitsCount: para de incrementar total_hits para evitar varreduras de ~370ms no HPRC.
static constexpr uint32_t kSafetyHardLimit = 100'000;
static constexpr size_t kMaxTotalHitsCountDefault = 1'000'000;

// Cap dinâmico: env var `GGQL_MAX_TOTAL_HITS` sobrescreve o default.
// Útil para validação científica em datasets >1M (HPRC) onde o usuário
// quer counts globais exatos. Valores não-numéricos ou ≤0 caem no default.
// Avaliado uma vez por processo (function-local static) — caching trivial.
[[nodiscard]] static size_t kMaxTotalHitsCount() {
    // Round 3.G (2026-05-20): override per-engine via
    // ggql_engine_set_max_total_hits() vence sobre env var.
    if (const auto o = ggql::query::ctx::max_total_hits_override(); o > 0) {
        return o;
    }
    static const size_t cap = []() -> size_t {
        if (const char* env = std::getenv("GGQL_MAX_TOTAL_HITS"); env && *env) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(env, &end, 10);
            if (end != env && v > 0) {
                return static_cast<size_t>(v);
            }
        }
        return kMaxTotalHitsCountDefault;
    }();
    return cap;
}

// Round 3.F/G: storage thread_local + implementação dos getters/setters
// declarados em include/ggql/query/query_context.hpp.
namespace {
thread_local std::atomic<bool>* g_cancel_token = nullptr;
thread_local std::size_t g_max_total_hits_override = 0;
} // namespace

} // namespace ggql

namespace ggql::query::ctx {

void set_cancel_token(std::atomic<bool>* p) noexcept {
    ggql::g_cancel_token = p;
}

void set_max_total_hits_override(std::size_t cap) noexcept {
    ggql::g_max_total_hits_override = cap;
}

bool cancel_requested() noexcept {
    auto* p = ggql::g_cancel_token;
    return p && p->load(std::memory_order_relaxed);
}

std::size_t max_total_hits_override() noexcept {
    return ggql::g_max_total_hits_override;
}

ScopedContext::ScopedContext(std::atomic<bool>* cancel,
                             std::size_t max_hits_override) noexcept
    : prev_cancel_(ggql::g_cancel_token),
      prev_max_hits_(ggql::g_max_total_hits_override) {
    ggql::g_cancel_token = cancel;
    ggql::g_max_total_hits_override = max_hits_override;
}

ScopedContext::~ScopedContext() noexcept {
    ggql::g_cancel_token = prev_cancel_;
    ggql::g_max_total_hits_override = prev_max_hits_;
}

} // namespace ggql::query::ctx

namespace ggql {

// ── helpers internos ─────────────────────────────────────────────────────────

namespace detail {

// Refactor 2026-05-20: fonte única em include/ggql/query/entity_columns.hpp.
// Aliases preservados para minimizar churn no resto deste arquivo.
using ggql::query::kNodeColumns;
using ggql::query::kBubbleColumns;
using ggql::query::kSegmentColumns;
using ggql::query::kEdgeColumns;
using ggql::query::kPathColumns;

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

// Tamanho da representação textual sem alocar a string.
// Usado em format_table para calcular larguras sem N alocações.
[[nodiscard]] static size_t value_text_size(const Value& v) noexcept {
    return std::visit(
        [](const auto& x) -> size_t {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, int64_t>) {
                return std::formatted_size("{}", x);
            } else if constexpr (std::is_same_v<T, double>) {
                return std::formatted_size("{:.6g}", x);
            } else {
                return x.size();
            }
        },
        v);
}

// Campos string conhecidos — aceitam valores não-numéricos em condições WHERE.
static constexpr std::array<std::string_view, 2> kStringFields = {"name", "to_orient"};

[[nodiscard]] static std::optional<std::string>
validate_query(const FindQuery& q, std::span<const std::string_view> valid_columns) noexcept {
    auto is_numeric_value = [](const Value& v) -> bool {
        if (std::holds_alternative<int64_t>(v)) {
            return true;
        }
        if (std::holds_alternative<double>(v)) {
            return true;
        }
        if (const auto* s = std::get_if<std::string>(&v)) {
            double r = 0;
            const auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), r);
            return (ec == std::errc{} && ptr == s->data() + s->size());
        }
        return false;
    };

    auto is_string_field = [](std::string_view field) noexcept {
        return std::ranges::contains(kStringFields, field);
    };

    if (q.where) {
        for (const auto& cond : q.where->conditions) {
            if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
                if (!std::ranges::contains(valid_columns, std::string_view{cc->field})) {
                    return "Erro de execução: campo desconhecido";
                }
                if (!is_string_field(cc->field) && !is_numeric_value(cc->value)) {
                    return "Erro de execução: tipo de dado incompatível para o campo numérico";
                }
            }
        }
    }
    for (const auto& f : q.return_fields) {
        if (!std::ranges::contains(valid_columns, std::string_view{f})) {
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
        for (auto sv : all_cols) {
            cols.emplace_back(sv);
        }
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
        if (op == LogicalOp::OR) {
            return std::nullopt;
        }
    }

    uint32_t lo = 0;
    uint32_t hi = std::numeric_limits<uint32_t>::max();
    bool found = false;

    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CompareCondition>(&cond);
        if (!cc || cc->field != "length") {
            continue;
        }

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
                if (uv == std::numeric_limits<uint32_t>::max()) {
                    return std::nullopt;
                }
                lo = std::max(lo, uv + 1u);
                found = true;
                break;
            case Operator::GEQ:
                lo = std::max(lo, uv);
                found = true;
                break;
            case Operator::LT:
                if (uv == 0) {
                    return std::nullopt;
                } // length < 0 — impossível
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

    if (!found || lo > hi) {
        return std::nullopt;
    }
    return std::make_pair(lo, hi);
}

// Tenta extrair um intervalo [lo, hi] de depth de uma WhereClause.
// Retorna nullopt se o WHERE tiver OR ou nenhuma condição sobre "depth".
[[nodiscard]] static std::optional<std::pair<float, float>>
extract_depth_range(const WhereClause& wc) noexcept {
    for (auto op : wc.connectors) {
        if (op == LogicalOp::OR) {
            return std::nullopt;
        }
    }

    float lo = 0.f;
    float hi = std::numeric_limits<float>::max();
    bool found = false;

    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CompareCondition>(&cond);
        if (!cc || cc->field != "depth") {
            continue;
        }

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

    if (!found || lo > hi) {
        return std::nullopt;
    }
    return std::make_pair(lo, hi);
}

// Retorna o valor exato de uma condição "name = X" em contexto AND-only.
// Retorna nullopt se houver OR, se não houver condição EQ sobre "name",
// ou se o predicado for NEQ/GT/etc.
[[nodiscard]] static std::optional<std::string_view>
extract_name_eq(const WhereClause& wc) noexcept {
    for (auto op : wc.connectors) {
        if (op == LogicalOp::OR) {
            return std::nullopt;
        }
    }
    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CompareCondition>(&cond);
        if (!cc || cc->field != "name" || cc->op != Operator::EQ) {
            continue;
        }
        const auto* s = std::get_if<std::string>(&cc->value);
        if (s) {
            return std::string_view{*s};
        }
    }
    return std::nullopt;
}

// ── Paginator ───────────────────────────────────────────────────────────────
// Encapsula o pattern de paginação + safety/truncation que aparece em todos
// os plan_*. advance() incrementa total_hits e classifica a iteração atual:
//   Stop  — break do loop (kMaxTotalHitsCount ou kSafetyHardLimit excedidos)
//   Skip  — continue (offset ainda não satisfeito ou take_n já atingido)
//   Emit  — emitir a row para result.flat
struct Paginator {
    uint32_t take_n = 0;
    bool has_explicit = false;
    uint32_t skip_n = 0;
    uint32_t pag_skipped = 0;
    // #4 (2026-05-19): queries estruturais (segment/edge/path/tip/repeat/
    // bubble) iteram um domínio naturalmente bounded por #segs do grafo —
    // sem risco de DoS. O cap kMaxTotalHitsCount truncava o COUNT delas em
    // ~1M, retornando número ERRADO silenciosamente (ex.: chm13 4,29M →
    // 1000001 sem GGQL_MAX_TOTAL_HITS). Aqui o count pode varrer o domínio
    // inteiro. SEARCH KMER NÃO seta isto: itera ocorrências do FM-Index
    // (ilimitado, DoS-prone — proteção I-1/I-7 preservada).
    bool unbounded_count = false;

    enum class Decision : uint8_t { Stop, Skip, Emit };

    template <typename Q>
    [[nodiscard]] static Paginator from_query(const Q& q,
                                              bool unbounded_count = false) noexcept {
        Paginator p;
        p.take_n = q.limit.value_or(kSafetyHardLimit);
        p.has_explicit = q.limit.has_value();
        if constexpr (requires { q.offset; }) {
            p.skip_n = q.offset.value_or(0);
        }
        p.unbounded_count = unbounded_count;
        return p;
    }

    [[nodiscard]] Decision advance(QueryResult& result) noexcept {
        ++result.total_hits;
        // Round 3.F (2026-05-20): cancelamento cooperativo. Checa antes dos
        // caps — caller setou via ggql_engine_cancel; aborta com Stop.
        // Custo: 1 load thread_local + 1 atomic_load relaxed (~ns) por hit.
        if (ggql::query::ctx::cancel_requested()) {
            result.cancelled = true;
            return Decision::Stop;
        }
        if (!unbounded_count && result.total_hits > kMaxTotalHitsCount()) {
            result.total_hits_truncated = true;
            return Decision::Stop;
        }
        if (!has_explicit && result.total_hits > kSafetyHardLimit) {
            result.safety_limit_hit = true;
            return Decision::Stop;
        }
        if (pag_skipped < skip_n) {
            ++pag_skipped;
            return Decision::Skip;
        }
        if (result.row_count >= take_n) {
            return Decision::Skip;
        }
        return Decision::Emit;
    }
};

// ── Emitters de coluna pré-computados ───────────────────────────────────────
// Pré-computa, fora do hot loop, qual valor cada coluna deve emitir.
// Elimina N comparações de string (col == "...") por linha emitida.
enum class NodeCol    : uint8_t { Id, InDeg, OutDeg };
enum class BubbleCol  : uint8_t { Source, Sink, BranchCount, InternalCount, HasCycles };
enum class SegmentCol : uint8_t { Id, Name, Length, ReadCount, Depth };
enum class EdgeCol    : uint8_t { FromId, ToId, ToOrient };
enum class PathCol    : uint8_t { Id, Name, NodeCount, Length };

[[nodiscard]] inline std::vector<NodeCol>
map_node_cols(std::span<const std::string> cols) {
    std::vector<NodeCol> out;
    out.reserve(cols.size());
    for (const auto& c : cols) {
        if (c == "id") { out.push_back(NodeCol::Id); }
        else if (c == "in_degree") { out.push_back(NodeCol::InDeg); }
        else if (c == "out_degree") { out.push_back(NodeCol::OutDeg); }
    }
    return out;
}

[[nodiscard]] inline std::vector<BubbleCol>
map_bubble_cols(std::span<const std::string> cols) {
    std::vector<BubbleCol> out;
    out.reserve(cols.size());
    for (const auto& c : cols) {
        if (c == "source") { out.push_back(BubbleCol::Source); }
        else if (c == "sink") { out.push_back(BubbleCol::Sink); }
        else if (c == "branch_count") { out.push_back(BubbleCol::BranchCount); }
        else if (c == "internal_node_count") { out.push_back(BubbleCol::InternalCount); }
        else if (c == "has_cycles") { out.push_back(BubbleCol::HasCycles); }
    }
    return out;
}

[[nodiscard]] inline std::vector<SegmentCol>
map_segment_cols(std::span<const std::string> cols) {
    std::vector<SegmentCol> out;
    out.reserve(cols.size());
    for (const auto& c : cols) {
        if (c == "id") { out.push_back(SegmentCol::Id); }
        else if (c == "name") { out.push_back(SegmentCol::Name); }
        else if (c == "length") { out.push_back(SegmentCol::Length); }
        else if (c == "read_count") { out.push_back(SegmentCol::ReadCount); }
        else if (c == "depth") { out.push_back(SegmentCol::Depth); }
    }
    return out;
}

[[nodiscard]] inline std::vector<EdgeCol>
map_edge_cols(std::span<const std::string> cols) {
    std::vector<EdgeCol> out;
    out.reserve(cols.size());
    for (const auto& c : cols) {
        if (c == "from_id") { out.push_back(EdgeCol::FromId); }
        else if (c == "to_id") { out.push_back(EdgeCol::ToId); }
        else if (c == "to_orient") { out.push_back(EdgeCol::ToOrient); }
    }
    return out;
}

[[nodiscard]] inline std::vector<PathCol>
map_path_cols(std::span<const std::string> cols) {
    std::vector<PathCol> out;
    out.reserve(cols.size());
    for (const auto& c : cols) {
        if (c == "id") { out.push_back(PathCol::Id); }
        else if (c == "name") { out.push_back(PathCol::Name); }
        else if (c == "node_count") { out.push_back(PathCol::NodeCount); }
        else if (c == "length") { out.push_back(PathCol::Length); }
    }
    return out;
}

// Pré-computa CrossesCache para todos os `crosses(<path>)` no WHERE.
// Para cada path_name único, faz lookup linear em path_names_ e armazena o
// conjunto de nodes desse path. Path names inexistentes não são inseridos —
// eval_condition retorna false naturalmente. Custo:
//   • lookup nome→pid:    O(P × L) onde P=#paths, L=avg len do nome (~bytes)
//   • path_nodes(target): O(N) onde N=nodes do target (extract via gBWT)
//   • set construction:   O(N)
// Para HPRC (P~100, N~milhões): ~poucos ms — pago UMA vez por query.
static void build_crosses_cache(const WhereClause& wc, const GfaCsrTopology& topo,
                                CrossesCache& out) {
    // Lookup linear nome→pid. Aceitável para HPRC (P~100); virar
    // unordered_map<string,size_t> se P escalar para 10k+.
    auto find_path_id = [&](std::string_view name) -> std::optional<size_t> {
        const size_t n = topo.path_count();
        for (size_t i = 0; i < n; ++i) {
            if (topo.path_name(i) == name) { return i; }
        }
        return std::nullopt;
    };

    std::vector<uint64_t> nodes_buf;
    for (const auto& cond : wc.conditions) {
        const auto* cc = std::get_if<CrossesCondition>(&cond);
        if (!cc) { continue; }
        if (out.contains(cc->path_name)) { continue; } // dedup mesma cond repetida

        const auto target_pid = find_path_id(cc->path_name);
        if (!target_pid) { continue; } // path inexistente — não cacheia

        nodes_buf.clear();
        topo.path_nodes(*target_pid, nodes_buf);
        out.emplace(cc->path_name,
                    std::unordered_set<uint64_t>(nodes_buf.begin(), nodes_buf.end()));
    }
}

} // namespace detail

// ── Construtor ────────────────────────────────────────────────────────────────

QueryPlanner::QueryPlanner(const GfaCsrTopology* topology, const GfaMetadataStore* metadata,
                           const GfaSequenceIndex* seq_index) noexcept
    : topology_(topology), metadata_(metadata), seq_index_(seq_index) {}

// ── QueryResult::format_table ────────────────────────────────────────────────

std::string QueryResult::format_table() const {
    if (row_count == 0) {
        return "(0 linhas)\n";
    }
    if (columns.empty()) {
        return "";
    }

    const size_t ncols = columns.size();

    // Calcula larguras máximas por coluna
    std::vector<size_t> widths(ncols);
    for (size_t c = 0; c < ncols; ++c) {
        widths[c] = columns[c].size();
    }
    for (size_t r = 0; r < row_count; ++r) {
        for (size_t c = 0; c < ncols; ++c) {
            widths[c] = std::max(widths[c], detail::value_text_size(flat[r * ncols + c]));
        }
    }

    std::string out;
    out.reserve(64 * (row_count + 2));

    // Cabeçalho
    out += ' ';
    for (size_t c = 0; c < ncols; ++c) {
        out += std::format("{:<{}}", columns[c], widths[c]);
        if (c + 1 < ncols) {
            out += " | ";
        }
    }
    out += '\n';

    // Separador
    out += '-';
    for (size_t c = 0; c < ncols; ++c) {
        out.append(widths[c], '-');
        if (c + 1 < ncols) {
            out += "-+-";
        }
    }
    out += '\n';

    // Linhas
    for (size_t r = 0; r < row_count; ++r) {
        out += ' ';
        for (size_t c = 0; c < ncols; ++c) {
            out += std::format("{:<{}}", detail::value_to_string(flat[r * ncols + c]), widths[c]);
            if (c + 1 < ncols) {
                out += " | ";
            }
        }
        out += '\n';
    }

    // Resumo de contagem
    auto format_num = [](size_t v) {
        std::string s = std::to_string(v);
        int pos = static_cast<int>(s.length()) - 3;
        while (pos > 0) {
            s.insert(pos, ".");
            pos -= 3;
        }
        return s;
    };

    if (safety_limit_hit) {
        out +=
            std::format("({} linhas mostradas — sem LIMIT explícito, limite de segurança atingido; "
                        "adicione LIMIT N ou um WHERE mais seletivo)\n",
                        format_num(row_count));
    } else if (total_hits_truncated) {
        out += std::format("({} de mais de {} resultados — contagem interrompida; "
                           "use WHERE mais seletivo ou LIMIT N)\n",
                           format_num(row_count), format_num(kMaxTotalHitsCount()));
    } else if (total_hits > row_count) {
        out += std::format("({} de {} linhas)\n", format_num(row_count), format_num(total_hits));
    } else {
        out += std::format("({} {})\n", format_num(row_count), row_count == 1 ? "linha" : "linhas");
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
        case EntityType::TIP:
            return plan_tips(q);
        case EntityType::REPEAT:
            return plan_repeats(q);
        case EntityType::BUBBLE:
            return plan_bubbles(q);
        case EntityType::SEGMENT:
            return plan_segments(q);
        case EntityType::EDGE:
            return plan_edges(q);
        case EntityType::PATH:
            return plan_paths(q);
    }
    return std::unexpected{"entidade desconhecida"};
}

// ── execute_search_kmer ───────────────────────────────────────────────────────

std::expected<QueryResult, std::string>
QueryPlanner::execute_search_kmer(const SearchKmerQuery& q) const {
    bool all_valid = std::ranges::all_of(q.return_fields, [](const std::string& f) {
        return std::ranges::contains(detail::kSegmentColumns, std::string_view{f});
    });
    if (!all_valid) {
        return std::unexpected{"Erro de execução: campo desconhecido"};
    }

    if (!seq_index_ || !metadata_) {
        return std::unexpected{"SEARCH KMER requer FM-Index e MetadataStore instanciados"};
    }

    // I-3: rejeita IUPAC ambíguos (R/Y/M/K/B/D/H/V/W/S) e qualquer char
    // fora de {A,C,G,N,T}. O FM-Index silencia esses k-mers (0 hits),
    // confundindo bioinformatas acostumados com consenso ambíguo.
    // Mensagem clara > 0 hits silencioso.
    for (const char c : q.kmer) {
        const char up = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        if (up != 'A' && up != 'C' && up != 'G' && up != 'N' && up != 'T') {
            return std::unexpected{
                std::string("SEARCH KMER: caractere '") + c +
                "' inválido. Apenas {A,C,G,N,T} aceitos — IUPAC ambíguos "
                "(R/Y/M/K/B/D/H/V/W/S) não são expandidos."
            };
        }
    }

    // I-1 (DoS) + I-7 (corretude): propaga max_results ao FM-Index. Sem isso,
    // k-mer com alto recall (k≤3 em pangenoma → ~25% × N matches) materializa
    // centenas de milhões de offsets antes da paginação.
    //
    // I-7 (auditoria DoD §11.6, 2026-05-15): a versão original do I-1 fazia
    // `std::min(*q.limit × 16, kSafetyHardLimit)`, o que impedia QUALQUER
    // SEARCH KMER de buscar >100k offsets — mesmo com LIMIT explícito.
    // Consequência: `SEARCH KMER "ACGT"` (≈2,9M occ → 286.696 segs) era
    // truncado silenciosamente para 56.311 segs, e `LIMIT 500000` era
    // ignorado acima do teto. Fix: o teto kSafetyHardLimit protege apenas
    // o caso SEM LIMIT (DoS por k-mer curto sem paginação). Com LIMIT
    // explícito o usuário assume a responsabilidade — boost 16× compensa
    // o dedup por-seg posterior, sem teto.
    constexpr size_t kKmerDedupBoost = 16;
    const size_t max_results = q.limit.has_value()
        ? static_cast<size_t>(*q.limit) * kKmerDedupBoost
        : size_t{kSafetyHardLimit};
    auto offsets = seq_index_->search_kmer(q.kmer, max_results);
    // I-4: dedup via unordered_set é O(N) vs O(N log N) do sort+unique.
    // Em k-mer comum (>100K offsets no mesmo seg), dedupa antes de alocar
    // vetor final — ~25% speedup grátis sem mudar semântica. Sort final
    // preserva determinismo da iteração (hash do unordered é não-estável).
    std::unordered_set<uint64_t> id_set;
    id_set.reserve(offsets.size());
    for (uint64_t pos : offsets) {
        if (auto id = metadata_->segment_id_from_offset(pos)) {
            id_set.insert(*id);
        }
    }
    std::vector<uint64_t> ids(id_set.begin(), id_set.end());
    std::sort(ids.begin(), ids.end());

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kSegmentColumns);
    const size_t ncols = cols.size();
    const auto emitters = detail::map_segment_cols(cols);

    // SEARCH KMER: sem offset (não há cláusula OFFSET no parser para kmer);
    // usa o mesmo Paginator para safety/truncation consistente com plan_*.
    auto pag = detail::Paginator::from_query(q);
    if (!q.limit.has_value()) {
        // Comportamento legado: sem LIMIT, retorna todos os ids encontrados
        // (FM-Index normalmente devolve poucos). O safety limit ainda protege.
        pag.take_n = static_cast<uint32_t>(std::min(ids.size(), size_t{kSafetyHardLimit}));
    }

    QueryResult result;
    result.columns = std::move(cols);
    result.flat.reserve(std::min(ids.size(), size_t{pag.take_n}) * ncols);

    for (uint64_t id : ids) {
        const auto d = pag.advance(result);
        if (d == detail::Paginator::Decision::Stop) { break; }
        if (d == detail::Paginator::Decision::Skip) { continue; }

        const int64_t len = metadata_->length(id);
        const int32_t rc_raw = metadata_->read_count(id);
        const float dp_raw = metadata_->depth(id);

        for (const auto e : emitters) {
            switch (e) {
                case detail::SegmentCol::Id:
                    result.flat.emplace_back(int64_t(id));
                    break;
                case detail::SegmentCol::Name:
                    result.flat.emplace_back(std::string(metadata_->original_id(id)));
                    break;
                case detail::SegmentCol::Length:
                    result.flat.emplace_back(int64_t(len));
                    break;
                case detail::SegmentCol::ReadCount:
                    result.flat.push_back(rc_raw >= 0 ? Value{int64_t(rc_raw)}
                                                      : Value{std::string{"NULL"}});
                    break;
                case detail::SegmentCol::Depth:
                    result.flat.push_back(dp_raw >= 0.f ? Value{double(dp_raw)}
                                                        : Value{std::string{"NULL"}});
                    break;
            }
        }
        ++result.row_count;
    }
    return result;
}

// ── plan_tips ─────────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_tips(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) {
        return std::unexpected{"FIND TIP requer GfaCsrTopology"};
    }

    const auto node_ids = topology_->find_tips();
    return plan_node_query(q, node_ids);
}

// ── plan_repeats ──────────────────────────────────────────────────────────────

std::expected<QueryResult, std::string> QueryPlanner::plan_repeats(const FindQuery& q) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) {
        return std::unexpected{"FIND REPEAT requer GfaCsrTopology"};
    }

    const auto node_ids = topology_->find_repeats(2);
    return plan_node_query(q, node_ids);
}

// ── plan_node_query (helper compartilhado por plan_tips e plan_repeats) ──────

std::expected<QueryResult, std::string>
QueryPlanner::plan_node_query(const FindQuery& q, std::span<const uint64_t> node_ids) const {
    auto cols = detail::resolve_return_fields(q.return_fields, detail::kNodeColumns);
    const size_t ncols = cols.size();
    const auto emitters = detail::map_node_cols(cols);
    auto pag = detail::Paginator::from_query(q, /*unbounded_count=*/true);

    QueryResult result;
    result.columns = std::move(cols);
    result.flat.reserve(std::min(node_ids.size(), size_t{pag.take_n}) * ncols);

    RowFields row;

    for (const uint64_t nid : node_ids) {
        const int64_t in_d = static_cast<int64_t>(topology_->in_degree(nid));
        const int64_t out_d = static_cast<int64_t>(topology_->out_degree(nid));

        if (q.where) {
            row.count = 0;
            row.insert("id", int64_t(nid));
            row.insert("in_degree", in_d);
            row.insert("out_degree", out_d);
            if (!eval_where(*q.where, row)) {
                continue;
            }
        }

        const auto d = pag.advance(result);
        if (d == detail::Paginator::Decision::Stop) { break; }
        if (d == detail::Paginator::Decision::Skip) { continue; }

        for (const auto e : emitters) {
            switch (e) {
                case detail::NodeCol::Id:     result.flat.emplace_back(int64_t(nid)); break;
                case detail::NodeCol::InDeg:  result.flat.emplace_back(in_d); break;
                case detail::NodeCol::OutDeg: result.flat.emplace_back(out_d); break;
            }
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
    if (!topology_) {
        return std::unexpected{"FIND BUBBLE requer GfaCsrTopology"};
    }
    if (!topology_->has_snarls()) {
        return std::unexpected{"FIND BUBBLE: snarls.bin ausente — execute o ingest novamente para "
                               "habilitar a query (snarls são pré-computados)."};
    }

    const auto snarls = topology_->snarls();
    auto cols = detail::resolve_return_fields(q.return_fields, detail::kBubbleColumns);
    const size_t ncols = cols.size();
    const auto emitters = detail::map_bubble_cols(cols);
    auto pag = detail::Paginator::from_query(q, /*unbounded_count=*/true);

    QueryResult result;
    result.columns = std::move(cols);
    result.flat.reserve(std::min(snarls.size(), size_t{pag.take_n}) * ncols);

    RowFields row;

    for (const auto& s : snarls) {
        const int64_t src = static_cast<int64_t>(s.source);
        const int64_t snk = static_cast<int64_t>(s.sink);
        const int64_t bc = static_cast<int64_t>(s.branch_count);
        const int64_t inc = static_cast<int64_t>(s.internal_node_count);
        const int64_t hc = static_cast<int64_t>(s.has_cycles);

        if (q.where) {
            row.count = 0;
            row.insert("source", src);
            row.insert("sink", snk);
            row.insert("branch_count", bc);
            row.insert("internal_node_count", inc);
            row.insert("has_cycles", hc);
            if (!eval_where(*q.where, row)) {
                continue;
            }
        }

        const auto d = pag.advance(result);
        if (d == detail::Paginator::Decision::Stop) { break; }
        if (d == detail::Paginator::Decision::Skip) { continue; }

        for (const auto e : emitters) {
            switch (e) {
                case detail::BubbleCol::Source:        result.flat.emplace_back(src); break;
                case detail::BubbleCol::Sink:          result.flat.emplace_back(snk); break;
                case detail::BubbleCol::BranchCount:   result.flat.emplace_back(bc);  break;
                case detail::BubbleCol::InternalCount: result.flat.emplace_back(inc); break;
                case detail::BubbleCol::HasCycles:     result.flat.emplace_back(hc);  break;
            }
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
    if (!metadata_) {
        return std::unexpected{"FIND SEGMENT requer GfaMetadataStore instanciado"};
    }

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kSegmentColumns);
    const size_t ncols = cols.size();
    const auto emitters = detail::map_segment_cols(cols);

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
    bool needs_name = std::ranges::contains(cols, "name");
    bool where_depth = false, where_rc = false;
    if (q.where) {
        for (const auto& cond : q.where->conditions) {
            if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
                if (cc->field == "name") {
                    needs_name = true;
                }
                if (cc->field == "depth") {
                    where_depth = true;
                }
                if (cc->field == "read_count") {
                    where_rc = true;
                }
            }
        }
    }

    // Campos opcionais no resultado (fora do loop — constantes por query)
    const bool ret_rc = std::ranges::contains(cols, "read_count");
    const bool ret_dp = std::ranges::contains(cols, "depth");

    auto pag = detail::Paginator::from_query(q, /*unbounded_count=*/true);

    QueryResult result;
    result.columns = cols;
    result.flat.reserve(std::min(metadata_->size(), size_t{pag.take_n}) * ncols);

    RowFields row;

    // Lambda que avalia WHERE e empurra o row para o resultado.
    // Usado tanto no caminho rápido (índice) como no full-scan.
    // Flags safety_limit_hit/total_hits_truncated devem ser verificadas no outer loop.
    auto process_id = [&](size_t id) {
        const int64_t len = static_cast<int64_t>(metadata_->length(id));

        if (q.where) {
            const int32_t rc_raw = where_rc ? metadata_->read_count(id) : -1;
            const float dp_raw = where_depth ? metadata_->depth(id) : -1.f;
            row.count = 0;
            row.insert("id", int64_t(id));
            row.insert("length", len);
            if (where_rc) {
                row.insert("read_count",
                           rc_raw >= 0 ? Value{int64_t(rc_raw)} : Value{std::string{"NULL"}});
            }
            if (where_depth) {
                row.insert("depth",
                           dp_raw >= 0.f ? Value{double(dp_raw)} : Value{std::string{"NULL"}});
            }
            if (needs_name) {
                row.insert("name", std::string(metadata_->original_id(id)));
            }
            if (!eval_where(*q.where, row, cache_ptr)) {
                return;
            }
        }

        const auto d = pag.advance(result);
        if (d != detail::Paginator::Decision::Emit) { return; }

        const int32_t rc_out = ret_rc ? metadata_->read_count(id) : -1;
        const float dp_out = ret_dp ? metadata_->depth(id) : -1.f;
        for (const auto e : emitters) {
            switch (e) {
                case detail::SegmentCol::Id:
                    result.flat.emplace_back(int64_t(id));
                    break;
                case detail::SegmentCol::Length:
                    result.flat.emplace_back(len);
                    break;
                case detail::SegmentCol::Name:
                    result.flat.emplace_back(std::string(metadata_->original_id(id)));
                    break;
                case detail::SegmentCol::ReadCount:
                    result.flat.push_back(rc_out >= 0 ? Value{int64_t(rc_out)}
                                                       : Value{std::string{"NULL"}});
                    break;
                case detail::SegmentCol::Depth:
                    result.flat.push_back(dp_out >= 0.f ? Value{double(dp_out)}
                                                         : Value{std::string{"NULL"}});
                    break;
            }
        }
        ++result.row_count;
    };

    auto should_stop = [&]() { return result.safety_limit_hit || result.total_hits_truncated; };

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
                if (should_stop()) {
                    break;
                }
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
                if (should_stop()) {
                    break;
                }
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
    if (!topology_) {
        return std::unexpected{"FIND EDGE requer GfaCsrTopology"};
    }

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kEdgeColumns);
    const size_t ncols = cols.size();
    const auto emitters = detail::map_edge_cols(cols);

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
                        ++orient_cond_count;
                    }
                    // valor não-string ou string vazia: deixa eval_where lidar
                }
            }
        }
        where_only_orient = orient_eq.has_value() &&
                            orient_cond_count == q.where->conditions.size();
    }

    auto pag = detail::Paginator::from_query(q, /*unbounded_count=*/true);

    QueryResult result;
    result.columns = std::move(cols);
    result.flat.reserve(std::min(topology_->num_edges(), size_t{pag.take_n}) * ncols);

    RowFields row;

    bool stop = false;
    for (uint64_t from = 0; from < topology_->num_nodes() && !stop; ++from) {
        for (const uint32_t enc : topology_->out_neighbors(from)) {
            const auto [to_id, to_orient] = topology_->decode(enc);

            // Comparação de char — sem alocação, elimina a maioria das arestas antes de
            // construir o RowFields.
            if (orient_eq && to_orient != *orient_eq) {
                continue;
            }

            if (q.where && !where_only_orient) {
                row.count = 0;
                row.insert("from_id", int64_t(from));
                row.insert("to_id", int64_t(to_id));
                row.insert("to_orient", std::string{to_orient});
                if (!eval_where(*q.where, row)) {
                    continue;
                }
            }

            const auto d = pag.advance(result);
            if (d == detail::Paginator::Decision::Stop) { stop = true; break; }
            if (d == detail::Paginator::Decision::Skip) { continue; }

            for (const auto e : emitters) {
                switch (e) {
                    case detail::EdgeCol::FromId:   result.flat.emplace_back(int64_t(from)); break;
                    case detail::EdgeCol::ToId:     result.flat.emplace_back(int64_t(to_id)); break;
                    case detail::EdgeCol::ToOrient: result.flat.emplace_back(std::string{to_orient}); break;
                }
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
    if (!topology_) {
        return std::unexpected{"FIND PATH requer GfaCsrTopology"};
    }

    const size_t count = topology_->path_count();
    if (count == 0) {
        return std::unexpected{"FIND PATH: nenhum path indexado — o GFA não contém linhas W ou P"};
    }

    const bool use_stats = topology_->has_path_stats();
    if (!use_stats && !metadata_) {
        return std::unexpected{
            "FIND PATH: requer path_stats no cache ou GfaMetadataStore para "
            "computar length (cache antigo sem path_stats — re-execute o ingest)"};
    }

    auto cols = detail::resolve_return_fields(q.return_fields, detail::kPathColumns);
    const size_t ncols = cols.size();
    const auto emitters = detail::map_path_cols(cols);
    auto pag = detail::Paginator::from_query(q, /*unbounded_count=*/true);

    QueryResult result;
    result.columns = std::move(cols);
    result.flat.reserve(std::min(count, size_t{pag.take_n}) * ncols);

    // CrossesCache — pré-computa path_name → set de nodes do target. Vazio
    // se o WHERE não tiver `crosses(...)` (passa nullptr ao eval).
    CrossesCache crosses_cache;
    if (q.where) {
        detail::build_crosses_cache(*q.where, *topology_, crosses_cache);
    }
    const CrossesCache* xc_ptr = crosses_cache.empty() ? nullptr : &crosses_cache;

    RowFields row;

    for (size_t pid = 0; pid < count; ++pid) {
        int64_t nc = 0;
        int64_t total_length = 0;

        if (use_stats) {
            nc = static_cast<int64_t>(topology_->path_node_count(pid));
            total_length = topology_->path_bp_length(pid);
        } else {
            // Pré-condição: metadata_ != nullptr (validado acima).
            const auto nodes = topology_->path_nodes(pid);
            nc = static_cast<int64_t>(nodes.size());
            for (const uint64_t nid : nodes) {
                total_length += static_cast<int64_t>(metadata_->length(nid));
            }
        }

        const std::string_view pname = topology_->path_name(pid);

        if (q.where) {
            row.count = 0;
            row.insert("id", int64_t(pid));
            row.insert("name", std::string(pname));
            row.insert("node_count", nc);
            row.insert("length", total_length);
            if (!eval_where(*q.where, row, /*kmer*/ nullptr, xc_ptr)) {
                continue;
            }
        }

        const auto d = pag.advance(result);
        if (d == detail::Paginator::Decision::Stop) { break; }
        if (d == detail::Paginator::Decision::Skip) { continue; }

        for (const auto e : emitters) {
            switch (e) {
                case detail::PathCol::Id:        result.flat.emplace_back(int64_t(pid)); break;
                case detail::PathCol::Name:      result.flat.emplace_back(std::string(pname)); break;
                case detail::PathCol::NodeCount: result.flat.emplace_back(nc); break;
                case detail::PathCol::Length:    result.flat.emplace_back(total_length); break;
            }
        }
        ++result.row_count;
    }
    return result;
}

} // namespace ggql
