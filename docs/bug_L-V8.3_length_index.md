# Bug L-V8.3 — Operador `>=` em comparação int retornava `>` (typo no `cmp_op`)

> **Status**: ✅ **FIXADO** em 2026-05-12 (`src/query/query_evaluator.cpp:33`)
> **Descoberto em**: 2026-05-11 (campanha de memcheck ASan)
> **Severidade**: 🔴 **CRÍTICA** — corrupção silenciosa de resultados em produção

## Sintomas

`FIND SEGMENT WHERE length >= 32` retorna **count incorreto** em
`chm13_subset_100w` (4.3M segs, 11,528 com length=32):

| Predicado | Resultado | Esperado | Status |
|---|---:|---:|:-:|
| `length = 32` | 11,528 | — | — |
| `length > 32` | 1,287,191 | — | — |
| **`length >= 32`** | **1,287,191** | **1,298,719** | ❌ **PERDE** os 11,528 com length=32 |
| `length = 32 OR > 32` | 1,298,719 | 1,298,719 | ✅ |
| `length > 31` | 1,298,719 | 1,298,719 | ✅ (equivalente algébrica) |
| `length >= 31` | 1,298,719 | — | ✅ |
| `length >= 33` | 1,275,686 | — | ✅ |

**Magnitude do erro**: `>=` perde **0.89% dos resultados** em chm13_100w.
Em datasets com mais duplicates pode ser pior.

## Reprodutibilidade

Determinístico:
- **3 runs no mesmo build** → mesmo resultado errado
- **Manifesta-se sob ASan** (`-fsanitize=address`, build-asan/)
- **Não se manifesta** em build padrão `-O2` (build/)

A divergência entre build/asan sugere **uninitialized memory read** ou
**read past array bounds** que sem ASan retorna lixo "feliz" mas com ASan
retorna zero/poisoned.

## Caminho de código

Query: `FIND SEGMENT WHERE length >= 32`

1. `query_planner.cpp:953` → `extract_length_range(*q.where)` retorna `(32, max)`
2. `query_planner.cpp:954` → `metadata_->ids_in_length_range(32, max)`
3. `metadata_store.hpp:268-276` → usa `std::lower_bound` e `std::upper_bound`:

```cpp
[[nodiscard]] std::span<const uint32_t>
ids_in_length_range(uint32_t lo, uint32_t hi) const noexcept {
    const auto beg = std::lower_bound(length_sorted_vals_.begin(),
                                      length_sorted_vals_.end(), lo);
    const auto end = std::upper_bound(beg, length_sorted_vals_.end(), hi);
    const size_t off = static_cast<size_t>(beg - length_sorted_vals_.begin());
    const size_t cnt = static_cast<size_t>(end - beg);
    return std::span<const uint32_t>(length_sorted_ids_.data() + off, cnt);
}
```

`length_sorted_vals_` é `vector<uint32_t>` construído por sort de pares
(length, id). 11528 elementos com `vals[i] == 32` ficam consecutivos.

## CAUSA RAIZ identificada (2026-05-12)

Typo no template `cmp_op<T>` em `src/query/query_evaluator.cpp:33`:

```cpp
case Operator::EQ:  return l == r;
case Operator::NEQ: return l != r;
case Operator::GT:  return l > r;
case Operator::LT:  return l < r;
case Operator::GEQ: return l > r;   // ← BUG: c/p de GT, deveria ser >=
case Operator::LEQ: return l <= r;
```

**Fix**: trocar `return l > r;` por `return l >= r;` na linha do `GEQ`.

### Por que não foi pego antes?

- `cmp_op` é template `constexpr noexcept` chamado para `int64_t`, `double`,
  `std::string`. **Todos os 3 instances afetados** (length, depth pré-L-V8 fix,
  e qualquer comparação string com `>=`).
- O caminho rápido `length_index.ids_in_length_range(lo, hi)` retornava IDs
  corretos, MAS `process_id()` re-avaliava `WHERE length >= X` via
  `eval_condition → compare_values → cmp_op` que rejeitava os com `len == X`.
- Sprints 1-12 nunca testaram `WHERE length >= X` em dataset com
  `count(length=X) > 0` significativo. R3+R4+R5 usavam `>` e `<` predominantemente.
- A mutação ROR `>= → >` em mutation testing (R13) detectaria o bug, mas o
  bug ESTÁ no source, então a mutação era equivalente (a mutação seria
  trocar `>` por `>=` e isso detectaria 1298719 mas no source temos 1287191).

### Impacto retroativo

Qualquer query no formato `WHERE <field int> >= X` com `count(field == X) > 0`
em datasets anteriores retornou **menos resultados** que o esperado:

- `WHERE length >= 32` (chm13_100w): perdia 11,528 segmentos
- `WHERE read_count >= 1` em datasets com depth tags: perdia todos com `rc=1`
- `WHERE depth >= 0` em datasets com depth=0: perdia esses (fixado por L-V8
  para float, mas int64 ainda afetado)

Bug existiu desde a introdução de `cmp_op` template. ⚠ Resultados publicados
em sessões anteriores que envolveram `>=` podem precisar de recalibração.

## Hipóteses (descartadas — bug não estava no length_index)

### H1 — `lower_bound` retorna iterator inválido pós-load

`build_length_index()` é chamado durante ingest. Mas após `load(dir)`,
`length_sorted_vals_` e `length_sorted_ids_` são lidos do disco. Pode
haver tamanhos inconsistentes ou ordering quebrado após deserialize.

**Teste**: imprimir `length_sorted_vals_.size()` após load + comparar com
`length_sorted_ids_.size()`. Verificar invariante `is_sorted()`.

### H2 — Race condition no construtor (improvável)

`build_length_index` é serial e síncrono. Improvável race.

### H3 — Promoção `uint32_t` para `int` em comparação

`std::lower_bound(begin, end, 32)`: o literal `32` é `int`. Comparador
default é `<`, que pode promover incorretamente se houver bits sinal.
Mas valores aqui são positivos e <2^32, não deveria afetar.

**Teste**: rodar com `static_cast<uint32_t>(32)` explícito.

### H4 — Bug em `extract_length_range` para `>=`

```cpp
case Operator::GEQ:
    lo = std::max(lo, uv);  // ← linha 208
    found = true;
    break;
```

Parece correto. Para `>= 32`: `lo = max(0, 32) = 32`. Range correto `(32, max)`.

**Teste**: adicionar print de `lo`/`hi` extraídos pelo extract_length_range.

### H5 — Read-only-mmap após load corrompe binário

`length_index.bin` é serializado/carregado. Se o load leu N-1 elementos
em vez de N (off-by-one no parse de header), o último valor 32 ficaria fora
do `length_sorted_vals_`, mas `lower_bound` sobre os N-1 primeiros não o
encontraria.

**Teste**: verificar tamanho de `length_index.bin` vs `num_nodes_ * 8` bytes
(2 vectors de uint32_t).

### H6 — Bug em `extract_length_range` quando há SÓ `>=` (sem outras conditions)

A condition WHERE `length >= 32` tem 1 cond + 0 connectors. O loop
de `extract_length_range` itera 1 vez, seta `lo=32`, `found=true`.

`hi = std::numeric_limits<uint32_t>::max()`.

Retorna `(32, max)`. Aparentemente correto.

**Teste**: imprimir o range retornado.

## Plano de mitigação

1. **Verificar tamanhos**: comparar `length_sorted_vals_.size()` com
   `metadata_->size()` antes e depois de load.
2. **Verificar invariante**: `is_sorted(length_sorted_vals_)` deve ser true.
3. **Imprimir resultado de lower_bound/upper_bound** com lo=32, hi=max.
4. **Comparar com força bruta**: full scan de `lengths_[i] >= 32` deve
   retornar 1,298,719.
5. **Se causa identificada**: fix + adicionar test boundary com duplicates
   massivos.

## Verificação pós-fix

```
WHERE length >= 32: 1298719  ✅ (era 1287191, Δ=+11528 = count(length=32))
test_query_planner_idx:       26/26 testes passam (era 20+5/26)
ctest completo:               14/14
```

## Severidade

🔴 **CRÍTICO** porque:
1. Bug silencioso (não erro, não warning) em produção
2. Resultado incorreto em filtro **clássico** (`length >= N`)
3. Magnitude do erro é proporcional ao número de duplicates em X
4. Cobertura de R3+R4+R5 não detectou (queries usavam `=`, `>`, `<` mas
   não `>=` exato no boundary)
5. Detectado por **acaso** durante memcheck — sem ASan e sem testes
   específicos, ficaria invisível

## Critério de fechamento

Bug L-V8.3 está fechado quando:
- ✅ Causa raiz identificada (uma das H1-H6 ou nova)
- ✅ Fix aplicado em código + regression test
- ✅ Build/build-asan ambos retornam 1,298,719 para `length >= 32`
- ✅ Test boundary com duplicates massivos em test_query_planner_idx

## Histórico

- **2026-05-11**: descoberto durante memcheck R_memcheck (ASan + LSan +
  UBSan). Sintoma reportado em `tests/validation/r_memcheck_results/`.
- **2026-05-11**: hipóteses H1-H6 listadas. Investigação em andamento.
  Hipótese H1 (length_index corrompido) descartada via `tools/debug_lv8_3`
  comparando `ids_in_length_range` vs brute-force `meta.length(i)` —
  ambos retornavam 1,298,719 corretamente.
- **2026-05-12**: causa raiz identificada — typo em
  `src/query/query_evaluator.cpp:33` no template `cmp_op<T>`. Linha
  `case Operator::GEQ: return l > r;` corrigida para `return l >= r;`.
  Fix de **uma única linha**. ctest 14/14, R13.A 26/26, query direta
  `WHERE length >= 32` → 1,298,719 (correto).
- **Lição**: o sintoma inicial (`>=` perde duplicates) parecia um problema
  de `length_index` ou `lower_bound`/`upper_bound`. Na verdade era um typo
  trivial muito mais a fundo na pilha. Mutation testing (R13) já tinha
  classificado a mutação `>= → >` como "killed após R13.A" — porque
  R13.A boundary failed expondo o bug indiretamente, mas só descobrimos
  a causa raiz quando comparamos resultados entre os 2 binários (run_queries
  stale vs test isolado). Memcheck ASan foi o gatilho final por evidenciar
  inconsistência inter-build.
