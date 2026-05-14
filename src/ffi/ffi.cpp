#include "ffi/ffi.h"
#include "ggql/cli/console.hpp"
#include "ggql/ingest/ingestor.hpp"
#include "ggql/ingest/metadata_store.hpp"
#include "ggql/ingest/profile.hpp"
#include "ggql/query/grammar.hpp"
#include "ggql/query/query_planner.hpp"
#include "ggql/storage/fm_index.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <variant>

using namespace ggql;

namespace {

// ── Engine wrapper ────────────────────────────────────────────────────────────
//
// Armazena os três backends e o planner. Os backends são proprietários dos dados;
// o ponteiro `topology` pode ser nulo (sem topologia = sem FIND TIP/EDGE/etc).

struct GgqlEngineWrapper {
    GfaMetadataStore metadata;
    GfaSequenceIndex fm_index;
    // Topology owned (preenchido em ggql_engine_load) OU referenciado externo
    // (ggql_engine_create). topology aponta para o que estiver disponível;
    // owned_topology vive enquanto o wrapper viver.
    std::optional<GfaCsrTopology> owned_topology;
    const GfaCsrTopology* topology = nullptr;  // pode ser null se ausente
    std::unique_ptr<QueryPlanner> planner;
    std::string last_error;
};

// ── Result wrapper ────────────────────────────────────────────────────────────
//
// Encapsula QueryResult. As strings em result.flat[] e result.columns[]
// ficam vivas até ggql_result_free(). Os ponteiros C retornados pelos acessores
// apontam diretamente para o std::string interno — zero cópias extras.

struct GgqlResultWrapper {
    QueryResult result;
};

// ── Default de limit na FFI ───────────────────────────────────────────────────
static constexpr uint32_t kFfiDefaultLimit = 100;

// ── Ponte cli::Event → GgqlEvent ─────────────────────────────────────────────
// Buffer local p/ copiar string_views em C-strings NULL-terminadas
// (as views do cli não garantem terminação em nul).
struct EventBridge {
    GgqlEventCallback fn;
    void* user_data;
    // Buffers reusáveis por chamada — evita alocação por evento.
    std::string text_buf;
    std::string detail_buf;
};

// noexcept enforça que o boundary FFI nunca propague exceção C++ para o
// callback C/Dart. Em caso patológico (bad_alloc nos buffers), terminate
// é preferível a UB silencioso atravessando boundary.
void dispatch_bridge(const cli::Event& ev, void* user) noexcept {
    auto* b = static_cast<EventBridge*>(user);
    GgqlEvent out;
    std::memset(&out, 0, sizeof(out));

    out.kind = static_cast<int32_t>(ev.kind);
    out.phase_number = ev.phase_number;

    if (!ev.text.empty()) {
        b->text_buf.assign(ev.text.data(), ev.text.size());
        out.text = b->text_buf.c_str();
    }
    if (!ev.detail.empty()) {
        b->detail_buf.assign(ev.detail.data(), ev.detail.size());
        out.detail = b->detail_buf.c_str();
    }

    out.percent = ev.percent;
    out.eta_s = ev.eta_s;
    out.rate_mbs = ev.rate_mbs;

    b->fn(&out, b->user_data);
}

} // namespace

extern "C" {

GgqlIngestorHandle ggql_ingestor_create() {
    try {
        return new GfaIngestorMmapParallel();
    } catch (...) {
        return nullptr;
    }
}

void ggql_ingestor_destroy(GgqlIngestorHandle handle) {
    if (handle) {
        delete static_cast<GfaIngestorMmapParallel*>(handle);
    }
}

void ggql_ingestor_reset(GgqlIngestorHandle handle) {
    if (handle) {
        static_cast<GfaIngestorMmapParallel*>(handle)->reset();
    }
}

// thread_local evita que múltiplas chamadas FFI simultâneas em threads
// diferentes sobrescrevam a error_message de outra.
thread_local std::string last_error_message;

namespace {

GgqlIngestStats do_ingest_ffi(
    GgqlIngestorHandle handle,
    const char* filepath,
    int num_threads,
    double gamma,
    bool verbose,
    bool save_metadata,
    bool build_topology,
    bool build_fm_index,
    bool build_gbwt,
    bool build_snarls,
    size_t max_gbwt_paths,
    bool keep_gbwt_tmp,
    const char* cache_dir,
    GgqlEventCallback event_cb,
    void* user_data
) {
    GgqlIngestStats stats = {};
    stats.success = false;

    if (!handle || !filepath || !cache_dir) {
        last_error_message = "Invalid arguments passed to ggql_ingestor_ingest (null pointers).";
        stats.error_message = last_error_message.c_str();
        return stats;
    }

    // Instala a ponte de eventos (thread_local p/ não vazar entre chamadas
    // paralelas em threads diferentes; o dispatcher em cli:: é atômico).
    thread_local EventBridge bridge;
    if (event_cb) {
        bridge.fn = event_cb;
        bridge.user_data = user_data;
        cli::set_event_callback(&dispatch_bridge, &bridge);
    }

    try {
        auto* ingestor = static_cast<GfaIngestorMmapParallel*>(handle);
        auto result = ingestor->ingest(
            std::string(filepath),
            num_threads,
            gamma,
            verbose,
            save_metadata,
            build_topology,
            build_fm_index,
            max_gbwt_paths,
            keep_gbwt_tmp,
            std::string(cache_dir),
            build_gbwt,
            build_snarls
        );

        if (result.has_value()) {
            stats.success = true;
            stats.segment_count = result->segment_count;
            stats.link_count = result->link_count;
            stats.node_count = result->node_count;
            stats.path_count = result->path_count;
            stats.elapsed_total_s = result->elapsed_total_s;
            stats.elapsed_io1_s = result->elapsed_io1_s;
            stats.elapsed_build_s = result->elapsed_build_s;
            stats.elapsed_ingest_s = result->elapsed_ingest_s;
            stats.elapsed_metadata_s = result->elapsed_metadata_s;
            stats.elapsed_topology_s = result->elapsed_topology_s;
            stats.elapsed_gbwt_s = result->elapsed_gbwt_s;
            stats.elapsed_fm_index_s = result->elapsed_fm_index_s;
            stats.gbwt_cpu_avg_s = result->gbwt_cpu_avg_s;
            stats.gbwt_cpu_max_s = result->gbwt_cpu_max_s;
            stats.gbwt_total_nodes = result->gbwt_total_nodes;
            stats.gbwt_peak_rss_kb = result->gbwt_peak_rss_kb;
            stats.gbwt_index_size_b = result->gbwt_index_size_b;
        } else {
            last_error_message = result.error();
            stats.error_message = last_error_message.c_str();
        }
    } catch (const std::exception& e) {
        last_error_message = std::string("C++ Exception: ") + e.what();
        stats.error_message = last_error_message.c_str();
    } catch (...) {
        last_error_message = "Unknown C++ exception occurred during ingest";
        stats.error_message = last_error_message.c_str();
    }

    // Remove o hook global antes de retornar para não vazar a referência.
    if (event_cb) {
        cli::clear_event_callback();
    }

    return stats;
}

} // namespace

GgqlIngestStats ggql_ingestor_ingest(
    GgqlIngestorHandle handle,
    const char* filepath,
    int num_threads,
    double gamma,
    bool verbose,
    bool save_metadata,
    bool build_topology,
    bool build_fm_index,
    size_t max_gbwt_paths,
    bool keep_gbwt_tmp,
    const char* cache_dir,
    GgqlEventCallback event_cb,
    void* user_data
) {
    // API legada: gBWT e snarls seguem build_topology (todos acoplados, como
    // era antes da separação). Para controle granular, use
    // ggql_ingestor_ingest_with_profile.
    return do_ingest_ffi(handle, filepath, num_threads, gamma, verbose,
                         save_metadata, build_topology, build_fm_index,
                         /*build_gbwt=*/build_topology,
                         /*build_snarls=*/build_topology,
                         max_gbwt_paths, keep_gbwt_tmp, cache_dir,
                         event_cb, user_data);
}

GgqlIngestStats ggql_ingestor_ingest_with_profile(
    GgqlIngestorHandle handle,
    const char* filepath,
    int num_threads,
    double gamma,
    bool verbose,
    GgqlIngestProfile profile,
    size_t max_gbwt_paths,
    bool keep_gbwt_tmp,
    const char* cache_dir,
    GgqlEventCallback event_cb,
    void* user_data
) {
    const auto pf = flags_for(static_cast<IngestProfile>(profile));
    return do_ingest_ffi(handle, filepath, num_threads, gamma, verbose,
                         pf.save_metadata, pf.build_topology, pf.build_fm_index,
                         pf.build_gbwt, pf.build_snarls,
                         max_gbwt_paths, keep_gbwt_tmp, cache_dir,
                         event_cb, user_data);
}

int32_t ggql_profile_from_name(const char* name, GgqlIngestProfile* out) {
    if (!name || !out) { return -1; }
    auto parsed = parse_profile(std::string_view(name));
    if (!parsed) { return -1; }
    *out = static_cast<GgqlIngestProfile>(*parsed);
    return 0;
}

const char* ggql_profile_name(GgqlIngestProfile p) {
    switch (p) {
    case GGQL_PROFILE_PANGENOME:    return "pangenome";
    case GGQL_PROFILE_METAGENOMICS: return "metagenomics";
    case GGQL_PROFILE_ASSEMBLY:     return "assembly";
    case GGQL_PROFILE_COMPARATIVE:  return "comparative";
    }
    return nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
// FFI de Query — Engine
// ══════════════════════════════════════════════════════════════════════════════

// thread_local para mensagens de erro de funções que não têm handle de engine.
thread_local std::string engine_create_error;

GgqlEngineHandle ggql_engine_load(const char* cache_dir) {
    if (!cache_dir) {
        engine_create_error = "cache_dir não pode ser NULL";
        return nullptr;
    }
    try {
        auto* w = new GgqlEngineWrapper();
        w->metadata.load(std::string(cache_dir));
        // I-2: FM-Index opcional. Ausência → is_loaded()==false silencioso.
        // Corrupção → preserva diagnóstico em engine_create_error (caller
        // pode reingerir após inspecionar a mensagem).
        if (auto r = w->fm_index.load(std::string(cache_dir)); !r) {
            engine_create_error = std::string("fm_index: ") + r.error();
        }
        // Topology opcional — perfis sem build_topology não terão E_*/ef_*.
        // Se ausente, FIND TIP/EDGE/PATH/BUBBLE/REPEAT (e crosses no PATH)
        // retornarão erro semântico via planner; SEGMENT/SEARCH KMER continuam.
        try {
            GfaCsrTopology topo;
            topo.load(std::string(cache_dir));
            w->owned_topology.emplace(std::move(topo));
            w->topology = &*w->owned_topology;
        } catch (...) {}
        w->planner = std::make_unique<QueryPlanner>(
            w->topology,
            &w->metadata,
            w->fm_index.is_loaded() ? &w->fm_index : nullptr
        );
        return w;
    } catch (const std::exception& e) {
        engine_create_error = std::string("ggql_engine_load: ") + e.what();
        return nullptr;
    } catch (...) {
        engine_create_error = "ggql_engine_load: erro desconhecido";
        return nullptr;
    }
}

GgqlEngineHandle ggql_engine_create(GgqlIngestorHandle ingestor, const char* cache_dir) {
    if (!ingestor || !cache_dir) {
        engine_create_error = "ingestor e cache_dir não podem ser NULL";
        return nullptr;
    }
    try {
        auto* w = new GgqlEngineWrapper();
        w->topology = static_cast<GfaIngestorMmapParallel*>(ingestor)->topology();
        w->metadata.load(std::string(cache_dir));
        // I-2: FM-Index opcional. Ausência silenciosa; corrupção preserva
        // diagnóstico em engine_create_error sem abortar — o caller pode
        // re-ingerir após inspecionar a mensagem.
        if (auto r = w->fm_index.load(std::string(cache_dir)); !r) {
            engine_create_error = std::string("fm_index: ") + r.error();
        }
        w->planner = std::make_unique<QueryPlanner>(
            w->topology,
            &w->metadata,
            w->fm_index.is_loaded() ? &w->fm_index : nullptr
        );
        return w;
    } catch (const std::exception& e) {
        engine_create_error = std::string("ggql_engine_create: ") + e.what();
        return nullptr;
    } catch (...) {
        engine_create_error = "ggql_engine_create: erro desconhecido";
        return nullptr;
    }
}

void ggql_engine_destroy(GgqlEngineHandle engine) {
    delete static_cast<GgqlEngineWrapper*>(engine);
}

const char* ggql_engine_last_error(GgqlEngineHandle engine) {
    if (!engine) { return engine_create_error.c_str(); }
    return static_cast<GgqlEngineWrapper*>(engine)->last_error.c_str();
}

GgqlResultHandle ggql_engine_execute(
    GgqlEngineHandle engine,
    const char*      query_str,
    uint32_t         limit,
    uint32_t         offset
) {
    if (!engine || !query_str) { return nullptr; }
    auto* w = static_cast<GgqlEngineWrapper*>(engine);

    // Parse da query
    auto parsed = grammar::parse_ggql(query_str);
    if (!parsed) {
        w->last_error = std::move(parsed.error());
        return nullptr;
    }

    // Injeta limit/offset nos campos do AST se não foram escritos na query.
    // Convenção: usar parâmetros FFI, não LIMIT/OFFSET na string.
    const uint32_t effective_limit  = (limit  == 0) ? kFfiDefaultLimit : limit;
    const uint32_t effective_offset = offset;

    std::visit([&](auto& q) {
        using T = std::decay_t<decltype(q)>;
        if constexpr (std::is_same_v<T, FindQuery>) {
            if (!q.limit.has_value())  { q.limit  = effective_limit;  }
            if (!q.offset.has_value() && effective_offset > 0) {
                q.offset = effective_offset;
            }
        } else if constexpr (std::is_same_v<T, SearchKmerQuery>) {
            if (!q.limit.has_value()) { q.limit = effective_limit; }
        }
    }, parsed.value());

    if (!w->planner) {
        w->last_error = "engine não inicializado corretamente";
        return nullptr;
    }
    auto exec = w->planner->execute(parsed.value());
    if (!exec) {
        w->last_error = std::move(exec.error());
        return nullptr;
    }

    auto* r = new GgqlResultWrapper{std::move(exec.value())};
    return r;
}

// ══════════════════════════════════════════════════════════════════════════════
// FFI de Query — Acessores de resultado
// ══════════════════════════════════════════════════════════════════════════════

int64_t ggql_result_row_count(GgqlResultHandle result) {
    if (!result) { return 0; }
    return static_cast<int64_t>(static_cast<GgqlResultWrapper*>(result)->result.row_count);
}

int64_t ggql_result_col_count(GgqlResultHandle result) {
    if (!result) { return 0; }
    return static_cast<int64_t>(static_cast<GgqlResultWrapper*>(result)->result.columns.size());
}

int64_t ggql_result_total_hits(GgqlResultHandle result) {
    if (!result) { return 0; }
    return static_cast<int64_t>(static_cast<GgqlResultWrapper*>(result)->result.total_hits);
}

bool ggql_result_total_truncated(GgqlResultHandle result) {
    if (!result) { return false; }
    return static_cast<GgqlResultWrapper*>(result)->result.total_hits_truncated;
}

bool ggql_result_safety_limit_hit(GgqlResultHandle result) {
    if (!result) { return false; }
    return static_cast<GgqlResultWrapper*>(result)->result.safety_limit_hit;
}

const char* ggql_result_col_name(GgqlResultHandle result, int32_t col) {
    if (!result) { return nullptr; }
    const auto& cols = static_cast<GgqlResultWrapper*>(result)->result.columns;
    if (col < 0 || static_cast<size_t>(col) >= cols.size()) { return nullptr; }
    return cols[static_cast<size_t>(col)].c_str();
}

// Índice linear em flat[]: row * col_count + col
static inline const Value* get_cell(const GgqlResultWrapper* w, int32_t row, int32_t col) {
    const auto& r = w->result;
    const size_t ncols = r.columns.size();
    if (row < 0 || col < 0
        || static_cast<size_t>(row) >= r.row_count
        || static_cast<size_t>(col) >= ncols) {
        return nullptr;
    }
    return &r.flat[static_cast<size_t>(row) * ncols + static_cast<size_t>(col)];
}

GgqlCellType ggql_result_cell_type(GgqlResultHandle result, int32_t row, int32_t col) {
    if (!result) { return GGQL_TYPE_NULL; }
    const Value* v = get_cell(static_cast<GgqlResultWrapper*>(result), row, col);
    if (!v) { return GGQL_TYPE_NULL; }
    if (std::holds_alternative<int64_t>(*v))    { return GGQL_TYPE_INT;    }
    if (std::holds_alternative<double>(*v))     { return GGQL_TYPE_DOUBLE; }
    if (std::holds_alternative<std::string>(*v)){ return GGQL_TYPE_STRING; }
    return GGQL_TYPE_NULL;
}

int64_t ggql_result_cell_int(GgqlResultHandle result, int32_t row, int32_t col) {
    if (!result) { return 0; }
    const Value* v = get_cell(static_cast<GgqlResultWrapper*>(result), row, col);
    if (!v || !std::holds_alternative<int64_t>(*v)) { return 0; }
    return std::get<int64_t>(*v);
}

double ggql_result_cell_double(GgqlResultHandle result, int32_t row, int32_t col) {
    if (!result) { return 0.0; }
    const Value* v = get_cell(static_cast<GgqlResultWrapper*>(result), row, col);
    if (!v || !std::holds_alternative<double>(*v)) { return 0.0; }
    return std::get<double>(*v);
}

const char* ggql_result_cell_string(GgqlResultHandle result, int32_t row, int32_t col) {
    if (!result) { return nullptr; }
    const Value* v = get_cell(static_cast<GgqlResultWrapper*>(result), row, col);
    if (!v || !std::holds_alternative<std::string>(*v)) { return nullptr; }
    return std::get<std::string>(*v).c_str();
}

void ggql_result_free(GgqlResultHandle result) {
    delete static_cast<GgqlResultWrapper*>(result);
}

} // extern "C"
