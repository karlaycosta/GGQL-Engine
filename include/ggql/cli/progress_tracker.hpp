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
    std::atomic<size_t> bytes_processed{0};
    size_t total_bytes{0};
    std::chrono::time_point<std::chrono::steady_clock> start_time;
    std::atomic<int64_t> last_report_ns{0};
    std::string phase_name;
    bool verbose;
    const char* unit;
    double rate_divisor;

    static constexpr int64_t REPORT_INTERVAL_NS = 500'000'000; // 500 ms

    ProgressTracker(size_t total, std::string phase, bool v,
                    const char* unit_ = "MB/s", double rate_div = 1024.0 * 1024.0)
        : total_bytes(total), phase_name(std::move(phase)), verbose(v),
          unit(unit_), rate_divisor(rate_div) {
        start_time = std::chrono::steady_clock::now();
    }

    void add_progress(size_t bytes) {
        if (!verbose) {
            return;
        }
        size_t current = bytes_processed.fetch_add(bytes, std::memory_order_relaxed) + bytes;
        const bool is_done = current >= total_bytes;

        int64_t now_ns = 0;
        if (!is_done) {
            int64_t last = last_report_ns.load(std::memory_order_relaxed);
            now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
            if (now_ns - last < REPORT_INTERVAL_NS) {
                return;
            }
            if (!last_report_ns.compare_exchange_strong(last, now_ns, std::memory_order_relaxed)) {
                return;
            }
        }

        const auto now = (now_ns != 0)
            ? std::chrono::time_point<std::chrono::steady_clock>(std::chrono::nanoseconds(now_ns))
            : std::chrono::steady_clock::now();
        const double elapsed_s = std::chrono::duration<double>(now - start_time).count();
        // Usa 0.0001s como piso para evitar divisão por zero; não descarta eventos
        // de fases rápidas (o throttle de 500ms já evita spam).
        const double safe_elapsed = elapsed_s > 0.0001 ? elapsed_s : 0.0001;
        const double rate = current / safe_elapsed;
        const double remaining_s = is_done ? 0.0 : ((total_bytes - current) / rate);
        const double percent =
            (static_cast<double>(std::min(current, total_bytes)) / total_bytes) * 100.0;
        cli::step_progress(phase_name, percent, remaining_s,
                           rate / (rate_divisor > 0.0 ? rate_divisor : 1.0), unit);
    }

    void finish() {
        if (!verbose) {
            return;
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
