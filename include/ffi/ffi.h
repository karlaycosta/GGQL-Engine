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
    GGQL_EV_PHASE_START = 1, // text = título; phase_number >= 0 ou -1 quando ausente
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
    int32_t phase_number;  // Só PhaseStart; -1 quando ausente, >= 0 caso contrário.
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

// ── Perfis de ingestão ───────────────────────────────────────────────────────
// Mapeia casos de uso biológicos a um subconjunto de estruturas a construir.
// Documentação: docs/ingest_profiles.md
typedef enum {
    GGQL_PROFILE_PANGENOME    = 0, // tudo habilitado (default)
    GGQL_PROFILE_METAGENOMICS = 1, // sem gBWT, sem snarls
    GGQL_PROFILE_ASSEMBLY     = 2, // sem gBWT, com snarls
    GGQL_PROFILE_COMPARATIVE  = 3, // com gBWT, sem snarls
} GgqlIngestProfile;

// Resolve um nome textual ("pangenome", "metagenomics", "assembly", "comparative",
// ou aliases curtos: "pan", "meta", "asm", "comp") para o enum.
// Retorna -1 em nome inválido. *out preenchido apenas em sucesso.
int32_t ggql_profile_from_name(const char* name, GgqlIngestProfile* out);

// Retorna o nome canônico (aponta para string estática). NULL em enum inválido.
const char* ggql_profile_name(GgqlIngestProfile p);

// Variante de ggql_ingestor_ingest que recebe perfil pronto. Equivalente a
// chamar ggql_ingestor_ingest com os flags derivados de `profile`.
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
);

// Reseta o ingestor
void ggql_ingestor_reset(GgqlIngestorHandle handle);

// ═══════════════════════════════════════════════════════════════════════════════
// API DE QUERY — Engine + Resultado
// ═══════════════════════════════════════════════════════════════════════════════

// Handles opacos para o engine e para resultados de query.
typedef void* GgqlEngineHandle;
typedef void* GgqlResultHandle;

// Tipos de célula retornados por ggql_result_cell_type().
typedef enum {
    GGQL_TYPE_NULL   = 0,  // campo ausente / NULL
    GGQL_TYPE_INT    = 1,  // int64_t
    GGQL_TYPE_DOUBLE = 2,  // double
    GGQL_TYPE_STRING = 3,  // const char*
} GgqlCellType;

// ── Criação do engine ──────────────────────────────────────────────────────────

// Carrega o engine a partir de um diretório de cache gerado por ggql_ingestor_ingest().
// Carrega metadados e FM-Index do disco. Topologia fica NULL (suficiente para a
// maioria das queries — FIND TIP/REPEAT/BUBBLE/EDGE/PATH requerem topologia).
// Retorna NULL em caso de erro; consulte ggql_engine_last_error().
GgqlEngineHandle ggql_engine_load(const char* cache_dir);

// Cria o engine a partir de um ingestor que já concluiu ggql_ingestor_ingest().
// Usa a topologia em memória do ingestor e carrega metadados + FM-Index do cache_dir.
// Retorna NULL em caso de erro.
// IMPORTANTE: o ingestor deve permanecer vivo enquanto o engine existir (a topologia
// é um ponteiro não-proprietário).
GgqlEngineHandle ggql_engine_create(GgqlIngestorHandle ingestor, const char* cache_dir);

// Destroi o engine. Não destrói o ingestor.
void ggql_engine_destroy(GgqlEngineHandle engine);

// Retorna a mensagem de erro da última chamada que falhou (ggql_engine_load,
// ggql_engine_create, ggql_engine_execute). Válida até a próxima chamada FFI.
const char* ggql_engine_last_error(GgqlEngineHandle engine);

// ── Execução de query ──────────────────────────────────────────────────────────

// Executa uma query GGQL com paginação.
//   query  — string GGQL terminada em '\0' (sem LIMIT/OFFSET se usar os parâmetros)
//   limit  — 0 usa o default FFI (100 linhas por página)
//   offset — 0 = primeira página; N = pular N linhas antes de emitir
//
// Retorna NULL em caso de erro de parse ou execução.
// O chamador DEVE liberar com ggql_result_free().
// Thread-safe: múltiplas queries simultâneas em threads diferentes são seguras.
GgqlResultHandle ggql_engine_execute(
    GgqlEngineHandle engine,
    const char*      query,
    uint32_t         limit,
    uint32_t         offset
);

// ── Metadados do resultado ─────────────────────────────────────────────────────

int64_t ggql_result_row_count       (GgqlResultHandle result);  // linhas neste batch
int64_t ggql_result_col_count       (GgqlResultHandle result);  // colunas
int64_t ggql_result_total_hits      (GgqlResultHandle result);  // matches (até 1M)
bool    ggql_result_total_truncated (GgqlResultHandle result);  // contagem parou em 1M?
bool    ggql_result_safety_limit_hit(GgqlResultHandle result);  // safety atingido?

// Nome da coluna col ∈ [0, col_count). Válido até ggql_result_free().
const char* ggql_result_col_name(GgqlResultHandle result, int32_t col);

// ── Acesso por célula ──────────────────────────────────────────────────────────
//
// row ∈ [0, row_count)   col ∈ [0, col_count)
// Chame ggql_result_cell_type() primeiro para saber qual acessor usar.
// Usar o acessor errado retorna 0 / NULL.

GgqlCellType ggql_result_cell_type  (GgqlResultHandle result, int32_t row, int32_t col);
int64_t      ggql_result_cell_int   (GgqlResultHandle result, int32_t row, int32_t col);
double       ggql_result_cell_double(GgqlResultHandle result, int32_t row, int32_t col);

// Ponteiro de string válido até ggql_result_free(). Copie se precisar reter.
const char*  ggql_result_cell_string(GgqlResultHandle result, int32_t row, int32_t col);

// Libera o resultado. Após essa chamada todos os ponteiros obtidos do resultado
// são inválidos.
void ggql_result_free(GgqlResultHandle result);

#ifdef __cplusplus
}
#endif

#endif // GGQL_FFI_H
