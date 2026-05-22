# FFI — Auditoria de robustez (Round 4 candidato)

**Data:** 2026-05-22
**Branch:** `ffi/refine-load-and-introspection`
**Escopo:** `src/ffi/ffi.cpp` + `include/ffi/ffi.h` (Rounds 1–3 implementadas)
**Referências cruzadas:** [`ffi_refinement_plan.md`](ffi_refinement_plan.md) (Rounds anteriores), [`ffi_flutter_python_integration.md`](ffi_flutter_python_integration.md) (guia consumer), [`analise_ggql_ffi.md`](analise_ggql_ffi.md) (análise anterior).

---

## 1. Resumo executivo

A FFI está **sólida em correção sequencial e ergonomia** após Rounds 1–3 + sprint de allocator split (commit `dafe6dd`). 31 símbolos expostos, validados via `flutter test` (12/12) e `test_ffi_query` (todas as seções).

Os problemas restantes são quase todos em **paralelismo dentro do mesmo engine** — para o caso de uso atual (UI desktop com 1 engine por sessão) nenhum morde. Mas para webapp Python multi-thread ou serviço com múltiplos clientes por engine, há buracos a fechar antes de escalar.

Esta auditoria lista 10 problemas em ordem de severidade prática, com causa raiz e fix concreto. Veja também §7 "Coordenação com auditoria cliente" — comparação cruzada com a auditoria Dart-side em [`ggql_ui/docs/ffi_ingestion_analysis.md`](file:///home/karlay/Documentos/flutter_projetos/ggql_ui/docs/ffi_ingestion_analysis.md).

---

## 2. Problemas priorizados

### 🔴 P1 — `GgqlEngineWrapper::last_error` não é thread-safe

**Severidade:** Alta — race confirmável em ambiente concorrente.

**Localização:** `src/ffi/ffi.cpp:54` (campo) + `ffi.cpp:651,679,690` (escritas) + `ffi.cpp:469` (leitura).

```cpp
// ffi.cpp:54
std::string last_error;
```

Campo de instância, não `thread_local`. O header documenta:

```c
// include/ffi/ffi.h (Thread-safe: múltiplas queries simultâneas em threads
// diferentes são seguras.)
```

mas em duas threads chamando `ggql_engine_execute()` no mesmo engine, ambas executam `w->last_error = std::move(parsed.error())` (ou similar) sem lock. Pior: `ggql_engine_last_error()` lê o string concorrente sem mutex — pode ler durante a escrita (`std::string` não é trivialmente copiable nem atomic).

**Como reproduzir:**
1. Criar engine; spawn 4 threads chamando `engine_execute()` com queries inválidas (parse failure).
2. Cada thread chama `engine_last_error()` imediatamente após.
3. Em ASAN/TSAN: data race em `std::string::operator=` reportada. Sem sanitizer: comportamento indefinido (pode retornar string truncada/corrompida ou crash).

**Fix proposto:** Mover `last_error` para `thread_local` no namespace anônimo, com lookup por engine pointer (ou simplesmente `thread_local std::string last_error_engine;` global se aceitarmos que múltiplos engines compartilham o slot por thread — análogo a `errno`).

```cpp
// namespace anônimo
thread_local std::string g_last_engine_error;

// nas escritas:
g_last_engine_error = std::move(parsed.error());

// em ggql_engine_last_error:
const char* ggql_engine_last_error(GgqlEngineHandle engine) {
    if (!engine) { return engine_create_error.c_str(); }
    return g_last_engine_error.c_str();
}
```

**Custo:** ~5 LOC. **Quebra ABI?** Não — só comportamento interno muda.

**Teste de regressão sugerido:** novo case em `test_ffi_query.cpp` com 4 threads × 100 iterações de `execute(query_invalida)` + `last_error()`; deve passar sob TSAN sem warnings.

---

### 🟠 P2 — `cli::set_event_callback` é global do processo

**Severidade:** Média — bug silencioso em ingest paralelo (eventos da thread A vão para callback da thread B).

**Localização:** `src/ffi/ffi.cpp:240,293` (set/clear no `do_ingest_ffi`).

```cpp
// ffi.cpp:236-241
thread_local EventBridge bridge;
if (event_cb) {
    bridge.fn = event_cb;
    bridge.user_data = user_data;
    cli::set_event_callback(&dispatch_bridge, &bridge);  // ⚠ global
}
```

O `EventBridge` é `thread_local` — bom. Mas `cli::set_event_callback` instala o callback num singleton do processo. Em paralelismo:

1. Thread A: `set_event_callback(dispatch_bridge, &bridge_A)`.
2. Thread B: `set_event_callback(dispatch_bridge, &bridge_B)` (sobrescreve).
3. Eventos emitidos por A → `dispatch_bridge(ev, &bridge_B)` → callback B com `user_data` errado.
4. Thread A termina, chama `clear_event_callback()` — agora B também perde eventos.

O comentário em `ffi.cpp:235-236` diz "o dispatcher em cli:: é atômico", mas isso só protege a **chamada** do callback de race no ponteiro de função — não a **configuração**.

**Fix proposto:** Tornar o dispatcher `cli::` thread_local, com fallback global se thread não setou o seu.

```cpp
// ggql/cli/console.cpp
thread_local cli::EventDispatch g_dispatch_tl{nullptr, nullptr};
cli::EventDispatch g_dispatch_global{nullptr, nullptr};

void set_event_callback_tl(EventFn fn, void* ud) {
    g_dispatch_tl = {fn, ud};
}
void emit_event(...) {
    auto d = g_dispatch_tl.fn ? g_dispatch_tl : g_dispatch_global;
    if (d.fn) d.fn(...);
}
```

**Custo:** ~30 LOC + audit dos call sites em `cli::`. Possivelmente intrusivo se outros módulos dependem do dispatcher global.

**Alternativa de baixo custo:** documentar em `ffi.h` que `ggql_ingestor_ingest()` **não é safe** com múltiplas threads no mesmo processo se `event_cb != NULL`. Serializar do lado do consumer.

---

### 🟠 P3 — Ausência de `struct_size` em structs ABI-públicos

**Severidade:** Média — não morde hoje, trava evolução do ABI.

**Localização:** `include/ffi/ffi.h:16-36` (`GgqlIngestStats`), `:206-215` (`GgqlEngineCapabilities`), `:230-235` (`GgqlParseResult`).

Próxima adição de campo em qualquer desses structs vai requerer recompilação síncrona de todos os consumers — caso contrário escritas C++ caem em offset inválido nos clientes antigos, ou leituras de clientes novos pegam lixo em campos não-preenchidos por libs antigas.

**Histórico relevante:** commit `bf9b8fd` ("refactor(ingestor)!: remove campos zumbi M1+B3 — BREAKING ABI da FFI") já reflete que mudanças em `GgqlIngestStats` ocorrem e não há proteção contra mismatch silencioso. O bug que corrigi no doc `ffi_flutter_python_integration.md` (campos zumbi `elapsed_gbwt_insert_s`/`compress_s` em exemplos Dart/Python) é exatamente esta classe de problema.

**Fix proposto (BREAKING, candidato para Round 4):**

```c
typedef struct {
    size_t struct_size;       // sizeof(GgqlIngestStats) — preenchido pelo caller
    uint64_t segment_count;
    ...
} GgqlIngestStats;

// Lado C++ checa antes de escrever:
if (stats->struct_size < offsetof(GgqlIngestStats, novo_campo) + sizeof(uint64_t)) {
    // cliente antigo — pula novo_campo
} else {
    stats->novo_campo = ...;
}
```

**Custo:** ~10 LOC por struct + atualização nos exemplos e na geração do ffigen. **Quebra ABI:** sim, intencional — uma única vez.

---

### 🟡 P4 — Handles `void*` permitem type confusion silenciosa

**Severidade:** Médio-baixo — raro em produção, mas zero-checagem.

**Localização:** `include/ffi/ffi.h:13,165-166`.

```c
typedef void* GgqlIngestorHandle;
typedef void* GgqlEngineHandle;
typedef void* GgqlResultHandle;
```

Todos são `void*` — o compilador C aceita silenciosamente:

```c
GgqlResultHandle r = ggql_engine_execute(eng, "...", 5, 0);
ggql_engine_destroy(r);  // ⚠ aceito: trata r como engine, UB silencioso
```

**Fix proposto (zero ABI change):**

```c
typedef struct GgqlIngestor_*  GgqlIngestorHandle;
typedef struct GgqlEngine_*    GgqlEngineHandle;
typedef struct GgqlResult_*    GgqlResultHandle;
```

Tipos opacos distintos. ABI continua idêntico (são todos ponteiros opacos), mas o compilador C agora rejeita mistura sem cast explícito.

**Custo:** 3 linhas. **Quebra ABI:** não.

---

### 🟡 P5 — Divergência cancel reset entre código e doc §6.6

**Severidade:** Baixa — bug de documentação, não de código.

**Localização:** `src/ffi/ffi.cpp:684` (reset) vs `docs/ffi_flutter_python_integration.md` §6.6.

```cpp
// ffi.cpp:684 (dentro de engine_execute)
w->cancel_requested.store(false, std::memory_order_release);  // RESETA
```

Doc afirma:

> Se nenhuma query estiver rodando, o próximo execute observa o flag e pode abortar imediatamente.

Mas o código reseta o flag no início de cada execute. Cancel armado pré-execute **é descartado**. Já documentei essa discrepância no §8.2 passo 8 da auditoria, mas o §6.6 ficou inalterado.

**Fix proposto (simples):** alinhar doc com código:

> O flag `cancel_requested` é resetado no início de cada `engine_execute`. Armar cancel quando nenhuma query está rodando é no-op — o cancel só morde para a query em vôo.

Ou, se o comportamento documentado é o desejado, mudar o código para `compare_exchange_strong(true, false)` (consome o cancel pré-armado uma vez).

**Custo:** 3 linhas no doc, ou ~5 LOC se mudar comportamento.

---

### 🟡 P6 — `EventBridge::text_buf` / `detail_buf` reusados entre eventos

**Severidade:** Baixo — documentado, mas exige disciplina rara.

**Localização:** `src/ffi/ffi.cpp:97-103,121-128`.

```cpp
struct EventBridge {
    GgqlEventCallback fn;
    void* user_data;
    std::string text_buf;
    std::string detail_buf;
};

// dispatch_bridge:
b->text_buf.assign(ev.text.data(), ev.text.size());
out.text = b->text_buf.c_str();   // ⚠ ponteiro reusado no próximo evento
```

Consumer assíncrono (Dart `NativeCallable.listener`) recebe o `GgqlEvent*` e processa **depois** do dispatcher retornar. Se outro evento chegar antes do processamento, `text_buf` é sobrescrito — Dart lê garbage.

Mitigação atual: `thread_local GgqlEvent out` em `dispatch_bridge` (commit anterior fixou isto). Mas os **buffers internos** (`text_buf`/`detail_buf`) ainda são reusados entre emissões da mesma thread. Doc §2.6 menciona: "para fidelidade total, leia apenas campos POD ou copie do lado C++".

**Fix proposto (clean):** Construir a string no stack temporário do `out` (cópia explícita por evento), ou usar small-string optimization fixa:

```cpp
// Opção A: passar struct com length, não c_str pointer
typedef struct {
    const char* text;
    size_t text_len;  // NOVO
    ...
} GgqlEvent;

// Opção B: arena por evento (mais alocação, mas semântica clara)
thread_local std::vector<std::string> g_event_arena;  // limpa a cada N
```

**Custo:** opção A é ~10 LOC + ABI breaking. Opção B é interno, sem ABI change, mas malloc por evento.

**Alternativa zero-custo:** documentar de forma destacada (warning box no §2.6) que `text/detail` NÃO devem ser lidos após o callback retornar; consumer deve copiar imediatamente.

---

### 🟢 P7 — `ggql_engine_destroy` durante queries pendentes — UAF silenciosa

**Severidade:** Baixo — contrato implícito, não há documentação.

**Localização:** `src/ffi/ffi.cpp:463-465`.

```cpp
void ggql_engine_destroy(GgqlEngineHandle engine) {
    delete static_cast<GgqlEngineWrapper*>(engine);  // ⚠ não checa pending queries
}
```

Se thread A está iterando `GgqlResultHandle` (que retém ponteiros para strings dentro do engine) e thread B chama `destroy(engine)`:
- `GgqlResultWrapper::result` foi feito por move de `QueryResult` — independente do engine, OK.
- Mas próximas chamadas `engine_execute` em thread A vão UAF.

**Fix proposto:** documentar em `ffi.h` que destroy é responsabilidade do consumer serializar; OU usar `shared_ptr<GgqlEngineWrapper>` + handles que retêm shared ref (BREAKING — handles deixam de ser raw ponteiros).

**Custo (docs apenas):** 5 linhas no header. **Custo (shared_ptr):** Refactor moderado, mudança comportamental.

**Decisão recomendada:** documentar. O caso é raro; consumer bem-comportado não destrói recursos em uso.

---

### 🟢 P8 — `set_max_total_hits` cap é per-engine, não per-query

**Severidade:** Baixa — limitação de design, não bug.

**Localização:** `src/ffi/ffi.cpp:480-485,687`.

O `max_total_hits_override` é instalado via `ScopedContext` (ffi.cpp:685) durante o execute, com valor lido do engine. Múltiplas threads chamando execute no mesmo engine compartilham o cap atual.

Se thread A quer cap=100 e thread B quer cap default simultaneamente, conflito sem solução API:
- Thread A: `set_max_total_hits(eng, 100)`
- Thread B: `set_max_total_hits(eng, 0)` (sobrescreve)
- Thread A: `execute(...)` — pega cap=0 (default), não o que setou

**Fix proposto:** Adicionar variante `execute_with_cap(eng, query, limit, offset, cap)` que passa o cap como parâmetro de chamada. Cap per-engine continua disponível como default.

**Custo:** ~15 LOC + 1 símbolo novo no ABI. Não-breaking.

---

### 🟢 P9 — Streaming não é true streaming

**Severidade:** Baixa — documentado, performance issue só em datasets enormes.

**Localização:** `src/ffi/ffi.cpp:490-526`.

```cpp
// loop: chama engine_execute por página (re-parseia + re-planeja a query)
GgqlResultHandle r = ggql_engine_execute(engine, query, page_size, offset);
```

Cada página re-executa a query inteira (com offset crescente). Para chm13 `FIND SEGMENT RETURN id,name = 4.29M linhas` com `page_size=1024`, são **~4200 execuções completas** — overhead O(N²) cumulativo se o planner não tem fast-skip para offset.

Doc §6.7 já menciona "NÃO é true streaming". Limitação assumida.

**Fix proposto (grande):** Refactor do planner para cursor-style:

```cpp
class QueryCursor {
    QueryResult fetch_page(uint32_t page_size);  // estado interno preserva offset
};

// FFI:
GgqlCursorHandle ggql_engine_execute_cursor(engine, query);
GgqlResultHandle ggql_cursor_next(cursor, page_size);
void ggql_cursor_destroy(cursor);
```

**Custo:** Refactor moderado-grande do planner. Pode esperar até ser dor real (não houve report de problema em prod).

---

### 🟠 P10 — Falta `ggql_ingestor_cancel(handle)` — assimetria com query cancel

**Severidade:** Média — gap real no contrato C ABI, observado pela auditoria cliente.

**Localização:** ausente em `include/ffi/ffi.h`. `ggql_engine_cancel` existe (`ffi.cpp:473`); `ggql_ingestor_cancel` **não existe**.

A ingestão pode levar dezenas de segundos a minutos (HPRC: ~24 min historicamente). UI que oferece botão "Cancelar" não tem como honrá-lo do lado do engine — o ingestor roda até o fim, escreve no `cacheDir` parcialmente, e o "cancel" do cliente vira fake (ele só esconde a UI). Documentado no doc cliente como item **1.4** (`ggql_ui/docs/ffi_ingestion_analysis.md`).

**Como reproduzir:**
1. UI inicia ingest de chm13_subset_100w (~25s).
2. UI chama `cancel()` na metade.
3. Cliente atual (`ingestion_provider.dart:179`): cancela `StreamSubscription` + `Timer`, mas o isolate worker continua rodando `ggql_ingestor_ingest_granular` até o fim.
4. Resultado: CPU/IO continuam, `.idx` é finalizado, cliente assumiu cancelamento → estado inconsistente.

**Fix proposto:** Adicionar símbolo `ggql_ingestor_cancel(handle)` espelhando `ggql_engine_cancel`:

```c
// ffi.h
void ggql_ingestor_cancel(GgqlIngestorHandle handle);
```

```cpp
// GfaIngestorMmapParallel
std::atomic<bool> cancel_requested_{false};
void cancel() noexcept { cancel_requested_.store(true, std::memory_order_release); }

// Pontos de checkpoint dentro do parser + gBWT build:
if (cancel_requested_.load(std::memory_order_acquire)) {
    return std::unexpected("ingestion cancelled");
}
```

**Custo:** Médio. Mais trabalho que cancel de query porque ingest é mais batched:
- 1 símbolo novo na FFI (~5 LOC)
- Flag atômica no `GfaIngestorMmapParallel` (~3 LOC)
- Checkpoints em pontos certos do parser (entre chunks de mmap), gBWT (entre paths), snarls (entre componentes) (~10-20 LOC dispersos)
- Cleanup de arquivos parciais no cache_dir se cancel ativar antes de persistência completa (~10 LOC)

**Quebra ABI?** Não — só adiciona símbolo novo.

**Teste de regressão sugerido:** novo case integration: spawn thread A com `ingest()`, thread B chama `ingestor_cancel()` após 1s; assertar que A retorna com `success=false`, `errorMessage="cancelled"`, total wall < 5s (não esperar os 25s).

---

## 3. Pontos fortes (preservar)

Para balanço, o que **não** mudar:

| Capability | Localização | Razão |
|---|---|---|
| Cell zero-copy strings | `ffi.cpp:770-774` | `c_str()` direto do `std::string` interno — UX rápida, sem alocação por leitura |
| Lazy FM-Index | `ffi.cpp:625-646` | `call_once` + planner swap atômico — engine load em ms vs segundos |
| Cancel cooperativo | `ffi.cpp:473-477` + `query_planner.cpp:411-414` | ~ns overhead por hit (atomic_load relaxed) |
| Capabilities introspection | `ffi.cpp:529-556` | Lê membros já em RAM, zero query |
| Schema introspection | `ffi.cpp:583-614` + literais estáticos | Ponteiros válidos pela vida da lib |
| thread_local para erros não-engine | `ffi.cpp:204,397,140` | Sem race em `engine_create_error`, `g_validate_error_buf`, `last_error_message` do ingest |
| noexcept boundary | `ffi.cpp:108` | `dispatch_bridge` não propaga exceção C++ pro callback C/Dart |
| Allocator split (commit `dafe6dd`) | `CMakeLists.txt:72-200` | mimalloc na .so resolve dlopen-safety sem LD_PRELOAD |
| Profile + granular | `ffi.cpp:301-372` | API legada + perfil + granular cobre os 4 perfis nomeados + combos arbitrários |

---

## 4. Roadmap recomendado (Round 4)

Ordem de execução, do que mais paga ao que pode esperar:

| # | Item | Custo | Quebra ABI? | Paga em |
|---|---|---|---|---|
| 1 | P1: `last_error` thread-safe | 5 LOC | Não | Bug real em paralelismo (próx. relato vai vir daqui) |
| 2 | P5: alinhar doc §6.6 com cancel reset | 3 linhas doc | Não | Confusão do consumer já documentada |
| 3 | P4: handles `void*` → opaque struct typedefs | 3 linhas header | Não | Type confusion em refactor descuidado |
| 4 | P7: documentar contrato destroy vs queries pendentes | 5 linhas header | Não | Bug futuro inevitável sem doc |
| 5 | P6: warning box no §2.6 sobre buffers reusados | 5 linhas doc | Não | Bug já visto em Flutter (kind=garbage histórico) |
| 6 | P2: dispatcher cli:: thread_local OU doc "não use event_cb paralelo" | 30 LOC ou 5 linhas | Não (doc) ou Não (interno) | Webapp multi-thread |
| 7 | P3: `struct_size` em structs públicos | ~30 LOC | **Sim** | Evolução futura do ABI |
| 8 | P10: `ggql_ingestor_cancel` | ~30 LOC dispersos | Não (novo símbolo) | UX real de cancelamento — cliente já espera |
| 9 | P8: `execute_with_cap` variant | 15 LOC | Não (novo símbolo) | Webapp multi-thread |
| 10 | P9: cursor-style streaming | Refactor planner | Não (novo símbolo) | Datasets multi-milhão de linhas |

Itens 1–5 são small wins. Item 6 escolhe entre fix técnico ou aceitar limitação via doc. Item 7 é BREAKING e deve ser planejado em janela coordenada com consumers conhecidos.

---

## 5. Notas de validação

Para cada fix, sugestão de test case:

| Item | Test |
|---|---|
| P1 | `tests/test_ffi_query.cpp`: 4 threads × 100 iter execute(query_invalida) + last_error; rodar sob TSAN (CMake preset `build-tsan` se houver, ou flag manual) |
| P2 | 2 threads chamando ingest com `event_cb` diferentes; assert que cada uma só recebe seus eventos |
| P3 | Test que verifica `struct_size` é preenchido e respeitado (lib velha + consumer novo, e vice-versa) |
| P4 | Compilação test que mistura handles — deve falhar com `incompatible pointer types` |
| P5 | Documentação só — review |
| P6 | Test em Dart com `NativeCallable.listener` + 100 eventos rápidos; assert que `text` sobrevive ao processing async |
| P7 | Doc apenas |
| P8 | `execute_with_cap(eng, q, 100, 0, cap=5)` paralelo com `execute_with_cap(eng, q, 100, 0, cap=0)`; assertar que caps são independentes |
| P9 | Bench: `execute_streaming` vs `cursor_next` em chm13 4.29M linhas; cursor deve escalar O(N), streaming atual O(N²) |
| P10 | Thread A: `ingest(chm13_subset_100w.gfa)`. Thread B: após 1s chama `ingestor_cancel()`. Assertar A retorna `success=false` + `errorMessage="cancelled"` + wall < 5s |

---

## 6. Coordenação com auditoria cliente

A auditoria espelho do lado Dart vive em [`ggql_ui/docs/ffi_ingestion_analysis.md`](file:///home/karlay/Documentos/flutter_projetos/ggql_ui/docs/ffi_ingestion_analysis.md) (2026-05-22). Esta seção mapeia o que se sobrepõe, o que conflita e a ordem coordenada de fix.

### 6.1 Pontos de contato

| Engine (este doc) | Cliente (`ffi_ingestion_analysis.md`) | Relação |
|---|---|---|
| **P6** — buffers `text_buf`/`detail_buf` reusados | **1.2** + **1.3** — text/detail nunca chegam ao Dart; race thread_local | **Mesma raiz**. Cliente 1.2 é consequência direta (cliente teme UB e não tenta ler); 1.3 é o UB latente |
| **P1** — `last_error` não thread-safe | **2.3** — `errorMessage` thread_local depende de ordem | **Mesma família** — cliente assume contrato que P1 não cumpre em paths de engine_execute/load |
| **P2** — `cli::set_event_callback` global | **1.3** — buffer thread_local race | **Sobrepostos** — ambos tornam ingest paralelo frágil. Hoje 1 isolate ativo, futuro N isolates ativa P2 |
| **P5** — cancel reset diverge de doc §6.6 | **1.4** — cancel ingest é fake | **Diferentes alvos** (query vs ingest), mesma classe — cancel cooperativo lacunoso |
| **P7** — destroy durante queries pendentes | **1.6** — sem guard contra ingest concorrente | **Diferentes alvos**, mesma classe (lifecycle sem proteção) |
| **P4** — `void*` permite type confusion | bindings ffigen usam `Pointer<Void>` | **Conflito** — fix P4 muda tipos gerados |
| **P3** — falta `struct_size` em structs | **1.5** — stats são consumidos hoje (sem versionamento) | **Conflito futuro** — fix P3 é BREAKING; bindings cliente precisam regerar |
| **P10** — falta `ggql_ingestor_cancel` | **1.4** — origem do item; cliente espera o símbolo | **Gap identificado pelo cliente**, adicionado a esta auditoria |

### 6.2 Conflitos diretos (fix engine quebra cliente)

#### Conflito A — P4 (opaque struct typedef) regenera bindings

- **Engine**: `typedef struct GgqlEngine_* GgqlEngineHandle;` no `ffi.h`.
- **Quebra cliente**: ffigen gera `Pointer<GgqlEngine>` em vez de `Pointer<Void>`. Call sites do cliente (`smoke_test.dart`, `smoke_round23_test.dart`, `ggql_ingestor.dart`) que tipam `Pointer<Void>` viram erro de compilação.
- **Coordenação**: regerar `lib/src/ffi/ggql_bindings.dart` no mesmo PR; sed `Pointer<Void> eng` → `GgqlEngineHandle eng`. Custo cliente: ~15 LOC. Sem pegadinha — ffigen lida bem com opaque structs.

#### Conflito B — P3 (struct_size em structs) BREAKING

- **Engine**: prefixa `GgqlIngestStats`, `GgqlEngineCapabilities`, `GgqlParseResult` com `size_t struct_size`. Cliente precisa preencher antes da chamada.
- **Quebra cliente**: bindings antigos (sem `struct_size`) escrevem em offset errado. UB silencioso ou crash imediato.
- **Coordenação**: deploy obrigatoriamente atômico (engine + cliente no mesmo upgrade). Considerar manter API legada `ggql_ingestor_ingest` sem `struct_size` + adicionar `_v2` com `struct_size` por 1-2 releases para transição suave.

### 6.3 Sinergias (fix engine resolve problema cliente sem ação adicional)

| Sinergia | Engine fix | Cliente ganha |
|---|---|---|
| A | **P6 opção B** (arena per-event no engine, sem ABI change) | Resolve **1.2** + **1.3** automaticamente. Cliente só precisa ler `ev.text.cast<Utf8>().toDartString()` no callback sem medo |
| B | **P1** (last_error thread_local em engine_execute/load) | Reforça assunção do cliente em **2.3**. Cliente não muda nada; vira mais seguro automaticamente |
| C | **P5** (alinhar doc §6.6 com cancel reset) | Elimina confusão quando cliente implementar **1.4** (ingest cancel real) + usar query cancel |
| D | **P10** (`ggql_ingestor_cancel`) | Cliente substitui fake-cancel em **1.4** por real — só precisa adicionar 1 chamada FFI no `cancel()` do provider |

### 6.4 Itens só-cliente (sem impacto no engine)

A maioria dos achados do doc cliente é puramente Dart-side e pode ser fixada em paralelo ao trabalho do engine:

- **1.1** path absoluto hardcoded no loader
- **1.5** `IngestStats` reais descartados (toast mock)
- **1.6** sem guard contra ingest concorrente
- **1.7** `Future.delayed(50ms)` mágico — substituir por evento sentinela
- **1.8** confunde `cancel` e `fail` no estado
- **1.9** `Isolate.spawn().catchError` ginástica — migrar para `Isolate.run`
- **1.10** path concat manual no hook — usar `package:path`
- **2.1** ausência de wrapper Dart para `GgqlEngine`/`GgqlResult`
- **2.2** `CodeAsset` hook vs `DynamicLibrary.open` manual — escolher um caminho
- **2.4** `ggql_ingestor_reset` existe mas não é usado
- **3.1** modelo de progresso ignora info semântica (consequência de 1.2 — fica desbloqueado após sinergia A)
- **3.2** singleton `GgqlLib` não cacheia erro
- **3.3** dois notifiers de ingest acoplados
- **3.4** sempre usa `_granular`, nunca `_with_profile`
- **4.1** sem teste do pipeline query (depende de 2.1)
- **4.2** `ingest_test.dart` com path absoluto de fixture

### 6.5 Ordem coordenada recomendada

**Fase 1 — quick wins independentes (zero conflito):**
1. Engine: P1 (`last_error` thread-safe) + P5 (alinhar doc §6.6) + P7 (doc destroy)
2. Cliente: 1.5 + 1.6 + 1.8 (3 small wins de UX/correção)

**Fase 2 — sinergia P6/1.2/1.3 (coordenado, sem ABI change):**
3. Engine: P6 opção B (arena per-event)
4. Cliente: ler `text/detail` no callback (1.2 + 1.3 resolvidos automaticamente)

**Fase 3 — gap ingest cancel:**
5. Engine: P10 (novo símbolo `ggql_ingestor_cancel`)
6. Cliente: substituir fake-cancel por real-cancel (1.4)

**Fase 4 — conflito A (coordenado num único PR):**
7. Engine: P4 (opaque struct typedef) + regerar `ggql_bindings.dart` do cliente

**Fase 5 — conflito B (BREAKING, janela isolada):**
8. Engine: P3 (`struct_size` em structs) + cliente atualiza call sites

**Fase 6 — fora do alvo imediato:**
- Engine P2, P8, P9 — só quando o uso correspondente escalar
- Cliente 2.1 (wrapper Dart) — quando query path for habilitado na UI

### 6.6 Trabalho desta auditoria vs auditoria cliente

| Aspecto | Engine (`ffi_problemas_round4.md`) | Cliente (`ffi_ingestion_analysis.md`) |
|---|---|---|
| Foco | Contrato C ABI + implementação C++ | Wrapper Dart, isolate, provider, UI |
| Severidade conhecida | 1 alta, 3 médias, 6 baixas | 4 críticos, 7 médios, 4 menores |
| Itens BREAKING | P3, (P4 indireto) | — |
| Itens só-um-lado | P2, P8, P9 (engine só); 1.1, 1.5–1.10, 1.X, 2.X, 3.X, 4.X (cliente só) |
| Itens cruzados | P6↔1.2/1.3, P1↔2.3, P5↔1.4, P7↔1.6, P10↔1.4 | |

Ambas as auditorias devem ser lidas em conjunto para planejar Round 4 — uma cobre o que a outra não vê.

---

## 7. Histórico desta auditoria

| Data | Ação |
|---|---|
| 2026-05-22 | Documento criado após análise criteriosa solicitada na sessão; nenhum dos itens foi implementado ainda — são candidatos para Round 4 |
| 2026-05-22 | Adicionado P10 (`ggql_ingestor_cancel`) após cross-check com auditoria cliente; nova §6 "Coordenação com auditoria cliente" |
| 2026-05-22 | **Fase 1 entregue** (`ggql_engine@28051f0`): P1/C9 (`last_error` thread_local), P5/C6 (doc cancel reset alinhada), P7/C4a (doc lifecycle destroy). Zero ABI change. Validado com `test_ffi_query`. |
| 2026-05-22 | **Fase 2 entregue** (`ggql_engine@7f8fd41`): P6/C1 opção B (arena circular de 16 slots em `EventBridge`), E3 (terminal Ok/Fail com sentinela `__ggql_ingest_done__` antes de `clear_event_callback`). Header documenta LIFETIME e TERMINAL EVENT. Validado com `test_ffi_query` + `ingest_test.dart` do cliente Flutter contra a `.so` nova. |
| 2026-05-22 | Header `ffi.h` ganhou **GARANTIA DE UNICIDADE** da sentinela `__ggql_ingest_done__` em §TERMINAL EVENT (`ggql_engine@d623a57`) — atende pendência do cliente em §4.6 do `ffi_conflitos_coordenacao.md` (validação empírica: 1 sentinela em tiny.gfa e chm13_subset_sm.gfa). |
| 2026-05-22 | **Fase 3 entregue** (próximo commit): P10/C2 — `ggql_ingestor_cancel(handle)` simétrico a `engine_cancel`. `GfaIngestorMmapParallel` ganhou flag `cancel_requested_` atômica + métodos `cancel()`/`is_cancelled()`; reset no início de `ingest()`; 7 checkpoints entre fases em `run_full_ingest` (Fase 1–6, **Fase 7 Persistência intencionalmente sem checkpoint**) + 1 em `complete_with_gbwt`. Símbolo exposto em `ffi.h`. Novo teste `test_ffi_ingest_cancel.cpp` valida 4 casos: cancel(NULL) no-op, ingest normal, cancel pré-armado é resetado, **cancel concorrente disparou** com `error_message == "ingest cancelled"`. Cliente já tinha o ponto de chamada esperando (cf. §4.1 de `ffi_conflitos_coordenacao.md` C2). |
| 2026-05-22 | **Fase 4 entregue** (próximo commit): P4/E1 — opaque struct typedef nos 3 handles (`typedef void*` → `typedef struct GgqlXxx_*` no `ffi.h`). 22 `static_cast` no `ffi.cpp` migrados para `reinterpret_cast` (sed em 3 passes); 4 sites de `return new X()` ganharam `reinterpret_cast<GgqlXxxHandle>(...)`. Sem ABI runtime change (`nm -D` ainda mostra 32 símbolos). Compilador agora rejeita type confusion entre handles. Bindings cliente regenerados via `dart run ffigen` — `ffigen` cria `final class GgqlXxx_ extends ffi.Opaque {}` + `typedef GgqlXxxHandle = Pointer<GgqlXxx_>`. Cliente teve 1 fix necessário no `test/ffi/smoke_round23_test.dart` (`Pointer<Void>.fromAddress` → `Pointer<GgqlEngine_>.fromAddress`). `flutter test test/ffi/`: 14 → **15/15** (cliente já tinha um teste C2 que ativou com bindings regenerados). |
| 2026-05-22 | **Fase 5 entregue** (próximo commit): P3/C5 — `struct_size` BREAKING + refactor das 3 funções ingest para out param. `GgqlIngestStats`, `GgqlEngineCapabilities`, `GgqlParseResult` ganharam `size_t struct_size` como primeiro campo. As 3 funções `ggql_ingestor_ingest*` mudaram de `GgqlIngestStats f(...)` para `int32_t f(..., GgqlIngestStats* out_stats)`; retorno: 0=ok, 1=fail, -1=NULL args, -2=struct_size insuficiente. `ggql_engine_capabilities` e `ggql_engine_validate` ganharam check `struct_size < sizeof(...)` → -2. Tests C++ atualizados — 2 cenários novos validando retorno -2 quando caller esquece de inicializar struct_size. Cliente: bindings regenerados via ffigen, `ggql_ingestor.dart` adaptado pra `calloc<GgqlIngestStats>()` + preencher `struct_size = sizeOf<GgqlIngestStats>()` + passar como out param, 2 sites em `smoke_round23_test.dart` ganharam preench de struct_size em PR + caps. `flutter test test/ffi/`: **15/15** mantido. |
| 2026-05-22 | **Fase 6 engine entregue** (próximo commit): 3 itens (C8/P8 + E2 + C3b). **C8** — `ggql_engine_execute_with_cap(eng, q, l, o, cap)` novo símbolo: variante de execute com cap per-query (cap=0 usa engine-level). Refactor: `execute_with_cap` virou o corpo canônico; `execute` legado delega passando cap=0 (zero duplicação). **E2** — `ggql_engine_last_error_into(eng, out, cap)` novo símbolo: copy-out API com semântica strlcpy (retorna `len`; truncamento se `retorno >= cap`; `out==NULL\|cap==0` sonda tamanho). Elimina dependência do consumer no `thread_local` interno. **C3b** — `cli::set_event_callback_tl` + `clear_event_callback_tl` + variável `thread_local` em `console.cpp`. `dispatch()` prefere TL, fallback no global (preserva back-compat para consumers legados). FFI migrou: `set_event_callback(&dispatch_bridge, &bridge)` → `set_event_callback_tl(...)`. Ingests paralelos em threads distintas agora têm canais isolados. 32 → **34 símbolos FFI** exportados (`execute_with_cap` + `last_error_into`; C3b é interno do `cli::`). `test_ffi_query` + `test_ffi_ingest_cancel`: ambos passam. Cliente: bindings regenerados; `flutter test test/ffi/`: **27/27** mantido. **Fase 6 lado engine 100% — falta cliente opcional ativar uso dos 2 novos símbolos via wrapper Dart `GgqlEngine`** (~5 LOC cada). |

**Arquivos analisados:**
- `src/ffi/ffi.cpp` (779 linhas, último mexido em `dafe6dd`)
- `include/ffi/ffi.h` (389 linhas)
- `docs/ffi_flutter_python_integration.md` (consumer-facing — para detectar divergências doc↔código)
- `tests/test_ffi_query.cpp` (cobertura existente de Round 1–3)
- `ggql_ui/docs/ffi_ingestion_analysis.md` (auditoria cliente — comparação cruzada na §6)

**Itens fora de escopo desta auditoria** (já endereçados em rounds anteriores ou não-FFI):
- TLS Initial-Exec / `LD_PRELOAD` — resolvido em `dafe6dd` (mimalloc na .so).
- `GgqlEvent` stack-local (kind=garbage) — resolvido com `thread_local out` em `dispatch_bridge`.
- Move ctor descartando snarls em FIND BUBBLE — resolvido em `5d9c8a5`.
- Performance de `path_stats` em HPRC — não-FFI (ver `path_stats_cache.md`).
