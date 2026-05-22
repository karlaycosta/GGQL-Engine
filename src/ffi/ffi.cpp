#include "ffi/ffi.h"
#include "ggql/cli/console.hpp"
#include "ggql/ingest/ingestor.hpp"
#include "ggql/ingest/metadata_store.hpp"
#include "ggql/ingest/profile.hpp"
#include "ggql/query/entity_columns.hpp"
#include "ggql/query/grammar.hpp"
#include "ggql/query/query_context.hpp"
#include "ggql/query/query_planner.hpp"
#include "ggql/storage/fm_index.hpp"

#include "ggql/storage/topology.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
    // #1A (2026-05-20): lazy FM-Index. O fm.load() é caro (multi-GB em
    // pangenoma grande); só carregamos na primeira query que precisa
    // (SEARCH KMER / WHERE contains_kmer). Plan
    //   - cache_dir_for_fm: caminho retido p/ chamar load() depois
    //   - fm_load_once: garante 1 só load por engine
    //   - fm_attempted: fast-path sem entrar no call_once após primeira tentativa
    //   - planner é shared_ptr p/ permitir swap atômico após fm_load
    //   - planner_mutex protege o swap (raríssimo) das leituras em execute
    std::string cache_dir_for_fm;
    std::once_flag fm_load_once;
    std::atomic<bool> fm_attempted{false};
    mutable std::mutex planner_mutex;
    std::shared_ptr<QueryPlanner> planner;
    // Round 4 (P1/C9 — 2026-05-22): last_error movido para `thread_local
    // g_last_engine_error` no namespace anônimo. Evita race entre threads
    // que chamam engine_execute concorrentemente no mesmo engine handle.
    // O símbolo `last_error` deixou de existir no wrapper — leituras antigas
    // via member access falham em build, forçando audit dos call sites.
    // Round 3.F: cancel cooperativo (checado em Paginator::advance via
    // ctx::cancel_requested). Reset no início de cada engine_execute.
    std::atomic<bool> cancel_requested{false};
    // Round 3.G: override per-engine de kMaxTotalHitsCount. 0 = default.
    std::atomic<std::size_t> max_total_hits_override{0};
};

// #1A FFI: o FM-Index só é consumido por SEARCH KMER / WHERE contains_kmer.
// "KMER" (case-insensitive) só ocorre nesses dois construtos da grammar GGQL;
// um falso-positivo apenas tenta o load à toa (sem risco de correção).
// Espelha tests/validation/run_queries.cpp:query_needs_fm (commit 458e127).
[[nodiscard]] static bool query_needs_fm(std::string_view q) noexcept {
    static constexpr std::string_view kK = "KMER";
    if (q.size() < kK.size()) { return false; }
    for (size_t i = 0; i + kK.size() <= q.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < kK.size(); ++j) {
            char c = q[i + j];
            if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 32); }
            if (c != kK[j]) { match = false; break; }
        }
        if (match) { return true; }
    }
    return false;
}

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
//
// C1 (Round 4 / 2026-05-22): arena CIRCULAR de N slots em vez de um buffer
// único reusado. Motivação: NativeCallable.listener (Dart) processa eventos
// em microtask DEPOIS do callback C retornar — antes do fix o handler
// async lia garbage quando outro evento sobrescrevia text_buf/detail_buf
// no intervalo. Pool de N=16 dá margem segura: emit avança o slot, slot N-16
// só é reusado após 16 emits subsequentes. Cliente que processa em <16
// callbacks de atraso lê string consistente.
//
// Janela de race residual: se >=16 eventos forem emitidos antes do
// consumer processar, slot 0 é sobrescrito. Em prática ingest emite
// ~10 eventos/s e Dart processa microtask em ~ms — efetivamente seguro.
// True streaming-safe exigiria heap alloc + ack do consumer (ABI break).
struct EventBridge {
    GgqlEventCallback fn;
    void* user_data;
    static constexpr size_t kArenaSize = 16;
    std::array<std::string, kArenaSize> text_arena;
    std::array<std::string, kArenaSize> detail_arena;
    size_t arena_idx = 0;
};

// noexcept enforça que o boundary FFI nunca propague exceção C++ para o
// callback C/Dart. Em caso patológico (bad_alloc nos buffers), terminate
// é preferível a UB silencioso atravessando boundary.
void dispatch_bridge(const cli::Event& ev, void* user) noexcept {
    auto* b = static_cast<EventBridge*>(user);
    // thread_local: alguns consumers (Dart NativeCallable.listener) processam
    // o GgqlEvent assincronamente — o ponteiro precisa permanecer válido após
    // este callback retornar. Variável thread_local persiste pela vida da
    // thread. Consumers síncronos (Python ctypes, Dart isolateLocal) não
    // são afetados.
    thread_local GgqlEvent out;
    std::memset(&out, 0, sizeof(out));

    out.kind = static_cast<int32_t>(ev.kind);
    out.phase_number = ev.phase_number;

    // C1: arena circular. Consome 1 slot por evento com strings; out.text/
    // detail apontam para o slot atual, que sobrevive por kArenaSize-1
    // eventos antes de ser reusado.
    if (!ev.text.empty() || !ev.detail.empty()) {
        const size_t slot = b->arena_idx % EventBridge::kArenaSize;
        ++b->arena_idx;
        if (!ev.text.empty()) {
            b->text_arena[slot].assign(ev.text.data(), ev.text.size());
            out.text = b->text_arena[slot].c_str();
        }
        if (!ev.detail.empty()) {
            b->detail_arena[slot].assign(ev.detail.data(), ev.detail.size());
            out.detail = b->detail_arena[slot].c_str();
        }
    }

    out.percent = ev.percent;
    out.eta_s = ev.eta_s;
    out.rate_mbs = ev.rate_mbs;

    b->fn(&out, b->user_data);
}

// ── Helpers Round 2 (têm template/constexpr — não podem viver em extern "C") ──

// Round 2.D: buffer thread_local para validate_error_message
thread_local std::string g_validate_error_buf;

// Round 2.E: schema introspection (fonte única em
// include/ggql/query/entity_columns.hpp). Convertemos
// std::array<string_view,N> → C-array de const char* uma vez (constexpr).
static const char* const kEntityNames[] = {
    "SEGMENT", "EDGE", "PATH", "BUBBLE", "TIP", "REPEAT"
};
static constexpr int32_t kEntityCount =
    sizeof(kEntityNames) / sizeof(kEntityNames[0]);

template <size_t N>
[[nodiscard]] static constexpr auto to_c_array(
    const std::array<std::string_view, N>& src) {
    std::array<const char*, N> r{};
    for (size_t i = 0; i < N; ++i) { r[i] = src[i].data(); }
    return r;
}

static constexpr auto kSegmentColsC = to_c_array(ggql::query::kSegmentColumns);
static constexpr auto kEdgeColsC    = to_c_array(ggql::query::kEdgeColumns);
static constexpr auto kPathColsC    = to_c_array(ggql::query::kPathColumns);
static constexpr auto kBubbleColsC  = to_c_array(ggql::query::kBubbleColumns);
static constexpr auto kNodeColsC    = to_c_array(ggql::query::kNodeColumns);

// Compara identificadores ASCII case-insensitive (sem locale).
[[nodiscard]] static bool ascii_iequal(std::string_view a,
                                       std::string_view b) noexcept {
    if (a.size() != b.size()) { return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') { ca = static_cast<char>(ca - 32); }
        if (cb >= 'a' && cb <= 'z') { cb = static_cast<char>(cb - 32); }
        if (ca != cb) { return false; }
    }
    return true;
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

// C2/P10 (Fase 3 / 2026-05-22): cancel cooperativo do ingest, simétrico a
// ggql_engine_cancel. Chamado tipicamente da thread UI durante um ingest
// longo (HPRC ~24min). Worker checa em checkpoints entre fases.
void ggql_ingestor_cancel(GgqlIngestorHandle handle) {
    if (!handle) { return; }
    static_cast<GfaIngestorMmapParallel*>(handle)->cancel();
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

    // E3 (Round 4 / 2026-05-22): emite SEMPRE um evento terminal (Ok ou
    // Fail) antes de remover o hook. Garantia documentada no header:
    // "o último evento emitido antes de ingest_*() retornar é
    // GGQL_EV_OK (kind=6) ou GGQL_EV_FAIL (kind=7) top-level".
    // Consumer pode usar como sentinela para fechar callbacks/streams
    // sem precisar de `Future.delayed(Nms)` mágico para drenar a fila.
    //
    // Disparado via dispatch_bridge diretamente (não via cli::set/clear)
    // para garantir ordem: terminal vem POR ÚLTIMO mesmo se o ingest já
    // emitiu seus próprios Ok/Fail durante a execução. Consumer detecta
    // pelo `text == "__ggql_ingest_done__"` (sentinela específico,
    // evita colisão com Ok/Fail intermediários gerados pelo cli::).
    if (event_cb) {
        cli::Event terminal{};
        terminal.kind = stats.success ? cli::EventKind::Ok
                                      : cli::EventKind::Fail;
        terminal.text = std::string_view{"__ggql_ingest_done__"};
        terminal.detail = stats.success
            ? std::string_view{}
            : std::string_view{stats.error_message ? stats.error_message
                                                   : "ingest_failed"};
        dispatch_bridge(terminal, &bridge);
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

// Round 1.C (2026-05-20): controle granular — `build_gbwt`/`build_snarls`
// expostos individualmente. Permite combos fora dos 4 perfis fixos.
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
) {
    return do_ingest_ffi(handle, filepath, num_threads, gamma, verbose,
                         save_metadata, build_topology, build_fm_index,
                         build_gbwt, build_snarls,
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

// Round 4 (P1/C9 — 2026-05-22): last_error por thread no caminho de query.
// Antes era membro de GgqlEngineWrapper, com race entre threads
// concorrentes no mesmo engine. Agora cada thread tem seu próprio buffer —
// `ggql_engine_last_error()` retorna o erro da última chamada FFI feita
// NESTA thread (regra "thread_local" já documentada em outros buffers).
thread_local std::string g_last_engine_error;

GgqlEngineHandle ggql_engine_load(const char* cache_dir) {
    if (!cache_dir) {
        engine_create_error = "cache_dir não pode ser NULL";
        return nullptr;
    }
    try {
        auto* w = new GgqlEngineWrapper();
        w->metadata.load(std::string(cache_dir));
        // #1A FFI (2026-05-20): NÃO carrega FM-Index aqui. É caro (multi-GB
        // em pangenoma grande — GRCh38 ~8 s) e a maioria das queries não
        // usa. Será carregado lazy em ggql_engine_execute na primeira query
        // que precisar (KMER). cache_dir_for_fm guarda o caminho.
        w->cache_dir_for_fm = std::string(cache_dir);
        // Topology opcional — perfis sem build_topology não terão E_*/ef_*.
        // Se ausente, FIND TIP/EDGE/PATH/BUBBLE/REPEAT (e crosses no PATH)
        // retornarão erro semântico via planner; SEGMENT/SEARCH KMER continuam.
        try {
            GfaCsrTopology topo;
            topo.load(std::string(cache_dir));
            w->owned_topology.emplace(std::move(topo));
            w->topology = &*w->owned_topology;
        } catch (...) {}
        // Planner começa SEM fm; lazy-rebuild no primeiro KMER (ver execute).
        w->planner = std::make_shared<QueryPlanner>(
            w->topology,
            &w->metadata,
            /*seq_index=*/nullptr
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
        // #1A FFI: FM-Index lazy (mesma motivação de ggql_engine_load).
        w->cache_dir_for_fm = std::string(cache_dir);
        w->planner = std::make_shared<QueryPlanner>(
            w->topology,
            &w->metadata,
            /*seq_index=*/nullptr
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
    // C9: lê do thread_local; ignora o handle (mantido na assinatura para
    // simetria e possível evolução futura). Buffer escrito pela mesma thread
    // em qualquer chamada FFI que falhou (execute, load lazy do FM-Index, …).
    (void)engine;
    return g_last_engine_error.c_str();
}

// Round 3.F (2026-05-20): cancel cooperativo.
void ggql_engine_cancel(GgqlEngineHandle engine) {
    if (!engine) { return; }
    auto* w = static_cast<GgqlEngineWrapper*>(engine);
    w->cancel_requested.store(true, std::memory_order_release);
}

// Round 3.G (2026-05-20): cap per-engine.
void ggql_engine_set_max_total_hits(GgqlEngineHandle engine, uint64_t cap) {
    if (!engine) { return; }
    auto* w = static_cast<GgqlEngineWrapper*>(engine);
    w->max_total_hits_override.store(static_cast<std::size_t>(cap),
                                     std::memory_order_release);
}

// Round 3.H (2026-05-20): streaming-via-pagination. Re-executa em
// páginas, chama callback row-a-row; para se cb=false, esgotamento ou
// cancel cooperativo (Round 3.F).
int64_t ggql_engine_execute_streaming(
    GgqlEngineHandle engine,
    const char*      query,
    uint32_t         page_size,
    GgqlRowCallback  cb,
    void*            user_data
) {
    if (!engine || !query || !cb) { return -1; }
    if (page_size == 0) { page_size = 1024; }
    int64_t total_emitted = 0;
    uint32_t offset = 0;
    bool keep_going = true;
    while (keep_going) {
        // Cancel checado entre páginas (e dentro do execute via Paginator).
        if (static_cast<GgqlEngineWrapper*>(engine)
              ->cancel_requested.load(std::memory_order_acquire)) {
            break;
        }
        GgqlResultHandle r = ggql_engine_execute(engine, query, page_size, offset);
        if (!r) {
            return total_emitted > 0 ? total_emitted : int64_t{-2};
        }
        const int64_t n = ggql_result_row_count(r);
        for (int64_t i = 0; i < n; ++i) {
            if (!cb(r, static_cast<int32_t>(i), total_emitted, user_data)) {
                keep_going = false;
                break;
            }
            ++total_emitted;
        }
        ggql_result_free(r);
        // Fim natural: página devolveu menos linhas que pedimos.
        if (static_cast<uint32_t>(n) < page_size) { break; }
        offset += page_size;
    }
    return total_emitted;
}

// Round 1.B (2026-05-20): introspecção de capabilities sem rodar queries.
int32_t ggql_engine_capabilities(GgqlEngineHandle engine,
                                 GgqlEngineCapabilities* out) {
    if (!engine || !out) { return -1; }
    auto* w = static_cast<GgqlEngineWrapper*>(engine);
    *out = {};
    // Metadata sempre carregado em load/create se chegamos a um handle válido.
    out->has_metadata = true;
    out->segment_count = static_cast<uint64_t>(w->metadata.size());
    // Topology + derivados
    out->has_topology = (w->topology != nullptr);
    if (w->topology) {
        out->edge_count = w->topology->num_edges();
        out->path_count = static_cast<uint64_t>(w->topology->path_count());
        out->has_gbwt = w->topology->has_path_storage_backend();
        out->has_snarls = w->topology->has_snarls();
    }
    // FM-Index: já carregado (pós primeiro KMER) OU disponível no disco
    // (#1A lazy — fm_index.bin existe mas ainda não foi materializado).
    // Ambos os casos significam "SEARCH KMER funcionará" do ponto de
    // vista do consumer.
    out->has_fm_index = w->fm_index.is_loaded();
    if (!out->has_fm_index && !w->cache_dir_for_fm.empty()) {
        std::error_code ec;
        out->has_fm_index = std::filesystem::exists(
            std::filesystem::path(w->cache_dir_for_fm) / "fm_index.bin", ec);
    }
    return 0;
}

// ── Round 2.D (2026-05-20): validate parse-only ──────────────────────────────
// Buffer thread_local em g_validate_error_buf (namespace anônimo acima do
// extern "C"; thread_local std::string não pode ficar dentro de extern "C").
int32_t ggql_engine_validate(GgqlEngineHandle /*engine*/,
                             const char* query,
                             GgqlParseResult* out) {
    if (!query || !out) { return -1; }
    *out = {};
    out->error_line = -1;
    out->error_col = -1;
    auto parsed = grammar::parse_ggql(query);
    if (parsed) {
        out->valid = true;
        out->error_message = nullptr;
        return 0;
    }
    out->valid = false;
    g_validate_error_buf = std::move(parsed.error());
    out->error_message = g_validate_error_buf.c_str();
    return 1;
}

// ── Round 2.E (2026-05-20): schema introspection ─────────────────────────────
// Statics (kEntityNames, kSegmentColsC, …) e ascii_iequal estão no namespace
// anônimo acima do extern "C" (template/constexpr exigem C++ linkage).
const char* const* ggql_engine_list_entities(int32_t* out_count) {
    if (out_count) { *out_count = kEntityCount; }
    return kEntityNames;
}

const char* const* ggql_engine_describe_entity(const char* entity_name,
                                               int32_t* out_count) {
    if (out_count) { *out_count = 0; }
    if (!entity_name) { return nullptr; }
    const std::string_view name{entity_name};
    if (ascii_iequal(name, "SEGMENT")) {
        if (out_count) { *out_count = static_cast<int32_t>(kSegmentColsC.size()); }
        return kSegmentColsC.data();
    }
    if (ascii_iequal(name, "EDGE")) {
        if (out_count) { *out_count = static_cast<int32_t>(kEdgeColsC.size()); }
        return kEdgeColsC.data();
    }
    if (ascii_iequal(name, "PATH")) {
        if (out_count) { *out_count = static_cast<int32_t>(kPathColsC.size()); }
        return kPathColsC.data();
    }
    if (ascii_iequal(name, "BUBBLE")) {
        if (out_count) { *out_count = static_cast<int32_t>(kBubbleColsC.size()); }
        return kBubbleColsC.data();
    }
    if (ascii_iequal(name, "TIP") || ascii_iequal(name, "REPEAT")) {
        if (out_count) { *out_count = static_cast<int32_t>(kNodeColsC.size()); }
        return kNodeColsC.data();
    }
    return nullptr;
}

GgqlResultHandle ggql_engine_execute(
    GgqlEngineHandle engine,
    const char*      query_str,
    uint32_t         limit,
    uint32_t         offset
) {
    if (!engine || !query_str) { return nullptr; }
    auto* w = static_cast<GgqlEngineWrapper*>(engine);

    // #1A FFI (2026-05-20): lazy-load do FM-Index na primeira KMER.
    // Fast-path com atomic load (relaxed→acquire só na 1ª travessia).
    if (query_needs_fm(query_str) &&
        !w->fm_attempted.load(std::memory_order_acquire)) {
        std::call_once(w->fm_load_once, [w]() {
            if (auto r = w->fm_index.load(w->cache_dir_for_fm); r) {
                if (w->fm_index.is_loaded()) {
                    // Reconstruct planner com fm ponteiro válido.
                    auto new_planner = std::make_shared<QueryPlanner>(
                        w->topology, &w->metadata, &w->fm_index);
                    std::lock_guard lk(w->planner_mutex);
                    w->planner = std::move(new_planner);
                }
            } else {
                // Corrupção/ausência: registra no last_error para o caller
                // diagnosticar; planner segue com nullptr fm e SEARCH KMER
                // retornará erro semântico.
                g_last_engine_error = std::string("fm_index: ") + r.error();
            }
            w->fm_attempted.store(true, std::memory_order_release);
        });
    }

    // Parse da query
    auto parsed = grammar::parse_ggql(query_str);
    if (!parsed) {
        g_last_engine_error = std::move(parsed.error());
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

    // #1A FFI: snapshot do planner sob lock breve (swap pós-lazy-fm é raro).
    std::shared_ptr<QueryPlanner> planner_snap;
    {
        std::lock_guard lk(w->planner_mutex);
        planner_snap = w->planner;
    }
    if (!planner_snap) {
        g_last_engine_error = "engine não inicializado corretamente";
        (void)w;
        return nullptr;
    }
    // Round 3.F: reset do cancel flag para esta query. Round 3.G: instala
    // override de cap (se setado). ScopedContext garante restore.
    w->cancel_requested.store(false, std::memory_order_release);
    ggql::query::ctx::ScopedContext qctx(
        &w->cancel_requested,
        w->max_total_hits_override.load(std::memory_order_acquire));
    auto exec = planner_snap->execute(parsed.value());
    if (!exec) {
        g_last_engine_error = std::move(exec.error());
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
