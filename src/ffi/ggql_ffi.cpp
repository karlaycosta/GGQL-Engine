#include "ffi/ggql_ffi.h"
#include "ggql/cli/console.hpp"
#include "ggql/ingest/gfa_ingestor.hpp"

#include <cstring>
#include <string>

using namespace ggql;

namespace {

// Ponte cli::Event → GgqlEvent. Buffer local p/ copiar string_views em
// C-strings NULL-terminadas (as views do cli não garantem terminação em nul).
struct EventBridge {
    GgqlEventCallback fn;
    void* user_data;
    // Buffers reusáveis por chamada — evita alocação por evento.
    std::string text_buf;
    std::string detail_buf;
};

void dispatch_bridge(const cli::Event& ev, void* user) {
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
            std::string(cache_dir)
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
            stats.elapsed_gbwt_insert_s = result->elapsed_gbwt_insert_s;
            stats.elapsed_gbwt_compress_s = result->elapsed_gbwt_compress_s;
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

} // extern "C"
