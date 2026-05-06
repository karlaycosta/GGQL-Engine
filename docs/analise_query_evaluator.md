# Análise — `src/query/query_evaluator.cpp`

Auditoria da **avaliação de cláusulas WHERE** do `QueryPlanner`. Contém
3 funções: `compare_values` (comparação tipada), `eval_condition`
(despacho via `std::visit` para 3 tipos de condição), `eval_where`
(short-circuit AND/OR).

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `src/query/query_evaluator.cpp` (151 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 5 | 1 (B1) |
| Pontos de melhoria de performance | 1 | 0 |
| Código morto / TODO documentado | 1 | 0 |
| Atributos C++23 / qualifiers | 0 | 0 |

Severidade geral: **média**. O arquivo é compacto e usa C++23
idiomáticos (`std::visit`, `if constexpr`, `std::unreachable`,
`std::variant`), mas tem 1 bug crítico (`std::get` em função `noexcept`
pode chamar `std::terminate`), comparação int×int via double (perda de
precisão), e tratamento inconsistente entre casos string-vs-int.

---

## Bugs / fragilidades

### B1 — `std::get<int64_t>` em `eval_condition` (`noexcept`) chama `terminate` em tipo errado
**Severidade:** alta (UB latente)

Linhas 103-106:
```cpp
const Value* id_val = row.find("id");
if (!id_val) { return false; }
const uint64_t current_id =
    static_cast<uint64_t>(std::get<int64_t>(*id_val));
```

`eval_condition` é declarada `noexcept`. Se `*id_val` armazenar
`std::string` ou `double` em vez de `int64_t` (alguém constrói row com
tipo errado), `std::get<int64_t>` lança `std::bad_variant_access` →
`std::terminate()` (UB pelo standard).

Em produção, `plan_segments` sempre coloca `int64_t` em "id" (linha
762 do `query_planner.cpp`: `row.insert("id", int64_t(id))`), então
nunca acontece. Mas é fragilidade.

**Fix sugerido:**
```cpp
const auto* id_int = std::get_if<int64_t>(id_val);
if (!id_int) { return false; }
const uint64_t current_id = static_cast<uint64_t>(*id_int);
```

`std::get_if` retorna nullptr (não lança) em tipo errado, alinhando
com o `noexcept` da função.

### B2 — Caso int64_t × int64_t usa `double` (perda de precisão)
**Severidade:** baixa (latente; valores típicos < 2^53)

Linhas 45-48:
```cpp
if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
    return cmp_dbl(static_cast<double>(std::get<int64_t>(lhs)), op,
                   static_cast<double>(std::get<int64_t>(rhs)));
}
```

`cmp_dbl` é a lambda que faz comparação via `double`. Para inteiros
com magnitude > 2^53 (~9.0×10¹⁵), `static_cast<double>(int64_t)` perde
precisão — comparações `==`/`!=` podem dar falso positivo/negativo.

Casos típicos no GGQL (length, depth, id, segment count) são todos
< 10⁹ — bug nunca dispara. Mas se um campo `bp_length` cumulativo
crescer (paths longos × bilhões de bases), pode ser exposto.

**Fix sugerido:** comparar inteiros diretamente, sem cast:
```cpp
if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<int64_t>(rhs)) {
    return cmp_int(std::get<int64_t>(lhs), op, std::get<int64_t>(rhs));
}
```

Onde `cmp_int` é função análoga a `cmp_dbl` mas com `int64_t`.

### B3 — Tratamento assimétrico de `string × int64_t` vs `int64_t × string`
**Severidade:** média (queries `WHERE "100" > id` se comportam diferente de `WHERE id > "100"`)

Linhas 73-79 (Caso 4):
```cpp
if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<std::string>(rhs)) {
    auto ld = as_double(lhs);
    auto rd = as_double(rhs);
    if (ld && rd) { return cmp_dbl(*ld, op, *rd); }
    return false;
}
```

Trata apenas `int64_t × string`. **Caso simétrico `string × int64_t`
cai no fallback final** (linha 81: `return op == Operator::NEQ`),
sempre retornando `op == NEQ`.

Isso significa:
- `WHERE id > "100"` (int×string) — converte ambos a double e compara.
- `WHERE "100" > id` (string×int) — sempre false (exceto NEQ).

Em PEGTL atual, lhs é sempre o `field` e rhs é o `value` literal,
então a assimetria não é exposta no parser. Mas se a API mudar
(p.ex. binding de queries via FFI com Value já-construído), quebra.

**Fix sugerido:** simetrizar e usar `op == NEQ` consistentemente:
```cpp
if ((std::holds_alternative<int64_t>(lhs) && std::holds_alternative<std::string>(rhs)) ||
    (std::holds_alternative<std::string>(lhs) && std::holds_alternative<int64_t>(rhs))) {
    auto ld = as_double(lhs);
    auto rd = as_double(rhs);
    if (ld && rd) { return cmp_dbl(*ld, op, *rd); }
    return op == Operator::NEQ;  // consistente com Caso 2
}
```

### B4 — Caso 4 retorna `false` em parse-fail, Caso 2 retorna `op == NEQ`
**Severidade:** baixa (inconsistência)

Linha 78: `return false;` (Caso 4 parse-fail).
Linha 55: `return op == Operator::NEQ;` (Caso 2 parse-fail).

Ambos os fallbacks tratam o mesmo cenário (tipos incompatíveis após
tentativa de promoção numérica) mas diferem no retorno. Padrão SQL é
NULL/UNKNOWN para tipos incompatíveis (que satisfaz NEQ mas não
predicados positivos). **Caso 2 está correto; Caso 4 está errado.**

Fix combinado com B3.

### B5 — `eval_where` faz left-to-right sem precedência AND > OR
**Severidade:** baixa (semântica documentada, mas não-padrão SQL)

Linhas 138-146:
```cpp
for (size_t i = 0; i < wc.connectors.size(); ++i) {
    if (wc.connectors[i] == LogicalOp::AND) {
        if (!result) { break; }
        result = eval_condition(wc.conditions[i + 1], row, cache);
    } else {
        if (result)  { break; }
        result = eval_condition(wc.conditions[i + 1], row, cache);
    }
}
```

Avaliação **estritamente esquerda-para-direita**. SQL e maioria das
linguagens dão AND > OR. Em GGQL atual:
- `A OR B AND C` → `(A OR B) AND C` (esquerda)
- SQL padrão: `A OR (B AND C)`

Para WHERE com 1-2 conditions (caso comum), nunca expõe diferença.
Para 3+ conditions com mix AND/OR, semântica diverge.

**Decisão:** documentar como design intencional OU implementar
precedência via parsing em árvore. Para escopo atual de queries
biológicas (filtros simples), aceitar como-está. **Fix: comentário
explícito acima de `eval_where` documentando a semântica.**

---

## Performance

### P1 — `cmp_dbl` lambda + switch duplicado em Caso 3 (string)
**Severidade:** marginal (cosmético + DRY)

Linhas 22-31 (cmp_dbl) e 62-70 (Caso 3 string):
```cpp
auto cmp_dbl = [](double l, Operator o, double r) -> bool {
    switch (o) { case EQ: return l==r; ... }
    std::unreachable();
};

// ...
if (...both string...) {
    switch (op) { case EQ: return l==r; ... }   // duplicação!
    std::unreachable();
}
```

Mesmo switch escrito 2 vezes. Refator com template (não-lambda) elimina:

```cpp
template <typename T>
[[nodiscard]] static constexpr bool cmp_op(const T& l, Operator o, const T& r) noexcept {
    switch (o) {
        case Operator::EQ:  return l == r;
        case Operator::NEQ: return l != r;
        case Operator::GT:  return l > r;
        case Operator::LT:  return l < r;
        case Operator::GEQ: return l >= r;
        case Operator::LEQ: return l <= r;
    }
    std::unreachable();
}
```

Usado:
```cpp
return cmp_op<double>(*ld, op, *rd);   // Caso 2/4
return cmp_op<std::string>(l, op, r);  // Caso 3
return cmp_op<int64_t>(li, op, ri);    // novo Caso 1 (B2 fix)
```

Compilador especializa por T, performance idêntica, código mais
DRY. Hot path inalterado.

---

## Código morto / TODO documentado

### M1 — `CrossesCondition` retorna sempre `false` (TODO Phase 2)
**Severidade:** zero (TODO documentado)

Linhas 97-99:
```cpp
} else if constexpr (std::is_same_v<T, CrossesCondition>) {
    // TODO Phase 2: integrar PathIndex
    return false;
}
```

Não é morto — a gramática permite `crosses(<path_name>)` em queries
(`grammar.hpp:103-106`), e o parser produz `CrossesCondition`. Mas
todas as avaliações retornam `false` → query com `crosses()` no WHERE
nunca matcha.

**Status:** known limitation. Manter o TODO. Considerar:
- Documentar na grammar.hpp/CHANGELOG que `crosses()` é parser-only
- Retornar erro em parse caso `crosses()` apareça (até implementar)

Atual passa silenciosamente — usuário pode escrever query sem saber
que sempre retorna 0 hits.

---

## Atributos C++23 / qualifiers

Estado já bom — declarações no header (`query_planner.hpp`) já têm:
- `[[nodiscard]]` em `compare_values`, `eval_condition`, `eval_where`
- `noexcept` em todas
- `static` em `compare_values`
- `const` em `eval_*` (métodos de classe)

C++23 features em uso:
- `std::variant` + `std::visit` + `if constexpr`
- `std::unreachable()` (2 sites)
- `std::optional`
- `std::from_chars` com `errc` check (idiomático)
- `std::holds_alternative` / `std::get_if` / `std::get`
- Lambda com explicit return type (`-> bool`, `-> std::optional<double>`)

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | B1 — `std::get_if` em vez de `std::get` em eval_condition | bug crítico | mínimo | ✅ aplicado |
| 2 | B3+B4 — Simetrizar Caso 4 + retorno consistente | bug | baixo | ✅ aplicado |
| 3 | P1 — Extrair `cmp_op<T>` template | cleanup/perf | mínimo | ✅ aplicado |
| 4 | B2 — Caso int×int comparar diretamente (com cmp_op) | bug latente | mínimo | ✅ aplicado |
| 5 | M1 — Documentar TODO `CrossesCondition` no header | doc | mínimo | ✅ aplicado |
| 6 | B5 — Documentar precedência left-to-right em `eval_where` | doc | mínimo | ✅ aplicado |

### Itens 1-6 aplicados (2026-05-05)

**B1** — `eval_condition` agora usa `std::get_if<int64_t>` (não-throwing)
em vez de `std::get<int64_t>`. Se row.find("id") retornar Value de tipo
errado, retorna `false` em vez de chamar `std::terminate()` (UB latente
quando função é `noexcept`).

**B2** — Caso 1 (int×int) agora compara `int64_t` diretamente via
`cmp_op<int64_t>`, sem promoção a double. Preserva precisão para
valores > 2^53.

**B3+B4** — Caso 4 simetrizado: trata tanto `int64×string` quanto
`string×int64`. Fallback consistente com Caso 2 (`op == NEQ` em vez de
`false`). Padrão SQL UNKNOWN: tipos incompatíveis ≠ qualquer valor.

**P1** — Extraído template `cmp_op<T>` em namespace anônimo. Substitui
3 switches duplicados (cmp_dbl lambda, Caso 3 string, Caso 1 int).
Usado em todos os 3 tipos numéricos/string. Compilador especializa por
T; performance idêntica, código mais DRY. Lambda `as_double` também
movida para namespace anônimo (`[[nodiscard]] inline noexcept`).

**M1** — Comentário expandido em `CrossesCondition` documentando
"known limitation": gramática aceita sintaxe, mas avaliação é stub
até integração com PathIndex (Phase 2).

**B5** — Comentário acima de `eval_where` documenta semântica de
avaliação estritamente esquerda-para-direita (sem precedência AND > OR).
Diferente de SQL — design intencional para WHERE simples; precedência
exigiria AST hierárquico no parser.

LOC: 151 → 174 (+23; refator com namespace anônimo + comentários
ampliados). 17/17 testes passando.

**Auditoria do `query_evaluator.cpp` 100% concluída.**

**Itens 1+3+4** podem ser combinados em um pass (refator de
`compare_values` com `cmp_op<T>` resolve B1 indireto via simplificação,
B2 via comparação direta, P1 via DRY).

**Item 2 (B3+B4)** é mudança semântica: simetriza tratamento e
muda fallback de `false` para `op == NEQ`. Risco baixo (caller atual
nunca testa `WHERE "x" > id`), mas mudança observável.

---

## Apêndice — itens avaliados e descartados

- **`std::expected<bool, std::string>` em vez de `bool`** — overhead
  de string error em hot path; bool é apropriado para predicado.
- **Cache de `compare_values` por (op, valor literal)** — caller já
  pré-computa via plan_segments fast paths (`extract_length_range`,
  `extract_depth_range`, `extract_name_eq`); cache aqui seria redundante.
- **Refator `eval_where` para árvore de expressão (precedência
  AND > OR)** — refator amplo na grammar (parser deve produzir
  AST hierárquico em vez de listas planas conditions/connectors).
  Fora do escopo desta auditoria.
- **`std::ranges::any_of`/`all_of` em `eval_where`** — short-circuit
  por op-alternation não casa com semântica `any_of`. Manter loop.
- **Mover `cmp_dbl`/`as_double` lambdas para escopo de namespace** —
  apenas `compare_values` usa; manter lambdas locais é mais coeso.
- **`compare_values` constexpr** — `from_chars` em string é constexpr
  desde C++23 (mas com restrições); benefício zero pois compare_values
  é runtime por natureza (Value é variant runtime).
- **`std::ranges::contains_if` em `search_kmer` fallback** —
  fallback path raramente exercitado; manter loop com `if`.
