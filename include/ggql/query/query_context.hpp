// query_context.hpp — Contexto thread-local da query em execução.
//
// Round 3 (2026-05-20) adiciona dois canais por thread sem alterar a
// assinatura de QueryPlanner::execute (que já é pesada):
//
//   - cancel_token: ponteiro para std::atomic<bool>. A query checa
//     periodicamente (Paginator::advance) e aborta se setado. Quem chama
//     ggql_engine_cancel() escreve true a partir de outra thread.
//
//   - max_total_hits_override: override per-call do
//     `kMaxTotalHitsCount()`. 0 = usar default/env var. Permite
//     ggql_engine_set_cap() sem mexer em variável de ambiente global.
//
// Storage thread_local em src/query/query_planner.cpp. Use `ScopedContext`
// (RAII) para empilhar/desempilhar — segura em queries aninhadas.

#ifndef GGQL_QUERY_QUERY_CONTEXT_HPP
#define GGQL_QUERY_QUERY_CONTEXT_HPP

#include <atomic>
#include <cstddef>

namespace ggql::query::ctx {

// Setters baixo-nível (preferir ScopedContext em código de aplicação).
void set_cancel_token(std::atomic<bool>* p) noexcept;
void set_max_total_hits_override(std::size_t cap) noexcept;

// Getters / consulta — usados pelo Paginator e por kMaxTotalHitsCount().
[[nodiscard]] bool cancel_requested() noexcept;
[[nodiscard]] std::size_t max_total_hits_override() noexcept;

// RAII: instala os dois canais na construção; restaura o valor anterior
// na destruição. Garante restore mesmo após exceção.
class ScopedContext {
public:
    ScopedContext(std::atomic<bool>* cancel, std::size_t max_hits_override) noexcept;
    ~ScopedContext() noexcept;
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    ScopedContext(ScopedContext&&) = delete;
    ScopedContext& operator=(ScopedContext&&) = delete;
private:
    std::atomic<bool>* prev_cancel_;
    std::size_t prev_max_hits_;
};

} // namespace ggql::query::ctx

#endif // GGQL_QUERY_QUERY_CONTEXT_HPP
