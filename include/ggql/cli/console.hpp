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
#include <cstdint>
#include <initializer_list>
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
[[nodiscard("string formatada deve ser usada (aloca)")]]
std::string format_count(std::size_t n);

// "a · b · c" — junta partes não vazias com " · " (espaço-bullet-espaço).
// Padrão visual da árvore: {quantidade · tamanho · tempo}.
// Partes vazias são puladas — facilita compor rhs condicional.
[[nodiscard("string formatada deve ser usada (aloca)")]]
std::string format_summary(std::initializer_list<std::string_view> parts);

// ─── RAII para fases ─────────────────────────────────────────────────────────
// Abre uma fase no ctor e fecha automaticamente no dtor (chama phase_end()).
// Garante a regra "uma fase = um └─" mesmo em paths com early-return ou throw.
//
// Uso:
//   {
//       cli::PhaseBlock p(1, "Ingestão de IDs");
//       cli::step_ok("Passada 1", "...");
//       cli::step_ok_end("Union-Find + bin-packing", "...");
//   }  // ← phase_end() automático aqui
//
// Se o último step da fase já foi um *_end (que emite └─), o phase_end()
// do dtor é no-op visual — mas continua emitindo o evento PhaseEnd para
// consumers FFI. Para suprimir explicitamente, use release().
class PhaseBlock {
  public:
    PhaseBlock(int number, std::string_view title);
    explicit PhaseBlock(std::string_view title);
    ~PhaseBlock();
    PhaseBlock(const PhaseBlock&)            = delete;
    PhaseBlock& operator=(const PhaseBlock&) = delete;
    PhaseBlock(PhaseBlock&&)                 = delete;
    PhaseBlock& operator=(PhaseBlock&&)      = delete;
    // Marca a fase como já fechada (algum step_*_end foi chamado pelo
    // usuário). Suprime phase_end() no dtor para evitar dupla emissão.
    void release() noexcept { released_ = true; }

  private:
    bool released_ = false;
};

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
//
// O callback NÃO pode lançar exceções C++ — atravessar boundary FFI com
// exceção é UB. O typedef abaixo enforça isso em compile-time via noexcept.

enum class EventKind : std::int32_t {
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
    // Sentinel defensivo: Event{} default-construído é tratado como Note vazio
    // por consumers — evita UB de leitura de enum indeterminado.
    EventKind kind = EventKind::Note;
    std::int32_t phase_number = -1;  // Só PhaseStart; -1 quando ausente.
    std::string_view text;           // Label/título. Vazio se não se aplica.
    std::string_view detail;         // rhs/contexto. Vazio se não se aplica.
    double percent = 0.0;            // Só Progress.
    double eta_s = 0.0;              // Só Progress.
    double rate_mbs = 0.0;           // Só Progress (valor bruto; rate_unit é em detail).
};

using EventCallback = void (*)(const Event& ev, void* user_data) noexcept;

void set_event_callback(EventCallback cb, void* user_data) noexcept;
void clear_event_callback() noexcept;

} // namespace ggql::cli
