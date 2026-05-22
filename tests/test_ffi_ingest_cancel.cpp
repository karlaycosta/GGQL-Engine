// test_ffi_ingest_cancel.cpp — Testes da FFI ggql_ingestor_cancel (C2 / P10).
//
// Cobre:
//   - ggql_ingestor_cancel(NULL) é no-op (não crasha).
//   - Ingest normal (sem cancel) tem success=true.
//   - cancel ARMADO ANTES de ingest é resetado no início — ingest sucede.
//   - cancel CONCORRENTE durante ingest aborta com error_message=="ingest cancelled".
//
// Requer um GFA de teste. Usa $GGQL_TEST_GFA (override) ou
// ../data/chm13_subset_10w.gfa (default — 439 MB, ~5–15s ingest, dá tempo
// pro cancel concorrente chegar).
//
// Para CI rápido, override com um GFA menor; o caso (4) pode não disparar
// o cancel a tempo e o teste apenas assert que se chegou, foi correto.

#include "ffi/ffi.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

static const char* get_gfa() {
    const char* env = std::getenv("GGQL_TEST_GFA");
    if (env && *env) { return env; }
    return "../data/chm13_subset_10w.gfa";
}

static void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FALHA: %s\n", msg);
        std::exit(1);
    }
}

static std::string make_tmp_cache_dir() {
    namespace fs = std::filesystem;
    auto base = fs::temp_directory_path() / "ggql_cancel_test_";
    base += std::to_string(static_cast<long>(::getpid()));
    base += "_";
    base += std::to_string(static_cast<long>(
        std::chrono::system_clock::now().time_since_epoch().count() & 0xffff));
    fs::create_directories(base);
    return base.string();
}

int main() {
    const char* gfa = get_gfa();

    // ── 1. cancel(NULL) — no-op ─────────────────────────────────────────────
    ggql_ingestor_cancel(nullptr);  // não deve crashar

    // ── 2. Ingest normal sem cancel ─────────────────────────────────────────
    {
        if (!std::filesystem::exists(gfa)) {
            std::fprintf(stderr, "AVISO: GFA ausente: %s — testes 2/3/4 pulados\n",
                         gfa);
            std::puts("test_ffi_ingest_cancel: parcial (cancel(NULL) OK)");
            return 0;
        }

        GgqlIngestorHandle h = ggql_ingestor_create();
        check(h != nullptr, "create() deve retornar handle");

        const std::string cache = make_tmp_cache_dir();
        GgqlIngestStats st = ggql_ingestor_ingest(
            h, gfa, 4, 1.0,
            /*verbose=*/false,
            /*save_metadata=*/true,
            /*build_topology=*/false,  // mais rápido (skip CSR/gBWT/snarls)
            /*build_fm_index=*/false,
            /*max_gbwt_paths=*/0,
            /*keep_gbwt_tmp=*/false,
            cache.c_str(),
            /*event_cb=*/nullptr,
            /*user_data=*/nullptr);
        check(st.success, "ingest normal deve ter success=true");
        check(st.segment_count > 0, "ingest normal deve produzir segments");

        ggql_ingestor_destroy(h);
        std::filesystem::remove_all(cache);
    }

    // ── 3. cancel armado ANTES de ingest é resetado ─────────────────────────
    {
        GgqlIngestorHandle h = ggql_ingestor_create();
        check(h != nullptr, "create() pré-cancel deve retornar handle");

        // Arma cancel ANTES de ingest começar. O flag deve ser resetado
        // no início do ingest (mesmo design de engine_execute) — ingest
        // completa normalmente.
        ggql_ingestor_cancel(h);

        const std::string cache = make_tmp_cache_dir();
        GgqlIngestStats st = ggql_ingestor_ingest(
            h, gfa, 4, 1.0,
            /*verbose=*/false, /*save_metadata=*/true,
            /*build_topology=*/false, /*build_fm_index=*/false,
            /*max_gbwt_paths=*/0, /*keep_gbwt_tmp=*/false,
            cache.c_str(), /*event_cb=*/nullptr, /*user_data=*/nullptr);
        check(st.success,
              "ingest pós-cancel-armado-precoce deve ter success=true (flag resetado)");

        ggql_ingestor_destroy(h);
        std::filesystem::remove_all(cache);
    }

    // ── 4. cancel CONCORRENTE durante ingest ────────────────────────────────
    // Thread B chama cancel ~20ms após o ingest começar. Em arquivos médios
    // (chm13_subset_10w = 439MB), ingest leva segundos, então o cancel
    // deve chegar entre fases. O teste é "best effort": valida o CONTRATO
    // (se cancelou, mensagem correta), não o timing exato.
    {
        GgqlIngestorHandle h = ggql_ingestor_create();
        check(h != nullptr, "create() concurrent-cancel deve retornar handle");

        std::atomic<bool> cancel_fired{false};
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            ggql_ingestor_cancel(h);
            cancel_fired.store(true);
        });

        const std::string cache = make_tmp_cache_dir();
        GgqlIngestStats st = ggql_ingestor_ingest(
            h, gfa, 4, 1.0,
            /*verbose=*/false, /*save_metadata=*/true,
            /*build_topology=*/true,  // path mais longo p/ aumentar chance do cancel
            /*build_fm_index=*/true,
            /*max_gbwt_paths=*/0, /*keep_gbwt_tmp=*/false,
            cache.c_str(), /*event_cb=*/nullptr, /*user_data=*/nullptr);
        canceller.join();
        check(cancel_fired.load(), "thread de cancel executou");

        if (st.success) {
            // Cancel chegou tarde demais (ingest terminou primeiro). Aceita —
            // ainda valida que o contrato não é violado.
            std::fprintf(stderr,
                         "INFO: cancel concorrente chegou tarde demais "
                         "(ingest sucedeu). GFA pode ser pequeno demais ou "
                         "CPU rápida. Use GGQL_TEST_GFA com arquivo maior "
                         "para exercitar o cancel real.\n");
        } else {
            // Cancel mordeu — deve ter a mensagem padronizada.
            check(st.error_message != nullptr,
                  "cancel-aborted ingest deve ter error_message não-NULL");
            check(std::strcmp(st.error_message, "ingest cancelled") == 0,
                  "cancel-aborted ingest deve ter error_message=='ingest cancelled'");
            std::fprintf(stderr, "INFO: cancel concorrente disparou — contrato OK\n");
        }

        ggql_ingestor_destroy(h);
        std::filesystem::remove_all(cache);
    }

    std::puts("test_ffi_ingest_cancel: todos os testes passaram.");
    return 0;
}
