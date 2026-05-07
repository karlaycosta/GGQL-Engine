#pragma once

#include "ggql/cli/console.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ggql {

// Tracker de progresso thread-safe para fases de build/ingestão.
// Throttle por tempo (CAS sobre last_report_ns): apenas uma thread por janela
// de REPORT_INTERVAL_NS emite o update visual, independente de quantas cruzem
// o threshold simultaneamente.
//
// BUG-TOPO-3: generalizado com `unit` e `rate_divisor` para substituir
// GbwTracker (paths/s) e tracker inline de build_csr (nodes/s).
struct ProgressTracker {
    using SteadyTimePoint = std::chrono::time_point<std::chrono::steady_clock>;

    // Mutáveis em hot path (atomics).
    std::atomic<size_t> bytes_processed{0};
    std::atomic<int64_t> last_report_ns{0};

    // Imutáveis pós-construção. const documenta invariante e fortalece
    // thread-safety: leituras concorrentes em add_progress/finish ficam
    // garantidamente sem data race com mutação acidental do caller.
    const size_t total_bytes;
    const SteadyTimePoint start_time;
    const std::string phase_name;
    const bool verbose;
    const char* const unit;
    const double rate_divisor;

    static constexpr int64_t REPORT_INTERVAL_NS = 500'000'000; // 500 ms

    // noexcept: steady_clock::now() é noexcept por contrato C++; std::string
    // move ctor é noexcept desde C++11; demais inits são copies de POD.
    ProgressTracker(size_t total, std::string phase, bool v,
                    const char* unit_ = "MB/s", double rate_div = 1024.0 * 1024.0) noexcept
        : total_bytes(total),
          start_time(std::chrono::steady_clock::now()),
          phase_name(std::move(phase)),
          verbose(v),
          unit(unit_),
          rate_divisor(rate_div) {}

    void add_progress(size_t bytes) {
        if (!verbose) [[unlikely]] {
            return;
        }
        if (total_bytes == 0) [[unlikely]] {
            return; // tracker no-op (evita NaN em percent/rate)
        }
        const size_t current = bytes_processed.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        const bool is_done = current >= total_bytes;

        // Throttle unificado: aplicado tanto a updates intermediários quanto ao
        // is_done. Sem isso, N threads cruzando o threshold simultaneamente
        // emitiriam N updates "100%" — UI repetida e dispatch FFI duplicado.
        // O resumo final definitivo fica para finish() (chamado uma vez no
        // main thread, sem throttle).
        int64_t last = last_report_ns.load(std::memory_order_relaxed);
        const auto now_tp = std::chrono::steady_clock::now();
        const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   now_tp.time_since_epoch()).count();
        if (now_ns - last < REPORT_INTERVAL_NS) {
            return;
        }
        if (!last_report_ns.compare_exchange_strong(last, now_ns, std::memory_order_relaxed)) {
            return; // outra thread já está emitindo este update
        }

        const double elapsed_s = std::chrono::duration<double>(now_tp - start_time).count();
        // Usa 0.0001s como piso para evitar divisão por zero; não descarta eventos
        // de fases rápidas (o throttle de 500ms já evita spam).
        const double safe_elapsed = elapsed_s > 0.0001 ? elapsed_s : 0.0001;
        const double rate = current / safe_elapsed;
        // Guarda rate<=0: protege contra add_progress(0) que produz current=0
        // → rate=0 → remaining_s=+inf. Display "ETA inf" evitado.
        const double remaining_s =
            (is_done || rate <= 0.0) ? 0.0 : ((total_bytes - current) / rate);
        const double percent =
            (static_cast<double>(std::min(current, total_bytes)) / total_bytes) * 100.0;
        cli::step_progress(phase_name, percent, remaining_s,
                           rate / (rate_divisor > 0.0 ? rate_divisor : 1.0), unit);
    }

    void finish() {
        if (!verbose) [[unlikely]] {
            return;
        }
        if (total_bytes == 0) [[unlikely]] {
            return; // tracker no-op
        }
        // Emite sempre um resumo final com 100% e rate real, independente de quantos
        // eventos intermediários dispararam. Sem isso, fases rápidas (Release -O3)
        // completam em < 500ms e o usuário não vê nenhum dado.
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(now - start_time).count();
        const double safe_elapsed = elapsed_s > 0.0001 ? elapsed_s : 0.0001;
        const double total = static_cast<double>(total_bytes);
        const double rate = (total / safe_elapsed) / (rate_divisor > 0.0 ? rate_divisor : 1.0);
        cli::step_progress(phase_name, 100.0, 0.0, rate, unit);
        cli::step_progress_finish();
    }
};

} // namespace ggql
