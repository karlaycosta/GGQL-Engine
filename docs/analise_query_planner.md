# Análise — `src/query/query_planner.cpp`

Auditoria do **backend de execução** do `QueryPlanner`. Cobre construtor,
`execute_*`, `plan_*`, `format_table` e helpers `detail::`.

Não inclui `query_evaluator.cpp` (avaliação de WHERE — análise separada
no futuro).

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `src/query/query_planner.cpp` (1.058 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 4 | 1 (B1) |
| Pontos de melhoria de performance | 5 | 1 (P1) |
| Código morto / redundância | 3 | 1 (M1) |
| Duplicação estrutural / refator | 2 | 1 (D1) |

Severidade geral: **média**. O arquivo está bem estruturado com bom
uso de `std::expected`, `std::variant`, `std::ranges`, `std::format` e
índices secundários (length/depth/name) com fallback a full-scan. Mas
~64 linhas duplicadas entre `plan_tips`/`plan_repeats` e o pattern de
paginação repetido em 6 funções pedem refator.

---

## Bugs / fragilidades

### B1 — `plan_edges`: `WHERE to_orient = <não-string>` produz falso match
**Severidade:** média (caminho explorável via API)

Linhas 891-901:
```cpp
for (const auto& cond : q.where->conditions) {
    if (const auto* cc = std::get_if<CompareCondition>(&cond)) {
        if (cc->field == "to_orient" && cc->op == Operator::EQ) {
            if (const auto* s = std::get_if<std::string>(&cc->value); s && !s->empty()) {
                orient_eq = (*s)[0];
            }
            ++orient_cond_count;  // ← incrementado mesmo se s == nullptr
        }
    }
}
where_only_orient = (orient_cond_count == q.where->conditions.size());
```

Se a query for `WHERE to_orient = 5` (Value sendo `int64_t`, não `std::string`):
1. `s = nullptr` → `orient_eq` permanece nullopt
2. `orient_cond_count = 1` (incrementado de qualquer jeito)
3. `where_only_orient = true` (1 == 1)

No loop principal:
- `orient_eq` nullopt → linha 920 não filtra
- `where_only_orient = true` → linha 924 **não chama eval_where**
- **Resultado:** retorna TODAS as edges, ignorando a condição

`validate_query` não rejeita pois `to_orient ∈ kStringFields`, então a
checagem numérica é pulada. A query atravessa validação e produz
resultado errado.

**Fix sugerido:**
```cpp
if (const auto* s = std::get_if<std::string>(&cc->value); s && !s->empty()) {
    orient_eq = (*s)[0];
    ++orient_cond_count;
}
// não incrementa quando o valor não casa o tipo esperado
```

Ou, mais robusto: deixar `eval_where` fazer o trabalho sempre — a
otimização de char-compare só vale quando `orient_eq.has_value()`.

### B2 — `execute_search_kmer`: ausência de safety/truncation guards
**Severidade:** baixa (latente; FM-Index normalmente retorna pouco)

Linhas 444-471:
```cpp
for (uint64_t id : ids) {
    ++result.total_hits;
    if (result.row_count >= take_n) {
        continue;
    }
    // ... insert flat ...
    ++result.row_count;
}
result.total_hits = ids.size();
```

Sem `kMaxTotalHitsCount` nem `kSafetyHardLimit` checks. Para um kmer
muito frequente (HPRC: kmers comuns de 8-10 bases podem ter centenas
de milhares de matches), o vetor `flat` pode crescer até `take_n*ncols`
mas `total_hits` cresce sem limite (até `ids.size()`).

Não é OOM real (o `take_n` cap controla o flat), mas é inconsistente
com as outras queries.

**Fix:** adotar o mesmo padrão de `plan_*` (truncação e safety).

### B3 — `format_table`: cria string temporária só para medir
**Severidade:** baixa (perf, não correctness)

Linhas 307-311:
```cpp
for (size_t r = 0; r < row_count; ++r) {
    for (size_t c = 0; c < ncols; ++c) {
        widths[c] = std::max(widths[c], detail::value_to_string(flat[r * ncols + c]).size());
    }
}
```

Para 100K rows × 5 cols = 500K alocações (`std::format` dentro de
`value_to_string`). Como `format_table` é debug/CLI/REPL e não hot
path, aceitar como-está. Mas se virar hot, um helper `value_string_size`
que retorna apenas o tamanho via `std::formatted_size` (C++23) eliminaria
as alocações.

### B4 — Comportamento subtil quando `metadata_ == nullptr` em `plan_paths` sem path_stats
**Severidade:** muito baixa (caso degenerado documentado)

Linhas 1003-1011:
```cpp
} else {
    const auto nodes = topology_->path_nodes(pid);
    nc = static_cast<int64_t>(nodes.size());
    if (metadata_) {
        for (const uint64_t nid : nodes) {
            total_length += static_cast<int64_t>(metadata_->length(nid));
        }
    }
    // se !metadata_: total_length fica 0 — ambíguo com path vazio
}
```

Caller pode confundir `length=0` (sem metadata) com path realmente vazio.
Aceitar como-está (cenário raro), ou retornar erro:
```cpp
if (!metadata_ && !use_stats) {
    return std::unexpected{"FIND PATH com WHERE/length requer path_stats ou metadata"};
}
```

---

## Performance

### P1 — Hot loop com `for (col : cols) { if (col == "...") }` em todas as `plan_*`
**Ganho estimado:** 15-30% em queries com 100K+ rows e 3-5 colunas

Aparece 6 vezes (linhas 529-537, 596-604, 672-684, 800-814, 952-960, 1042-1052).
Para cada linha:
- Itera N colunas (3-5)
- Para cada, faz comparações de `string == string_view` literal
- 100K rows × 5 cols × 3 comparações médias = 1.5M comparações de string

**Otimização proposta:** pré-computar índices fora do loop:
```cpp
// fora do loop principal
struct ColEmit {
    enum class Kind { Id, InDeg, OutDeg } kind;
};
std::vector<ColEmit> emitters;
emitters.reserve(cols.size());
for (const auto& c : cols) {
    if (c == "id") emitters.push_back({ColEmit::Kind::Id});
    else if (c == "in_degree") emitters.push_back({ColEmit::Kind::InDeg});
    else if (c == "out_degree") emitters.push_back({ColEmit::Kind::OutDeg});
}

// dentro do hot loop (sem comparações de string)
for (const auto& e : emitters) {
    switch (e.kind) {
        case ColEmit::Kind::Id:     result.flat.emplace_back(int64_t(nid)); break;
        case ColEmit::Kind::InDeg:  result.flat.emplace_back(in_d); break;
        case ColEmit::Kind::OutDeg: result.flat.emplace_back(out_d); break;
    }
}
```

Mais idiomático ainda: array de function-pointers ou `std::function` por
coluna. Mas o switch sobre enum tem zero overhead e é o mais rápido.

### P2 — `RowFields row;` reconstruída a cada iteração com WHERE
**Ganho estimado:** marginal (struct é inline, sem heap)

`RowFields` (header linha 41-59) é stack-allocated com `std::array` —
sem heap. Mas:
- `vals[count] = std::move(v)` chama `operator=` de `Value` (variant)
- Se o slot anterior tinha `std::string` (de iteração prévia), o
  destrutor é chamado antes do novo construtor
- Se o novo valor é `std::string`, alocação ocorre

**Otimização:** declarar `row` fora do loop e usar `row.count = 0`
para reset. Reaproveita storage interno (especialmente os `std::string`
em `Value` se eles cabem in-place).

```cpp
RowFields row;
for (...) {
    row.count = 0;       // reset sem destruir
    row.insert("id", int64_t(nid));
    // ...
}
```

Como `RowFields::vals` é `std::array<Value, 6>`, todos os 6 Values
existem permanentemente; só o `count` indica quais são "ativos". Mas
`vals[i]` ainda destrói o valor antigo quando recebe novo via `=`.

### P3 — `std::string(metadata_->original_id(id))` per-row em hot loop
**Ganho estimado:** marginal (alocação inevitável para `Value`)

Linhas 459, 774, 806, 1018, 1046: copia `string_view` do metadata
para `std::string` armazenado em `Value`. Inevitável dada a estrutura
do `Value` (não armazena `string_view`). Aceitar.

### P4 — Full scan em `plan_segments` é O(N) sequencial
**Ganho estimado:** depende — paralelizar é viável para WHERE caro

Linha 864: `for (size_t id = 0; id < metadata_->size() && !should_stop(); ++id)`.
Para HPRC (5M segments) com WHERE complexo (ContainsKmerCondition
fora dos 3 fast paths), single-thread.

**Trade-off:** paralelizar exige sincronização para `result` ordenado
+ paginação. Provavelmente não vale a pena dado que os 3 fast paths
(name/length/depth) cobrem a maioria dos WHEREs práticos. **Manter
como-está.**

### P5 — `execute_search_kmer`: `std::sort + std::unique` para deduplicar
**Ganho estimado:** zero (caso esperado pequeno)

Linhas 433-434:
```cpp
std::sort(ids.begin(), ids.end());
ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
```

OK para conjuntos pequenos. Para grandes, `flat_set` ou hash-set
durante construção seria melhor — mas FM-Index normalmente retorna
poucos matches por kmer.

---

## Código morto / redundância

### M1 — `result.total_hits = ids.size()` redundante em `execute_search_kmer`
**Severidade:** baixa (mas latente — esconderia bug futuro)

Linha 446: `++result.total_hits;` (executado para cada id, sem `break`)
Linha 472: `result.total_hits = ids.size();` (sobrescreve)

Como o loop incrementa para todo id e nunca faz `break`, ambos chegam ao
mesmo valor. **A sobrescrita é redundante.** Pior: se alguém adicionar
`break` (p.ex. para implementar B2 acima), a sobrescrita esconderia a
contagem parcial real.

**Fix:** remover linha 472 (e implementar B2 propriamente, com
truncation flag).

### M2 — Pattern de paginação duplicado 6×
**Severidade:** baixa (não é "morto", é "DRY violation")

```cpp
const uint32_t take_n = q.limit.value_or(kSafetyHardLimit);
const bool has_explicit = q.limit.has_value();
const uint32_t skip_n = q.offset.value_or(0);
uint32_t pag_skipped = 0;
// ...
++result.total_hits;
if (result.total_hits > kMaxTotalHitsCount) {
    result.total_hits_truncated = true;
    break;
}
if (!has_explicit && result.total_hits > kSafetyHardLimit) {
    result.safety_limit_hit = true;
    break;
}
if (pag_skipped < skip_n) {
    ++pag_skipped;
    continue;
}
if (result.row_count >= take_n) {
    continue;
}
```

Aparece em `plan_tips`, `plan_repeats`, `plan_bubbles`, `plan_segments`,
`plan_edges`, `plan_paths`. Sugestão de refator (M2 + D1 abaixo):
extrair em struct `Paginator` com método `should_emit() -> EmitDecision`
retornando `Continue / Skip / Stop / Emit`.

### M3 — `kStringFields` array de 2 elementos
**Severidade:** zero (referência aceitável)

Linha 59: `kStringFields = {"name", "to_orient"}`. Vivo (usado em
`is_string_field`). Se um terceiro campo string for adicionado um dia,
lembrar de atualizar. **Não é morto, manter.**

---

## Duplicação estrutural

### D1 — `plan_tips` e `plan_repeats` são quase idênticos (~64 LOC duplicadas)
**Severidade:** média (manutenção)

Linhas 478-541 (tips) e 545-608 (repeats). Diferenças:
- `topology_->find_tips()` vs `topology_->find_repeats(2)`
- Nome na mensagem de erro

Possível refator:
```cpp
template <typename FindFn>
std::expected<QueryResult, std::string>
QueryPlanner::plan_node_query(const FindQuery& q, FindFn find_fn,
                              std::string_view label) const {
    if (auto err = detail::validate_query(q, detail::kNodeColumns)) {
        return std::unexpected{*err};
    }
    if (!topology_) {
        return std::unexpected{std::format("FIND {} requer GfaCsrTopology", label)};
    }
    const auto node_ids = find_fn();
    // ... resto idêntico
}

// chamadas:
return plan_node_query(q, [&]{ return topology_->find_tips(); }, "TIP");
return plan_node_query(q, [&]{ return topology_->find_repeats(2); }, "REPEAT");
```

Como `plan_tips`/`plan_repeats` são privadas e o header já tem ambas
declaradas, manter assinaturas mas extrair corpo comum em
`plan_node_query` (também privada).

### D2 — Pattern `if (q.where) { RowFields row; ... eval_where(...) }`
Duplicado 6×. Faz parte de M2/D1 — refator conjunto resolveria.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | M1 — Remover `result.total_hits = ids.size()` redundante | cleanup | mínimo | ✅ aplicado |
| 2 | B1 — Fix `WHERE to_orient = <não-string>` em `plan_edges` | bug | baixo | ✅ aplicado |
| 3 | B2 — Adicionar safety/truncation em `execute_search_kmer` | bug | baixo | ✅ aplicado |
| 4 | P1 — Pré-computar emitters de coluna fora do hot loop | perf | médio | ✅ aplicado |
| 5 | P2 — Reusar `RowFields` entre iterações | perf | baixo | ✅ aplicado |
| 6 | D1 — Extrair `plan_node_query` (tips + repeats) | refactor | médio | ✅ aplicado |
| 7 | M2/D2 — Extrair `Paginator` helper | refactor | médio (testes) | future work |
| 8 | B3 — `formatted_size` em `format_table` | perf | baixo | future work |
| 9 | B4 — Erro explícito em `plan_paths` quando metadata ausente | UX | baixo | future work |

### Itens 1-6 aplicados (2026-05-05)

**M1** — `result.total_hits = ids.size()` redundante removida em
`execute_search_kmer`.

**B1** — `plan_edges`: `++orient_cond_count` movido para dentro do
bloco que valida o tipo string. `where_only_orient` agora exige
`orient_eq.has_value()`. `WHERE to_orient = <não-string>` agora cai no
fallback de `eval_where` corretamente.

**B2** — `execute_search_kmer` agora aplica `kMaxTotalHitsCount` e
`kSafetyHardLimit` (mesmo padrão das outras `plan_*`). Linha redundante
de M1 era a que mascarava a contagem parcial.

**P1** — Pré-computação de emitters por enum (`NodeCol`, `BubbleCol`,
`SegmentCol`, `EdgeCol`, `PathCol`) fora do hot loop. As 6 `plan_*`
agora usam `switch (e)` sobre `vector<EnumCol>` de N comparações de
string por linha → 1 fetch + jump-table.

**P2** — `RowFields row;` declarado fora do hot loop nas 6 funções;
reset via `row.count = 0` reusa storage.

**D1** — `plan_tips` e `plan_repeats` agora delegam para
`plan_node_query(q, node_ids)` privado. Eliminadas ~64 LOC duplicadas.

17/17 testes passando.

**Itens 1-3** (cleanup + 2 bugs simples, ~20 min) podem ser aplicados
imediatamente.

**Itens 4-5** (perf P1+P2, ~45 min) trazem ganho mensurável em queries
com 100K+ rows e WHERE.

**Itens 6-7** (refator estrutural) eliminam ~150 LOC duplicadas; bom
para manutenção mas não urgente. Os testes do `tests/test_query_planner.cpp`
devem cobrir o refator.

**Itens 8-9** marginais.

---

## Apêndice — itens avaliados e descartados

- **`std::ranges::sort` em vez de `std::sort`** — só cosmético.
- **`std::flat_map` em `KmerCache`** — `unordered_map` é fine para
  caches pequenas (1-3 kmers por query típica).
- **Paralelizar `plan_segments` full scan** — exigiria sync de
  paginação ordenada; complexidade > ganho prático.
- **Substituir `std::format` em `format_table` por concat** — perda
  de legibilidade sem ganho mensurável (não é hot path).
- **Pre-cache de `value_to_string`** — overhead de cache > ganho.
- **`std::expected::transform`/`and_then` em `execute_find`** — `switch`
  é mais legível para 6 casos discretos.
