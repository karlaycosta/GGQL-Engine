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

// Result struct para ggql_ingestor_ingest_*().
//
// C5 / Fase 5 (Round 4 / 2026-05-22): struct evolve-friendly via
// `struct_size`. Caller DEVE preencher `struct_size = sizeof(GgqlIngestStats)`
// ANTES de passar como out param. Engine checa que o tamanho passado é
// compatível (>= versão mínima esperada); rejeita com retorno -2 se
// caller passou um struct stale (recompile contra header novo).
//
// Quando o engine adicionar campos no futuro, o pattern é:
//   - Caller compilado contra header NOVO passa sizeof maior, recebe
//     todos os campos.
//   - Caller compilado contra header VELHO passa sizeof menor; engine
//     escreve só até o offset suportado pelo caller, garantindo que
//     campos novos não sobrescrevam memória do caller.
typedef struct {
    size_t   struct_size;      // = sizeof(GgqlIngestStats) — preenchido pelo caller
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
//
// C5 / Fase 5 (2026-05-22 — BREAKING vs versões anteriores): retorna
// `int32_t` em vez de `GgqlIngestStats` direto. Stats vai pelo out param
// `out_stats` — caller DEVE preencher `out_stats->struct_size =
// sizeof(GgqlIngestStats)` antes de chamar.
//
// Retorno:
//    0 — ingest OK; out_stats->success == true
//    1 — ingest falhou; out_stats->success == false (error_message preenchido
//        thread_local — válido até a próxima chamada FFI nesta thread)
//   -1 — argumentos NULL (handle, filepath, cache_dir ou out_stats)
//   -2 — out_stats->struct_size insuficiente (caller compilado contra
//        header antigo; recompile)
int32_t ggql_ingestor_ingest(
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
    void* user_data,
    GgqlIngestStats* out_stats
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

// ── Sugestão de perfil via varredura leve (2026-05-25; estendida 2026-05-26) ──
//
// Permite a UI/CLI obter um perfil sugerido ANTES de chamar ingest, baseado
// em contagens estruturais do GFA (S/L/W/P-lines + densidade). A varredura
// é paralela, não aloca nada além dos counters, e é tipicamente sub-segundo
// pra GFAs até dezenas de GB (limitada por largura de banda de RAM/disco).
//
// Custo: ~0,5 s pra GRCh38-464 (3,3 GB, 8 threads). Não toca em arquivos
// de cache (.idx); operação read-only sobre o GFA.
//
// Heurística de `suggested_profile` (para tooltip do badge "Recomendado"):
//   density = link_count / segment_count
//   - path_count == 0 && density > 1.5  → Assembly      (sem paths, ramificado)
//   - path_count == 0 && density ≤ 1.5  → Metagenomics  (sem paths, simples)
//   - path_count >  0 && density > 1.5  → Pangenome     (paths + emaranhado)
//   - path_count >  0 && density ≤ 1.5  → Comparative   (paths + simples)
// Threshold 1.5: pangenomas HPRC ficam ~1,4–1,5; assembly graphs ramificados
// passam disso. Calibrado empiricamente — pode evoluir com mais datasets.
typedef struct {
    size_t      struct_size;            // Caller seta = sizeof(GgqlSurvey).
    uint64_t    segment_count;          // S-lines (nós).
    uint64_t    link_count;             // L-lines (arestas).
    uint64_t    path_count;             // W-lines + P-lines combinados.
    bool        has_w;                  // C11/S2: true se ao menos 1 W-line foi vista.
    uint8_t     _pad1[7];
    uint64_t    total_sequence_bytes;   // C11/Q2: soma de seq das S-lines (≠ file_size).
    double      avg_path_length;        // C11/Q2: média de nós por path (0 se path_count==0).
    uint64_t    file_size_bytes;
    double      elapsed_s;
    uint8_t     suggested_profile;      // GgqlIngestProfile (cast pra/de int)
    uint8_t     _pad2[7];
    const char* error_message;          // NULL em sucesso; thread_local, válido
                                        // até próxima chamada FFI nesta thread.
} GgqlSurvey;

// Executa survey + heurística. Função stateless — não precisa de handle.
//
// Retorno:
//    0 — sucesso; out preenchido, error_message=NULL
//    1 — survey falhou; out->error_message preenchido (thread_local)
//   -1 — argumentos NULL (filepath ou out)
//   -2 — out->struct_size insuficiente (recompile)
int32_t ggql_survey_gfa(const char* filepath, int num_threads, GgqlSurvey* out);

// ── Estimativa de recursos por perfil (C11, 2026-05-26) ──────────────────────
//
// Combina um GgqlSurvey (resultado de ggql_survey_gfa) com flags de profile
// pra predizer RAM peak, espaço em disco do .idx, e tempo de ingest.
// Custo: <1 ms (puro CPU). Chame N vezes (1 por combo de flags) reusando o
// mesmo survey — caro foi o scan.
//
// Cada métrica vem com banda [lo, point, hi] — UI deve exibir como range
// `~lo–hi` honesto. `confidence_pct` reflete qualidade da calibração:
//   30  — preview (1 ponto medido; bands ±30% em torno do point)
//   70+ — calibrado contra suite multi-ponto (target após 5+ datasets)
typedef struct {
    size_t   struct_size;

    // Predições com banda lo/hi (em bytes / segundos)
    uint64_t ram_peak_bytes_lo;
    uint64_t ram_peak_bytes_point;
    uint64_t ram_peak_bytes_hi;
    uint64_t idx_disk_bytes_lo;
    uint64_t idx_disk_bytes_point;
    uint64_t idx_disk_bytes_hi;
    double   elapsed_s_lo;
    double   elapsed_s_point;
    double   elapsed_s_hi;

    // Métricas auxiliares
    uint8_t  confidence_pct;   // 0–100
    uint8_t  methodology;      // 0=stat · 1=survey · 2=survey+history
    uint8_t  _pad[6];
    const char* notes;         // NULL ou texto informativo; thread_local.
} GgqlIngestEstimate;

// Computa estimativas a partir de um survey + flags + threads.
//
// num_threads: 0 = detecta via omp_get_max_threads().
//
// Retorno:
//    0 — sucesso
//   -1 — argumentos NULL (survey ou out)
//   -2 — survey->struct_size ou out->struct_size insuficiente
int32_t ggql_estimate_ingest(
    const GgqlSurvey* survey,
    int num_threads,
    bool build_fm_index,
    bool build_gbwt,
    bool build_snarls,
    GgqlIngestEstimate* out
);

// Variante de ggql_ingestor_ingest que recebe perfil pronto. Equivalente a
// chamar ggql_ingestor_ingest com os flags derivados de `profile`.
// C5 / Fase 5: nova assinatura (ver ggql_ingestor_ingest acima).
int32_t ggql_ingestor_ingest_with_profile(
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
    void* user_data,
    GgqlIngestStats* out_stats
);

// Variante granular: expõe `build_gbwt` e `build_snarls` separadamente
// (na API legada eles eram acoplados a `build_topology`; nos perfis ficam
// fixos). Use para combos fora dos 4 perfis nomeados — p.ex. "topology
// com snarls mas sem gBWT", ou "gBWT sem snarls".
//
// Restrição: `build_gbwt`/`build_snarls` exigem `build_topology=true`.
// C5 / Fase 5: nova assinatura (ver ggql_ingestor_ingest acima).
int32_t ggql_ingestor_ingest_granular(
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
    void* user_data,
    GgqlIngestStats* out_stats
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

// ── Copy-out API para last_error (E2 / Fase 6, 2026-05-22) ───────────────────
//
// Copia a mensagem de erro para um buffer ownership-do-caller. Elimina a
// dependência do thread_local interno do engine — útil para wrappers
// idiomáticos (Dart, Python) que querem isolar o ciclo de vida do buffer
// da próxima chamada FFI feita pela mesma thread.
//
// Semântica (à la strlcpy do BSD):
//   - Retorna o número de bytes da mensagem, SEM contar '\0'.
//   - Sempre escreve '\0' em out[min(cap-1, len)] quando cap > 0 e out != NULL.
//   - Trunca se cap < len+1. Caller detecta truncamento com `retorno >= cap`.
//   - out == NULL OU cap == 0 → não escreve nada; retorna len (sondagem
//     de tamanho — caller pode dimensionar buffer com retorno + 1).
//
// engine == NULL → lê do buffer engine_create_error thread_local (mensagens
//   de ggql_engine_load / ggql_engine_create que falharam).
// engine != NULL → lê do g_last_engine_error thread_local (mensagens de
//   ggql_engine_execute, lazy-load do FM-Index, etc.).
size_t ggql_engine_last_error_into(GgqlEngineHandle engine,
                                   char* out,
                                   size_t cap);

// ── Introspecção de capabilities (Round 1.B, 2026-05-20) ──────────────────────
//
// Permite ao consumer descobrir o que este engine pode fazer SEM rodar uma
// query. Counts (segment/edge/path) vêm de metadata/topology em memória — fast.
//
// `has_fm_index` reflete "FM-Index está construído neste .idx" (true mesmo
// antes do lazy-load — checa existência de fm_index.bin no disco). UI pode
// usar para cinzar botão "Buscar k-mer" quando false.
typedef struct {
    size_t struct_size;     // C5/Fase 5: caller seta = sizeof(GgqlEngineCapabilities)
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
    size_t struct_size;        // C5/Fase 5: caller seta = sizeof(GgqlParseResult)
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

// ── Cursor stateful streaming (C7 / 2026-05-22) ───────────────────────────────
//
// True streaming via std::generator no engine. Mantém estado de iteração
// entre páginas — não re-parseia/re-planeja a query nem repete o skip
// interno (que tornava ggql_engine_execute_streaming O(N²) cumulativo).
//
// ESCOPO ATUAL (C7.1): apenas `FIND SEGMENT` em full-scan (sem caminhos
// rápidos de index — name/length/depth). Outros backends e SEARCH KMER
// retornam NULL com last_error setado — caller cai em
// ggql_engine_execute() ou ggql_engine_execute_streaming() legados.
// Refactor incremental por backend em C7.2+.
//
// Uso típico (C):
//   GgqlCursorHandle c = ggql_engine_execute_cursor(eng, "FIND SEGMENT");
//   if (!c) { /* lê ggql_engine_last_error(eng) */ return; }
//   while (1) {
//       GgqlResultHandle page = ggql_cursor_next(c, 1024);
//       if (!page) break;  // erro
//       int64_t rows = ggql_result_row_count(page);
//       if (rows == 0) { ggql_result_free(page); break; }  // fim
//       /* processa page */
//       ggql_result_free(page);
//   }
//   ggql_cursor_destroy(c);
//
// O cursor mantém referência ao engine — destruir o engine antes de
// ggql_cursor_destroy é UB.
typedef struct GgqlCursor_* GgqlCursorHandle;

// Cria cursor para a query. Retorna NULL em erro (mensagem em
// ggql_engine_last_error). NULL também se backend/forma não suportada
// pela versão atual (C7.1: só FIND SEGMENT full-scan).
GgqlCursorHandle ggql_engine_execute_cursor(GgqlEngineHandle engine,
                                            const char*      query);

// Materializa até `page_size` linhas. Retorna handle não-NULL mesmo no
// fim do stream (row_count == 0 indica esgotamento). NULL em erro.
// O chamador DEVE liberar com ggql_result_free().
GgqlResultHandle ggql_cursor_next(GgqlCursorHandle cursor, uint32_t page_size);

// Total cumulativo de linhas emitidas por este cursor desde criação.
int64_t ggql_cursor_total_emitted(GgqlCursorHandle cursor);

// true se o generator está esgotado (próxima next() vai retornar 0 linhas).
bool ggql_cursor_exhausted(GgqlCursorHandle cursor);

void ggql_cursor_destroy(GgqlCursorHandle cursor);

// ── Variante com cap per-query (C8 / Fase 6, 2026-05-22) ──────────────────────
//
// Igual a ggql_engine_execute, mas passa `cap` (override per-query do
// kMaxTotalHitsCount) inline em vez de via ggql_engine_set_max_total_hits.
// Resolve a limitação de cap per-engine em paralelismo: cada thread pode
// chamar execute_with_cap com seu próprio cap sem afetar outras queries.
//
// cap == 0 → usa o cap engine-level setado via set_max_total_hits (ou o
//             default ~1M se nenhum foi setado). Equivalente a chamar
//             ggql_engine_execute.
// cap > 0  → cap per-query, sobrescreve engine-level e default.
//
// Após Round 1.#4 do engine, counts estruturais não truncam — este cap
// só morde SEARCH KMER e queries WHERE complexas (proteção DoS).
GgqlResultHandle ggql_engine_execute_with_cap(
    GgqlEngineHandle engine,
    const char*      query,
    uint32_t         limit,
    uint32_t         offset,
    uint64_t         cap
);

// ── Subgrafo induzido para o GraphView (C12 / Round 5, 2026-05-29) ────────────
//
// O GraphView do cliente é result-driven: desenha o subgrafo dos nós de um
// RESULTADO de query, não a base inteira (render de 85M nós é inviável). Mas
// `FIND SEGMENT` devolve só nós (id/name), sem arestas — não há topologia para
// desenhar. Este símbolo recebe os segment ids de um resultado e devolve o
// SUBGRAFO INDUZIDO por eles + um HALO de `halo_hops` (vizinhos diretos como
// contexto), em índices LOCAIS densos [0, node_count), com cap rígido.
//
// Entrada por LISTA de segment ids (não por GgqlResultHandle): o cliente extrai
// os ids via ggql_result_cell_int do `FIND SEGMENT RETURN id` — esse id é o id
// interno denso (espaço MPHF) que indexa a topologia, sem descasamento. Ids
// fora de [0, num_nodes) ou duplicados são validados/dedupados internamente.
//
// Contrato two-pass (struct_size C5 + copy-out E2):
//   1ª passada (dims-only): edges/seg_ids/is_result = NULL e *_cap = 0. O engine
//     conta os nós (resultado + halo) e, se o total ultrapassar `node_cap`,
//     ABORTA cedo, devolve dims com truncated=true e NÃO materializa nada. O
//     cliente vê truncated → mostra "refine query" e NÃO faz a 2ª passada.
//   2ª passada (preenche): o cliente aloca pelos dims e chama de novo com os
//     buffers. O engine recomputa (topologia imutável → resultado idêntico) e
//     preenche cada buffer não-NULL fornecido.
//
// `node_cap` é o orçamento TOTAL (resultado + halo). node_cap <= 0 = sem cap.
// `halo_hops`: 0 (só o resultado) ou 1 (resultado + vizinhos diretos). Valores
//   > 1 retornam erro (-4) nesta versão — parâmetro fica genérico p/ evolução.
// `edges` são pares achatados [a0,b0,a1,b1,…] em índice LOCAL, NÃO-DIRECIONADOS
//   e DEDUPADOS (forma canônica min<max, sem self-loops): edges_cap conta ints
//   (= 2 × edge_count). `seg_ids[local]` = segment id global (labels/hit-test).
//   `is_result[local]` = 1 se nó do resultado, 0 se halo.
//
// Retorno: 0 = OK (dims preenchido; buffers preenchidos se fornecidos e cabem).
//   -1 = args NULL (engine/dims, ou seg_ids_in NULL com seg_count>0, ou seg_count<0).
//   -2 = dims->struct_size < sizeof(GgqlSubgraphDims).
//   -3 = topologia indisponível (engine sem CSR — ver has_topology).
//   -4 = halo_hops não suportado (só 0 ou 1 nesta versão).
//   -5 = buffer fornecido com capacidade insuficiente (edges_cap < 2×edge_count,
//        ids_cap < node_count ou flags_cap < node_count). Realoque pelos dims.
typedef struct {
    uint32_t struct_size;   // = sizeof(GgqlSubgraphDims) — preenchido pelo caller
    int64_t  node_count;    // total de nós no subgrafo (resultado + halo, após cap)
    int64_t  edge_count;    // arestas não-direcionadas dedupadas (edges tem 2× ints)
    int64_t  result_count;  // quantos dos node_count são do resultado (resto = halo)
    bool     truncated;     // node_cap estourado → cliente mostra "refine query"
} GgqlSubgraphDims;

int64_t ggql_engine_subgraph_from_result(
    GgqlEngineHandle  engine,
    const int64_t*    seg_ids_in,  int64_t seg_count,   // nós do resultado (ids globais)
    int32_t           halo_hops,   int64_t node_cap,
    GgqlSubgraphDims* dims,
    int32_t*          edges,       int64_t edges_cap,    // pares achatados, idx LOCAL
    int64_t*          seg_ids,     int64_t ids_cap,      // idx local → segment id global
    uint8_t*          is_result,   int64_t flags_cap     // 1 = resultado, 0 = halo
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
