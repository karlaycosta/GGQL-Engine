# Análise — `include/ggql/query/query_planner.hpp`

Auditoria do **header público** do `QueryPlanner`. Diferente do `.cpp`
(já auditado em `analise_query_planner.md`), este foco é o header:
classes públicas (`RowFields`, `QueryResult`, `QueryPlanner`), aliases
de tipo (`KmerCache`), contrato de API exposto a callers externos
(`ffi.cpp`, `ggql_repl.cpp`, `query_evaluator.cpp`,
`test_query_planner.cpp`) e custo de transitive includes.

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/query/query_planner.hpp` (121 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 2 | 1 (B1) |
| Pontos de melhoria de performance / build | 2 | 0 |
| Código morto / docs stale | 2 | 0 |
| Atributos C++23 / qualifiers | 2 | 0 |

Severidade geral: **baixa**. O header é compacto (121 LOC) e bem
estruturado pós-auditoria do `.cpp`. Os achados são pequenos: um
buffer-overflow latente em `RowFields::insert`, comentário stale na
tabela de backends, e potencial economia de transitive-includes.

---

## Bugs / fragilidades

### B1 — `RowFields::insert` sem bounds check
**Severidade:** baixa (latente; uso atual é seguro)

```cpp
struct RowFields {
    static constexpr size_t kMaxFields = 6;
    std::array<std::string_view, kMaxFields> keys{};
    std::array<Value,            kMaxFields> vals{};
    size_t                                   count = 0;

    void insert(std::string_view k, Value v) noexcept {
        keys[count] = k;          // ← se count == 6, acesso fora dos bounds (UB)
        vals[count] = std::move(v);
        ++count;
    }
};
```

Atualmente, o uso máximo é 5 campos (em `plan_segments`: `id`, `length`,
`read_count`, `depth`, `name`) — a margem de 1 está respeitada. Mas se
um plan futuro adicionar 7+ campos:
- `keys[6] = k;` → escrita em endereço inválido → UB
- `noexcept` impede sinalização via exceção; falha silenciosa ou crash

**Fix sugerido (mínimo):**
```cpp
void insert(std::string_view k, Value v) noexcept {
    if (count >= kMaxFields) [[unlikely]] { return; }
    keys[count] = k;
    vals[count] = std::move(v);
    ++count;
}
```

Custos: 1 branch (predicted false) + return early. Em hot loop, custo
desprezível dado que count <= 5 em todos os callers atuais.

**Alternativa (debug-only):**
```cpp
#ifndef NDEBUG
    if (count >= kMaxFields) { std::abort(); }
#endif
```

Mais agressivo mas evita zero-cost overhead em release.

### B2 — `RowFields::find` é O(N) linear
**Severidade:** zero (N=6 max, linear é ideal)

Para `kMaxFields = 6`, comparação linear de `string_view` (≈ 16 B cada)
é o algoritmo mais rápido — qualquer hash/binary-search teria mais
overhead que ganho. **Manter.**

---

## Performance / build

### P1 — Headers transitivos pesados (`topology.hpp`, `metadata_store.hpp`, `fm_index.hpp`)
**Ganho estimado:** marginal (callers principais já usam os tipos completos)

Linhas 28-30:
```cpp
#include <ggql/ingest/metadata_store.hpp>
#include <ggql/storage/fm_index.hpp>
#include <ggql/storage/topology.hpp>
```

A classe `QueryPlanner` apenas armazena ponteiros (`topology_`,
`metadata_`, `seq_index_`). Os métodos públicos não retornam nem
recebem instâncias completas desses tipos. Forward-declaration
reduziria transitive-includes:

```cpp
namespace ggql {
class GfaCsrTopology;
class GfaMetadataStore;
class GfaSequenceIndex;
}
```

**Análise de impacto:**
- `ffi.cpp` — instancia `GfaMetadataStore` e `GfaSequenceIndex`,
  precisa do header completo (próprio).
- `ggql_repl.cpp` — idem, instancia ambos.
- `test_query_planner.cpp` — instancia `GfaCsrTopology`.
- `query_evaluator.cpp` — só usa `RowFields::find` que retorna `Value*`;
  não acessa membros de topology/metadata. **Beneficiaria.**
- `query_planner.cpp` — usa todos os métodos. Precisa do header completo.

**Conclusão:** o ganho real é só para `query_evaluator.cpp` e talvez
TUs futuros. Como os callers principais (ffi, repl, test, planner)
precisam dos headers completos de qualquer forma, o ganho de
compilação é marginal. **Aceitar como-está**, mas anotar para
considerar se o build começar a ficar lento.

### P2 — Includes `<unordered_map>` + `<unordered_set>` puxados por todo TU
**Ganho estimado:** zero

Linhas 24-25 são inevitáveis enquanto `KmerCache` for typedef público no
header. Mover `KmerCache` para detalhe interno do `.cpp` exigiria
refator: `eval_where(...const KmerCache* cache=nullptr)` na API privada
seria substituído por `eval_where(...std::function<bool(...)>)` ou
similar. **Trade-off: simplicidade da API > economia de includes.**

---

## Código morto / docs stale

### M1 — Comentário stale: `BUBBLE → snarls Phase 2`
**Severidade:** zero (apenas legibilidade)

Linha 11:
```cpp
//   BUBBLE       │ GfaCsrTopology::find_bubbles()     │ ⚠️  (→ snarls Phase 2)
```

Comentário **desatualizado**. A implementação atual (`plan_bubbles` em
query_planner.cpp:693) usa `topology_->snarls()` (snarl_decomposition
pré-computado), não `find_bubbles()`. Phase 2 já está concluída e em
produção desde o commit `6e58bc1`. Atualizar para:

```cpp
//   BUBBLE       │ GfaCsrTopology::snarls() (pré-computado) │ ✅
```

### M2 — Backend de PATH na tabela menciona apenas `path_nodes()`
**Severidade:** zero

Linha 14:
```cpp
//   PATH         │ GfaCsrTopology::path_nodes()       │ ✅
```

Atualmente `plan_paths` usa `path_node_count` + `path_bp_length` (path
stats pré-computado) como caminho rápido, com fallback a `path_nodes()`.
Atualizar para:

```cpp
//   PATH         │ path_stats / path_nodes() (fallback) │ ✅
```

---

## Atributos C++23 / qualifiers

### A1 — `RowFields::keys{}` e `vals{}` value-initialization explícita redundante
**Severidade:** zero

Linhas 43-44:
```cpp
std::array<std::string_view, kMaxFields> keys{};
std::array<Value,            kMaxFields> vals{};
```

`std::array` member-initializers `{}` aplicam value-initialization
mesmo sem o `{}` explícito (membros não-inicializados em
default-initialized struct). É cosmético — mantém intent claro,
mas é redundante. **Manter** (auto-documenta intenção).

### A2 — `QueryPlanner` sem move ctor/assign explícito
**Severidade:** baixa (limitação API)

```cpp
QueryPlanner(const QueryPlanner&)            = delete;
QueryPlanner& operator=(const QueryPlanner&) = delete;
// faltando: move ctor + move assign
```

Pela rule-of-5, copy=delete sem move-explícito → move-ctor e
move-assign **implicitamente deletados**. Atualmente nenhum caller
move QueryPlanner (todos passam por referência ou ponteiro), então
não é bug ativo. Mas:
- `std::vector<QueryPlanner>` não compila
- `std::optional<QueryPlanner>::emplace` usando construtor não-default
  precisa de move

**Fix sugerido:**
```cpp
QueryPlanner(QueryPlanner&&) noexcept            = default;
QueryPlanner& operator=(QueryPlanner&&) noexcept = default;
```

A classe tem só 3 ponteiros não-proprietários — move é trivial e
zero-cost.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | M1 — Comentário stale `BUBBLE → snarls Phase 2` | doc | mínimo | ✅ aplicado |
| 2 | M2 — Atualizar backend PATH para path_stats | doc | mínimo | ✅ aplicado |
| 3 | B1 — Bounds check em `RowFields::insert` | bug | mínimo | ✅ aplicado |
| 4 | A2 — Adicionar move-ctor/assign explícitos | API | mínimo | ✅ aplicado |
| 5 | P1 — Forward-decls dos 3 headers grandes | build | médio (semantic) | ✅ aplicado |
| 6 | A1 — Manter `keys{}` / `vals{}` (cosmético) | n/a | n/a | confirmado n/a |

### Itens 1-4 aplicados (2026-05-05)

**M1+M2** — Tabela de backends atualizada na docstring:
- BUBBLE: `find_bubbles() ⚠️ Phase 2` → `snarls() (pré-computado) ✅`
- PATH: `path_nodes()` → `path_stats / path_nodes() (fallback)`

**B1** — `RowFields::insert` agora rejeita silenciosamente quando
`count >= kMaxFields`:
```cpp
if (count >= kMaxFields) [[unlikely]] { return; }
```
Branch predito como falso (uso atual sempre passa); zero-cost em
release. Elimina UB latente caso plan futuro adicione 7+ campos.

**A2** — Adicionados move-ctor e move-assign `= default noexcept`
explícitos. `QueryPlanner` agora pode ser usado em `std::vector`,
`std::optional::emplace`, etc. Move é trivial (3 ponteiros
não-proprietários, zero-cost).

17/17 testes passando.

### Item 5 aplicado (2026-05-05)

**P1** — Forward-declarations substituem os 3 includes pesados:
```cpp
namespace ggql {
class GfaCsrTopology;
class GfaMetadataStore;
class GfaSequenceIndex;
}
```

Includes diretos adicionados nos `.cpp` que precisam dos tipos completos
(usam `metadata_->...`, `seq_index_->...`, `topology->...`):
- `src/query/query_planner.cpp`: + 3 includes
- `src/query/query_evaluator.cpp`: + 2 (não usa topology diretamente)
- `src/repl/ggql_repl.cpp`: + 2 (instancia metadata + fm_index)
- `src/ffi/ffi.cpp`: já tinha
- `tests/test_query_planner.cpp`: já incluía topology.hpp via outros paths

**Impacto:** TUs que apenas passam ponteiros de QueryPlanner agora
não recompilam quando `topology.hpp`, `metadata_store.hpp` ou
`fm_index.hpp` mudam (a menos que toquem nos próprios usos
diretos). Ganho real é incremental — depende de quão frequentes são
mudanças nesses headers.

17/17 testes passando após fix dos includes diretos.

### Item 6 confirmado n/a

`std::array` member-initializers `{}` em `RowFields::keys` e `vals`
são redundantes mas auto-documentam intent. **Mantidos como-está.**

---

## Auditoria do header completa

6/6 itens cobertos: itens 1-5 aplicados em 2 commits sequenciais
(`bb8f464` para 1-4, próximo para 5); item 6 confirmado como n/a.

**Itens 1-4** (cleanup baixo-risco, ~5 min) podem ser aplicados
imediatamente. **Item 5** (forward-decls) tem ganho marginal e exige
revalidar todos os 5 callers.

---

## Apêndice — itens avaliados e descartados

- **`RowFields` como `class` em vez de `struct`** com getters — POD-like
  é apropriado para uso interno; getters seriam boilerplate.
- **`std::flat_map` em `KmerCache`** — análise idêntica ao `.cpp`:
  unordered_map é fine para 1-3 kmers.
- **`compare_values` `[[nodiscard]] noexcept`** — já tem ambos.
- **Particionar header em `query_planner_types.hpp` (RowFields,
  QueryResult, KmerCache) + `query_planner.hpp` (classe)** — refator
  estrutural sem ganho real para 121 LOC.
- **`RowFields::find` retornar `std::optional<reference_wrapper<Value>>`**
  — `const Value*` com nullptr é idiomático em C++ e mais leve.
- **`KmerCache` virar `flat_set<pair<...>>` ou similar** — overhead
  de transformação > ganho.
- **`#include <ggql/ingest/metadata_store.hpp>` substituir só por
  `<gfa_metadata_store.fwd.hpp>`** — não há padrão `.fwd.hpp` no
  projeto; criar um seria infrastructure novo sem ganho mensurável.
