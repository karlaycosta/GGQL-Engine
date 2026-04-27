#ifndef GGQL_FFI_H
#define GGQL_FFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef void* GgqlIngestorHandle;

// Result Structs for IngestStatsParallel
typedef struct {
    uint64_t segment_count;
    uint64_t link_count;
    uint64_t node_count;
    uint64_t path_count;
    double elapsed_total_s;
    double elapsed_io1_s;
    double elapsed_build_s;
    double elapsed_ingest_s;
    double elapsed_metadata_s;
    double elapsed_topology_s;
    double elapsed_gbwt_s;
    double elapsed_gbwt_insert_s;
    double elapsed_gbwt_compress_s;
    double elapsed_fm_index_s;
    double gbwt_cpu_avg_s;
    double gbwt_cpu_max_s;
    uint64_t gbwt_total_nodes;
    size_t gbwt_peak_rss_kb;
    size_t gbwt_index_size_b;
    bool success;
    const char* error_message; // Valido apenas se success == false
} GgqlIngestStats;

// ── Eventos de progresso estruturados ─────────────────────────────────────────
//
// O motor emite um stream de eventos durante a ingestão. Cada evento tem um
// kind (enum abaixo) e um subconjunto dos campos preenchido. O callback é
// chamado na thread que gerou o evento — o consumidor é responsável por
// marshalling para a thread da UI.
//
// As strings `text` e `detail` SÓ são válidas durante a chamada. Copie se
// precisar reter (ex.: Flutter converte via toDartString() imediatamente).

typedef enum {
    GGQL_EV_PHASE_START = 1, // text = título; phase_number >= 0
    GGQL_EV_PHASE_END   = 2, // sem campos
    GGQL_EV_STEP_OK     = 3, // text = label; detail = rhs (pode ser NULL)
    GGQL_EV_STEP_FAIL   = 4, // text = label; detail = rhs (pode ser NULL)
    GGQL_EV_STEP_INFO   = 5, // text = label; detail = rhs (pode ser NULL)
    GGQL_EV_OK          = 6, // ok top-level: text + detail
    GGQL_EV_FAIL        = 7, // fail top-level
    GGQL_EV_NOTE        = 8, // linha neutra
    GGQL_EV_PROGRESS    = 9, // text = phase; detail = unit; percent/eta_s/rate_mbs
} GgqlEventKind;

typedef struct {
    int32_t kind;          // GgqlEventKind
    int32_t phase_number;  // Só PhaseStart; 0 quando ausente.
    const char* text;      // Label/título. NULL se não se aplica.
    const char* detail;    // rhs/unidade. NULL se não se aplica.
    double percent;        // Só Progress (0..100).
    double eta_s;          // Só Progress.
    double rate_mbs;       // Só Progress.
} GgqlEvent;

// Assinatura do callback de eventos.
// user_data: ponteiro opaco passado em ggql_ingestor_ingest — repassado aqui
//            sem modificação; sirva-se para encaixar Isolate/stream/etc.
typedef void (*GgqlEventCallback)(const GgqlEvent* ev, void* user_data);

// Cria uma nova instancia do Ingestor
GgqlIngestorHandle ggql_ingestor_create();

// Destroi a instancia
void ggql_ingestor_destroy(GgqlIngestorHandle handle);

// Inicia a ingestao. event_cb pode ser NULL se o consumidor não quiser
// receber eventos.
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
);

// Reseta o ingestor
void ggql_ingestor_reset(GgqlIngestorHandle handle);

#ifdef __cplusplus
}
#endif

#endif // GGQL_FFI_H
