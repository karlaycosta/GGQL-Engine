#ifndef GGQL_FFI_H
#define GGQL_FFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
//
// E1 / Fase 4 (Round 4 / 2026-05-22): opaque struct typedef em vez de
// void* genérico. Compilador C/C++ rejeita silenciosamente a passagem
// de um Result onde um Engine é esperado (e vice-versa) — antes esses
// erros passavam sem warning porque todos eram apenas void*. As structs
// nunca têm definição completa (forward declaration somente); são
// usadas apenas como tipos opacos para distinguir handles. Sem mudança
// de ABI runtime — ponteiros opacos continuam idênticos em layout.
// Consumers via ffigen recebem tipos Dart distintos (Pointer<GgqlIngestor>
// em vez de Pointer<Void>) — type-safety propagada para o lado Dart.
typedef struct GgqlIngestor_* GgqlIngestorHandle;

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
// LIFETIME das strings (atualizado Round 4 / 2026-05-22):
//   - `text` e `detail` ficam válidas por até 16 emits subsequentes na
//     MESMA thread (arena circular interna). Consumer assíncrono — ex.
//     Dart NativeCallable.listener — pode processar com segurança desde
//     que não atrase mais de 16 eventos.
//   - Para retenção indefinida, copie imediatamente (toDartString(),
//     bytes do ctypes.c_char_p, etc.).
//
// TERMINAL EVENT (E3 — garantia desde 2026-05-22):
//   - O último evento emitido antes de qualquer ggql_ingestor_ingest_*()
//     retornar é SEMPRE GGQL_EV_OK (sucesso) ou GGQL_EV_FAIL (erro)
//     top-level, com `text == "__ggql_ingest_done__"`. Em falha,
//     `detail` traz a mensagem de erro (mesma string de
//     GgqlIngestStats.error_message).
//   - GARANTIA DE UNICIDADE: a sentinela "__ggql_ingest_done__" é emitida
//     EXATAMENTE UMA VEZ por chamada de ingest, no fim. Eventos
//     intermediários (PhaseStart/StepOk/Progress/...) NUNCA usam essa
//     string em `text`. Validado empiricamente em tiny.gfa (1 evento) e
//     chm13_subset_sm.gfa (6 eventos, 1 sentinela). Consumer pode contar
//     com "primeiro match da sentinela = terminal" sem ambiguidade.
//   - Consumer pode usar esse evento como sentinela para fechar callbacks/
//     streams com segurança — não precisa de delays/timeouts mágicos
//     para drenar a fila de eventos pendentes.

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

// Variante granular: expõe `build_gbwt` e `build_snarls` separadamente
// (na API legada eles eram acoplados a `build_topology`; nos perfis ficam
// fixos). Use para combos fora dos 4 perfis nomeados — p.ex. "topology
// com snarls mas sem gBWT", ou "gBWT sem snarls".
//
// Restrição: `build_gbwt`/`build_snarls` exigem `build_topology=true`.
GgqlIngestStats ggql_ingestor_ingest_granular(
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
);

// Reseta o ingestor
void ggql_ingestor_reset(GgqlIngestorHandle handle);

// ── Cancelamento cooperativo do ingest (Round 4 Fase 3 / C2 / P10, 2026-05-22) ─
//
// Sinaliza o ingest atualmente em execução neste ingestor handle para
// abortar. Seguro chamar de qualquer thread (típico: thread UI). O ingest
// worker checa o flag em CHECKPOINTS ENTRE FASES (Fase 1: parse + MPHF,
// Fase 2: links/paths, Fase 3: FM-Index, Fase 4: CSR Topology, Fase 5:
// gBWT, Fase 6: Snarls). Granularidade: cancel chega em <1s na próxima
// transição de fase. Não há checkpoints dentro dos loops internos (parse
// OpenMP, gBWT por path, snarls por componente) — refinamento futuro.
//
// Comportamento simétrico a ggql_engine_cancel:
//   - O flag é RESETADO no início de cada ggql_ingestor_ingest_*(). Armar
//     cancel antes de começar é no-op (mesmo design conservador).
//   - Quando o cancel morde, a função de ingest retorna com
//     GgqlIngestStats.success = false e
//     GgqlIngestStats.error_message = "ingest cancelled".
//   - Fase 7 (Persistência) NÃO tem checkpoint — interromper save deixaria
//     cache parcial no disco. Cancel pega até o fim da Fase 6.
//   - Arquivos intermediários no cache_dir podem ficar parcialmente
//     escritos se cancel chegar entre Fase 6 e Fase 7. Consumer pode
//     limpar o cache_dir ao receber "ingest cancelled".
//
// No-op se handle == NULL.
void ggql_ingestor_cancel(GgqlIngestorHandle handle);

// ═══════════════════════════════════════════════════════════════════════════════
// API DE QUERY — Engine + Resultado
// ═══════════════════════════════════════════════════════════════════════════════

// Handles opacos para o engine e para resultados de query.
// E1 / Fase 4: opaque struct typedef — ver nota em GgqlIngestorHandle.
typedef struct GgqlEngine_* GgqlEngineHandle;
typedef struct GgqlResult_* GgqlResultHandle;

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
//
// LIFECYCLE (C4a, documentado em 2026-05-22): destruir o engine enquanto
// existem GgqlResultHandle vivos obtidos dele é COMPORTAMENTO INDEFINIDO.
// Embora `GgqlResultWrapper::result` retenha cópia (move) dos dados de
// célula e portanto cells continuem legíveis após destroy do engine,
// qualquer outra chamada FFI que use o handle do engine após destroy é
// UAF. O consumer DEVE serializar: liberar todos os Results (via
// `ggql_result_free`) antes de chamar `ggql_engine_destroy`. Em wrappers
// idiomáticos (Dart/Python), use refcount / Finalizer para garantir essa
// ordem automaticamente.
void ggql_engine_destroy(GgqlEngineHandle engine);

// Retorna a mensagem de erro da última chamada que falhou (ggql_engine_load,
// ggql_engine_create, ggql_engine_execute). Válida até a próxima chamada FFI.
const char* ggql_engine_last_error(GgqlEngineHandle engine);

// ── Introspecção de capabilities (Round 1.B, 2026-05-20) ──────────────────────
//
// Permite ao consumer descobrir o que este engine pode fazer SEM rodar uma
// query. Counts (segment/edge/path) vêm de metadata/topology em memória — fast.
//
// `has_fm_index` reflete "FM-Index está construído neste .idx" (true mesmo
// antes do lazy-load — checa existência de fm_index.bin no disco). UI pode
// usar para cinzar botão "Buscar k-mer" quando false.
typedef struct {
    bool has_metadata;      // true se engine_load/create teve sucesso
    bool has_topology;      // true se topology.bin foi carregado
    bool has_fm_index;      // true se fm_index.bin existe (mesmo não-carregado)
    bool has_gbwt;          // true se path_stores presente (gBWT)
    bool has_snarls;        // true se snarls.bin foi carregado
    uint64_t segment_count; // requer has_metadata
    uint64_t edge_count;    // requer has_topology
    uint64_t path_count;    // requer has_topology
} GgqlEngineCapabilities;

// Preenche `out` com capabilities + counts. Retorna 0 em sucesso, -1 em
// erro (engine NULL ou out NULL).
int32_t ggql_engine_capabilities(GgqlEngineHandle engine,
                                 GgqlEngineCapabilities* out);

// ── Validação parse-only (Round 2.D, 2026-05-20) ─────────────────────────────
//
// Parseia a query sem executar. Para REPL/IDE: syntax highlighting,
// autocomplete, marcar erro enquanto o usuário digita — sem o custo de
// rodar a query (que em pangenoma pode ser caro).
//
// `error_line`/`error_col` ficam em -1 (não exposto pelo parser PEGTL atual;
// `error_message` carrega a posição em texto livre quando disponível).
typedef struct {
    bool valid;
    int32_t error_line;
    int32_t error_col;
    const char* error_message; // válido até a próxima chamada FFI (thread_local)
} GgqlParseResult;

// Parseia `query`. *out preenchido sempre (success OU fail).
// Retorna:
//    0 — parse OK, out->valid = true
//    1 — parse falhou, out->valid = false (error_message preenchido)
//   -1 — argumentos inválidos (query NULL ou out NULL)
//
// O parâmetro `engine` é opcional (pode ser NULL) — parse é
// engine-independente; aceito para simetria com as demais funções e p/
// permitir futura validação semântica contra capabilities do engine.
int32_t ggql_engine_validate(GgqlEngineHandle engine,
                             const char* query,
                             GgqlParseResult* out);

// ── Schema introspection (Round 2.E, 2026-05-20) ─────────────────────────────
//
// Lista entidades conhecidas pela query engine e suas colunas. Permite UI
// gerar autocomplete / formulários / colunas de tabela sem hardcode.
//
// Pointers retornados apontam para literais estáticos — válidos pelo tempo
// de vida da lib carregada.

// Retorna ponteiro para array de C-strings (nomes de entidade) + *out_count.
// Entidades atuais: SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT.
const char* const* ggql_engine_list_entities(int32_t* out_count);

// Retorna ponteiro para array de C-strings (nomes de coluna da entidade) +
// *out_count. NULL se `entity_name` não for reconhecida (case-insensitive).
// *out_count = 0 nesse caso.
const char* const* ggql_engine_describe_entity(const char* entity_name,
                                               int32_t* out_count);

// ── Cancelamento cooperativo (Round 3.F, 2026-05-20) ─────────────────────────
//
// Sinaliza a query atualmente em execução neste engine para abortar. Seguro
// chamar de qualquer thread (típico: thread UI). A query worker checa o
// flag em pontos de checkpoint (Paginator::advance, ~ns por iteração) e
// retorna o resultado parcial acumulado até ali.
//
// Comportamento:
//   - ggql_engine_execute RESETA o flag cancel_requested no início de cada
//     chamada. Logo, armar cancel quando nenhuma query está rodando é
//     no-op — o cancel só morde para a query atualmente em vôo. Esse design
//     conservador evita cancel "preso" entre queries (caso o consumer
//     esqueça de re-armar entre execs).
//   - O result retornado por uma query cancelada é VÁLIDO mas parcial.
//     ggql_result_total_hits dá quantos hits foram contados até o cancel.
//   - SEARCH KMER em datasets grandes: aborta no próximo `advance()` do
//     pagintor (não interrompe o search_kmer interno do FM-Index — para
//     interrupção mais fina, ver roadmap).
//
// No-op se engine == NULL.
void ggql_engine_cancel(GgqlEngineHandle engine);

// ── Cap per-engine de total_hits (Round 3.G, 2026-05-20) ─────────────────────
//
// Sobrescreve o teto `kMaxTotalHitsCount` (default 1.000.000 ou
// GGQL_MAX_TOTAL_HITS) somente para queries deste engine. `cap == 0`
// restaura o default.
//
// Após o Round 1.#4 (count estrutural não trunca), este cap só morde:
//   - SEARCH KMER (proteção I-1/I-7 DoS preservada)
//   - Queries com WHERE complexo que aborta antes do count completo
//
// No-op se engine == NULL.
void ggql_engine_set_max_total_hits(GgqlEngineHandle engine, uint64_t cap);

// ── Execução streaming via callback paginado (Round 3.H, 2026-05-20) ─────────
//
// Conveniência sobre ggql_engine_execute: re-executa em páginas até esgotar
// o resultado ou o callback pedir parada. Útil quando o consumer não quer
// gerenciar offset manualmente e o resultado total é grande (chm13:
// FIND SEGMENT RETURN id,name = 4,29M linhas).
//
// NÃO é "true streaming" — internamente paga 1 execute por página (cada
// uma materializa até `page_size` linhas). O ganho é UX (1 chamada vs N)
// e memória limitada (nunca mais de `page_size` linhas vivas ao mesmo
// tempo). True streaming exigiria refactor do planner; ver §H roadmap.
//
// O callback recebe:
//   - result: GgqlResultHandle válido pelo tempo desta linha
//             (acesse via ggql_result_cell_*; row=`row_in_page`)
//   - row_in_page: índice [0, ggql_result_row_count(result))
//   - global_row: índice 0-based desde o início da query (atravessa páginas)
//   - user_data: passado opaco
// Retorne `false` para parar (linhas restantes não serão emitidas).
//
// Retorna o total de linhas emitidas (após callback retornar true), ou:
//   -1: argumentos inválidos
//   -2: erro de parse/execução (consulte ggql_engine_last_error)
typedef bool (*GgqlRowCallback)(
    GgqlResultHandle result,
    int32_t row_in_page,
    int64_t global_row,
    void* user_data
);

int64_t ggql_engine_execute_streaming(
    GgqlEngineHandle engine,
    const char* query,
    uint32_t page_size,         // 0 → default 1024
    GgqlRowCallback cb,
    void* user_data
);

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
