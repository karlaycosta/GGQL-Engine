# Análise — `src/cli/console.cpp`

Auditoria do **vocabulário visual da CLI** ("passos"): primitivos para
fases, sub-steps, spinner animado, barra de progresso e par chave-valor.
Arquivo médio (665 LOC) com lógica multi-thread (spinner background +
step_progress concorrente via OpenMP).

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `src/cli/console.cpp` (665 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 3 | 1 (B1) — todos resolvidos |
| Pontos de melhoria de performance | 1 | 0 |
| Código morto | 0 | 0 |
| Atributos C++23 / qualifiers | 1 | 0 |
| Estilo / consistência | 1 | 0 |

Severidade geral: **baixa-média**. Arquivo é UI/CLI (não hot path),
mas tem multi-threading complexo. 3 bugs latentes em torno do ciclo
de vida do spinner — cenários edge mas com risco de deadlock 80ms ou
terminate em shutdown.

---

## Bugs / fragilidades

### B1 — `SpinnerCtx::stop()` pode bloquear até 80ms desnecessariamente
**Severidade:** média (deadlock soft em start_spinner durante mudança de fase)

```cpp
void stop() {
    if (!thread.joinable()) {
        return;
    }
    wake_cv.notify_all();
    thread.join();
}
```

`stop()` faz `notify_all` + `join` mas **não seta `g_in_progress = false`
antes**. A thread do spinner em `spinner_thread_fn`:
```cpp
while (true) {
    if (!g_in_progress.load(std::memory_order_acquire)) { break; }
    // ... draw ...
    if (g_spin.wake_cv.wait_for(lk, 80ms) != cv_status::timeout) { break; }
}
```

Se a thread já está dentro do `wait_for` quando `notify_all` chega:
acorda, `break` (porque retorno != timeout). **OK.**

Mas se thread está entre o `if (!g_in_progress)` check e o `wait_for`
quando `notify_all` chega:
1. Thread continua até `wait_for`.
2. `wait_for` bloqueia. Pode dormir 80ms.
3. Em paralelo, `start_spinner` chama `g_spin.stop() → join()` que
   bloqueia até a thread sair.
4. Thread acorda em 80ms (timeout), checa `g_in_progress` → ainda
   `true` (não foi setado false antes do stop), continua loop.
5. Race: `start_spinner` pode setar `g_in_progress = true` (linha 209)
   enquanto thread antiga ainda existe.

Em prática, `start_spinner` tem chance de bloquear até 80ms entre
fases. UI lenta para usuários.

`stop_spinner` (linha 213-222) já seta `g_in_progress = false` antes
de chamar `g_spin.stop()`. **Mas `start_spinner` linha 203 chama
`g_spin.stop()` direto** sem seguir esse padrão.

**Fix sugerido:** mover o reset para `stop()`:
```cpp
void stop() {
    if (!thread.joinable()) { return; }
    g_in_progress.store(false, std::memory_order_release);
    wake_cv.notify_all();
    thread.join();
}
```

Garantia: thread acorda imediatamente do `wait_for` (cv signal) e na
próxima iteração vê `g_in_progress=false` → sai. Sem 80ms desperdiçados.

### B2 — `~SpinnerCtx()` pode chamar `std::terminate` via `thread.join()`
**Severidade:** baixa (apenas em shutdown patológico)

```cpp
~SpinnerCtx() {
    g_in_progress.store(false, std::memory_order_relaxed);
    if (thread.joinable()) {
        wake_cv.notify_all();
        thread.join();  // ← pode lançar std::system_error
    }
}
```

`std::thread::join()` lança `std::system_error` em casos patológicos
(deadlock detectado pelo OS, recurso já invalido). Em destrutor sem
`try/catch`, exceção = `std::terminate()`.

Para `g_spin` (objeto static), o destrutor é chamado durante shutdown
do programa. Patológico mas latente.

**Fix sugerido:**
```cpp
~SpinnerCtx() {
    g_in_progress.store(false, std::memory_order_relaxed);
    if (thread.joinable()) {
        try {
            wake_cv.notify_all();
            thread.join();
        } catch (...) {
            // Não pode lançar em destrutor; join falha é raro
            // (deadlock OS-level) — silenciamos para evitar terminate.
        }
    }
}
```

### B3 — TOCTOU em `dispatch` entre `cb` e `user_data`
**Severidade:** muito baixa (race teórica)

```cpp
void dispatch(const Event& ev) {
    if (auto* cb = g_event_cb.load(std::memory_order_acquire); cb != nullptr) {
        cb(ev, g_event_user.load(std::memory_order_acquire));
    }
}
```

Entre o `load(cb)` e `load(user_data)`, `clear_event_callback` pode
setar ambos a `nullptr`. `dispatch` chama `cb(ev, nullptr)` com cb
válido mas user_data=null. Caller que assumir user_data não-null pode
crashar.

`clear_event_callback`:
```cpp
g_event_cb.store(nullptr, ...);
g_event_user.store(nullptr, ...);
```

Limpa cb antes de user_data — mitiga: se dispatch ler cb=nullptr,
não usa user_data. Mas race ainda existe se dispatch já leu cb non-null
e clear roda entre os 2 loads.

**Caso atual:** callback é setado uma vez no início e nunca cleared
durante operação normal (FFI passa callback do binding e mantém
até teardown). Race nunca exposta.

**Fix opcional (defensivo):** combinar cb+user em struct atomic, ou
permitir user_data=nullptr (callback robusto).

---

## Performance

### P1 — `std::condition_variable_any` desnecessário (use `std::condition_variable`)
**Severidade:** marginal

```cpp
std::condition_variable_any wake_cv;  // linha 99
```

`_any` aceita qualquer Lockable; mais pesado que `_condition_variable`
(que requer `std::unique_lock<std::mutex>`). Como `wake_mtx` é
`std::mutex` e `wait_for` usa `unique_lock`, a versão simples basta:

```cpp
std::condition_variable wake_cv;
```

Ganho: zero perceptível (uma CV por SpinnerCtx). Apenas alinhamento
com idioma C++ comum.

---

## Código morto

**Nenhum.** Investigado:
- `ev_text`, `ev_phase`, `ev_progress` — usados em `dispatch`.
- `write_bar_prefix`, `write_end_prefix` — usados em `write_step`,
  `write_end_step`.
- Todos os helpers de estilo (`s_phase`, `s_bar`, etc.) — usados.
- `make_bar`, `format_count` — usados.

---

## Atributos C++23 / qualifiers

### A1 — `g_is_phase = false` sem ordem explícita (linhas 344, 355)
**Severidade:** zero (consistência)

```cpp
void write_end_step(...) {
    // ...
    stop_spinner();
    g_is_phase = false;  // operator= sem ordem (defaulta a seq_cst)
}
```

`std::atomic<bool>::operator=` defaulta a `memory_order_seq_cst`.
Outros stores no arquivo usam `memory_order_release` ou `relaxed`
explicitamente. Inconsistência cosmética.

```cpp
g_is_phase.store(false, std::memory_order_release);
```

### Confirmados em estado bom

- **`[[nodiscard]]`** aplicado em getters: `detect_color`, `styled`,
  `s_phase`/`s_bar`/`s_ok`/`s_fail`/`s_dot`/`s_rhs`/`s_label`,
  `make_bar`, `ev_text`/`ev_phase`/`ev_progress`,
  `is_quiet`/`color_enabled`, `format_count` (declarado no header).
- **`noexcept`** aplicado em getters, setters, `emit_newline`,
  `maybe_end_progress`, `write_bar_prefix`/`write_end_prefix`,
  `step_progress_finish`, `blank`, `set_event_callback`,
  `clear_event_callback`, `set_color_enabled`, `set_quiet`.
- **C++23 features em uso:**
  - `std::atomic<T>` para estado global thread-safe
  - `std::unique_lock` com `std::try_to_lock` em hot path UI
  - `std::condition_variable_any` (poderia ser `std::condition_variable`)
  - `[[nodiscard]]` em helpers
  - `std::string_view` para labels/text
  - Lambda em ranges-for
  - `if constexpr` n/a (não há templates aqui)
- **Memory ordering** explícito em loads/stores críticos
  (`acquire`/`release`/`relaxed` apropriadamente).

---

## Estilo / consistência

### S1 — Comentários de pré-condição de thread-safety bem documentados
**Status:** ✅ excelente

Linhas 92-94 documentam o ciclo de vida da `SpinnerCtx::title`:
> "Escrito pelo main thread ANTES de criar a thread; lido pela thread
> do spinner. Acesso seguro: não há escrita concorrente."

Linha 33-34 documenta o uso de `try_lock` em `g_progress_mutex`.

Linha 232-233 documenta o exchange-once pattern de `maybe_end_progress`.

Manter como referência para futuras adições.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | B1 — `SpinnerCtx::stop()` setar `g_in_progress=false` antes de notify+join | bug | mínimo | ✅ aplicado |
| 2 | B2 — Try/catch em `~SpinnerCtx()` ao redor de `join()` | bug | mínimo | ✅ aplicado |
| 3 | A1 — Padronizar `g_is_phase.store(false, release)` | consistência | mínimo | ✅ aplicado |
| 4 | P1 — `condition_variable_any` → `condition_variable` | cleanup | mínimo | ✅ aplicado |
| 5 | B3 — Combinar cb+user_data em struct atomic | API | médio | ✅ aplicado |

### Itens 1-5 aplicados (2026-05-05)

**B1** — `SpinnerCtx::stop()` agora seta `g_in_progress=false` antes
de `notify_all + join`. Garante que a thread sai do loop na próxima
iteração mesmo perdendo o sinal CV em race. Elimina possível atraso
de 80ms entre fases.

**B2** — `~SpinnerCtx()` envolve `notify_all + join` em try/catch.
`join()` lança `std::system_error` em casos patológicos (deadlock
detectado pelo OS); destrutor não pode lançar — terminate evitado
em shutdown.

**A1** — `g_is_phase = false` (linhas 344, 355) substituído por
`g_is_phase.store(false, std::memory_order_release)`. Consistente
com outros stores no arquivo.

**P1** — `std::condition_variable_any wake_cv` → `std::condition_variable`.
A versão `_any` aceita qualquer Lockable; como `wake_mtx` é
`std::mutex` e `wait_for` usa `unique_lock`, a versão simples basta
e é mais leve (sem virtual call).

**B3** — TOCTOU entre `cb` e `user_data` eliminada via **sequence
lock (seqlock)**: contador `g_event_seq` (par = estável, ímpar =
writer ativo) cerca os dois `std::atomic<>` de 8 bytes. `dispatch`
lê seq → loads relaxed de cb/user → fence acquire → relê seq;
retry se mudou ou era ímpar. Writers (`set`/`clear`) incrementam
seq antes e depois dos stores.

Tentativa anterior usando `std::atomic<CallbackEntry>` (struct de
16 bytes com `alignas(16)`) foi descartada: requer `libatomic`
(`__atomic_load_16`) cuja implementação usa hash de mutexes globais
— "atomic" só no nome, mais lento que a versão original. Seqlock
mantém todos os atomics em 8 bytes (lock-free nativo no x86_64),
sem dependências externas.

Custo: dispatch passa de 2 para 3 atomic loads + 1 fence no caminho
feliz; sem retry quando não há writer concorrente (caso da operação
normal — callback FFI setado uma vez no startup).

17/17 testes passando.

---

## Reanálise pós-aplicação (2026-05-06)

Segunda rodada de auditoria após B1–B3 + P1 + A1 aplicados encontrou
itens residuais — todos triviais, predominantemente cleanup.

### Itens 6-11 aplicados

| # | Item | Tipo | Status |
|---|---|---|---|
| 6 | M1 — Remover `g_stop_spin` (atomic morto) | dead code | ✅ |
| 7 | M2 — Remover `GLYPH_PHASE` (apenas `GLYPH_PHASE_DONE` é usado) | dead code | ✅ |
| 8 | B4 — Refatorar loop `dispatch`: eliminar UB de leitura de `s2` não-inicializado quando `seq` é ímpar (`do-while` → `while(true)+break`) + `std::this_thread::yield()` no caminho do writer ativo | bug | ✅ |
| 9 | C1 — `g_in_progress.store(true, release)` em `step_progress` (era `relaxed`, inconsistente com demais stores do arquivo) | consistência | ✅ |
| 10 | I1 — Adicionar `#include <array>` explícito | estilo | ✅ |
| 11 | P2 — `format_count` em uma alocação (era 3: `fmt::format` + `out.reserve` + reverso) | performance | ✅ |

### Confirmados em estado bom (nada a mexer)

- **`[[nodiscard]]`** — todos os getters cobertos.
- **`noexcept`** — aplicado consistentemente onde lança não é possível
  ou indesejado. Funções com `fmt::print` cuja saída pode falhar
  ficam não-noexcept conscientemente.
- **`const`** — parâmetros e locais qualificados onde aplicável.
- **C++23 features avaliadas e descartadas em segunda rodada:**
  - `[[assume]]` em `make_bar` para guarantee de range — overkill UI.
  - `std::ranges` em loops curtos — sem ganho.
  - Deducing `this` — sem método de classe que se beneficie.
  - `std::print`/`std::println` — fmt já é dep consolidada.
  - `std::stop_token` + `std::jthread` — API atual `start_spinner`/
    `stop_spinner` é coesa; refator perde mais que ganha.

17/17 testes passando após reanálise.

**Auditoria do `console.cpp` 100% concluída — 11 itens aplicados.**

---

## Apêndice — itens avaliados e descartados

- **Substituir `fmt::print` por `std::print` (C++23)** — fmt já é
  usado em todo o projeto; troca seria refator amplo sem ganho real.
  fmt suporta features adicionais que `std::print` ainda não tem.
- **Composição de linhas de spinner em buffer único + 1 syscall**
  — UI não é hot path; fmt::print agrupa internamente quando tem
  buffer.
- **`std::stop_token` em vez de `g_in_progress` + CV** — modernização
  via `std::jthread`; mas `start_spinner`/`stop_spinner` API atual
  é coesa.
- **`std::format` em `format_count`** — `fmt::format` é equivalente
  e já é usado.
- **`std::ranges::for_each` em loops simples** — sem ganho real para
  loops de 18 iterações (BAR_WIDTH).
- **`std::expected<void, std::string>` em primitivos de UI** — UI
  não retorna erros; falhas de I/O em stderr são silenciadas pelo OS.
- **Adicionar testes unitários da máquina de estado do spinner** —
  out of scope; testes de UI requerem infraestrutura maior (mock TTY).
