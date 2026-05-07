// ggql/cli/console.cpp — Implementação do vocabulário visual "passos".

#include "ggql/cli/console.hpp"

#include "ggql/utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fmt/color.h>
#include <fmt/core.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace ggql::cli {

namespace {

// ─── Estado global ───────────────────────────────────────────────────────────

std::atomic<int>  g_color_state{-1}; // -1=uninit, 0=off, 1=on
std::atomic<bool> g_quiet{false};
std::atomic<bool> g_in_progress{false};
std::atomic<bool> g_is_phase{false};
std::atomic<bool> g_is_step{false};

// Serializa escritas concorrentes de step_progress (threads OpenMP).
// try_lock descarta updates extras — a display converge no próximo update.
std::mutex g_progress_mutex;

[[nodiscard]] bool detect_color() noexcept {
    if (const char* nc = std::getenv("NO_COLOR"); nc != nullptr && *nc != '\0') {
        return false;
    }
    if (const char* term = std::getenv("TERM"); term != nullptr) {
        if (std::string_view{term} == "dumb") {
            return false;
        }
    }
    return ::isatty(fileno(stderr)) != 0;
}

[[nodiscard]] fmt::text_style styled(fmt::text_style s) noexcept {
    return color_enabled() ? s : fmt::text_style{};
}

// ─── Glifos ──────────────────────────────────────────────────────────────────

constexpr std::string_view GLYPH_PHASE_DONE = "●";
constexpr std::string_view GLYPH_BAR = "│";
constexpr std::string_view GLYPH_END = "└─";
constexpr std::string_view GLYPH_OK = "✔";
constexpr std::string_view GLYPH_FAIL = "✘";
constexpr std::string_view GLYPH_DOT = "·";

constexpr std::array<std::string_view, 10> SPINNER_FRAMES = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                             "⠴", "⠦", "⠧", "⠇", "⠏"};

// ─── Estilos semânticos ──────────────────────────────────────────────────────

[[nodiscard]] fmt::text_style s_phase() noexcept {
    return fg(fmt::color::dark_orange) | fmt::emphasis::bold;
}
[[nodiscard]] fmt::text_style s_bar() noexcept {
    return fg(fmt::color::dim_gray);
}
[[nodiscard]] fmt::text_style s_ok() noexcept {
    return fg(fmt::color::orange) | fmt::emphasis::bold;
}
[[nodiscard]] fmt::text_style s_fail() noexcept {
    return fg(fmt::color::crimson) | fmt::emphasis::bold;
}
[[nodiscard]] fmt::text_style s_dot() noexcept {
    return fg(fmt::color::dim_gray);
}
[[nodiscard]] fmt::text_style s_rhs() noexcept {
    return fg(fmt::color::dim_gray);
}
[[nodiscard]] fmt::text_style s_label() noexcept {
    return fmt::text_style{};
}

// ─── Spinner ─────────────────────────────────────────────────────────────────

// Estado compartilhado com a thread do spinner.
// Escrito pelo main thread ANTES de criar a thread; lido pela thread do spinner.
// Acesso seguro: não há escrita concorrente (thread é criada depois dos writes).
struct SpinnerCtx {
    int phase_number = -1;
    std::string title;
    std::mutex wake_mtx;
    std::condition_variable wake_cv;
    std::thread thread;

    // Para e join imediato via CV — não bloqueia os 80 ms do sleep.
    // Seta g_in_progress=false antes do notify_all para garantir que a
    // thread do spinner saia do loop na próxima iteração (mesmo que perca
    // o sinal CV em uma race com wait_for).
    void stop() {
        if (!thread.joinable()) {
            return;
        }
        g_in_progress.store(false, std::memory_order_release);
        wake_cv.notify_all();
        thread.join();
    }

    // Garante join no exit — evita std::terminate() do ~thread() com thread joinable.
    // try/catch porque join() pode lançar std::system_error em casos patológicos
    // (deadlock detectado pelo OS), e exceção em destrutor → terminate.
    ~SpinnerCtx() {
        g_in_progress.store(false, std::memory_order_relaxed);
        if (thread.joinable()) {
            try {
                wake_cv.notify_all();
                thread.join();
            } catch (...) {
                // Suprime — não pode lançar em destrutor de objeto static.
            }
        }
    }
} g_spin;

void draw_finished_step() {
    fmt::print(stderr, "\r");
    fmt::print(stderr, styled(s_bar()), "{}  ", GLYPH_BAR);
    fmt::print(stderr, styled(s_dot()), "{} ", GLYPH_DOT);
    fmt::print(stderr, styled(s_label()), "{}", g_spin.title);
    fmt::print(stderr, "\033[K");
    std::fflush(stderr);
}

// Redesenha a linha de fase com o frame atual do spinner.
// Chamado DENTRO do g_progress_mutex (try_lock) — não interfere com step_progress.
void draw_spinner_frame(int frame) {
    fmt::print(stderr, "\r");
    if (g_is_step.load(std::memory_order_relaxed)) {
        fmt::print(stderr, styled(s_bar()), "{}  ", GLYPH_BAR);
        fmt::print(stderr, styled(s_phase()), "{} ", SPINNER_FRAMES[frame % 10]);
        fmt::print(stderr, styled(s_label()), "{}", g_spin.title);
    } else {
        fmt::print(stderr, styled(s_phase()), "{} ", SPINNER_FRAMES[frame % 10]);
        if (g_spin.phase_number >= 0) {
            fmt::print(stderr, styled(fmt::emphasis::bold), "Fase {}: {}", g_spin.phase_number,
                       g_spin.title);
        } else {
            fmt::print(stderr, styled(fmt::emphasis::bold), "{}", g_spin.title);
        }
    }
    fmt::print(stderr, "\033[K"); // limpa resíduos à direita
    std::fflush(stderr);
}

void draw_finished_phase() {
    fmt::print(stderr, "\r");
    fmt::print(stderr, styled(s_phase()), "{} ", GLYPH_PHASE_DONE);
    if (g_spin.phase_number >= 0) {
        fmt::print(stderr, styled(fmt::emphasis::bold), "Fase {}: {}", g_spin.phase_number,
                   g_spin.title);
    } else {
        fmt::print(stderr, styled(fmt::emphasis::bold), "{}", g_spin.title);
    }
    fmt::print(stderr, "\033[K");
    std::fflush(stderr);
}

void spinner_thread_fn() {
    int frame = 0;
    // Pequeno delay inicial para evitar "flash" de spinner em subetapas instantâneas.
    {
        std::unique_lock lk{g_spin.wake_mtx};
        if (g_spin.wake_cv.wait_for(lk, std::chrono::milliseconds(50)) != std::cv_status::timeout) {
            return; // Encerrado antes do primeiro frame
        }
    }

    while (true) {
        // Sai se a linha já foi fechada por um step_* ou phase_end.
        if (!g_in_progress.load(std::memory_order_acquire)) {
            break;
        }

        // try_lock: não interfere se step_progress estiver desenhando.
        std::unique_lock prog_lk{g_progress_mutex, std::try_to_lock};
        if (prog_lk.owns_lock()) {
            // Verifica novamente sob o lock (race com maybe_end_progress).
            if (!g_in_progress.load(std::memory_order_acquire)) {
                break;
            }
            draw_spinner_frame(frame++);
        }

        // Aguarda 80 ms ou até ser acordado.
        {
            std::unique_lock lk{g_spin.wake_mtx};
            if (g_spin.wake_cv.wait_for(lk, std::chrono::milliseconds(80)) != std::cv_status::timeout) {
                break;
            }
        }
    }
}

void start_spinner(int number, std::string_view title, bool is_step = false) {
    // Encerra spinner anterior.
    if (g_spin.thread.joinable()) {
        g_spin.stop();
    }
    g_spin.phase_number = number;
    g_spin.title = std::string(title);
    g_is_phase.store(!is_step, std::memory_order_release);
    g_is_step.store(is_step, std::memory_order_release);
    g_in_progress.store(true, std::memory_order_release);
    g_spin.thread = std::thread(spinner_thread_fn);
}

void stop_spinner() {
    if (!g_spin.thread.joinable()) {
        return;
    }
    // Sinaliza saída: limpa g_in_progress para que a thread cheque e saia.
    // A CV acorda imediatamente — sem esperar os 80 ms do sleep.
    g_in_progress.store(false, std::memory_order_release);
    g_spin.stop();
    // NÃO limpa g_is_phase aqui; maybe_end_progress fará isso ao 'congelar' a linha.
}

// ─── Primitivos de saída ──────────────────────────────────────────────────────

void emit_newline() noexcept {
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// Finaliza a linha de progresso/spinner ativa.
// Thread-safe: apenas a primeira thread que fizer exchange(false) emite o \n.
void maybe_end_progress() noexcept {
    if (g_in_progress.exchange(false, std::memory_order_acq_rel)) {
        g_spin.wake_cv.notify_all(); // acorda spinner para que perceba g_in_progress=false
        
        // Se era uma fase ativa (com spinner), imprime o estado final (●)
        if (g_is_phase.exchange(false, std::memory_order_acq_rel)) {
            draw_finished_phase();
        } else if (g_is_step.exchange(false, std::memory_order_acq_rel)) {
            draw_finished_step();
        }

        std::fputc('\n', stderr);
        std::fflush(stderr);
    } else {
        g_is_phase.store(false, std::memory_order_relaxed);
        g_is_step.store(false, std::memory_order_relaxed);
    }
}

// ─── Barra de progresso Unicode ──────────────────────────────────────────────

constexpr int BAR_WIDTH = 18;

[[nodiscard]] std::string make_bar(double percent) {
    const int filled =
        std::clamp(static_cast<int>(percent / 100.0 * BAR_WIDTH + 0.5), 0, BAR_WIDTH);
    std::string bar;
    bar.reserve(BAR_WIDTH * 3);
    for (int i = 0; i < filled; ++i) {
        bar += "█";
    }
    for (int i = filled; i < BAR_WIDTH; ++i) {
        bar += "░";
    }
    return bar;
}

// ─── Event dispatcher ────────────────────────────────────────────────────────

// Sequence lock (seqlock) sobre (cb, user_data) — elimina TOCTOU entre os
// dois ponteiros sem exigir atomic de 16 bytes (libatomic). g_event_seq
// é par quando o par está estável e ímpar enquanto um writer modifica.
// Leitor confere seq antes/depois dos loads e refaz se mudou ou era ímpar.
std::atomic<unsigned> g_event_seq{0};
std::atomic<EventCallback> g_event_cb{nullptr};
std::atomic<void*> g_event_user{nullptr};

// noexcept: todas as operações (atomic loads/fence, this_thread::yield, e
// o callback via typedef noexcept após B2) são noexcept. Documenta o
// contrato e permite chamada de contextos noexcept.
void dispatch(const Event& ev) noexcept {
    EventCallback cb = nullptr;
    void* user = nullptr;
    while (true) {
        const unsigned s1 = g_event_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) {
            std::this_thread::yield(); // writer ativo — cede tempo de CPU
            continue;
        }
        cb = g_event_cb.load(std::memory_order_relaxed);
        user = g_event_user.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s1 == g_event_seq.load(std::memory_order_relaxed)) {
            break;
        }
    }
    if (cb != nullptr) {
        cb(ev, user);
    }
}

[[nodiscard]] Event ev_text(EventKind k, std::string_view text,
                           std::string_view detail = {}) noexcept {
    Event e;
    e.kind = k;
    e.text = text;
    e.detail = detail;
    return e;
}

[[nodiscard]] Event ev_phase(int number, std::string_view title) noexcept {
    Event e;
    e.kind = EventKind::PhaseStart;
    e.phase_number = number;
    e.text = title;
    return e;
}

[[nodiscard]] Event ev_progress(std::string_view phase, double percent, double eta_s,
                               double rate_mbs, std::string_view rate_unit) noexcept {
    Event e;
    e.kind = EventKind::Progress;
    e.text = phase;
    e.detail = rate_unit;
    e.percent = percent;
    e.eta_s = eta_s;
    e.rate_mbs = rate_mbs;
    return e;
}

// ─── Substep helpers ─────────────────────────────────────────────────────────

void write_bar_prefix() noexcept {
    maybe_end_progress();
    fmt::print(stderr, styled(s_bar()), "{}  ", GLYPH_BAR);
}

void write_end_prefix() noexcept {
    maybe_end_progress();
    fmt::print(stderr, styled(s_bar()), "{} ", GLYPH_END);
}

void write_step(std::string_view marker, fmt::text_style marker_style, std::string_view text) {
    write_bar_prefix();
    fmt::print(stderr, styled(marker_style), "{} ", marker);
    fmt::print(stderr, styled(s_label()), "{}", text);
    emit_newline();
}

void write_step(std::string_view marker, fmt::text_style marker_style, std::string_view text,
                std::string_view rhs) {
    write_bar_prefix();
    fmt::print(stderr, styled(marker_style), "{} ", marker);
    fmt::print(stderr, styled(s_label()), "{}", text);
    fmt::print(stderr, styled(s_rhs()), "  {}", rhs);
    emit_newline();
}

void write_end_step(std::string_view marker, fmt::text_style marker_style, std::string_view text) {
    write_end_prefix();
    fmt::print(stderr, styled(marker_style), "{} ", marker);
    fmt::print(stderr, styled(s_label()), "{}", text);
    emit_newline();
    stop_spinner();
    g_is_phase.store(false, std::memory_order_release);
}

void write_end_step(std::string_view marker, fmt::text_style marker_style, std::string_view text,
                    std::string_view rhs) {
    write_end_prefix();
    fmt::print(stderr, styled(marker_style), "{} ", marker);
    fmt::print(stderr, styled(s_label()), "{}", text);
    fmt::print(stderr, styled(s_rhs()), "  {}", rhs);
    emit_newline();
    stop_spinner();
    g_is_phase.store(false, std::memory_order_release);
}

} // namespace

// ─── Gate de cor ─────────────────────────────────────────────────────────────

bool color_enabled() noexcept {
    int s = g_color_state.load(std::memory_order_acquire);
    if (s < 0) {
        s = detect_color() ? 1 : 0;
        g_color_state.store(s, std::memory_order_release);
    }
    return s == 1;
}

void set_color_enabled(bool on) noexcept {
    g_color_state.store(on ? 1 : 0, std::memory_order_release);
}

void set_quiet(bool on) noexcept {
    g_quiet.store(on, std::memory_order_release);
}

[[nodiscard]] bool is_quiet() noexcept {
    return g_quiet.load(std::memory_order_acquire);
}

// ─── Fases ───────────────────────────────────────────────────────────────────

void phase(int number, std::string_view title) {
    if (is_quiet()) { dispatch(ev_phase(number, title)); return; }
    stop_spinner();
    maybe_end_progress();

    if (color_enabled()) {
        fmt::print(stderr, styled(s_phase()), "{} ", GLYPH_PHASE_DONE);
        if (number >= 0) {
            fmt::print(stderr, styled(fmt::emphasis::bold), "Fase {}: {}", number, title);
        } else {
            fmt::print(stderr, styled(fmt::emphasis::bold), "{}", title);
        }
        emit_newline();
    } else {
        fmt::print(stderr, "● Fase {}: {}...\n", number, title);
    }
    dispatch(ev_phase(number, title));
}

void phase(std::string_view title) {
    if (is_quiet()) { dispatch(ev_phase(-1, title)); return; }
    stop_spinner();
    maybe_end_progress();

    if (color_enabled()) {
        fmt::print(stderr, styled(s_phase()), "{} ", GLYPH_PHASE_DONE);
        fmt::print(stderr, styled(fmt::emphasis::bold), "{}", title);
        emit_newline();
    } else {
        fmt::print(stderr, "● {}...\n", title);
    }
    dispatch(ev_phase(-1, title));
}

void phase_end() {
    if (is_quiet()) { dispatch(ev_text(EventKind::PhaseEnd, {})); return; }
    stop_spinner();       // encerra thread + seta g_in_progress=false
    maybe_end_progress(); // emite \n se a linha de fase ainda estava aberta
    fmt::print(stderr, styled(s_bar()), "{}", GLYPH_END);
    emit_newline();
    dispatch(ev_text(EventKind::PhaseEnd, {}));
}

// ─── Substeps ────────────────────────────────────────────────────────────────

void step_ok(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepOk, text)); return; }
    write_step(GLYPH_OK, s_ok(), text);
    dispatch(ev_text(EventKind::StepOk, text));
}
void step_ok(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepOk, text, rhs)); return; }
    write_step(GLYPH_OK, s_ok(), text, rhs);
    dispatch(ev_text(EventKind::StepOk, text, rhs));
}
void step_fail(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepFail, text)); return; }
    write_step(GLYPH_FAIL, s_fail(), text);
    dispatch(ev_text(EventKind::StepFail, text));
}
void step_fail(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepFail, text, rhs)); return; }
    write_step(GLYPH_FAIL, s_fail(), text, rhs);
    dispatch(ev_text(EventKind::StepFail, text, rhs));
}
void step_info(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepInfo, text)); return; }
    if (color_enabled()) {
        // TTY: Desenha o estado inicial (·) e inicia o spinner.
        // Se em 50ms nada acontecer, o spinner assume.
        write_bar_prefix();
        fmt::print(stderr, styled(s_dot()), "{} ", GLYPH_DOT);
        fmt::print(stderr, styled(s_label()), "{}", text);
        std::fflush(stderr);
        start_spinner(-1, text, true);
    } else {
        write_step(GLYPH_DOT, s_dot(), text);
    }
    dispatch(ev_text(EventKind::StepInfo, text));
}
void step_info(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepInfo, text, rhs)); return; }
    write_step(GLYPH_DOT, s_dot(), text, rhs);
    dispatch(ev_text(EventKind::StepInfo, text, rhs));
}

void step_ok_end(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepOk, text)); return; }
    write_end_step(GLYPH_OK, s_ok(), text);
    dispatch(ev_text(EventKind::StepOk, text));
}
void step_ok_end(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepOk, text, rhs)); return; }
    write_end_step(GLYPH_OK, s_ok(), text, rhs);
    dispatch(ev_text(EventKind::StepOk, text, rhs));
}
void step_fail_end(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepFail, text)); return; }
    write_end_step(GLYPH_FAIL, s_fail(), text);
    dispatch(ev_text(EventKind::StepFail, text));
}
void step_fail_end(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepFail, text, rhs)); return; }
    write_end_step(GLYPH_FAIL, s_fail(), text, rhs);
    dispatch(ev_text(EventKind::StepFail, text, rhs));
}
void step_info_end(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepInfo, text)); return; }
    write_end_step(GLYPH_DOT, s_dot(), text);
    dispatch(ev_text(EventKind::StepInfo, text));
}
void step_info_end(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::StepInfo, text, rhs)); return; }
    write_end_step(GLYPH_DOT, s_dot(), text, rhs);
    dispatch(ev_text(EventKind::StepInfo, text, rhs));
}

// ─── Top-level ───────────────────────────────────────────────────────────────

void ok(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::Ok, text)); return; }
    maybe_end_progress();
    fmt::print(stderr, styled(s_ok()), "{} ", GLYPH_OK);
    fmt::print(stderr, styled(s_label()), "{}", text);
    emit_newline();
    dispatch(ev_text(EventKind::Ok, text));
}

void ok(std::string_view text, std::string_view rhs) {
    if (is_quiet()) { dispatch(ev_text(EventKind::Ok, text, rhs)); return; }
    maybe_end_progress();
    fmt::print(stderr, styled(s_ok()), "{} ", GLYPH_OK);
    fmt::print(stderr, styled(s_label()), "{}", text);
    fmt::print(stderr, styled(s_rhs()), "  {}", rhs);
    emit_newline();
    dispatch(ev_text(EventKind::Ok, text, rhs));
}

void fail(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::Fail, text)); return; }
    maybe_end_progress();
    fmt::print(stderr, styled(s_fail()), "{} ", GLYPH_FAIL);
    fmt::print(stderr, styled(s_label()), "{}", text);
    emit_newline();
    dispatch(ev_text(EventKind::Fail, text));
}

void note(std::string_view text) {
    if (is_quiet()) { dispatch(ev_text(EventKind::Note, text)); return; }
    maybe_end_progress();
    fmt::print(stderr, styled(s_dot()), "{}", text);
    emit_newline();
    dispatch(ev_text(EventKind::Note, text));
}

// ─── Progresso em linha ──────────────────────────────────────────────────────

void step_progress(std::string_view phase, double percent, double eta_s, double rate_mbs,
                   std::string_view rate_unit) {
    if (is_quiet()) { dispatch(ev_progress(phase, percent, eta_s, rate_mbs, rate_unit)); return; }
    if (color_enabled()) {
        std::unique_lock lk{g_progress_mutex, std::try_to_lock};
        if (lk.owns_lock()) {
            // Se viemos de uma fase ou de uma subetapa ativa (spinner), fecha a linha.
            if (!g_in_progress.load(std::memory_order_acquire) || g_is_phase.load(std::memory_order_acquire) || g_is_step.load(std::memory_order_acquire)) {
                maybe_end_progress();
            }

            const std::string bar = make_bar(percent);
            fmt::print(stderr, "\r");
            fmt::print(stderr, styled(s_bar()), "{}  ", GLYPH_BAR);
            fmt::print(stderr, styled(s_dot()), "{} ", GLYPH_DOT);
            fmt::print(stderr, styled(s_label()), "{}", phase);
            if (percent >= 99.95) {
                fmt::print(stderr, styled(s_ok()), "  {}", bar);
                fmt::print(stderr, styled(s_rhs()), "  100%  {:7.1f} {}", rate_mbs, rate_unit);
            } else {
                fmt::print(stderr, styled(s_rhs()), "  {}  {:3.0f}%  ETA {:3.1f}s  {:7.1f} {}", bar,
                           percent, eta_s, rate_mbs, rate_unit);
            }
            fmt::print(stderr, "\033[K");
            std::fflush(stderr);
            g_in_progress.store(true, std::memory_order_release);
        }
    }
    dispatch(ev_progress(phase, percent, eta_s, rate_mbs, rate_unit));
}

void step_progress_finish() noexcept {
    if (is_quiet()) { return; }
    maybe_end_progress();
}

// ─── Títulos e seções ────────────────────────────────────────────────────────

void title(std::string_view text) {
    if (is_quiet()) { return; }
    maybe_end_progress();
    emit_newline();
    fmt::print(stderr, styled(fmt::emphasis::bold), "  {}", text);
    emit_newline();
    fmt::print(stderr, styled(s_bar()),
               "  ────────────────────────────────────────────────────────────");
    emit_newline();
}

void section(std::string_view text) {
    if (is_quiet()) { return; }
    maybe_end_progress();
    emit_newline();
    fmt::print(stderr, styled(fmt::emphasis::bold), "  {}", text);
    emit_newline();
}

void blank() noexcept {
    if (is_quiet()) { return; }
    maybe_end_progress();
    emit_newline();
}

// ─── Par chave-valor ─────────────────────────────────────────────────────────

namespace {
constexpr int KV_LABEL_WIDTH = 32;
constexpr int KV_VALUE_WIDTH = 22;
} // namespace

void kv(std::string_view label, std::string_view value) {
    if (is_quiet()) { return; }
    maybe_end_progress();
    fmt::print(stderr, "  ");
    fmt::print(stderr, styled(s_label()), "{:<{}}", label, KV_LABEL_WIDTH);
    fmt::print(stderr, "  ");
    fmt::print(stderr, styled(fmt::emphasis::bold), "{:>{}}", value, KV_VALUE_WIDTH);
    emit_newline();
}

void kv_time(std::string_view label, double seconds) {
    kv(label, ggql::format_seconds(seconds));
}
void kv_size(std::string_view label, std::size_t bytes) {
    kv(label, ggql::format_size(bytes));
}
void kv_count(std::string_view label, std::size_t n) {
    kv(label, format_count(n));
}

void kv_number(std::string_view label, double value, std::string_view unit) {
    kv(label, unit.empty() ? fmt::format("{:.2f}", value) : fmt::format("{:.2f} {}", value, unit));
}

// ─── Event callback register ─────────────────────────────────────────────────

void set_event_callback(EventCallback cb, void* user_data) noexcept {
    g_event_seq.fetch_add(1, std::memory_order_relaxed); // par→ímpar (writer ativo)
    std::atomic_thread_fence(std::memory_order_release);
    g_event_cb.store(cb, std::memory_order_relaxed);
    g_event_user.store(user_data, std::memory_order_relaxed);
    g_event_seq.fetch_add(1, std::memory_order_release); // ímpar→par
}

void clear_event_callback() noexcept {
    g_event_seq.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_event_cb.store(nullptr, std::memory_order_relaxed);
    g_event_user.store(nullptr, std::memory_order_relaxed);
    g_event_seq.fetch_add(1, std::memory_order_release);
}

// ─── Formatadores ────────────────────────────────────────────────────────────

std::string format_count(std::size_t n) {
    const auto raw = fmt::format("{}", n);
    const std::size_t first = ((raw.size() - 1) % 3) + 1;
    std::string out;
    out.reserve(raw.size() + (raw.size() - 1) / 3);
    out.append(raw, 0, first);
    for (std::size_t i = first; i < raw.size(); i += 3) {
        out.push_back('.');
        out.append(raw, i, 3);
    }
    return out;
}

} // namespace ggql::cli
