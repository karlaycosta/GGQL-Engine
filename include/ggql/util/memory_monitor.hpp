#pragma once

// MemoryMonitor + available_ram_kb — utilitários de monitoramento de RSS e
// RAM disponível em Linux. Header-only inline para evitar duplicação entre
// translation units.
//
// Antes deste header, MemoryMonitor estava duplicado em path_index.cpp e
// graph_algorithms.cpp, o que produzia drift entre as cópias (uma usava
// /proc/self/statm e compare_exchange_strong; a outra usava /proc/self/status
// e compare_exchange_weak). Esta versão consolidada incorpora as melhorias
// aplicadas em path_index pós-cleanup:
//
//   - Lê /proc/self/statm (1 linha) em vez de /proc/self/status
//     (parser linha-a-linha em ~50 linhas). ~5× mais barato por amostra.
//   - sysconf(_SC_PAGESIZE) cached em static const (1 syscall por programa).
//   - compare_exchange_strong (sem contention; elimina spin de spurious
//     failures do weak).
//   - [[nodiscard]] e noexcept onde aplicável.

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace ggql::util {

/// Helper para monitoramento isolado de Resident Set Size (RSS).
/// Usa std::jthread com stop_token (C++23) — auto-join no destrutor.
class MemoryMonitor {
  public:
    explicit MemoryMonitor(
        std::chrono::milliseconds interval = std::chrono::milliseconds(100)) {
        worker_ = std::jthread([this, interval](std::stop_token st) {
            while (!st.stop_requested()) {
                const size_t current = get_current_rss_kb();
                size_t m = peak_.load(std::memory_order_relaxed);
                while (current > m && !peak_.compare_exchange_strong(
                                          m, current,
                                          std::memory_order_release,
                                          std::memory_order_relaxed)) {
                }
                // Sleep interruptible: condition_variable_any::wait_for com
                // stop_token (C++20) acorda imediatamente em request_stop().
                // Sem isto, stop() bloquearia até interval (até 100 ms).
                // Predicate sempre-false: sem condição custom de break early
                // — apenas timeout, stop_token ou notify acordam o wait.
                std::unique_lock lk{wake_mtx_};
                wake_cv_.wait_for(lk, st, interval, [] { return false; });
            }
        });
    }

    // Suprime exceções de join() patológico (deadlock OS-level) — destrutor
    // não pode lançar. Match com pattern de ~SpinnerCtx em console.cpp.
    ~MemoryMonitor() {
        try {
            stop();
        } catch (...) {
            // Suprime — não pode lançar em destrutor.
        }
    }

    void stop() {
        worker_.request_stop();
        wake_cv_.notify_one(); // defensivo: 1 worker espera no CV
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] size_t peak_kb() const noexcept {
        return peak_.load(std::memory_order_acquire);
    }

    MemoryMonitor(const MemoryMonitor&) = delete;
    MemoryMonitor& operator=(const MemoryMonitor&) = delete;
    MemoryMonitor(MemoryMonitor&&) = delete;
    MemoryMonitor& operator=(MemoryMonitor&&) = delete;

  private:
    // /proc/self/statm: "<size> <resident> <shared> <text> <lib> <data> <dt>"
    // (em pages). Resident é o 2º campo. Mais barato que /proc/self/status.
    [[nodiscard]] static size_t get_current_rss_kb() noexcept {
        // sysconf não muda durante a vida do processo — cache para evitar
        // syscall a cada amostra (100 ms × N segundos de monitoramento).
        static const size_t page_kb =
            static_cast<size_t>(::sysconf(_SC_PAGESIZE)) / 1024;
        std::ifstream f("/proc/self/statm");
        size_t total_pages = 0;
        size_t rss_pages = 0;
        f >> total_pages >> rss_pages;
        return rss_pages * page_kb;
    }

    std::atomic<size_t> peak_{0};
    std::mutex wake_mtx_;
    std::condition_variable_any wake_cv_;
    std::jthread worker_;
};

/// Lê MemAvailable de /proc/meminfo (Linux).
/// Retorna 0 se não conseguir ler (fallback conservador no chamador).
[[nodiscard]] inline size_t available_ram_kb() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.starts_with("MemAvailable:")) {
            std::string_view sv(line);
            sv.remove_prefix(13);
            while (!sv.empty() && sv.front() == ' ') {
                sv.remove_prefix(1);
            }
            size_t val = 0;
            std::from_chars(sv.data(), sv.data() + sv.size(), val);
            return val;
        }
    }
    return 0;
}

} // namespace ggql::util
