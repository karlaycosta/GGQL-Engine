// ggql/cli/console.hpp — Vocabulário visual no estilo "passos" (tree view).
//
// Emite eventos estruturados para o terminal:
//
//   ○ Fase 1: Ingestão de IDs...
//   │  ✔ Mapeamento concluído (632 MB)
//   │  ✔ MPHF construído (1.90 MB)
//   └─
//
// Cada função representa UM EVENTO semântico (phase_start, step_ok, etc.).
// No futuro, um callback FFI (ver include/ffi/ffi.h) poderá se plugar
// aqui para espelhar os mesmos eventos para a GUI Flutter sem duplicar
// pontos de chamada no resto da engine.
//
// Gate de cor: por padrão auto-detecta TTY + NO_COLOR + TERM=dumb. Pode ser
// forçado via set_color_enabled().
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ggql::cli {

// ─── Gate de cor/TTY ──────────────────────────────────────────────────────────
// Primeira chamada decide; subsequentes retornam o valor cacheado.
[[nodiscard]] bool color_enabled() noexcept;
void set_color_enabled(bool on) noexcept;

// ─── Modo silencioso ──────────────────────────────────────────────────────────
// Quando ativo, todas as funções de saída retornam imediatamente sem escrever
// nada (eventos ainda são disparados). Útil para benchmarks e testes.
void set_quiet(bool on) noexcept;
[[nodiscard]] bool is_quiet() noexcept;

// ─── Títulos e seções (para relatórios fora da árvore de fases) ───────────────
// "title" é usado como banner/cabeçalho (substitui caixa ╔═╗).
void title(std::string_view text);
// "section" é um subtítulo dentro de um título (substitui "── X ──").
void section(std::string_view text);
// Imprime uma linha em branco (sem disparar evento).
void blank() noexcept;

// ─── Par chave-valor em colunas (relatório) ───────────────────────────────────
// "  {label:<32}  {value:>22}"
void kv(std::string_view label, std::string_view value);
void kv_time(std::string_view label, double seconds);
void kv_size(std::string_view label, std::size_t bytes);
void kv_count(std::string_view label, std::size_t n);
void kv_number(std::string_view label, double value, std::string_view unit = "");

// ─── Fases (top-level, cyan bold) ─────────────────────────────────────────────
// "○ Fase {n}: {title}..."
void phase(int number, std::string_view title);
// "○ {title}..."
void phase(std::string_view title);
// Fecha a fase atual: imprime "└─" + linha em branco.
void phase_end();

// ─── Substeps (filhos da fase atual, prefixo "│  ") ───────────────────────────
// "│  ✔ {text}"                  ou  "│  ✔ {text}  {rhs}" (rhs em dim)
void step_ok(std::string_view text);
void step_ok(std::string_view text, std::string_view rhs);

// "│  ✘ {text}"                  ou  "│  ✘ {text}  {rhs}"
void step_fail(std::string_view text);
void step_fail(std::string_view text, std::string_view rhs);

// "│  · {text}" — sub-linha neutra, para progresso/contexto.
void step_info(std::string_view text);
void step_info(std::string_view text, std::string_view rhs);

// ─── Ending steps (prefixo "└─ " e finalizam a fase) ─────────────────────────
// Úteis para encerrar uma fase com a última informação disponível na árvore.
void step_ok_end(std::string_view text);
void step_ok_end(std::string_view text, std::string_view rhs);
void step_fail_end(std::string_view text);
void step_fail_end(std::string_view text, std::string_view rhs);
void step_info_end(std::string_view text);
void step_info_end(std::string_view text, std::string_view rhs);

// ─── Linhas top-level (sem prefixo de árvore) ─────────────────────────────────
void ok(std::string_view text);
void ok(std::string_view text, std::string_view rhs);
void fail(std::string_view text);
void note(std::string_view text);

// ─── Formatadores auxiliares ──────────────────────────────────────────────────
// "4.294.760" — separador de milhar (estilo pt-BR: ponto).
[[nodiscard]] std::string format_count(std::size_t n);

// ─── Progresso dentro da fase atual ──────────────────────────────────────────
// "│  … {phase}  {percent:5.1f}%  ETA {eta}s  {rate}"
// Reimpresso no mesmo linha (\r) em TTY; emite 1 evento PROGRESS por update.
void step_progress(std::string_view phase, double percent, double eta_s, double rate_mbs,
                   std::string_view rate_unit = "MB/s");
void step_progress_finish() noexcept; // imprime \n no TTY (não emite evento)

// ─── Event stream (callback consumer) ────────────────────────────────────────
//
// Cada função pública (phase, step_ok, step_info, etc.) dispara UM evento.
// Serve para espelhar o que o terminal mostra em consumidores externos:
// hoje o FFI (include/ffi/ffi.h); potencialmente tracing, logging, etc.
//
// O callback é chamado na thread que emitiu o evento — pode ser chamado
// concorrentemente. O consumidor é responsável por serializar se precisar.
// As string_views do evento SÓ são válidas durante a chamada; copie se precisa
// reter.

enum class EventKind {
    PhaseStart = 1,
    PhaseEnd   = 2,
    StepOk     = 3,
    StepFail   = 4,
    StepInfo   = 5,
    Ok         = 6,
    Fail       = 7,
    Note       = 8,
    Progress   = 9,
};

struct Event {
    EventKind kind;
    int phase_number = 0;            // Só PhaseStart; 0 quando ausente.
    std::string_view text;           // Label/título. Vazio se não se aplica.
    std::string_view detail;         // rhs/contexto. Vazio se não se aplica.
    double percent = 0.0;            // Só Progress.
    double eta_s = 0.0;              // Só Progress.
    double rate_mbs = 0.0;           // Só Progress (valor bruto; rate_unit é em detail).
};

using EventCallback = void (*)(const Event& ev, void* user_data);

void set_event_callback(EventCallback cb, void* user_data) noexcept;
void clear_event_callback() noexcept;

} // namespace ggql::cli
