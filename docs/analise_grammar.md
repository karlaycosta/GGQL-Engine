# Análise — `include/ggql/query/grammar.hpp`

Auditoria do **header header-only** que define a gramática PEGTL para
GGQL (FIND/SEARCH KMER), as ações que constroem o AST, e o `parse_ggql`
público. Inclui regras PEG, `ParseState`, helpers `detail::parse_entity`/
`parse_op`, e actions.

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/query/grammar.hpp` (428 linhas, header-only)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 3 | 0 |
| Pontos de melhoria de performance | 0 | 0 |
| Código morto / duplicações **aparentes** | 3 | 0 (todas intencionais) |
| Atributos C++23 / qualifiers | 1 | 0 |
| Estilo / cosmético | 1 | 0 |

Severidade geral: **muito baixa**. O header está em excelente estado:
faz uso idiomático de PEGTL, C++23 (`std::expected`, `std::unreachable`,
`using enum`, `if constexpr`, `std::variant<...,std::string_view>` para
zero-copy), `[[nodiscard]] constexpr noexcept` aplicados corretamente.
As "duplicações" são padrão necessário para PEGTL action dispatch.

---

## Bugs / fragilidades

### B1 — `detail::parse_entity` e `parse_op` com fall-through ambíguo
**Severidade:** baixa (latente; caller real sempre passa input pós-PEGTL)

```cpp
[[nodiscard]] constexpr EntityType parse_entity(std::string_view s) noexcept {
    using enum EntityType;
    if (s == "SEGMENT") { return SEGMENT; }
    if (s == "EDGE") {    return EDGE;    }
    if (s == "PATH") {    return PATH;    }
    if (s == "BUBBLE") {  return BUBBLE;  }
    if (s == "TIP") {     return TIP;     }
    /* REPEAT */        return REPEAT;
}
```

Se `s` for uma string inválida (ex.: "BANANA"), retorna `REPEAT`
silenciosamente. PEGTL valida antes via `entity_type` rule, então
nunca acontece em produção. Mas se alguém chamar `parse_entity`
diretamente para outro propósito, retorno errôneo.

**Fix sugerido:**
```cpp
[[nodiscard]] constexpr EntityType parse_entity(std::string_view s) noexcept {
    using enum EntityType;
    if (s == "SEGMENT") { return SEGMENT; }
    if (s == "EDGE")    { return EDGE; }
    if (s == "PATH")    { return PATH; }
    if (s == "BUBBLE")  { return BUBBLE; }
    if (s == "TIP")     { return TIP; }
    if (s == "REPEAT")  { return REPEAT; }
    std::unreachable(); // PEGTL garante que só passa entities válidas
}
```

`std::unreachable()` (C++23) é melhor que comentário `/* REPEAT */` —
sinaliza UB se atingido (compilador otimiza assumindo impossível) e
documenta a precondição. Semelhante ao já feito em `logical_op` action
linha 333.

Mesmo fix para `parse_op`.

### B2 — `from_chars` sem checagem de erro em `limit_val`/`offset_val`
**Severidade:** baixa (overflow em LIMIT/OFFSET astronômicos)

```cpp
template <> struct action<limit_val> {
    template <typename In> static void apply(const In& in, ParseState& s) {
        const auto sv = in.string_view();
        uint32_t v = 0;
        std::from_chars(sv.data(), sv.data() + sv.size(), v);
        s.tmp_limit = v;
    }
};
```

Se a query tem `LIMIT 99999999999` (>4 G), `std::from_chars` retorna
`errc::result_out_of_range` e `v` fica em estado indefinido (varia
entre implementações: zerado ou `UINT32_MAX`). O caller ainda recebe
`tmp_limit` válido com valor errado.

**Fix sugerido:** validar `ec` e clampar a `UINT32_MAX`:
```cpp
const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
if (ec == std::errc::result_out_of_range) {
    v = std::numeric_limits<uint32_t>::max();
}
s.tmp_limit = v;
```

Idem para `offset_val`, `int_value`, `float_value`.

### B3 — `int_value` overflow para int64_t
**Severidade:** muito baixa (20+ dígitos, caso excêntrico)

Análogo a B2 mas com `int64_t v`. Para queries `WHERE length > 99999999999999999999`,
overflow silencioso. PEGTL aceita qualquer sequência de dígitos
(`peg::plus<peg::digit>`).

**Fix:** mesma estratégia — checar `errc` e clampar a
`numeric_limits<int64_t>::max()`.

---

## Performance

Nenhum ponto crítico encontrado. O parser PEGTL gera código bem
inlinado, `std::from_chars` é o método mais rápido para parse de
inteiros, e `std::variant<int64_t, double, std::string_view>` em
`tmp_value` evita alocação de strings durante o parse (zero-copy
até a action `condition_expr` que materializa).

Possíveis micro-melhorias avaliadas e descartadas:
- **`std::string{sv}` em actions** — alocação inevitável para o
  AST persistir além da vida do input mmap.
- **`s.where_conditions.reserve(N)`** em `ParseState` — N variável
  por query; pré-aloc seria adivinhação.

---

## Código morto / duplicações aparentes

### M1 — `where_field` / `return_field` são idênticos
**Severidade:** zero (intencional)

```cpp
struct where_field : peg::seq<peg::not_at<keyword_any>, peg::identifier> {};
struct return_field : peg::seq<peg::not_at<keyword_any>, peg::identifier> {};
```

Mesma definição. Substituir por `using return_field = where_field;`
**quebraria** o dispatch de actions: `action<where_field>` e
`action<return_field>` precisam ser tipos distintos para template
specialization. Atual `where_field` captura em `tmp_field` (inline em
condition); `return_field` empurra em `return_fields` (vetor).

**Manter como-está.**

### M2 — `fn_contains_arg` / `kmer_arg` são idênticos
**Severidade:** zero (intencional)

```cpp
struct fn_contains_arg : peg::star<peg::not_one<'"'>> {};
struct kmer_arg : peg::star<peg::not_one<'"'>> {};
```

Mesma justificativa: actions diferentes (`fn_contains_arg` →
`tmp_fn_arg` / `kmer_arg` → `kmer_str`). **Manter.**

### M3 — `limit_val` / `offset_val` idênticos
**Severidade:** zero (intencional)

```cpp
struct limit_val  : peg::plus<peg::digit> {};
struct offset_val : peg::plus<peg::digit> {};
```

Mesma razão. **Manter.**

---

## Atributos C++23 / qualifiers

### A1 — `parse_ggql` poderia delegar a um `inline` separado e marcar `[[nodiscard]]` no result

Já está com `[[nodiscard]] inline`. **OK.**

### Confirmados em bom estado

- **`detail::parse_entity` e `parse_op`** — `[[nodiscard]] constexpr noexcept`
- **`parse_ggql`** — `[[nodiscard] inline` (não pode ser `noexcept` por causa do try/catch)
- **Actions `static void apply(...)`** — não podem ser `noexcept` (chamam
  `vector::push_back` que aloca)
- **`std::variant<int64_t, double, std::string_view>`** — uso correto
  com `if constexpr` na visit
- **`std::expected<GgqlQuery, std::string>`** — retorno idiomático
- **`std::unreachable()`** — usado em `logical_op` action (poderia ser
  estendido para B1 fix)
- **`using enum EntityType`** (C++20) — `parse_entity`

---

## Estilo / cosmético

### S1 — Estilo de braces inconsistente em `parse_entity`/`parse_op`
**Severidade:** zero (apenas aparência)

```cpp
if (s == "SEGMENT") { return SEGMENT;
}
if (s == "EDGE") {    return EDGE;
}
```

Auto-formatter quebrou linha entre `;` e `}`. Funcional mas estranho.
Se for aplicar B1 fix, padronizar:
```cpp
if (s == "SEGMENT") { return SEGMENT; }
if (s == "EDGE")    { return EDGE; }
```

Mesmo issue em `parse_op` e na action `logical_op` (linha 331-334).

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | B1 — `std::unreachable()` em `parse_entity`/`parse_op` | bug + estilo | mínimo | ✅ aplicado |
| 2 | S1 — Padronizar braces em `parse_entity`/`parse_op` (com B1) | cosmético | mínimo | ✅ aplicado |
| 3 | B2 — Checar `ec` em `limit_val`/`offset_val` | bug | baixo | ✅ aplicado |
| 4 | B3 — Checar `ec` em `int_value`/`float_value` | bug | baixo | ✅ aplicado |

### Itens 1-4 aplicados (2026-05-05)

**B1+S1** — `parse_entity` e `parse_op` agora têm match explícito para
todos os enum values + `std::unreachable()` no fall-through:
```cpp
if (s == "REPEAT")  { return REPEAT; }
std::unreachable(); // PEGTL `entity_type` rule garante input válido
```
Braces padronizados em uma linha (`{ return X; }` em vez de quebra
auto-formatter). Compilador agora pode otimizar mais agressivamente
(remove dead branch para inputs inválidos vindos da PEGTL).

**B2+B3** — `from_chars` agora checa `errc::result_out_of_range` em 4
actions (`int_value`, `float_value`, `limit_val`, `offset_val`):
```cpp
const auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
if (ec == std::errc::result_out_of_range) {
    v = std::numeric_limits<T>::max();
}
```
Para queries com números absurdamente grandes (ex.: `LIMIT 99999999999`),
agora clampa a `numeric_limits::max()` em vez de produzir valor
indefinido. Adicionados `<limits>` e `<utility>` aos includes.

17/17 testes passando. **Auditoria do `grammar.hpp` concluída.**

---

## Re-auditoria final pós-fixes (2026-05-05)

Único achado residual encontrado:

**S2** — Auto-formatter ainda quebrado em 3 lugares restantes:
- `condition_expr` action (lambda interna `if constexpr/else`)
- `logical_op` action (chain if/else if/else com `}` quebrado)
- `parse_ggql` (corpo do try/catch)

Padronizado o estilo:
```cpp
// antes (logical_op):
if (sv == "AND") {      s.where_connectors.push_back(LogicalOp::AND);
} else if (sv == "OR") {  s.where_connectors.push_back(LogicalOp::OR);
} else {                  std::unreachable();
}

// depois:
if (sv == "AND") {
    s.where_connectors.push_back(LogicalOp::AND);
} else if (sv == "OR") {
    s.where_connectors.push_back(LogicalOp::OR);
} else {
    std::unreachable(); // PEGTL `logical_op` rule garante AND/OR
}
```

### Demais aspectos confirmados em estado bom

- **`[[nodiscard]]`** — `parse_entity`, `parse_op`, `parse_ggql`,
  `find` (em RowFields no header relacionado).
- **`constexpr`** — `parse_entity`, `parse_op`.
- **`noexcept`** — `parse_entity`, `parse_op`. Actions não podem
  (chamam `vector::push_back`); `parse_ggql` tem try/catch.
- **`const`** — actions são `static`; `parse_ggql` recebe `string_view`
  por valor.
- **C++23 features em uso:**
  - `std::expected` / `std::unexpected`
  - `std::variant` + `std::visit` + `if constexpr`
  - `std::unreachable()` (3 sites: 2 em `detail::` + 1 em action)
  - `using enum EntityType` (C++20)
  - `std::from_chars` com `errc::result_out_of_range` check
  - `std::numeric_limits<T>::max()` para clamp
  - `std::string_view` zero-copy em `tmp_value`
  - Lambdas com explicit template parameter `[]<typename T>(...)` (C++20)
  - `[[nodiscard]] inline` em `parse_ggql`

**Auditoria do `grammar.hpp` 100% concluída.** Não há mais bugs
latentes, código morto ou pontos de melhoria significativos.

17/17 testes passando.

**Itens 1-2** podem ser combinados em um único pass de cleanup. O fix
muda o **comportamento em caso de input inválido**: antes retornava
último enum ("REPEAT"/"EQ") silenciosamente; depois invoca UB declarado
(`std::unreachable`). Em produção (após PEGTL match), o caminho é
inalcançável de qualquer forma.

**Itens 3-4** previnem overflow silencioso em queries com números
absurdamente grandes. Casos extremos, mas trivial de resolver.

---

## Apêndice — itens avaliados e descartados

- **Mover regras PEGTL para um `.cpp` (.tcc?)** — PEGTL é template-heavy
  e exige tipos visíveis em todos TUs. Mover quebraria PEGTL.
- **`<tao/pegtl.hpp>` em fwd-decl** — impossível, PEGTL precisa de
  todos os templates.
- **Constexpr `parse_op` em compile-time** — já é `constexpr`. A grammar
  só chama em runtime, mas pode ser usada em testes constexpr.
- **`std::flat_map<std::string_view, EntityType>`** para lookup em
  `parse_entity` — overhead > 6 if-elses lineares para entrada
  conhecida.
- **`std::ranges::contains`** — não há lista linear que compare igualdade
  no parser.
- **`peg::eol`** vs `peg::space` — `space` cobre tudo (incluindo
  `\n\r\t`); preferência atual está bem.
- **Aliases `using` para regras duplicadas** (`using return_field = where_field;`)
  — quebra dispatch de PEGTL, não fazer.
- **Validação de queries vazias** — `peg::eof` na regra `query` já
  garante input completo.
