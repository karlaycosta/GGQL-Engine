# Análise — `include/ggql/query/ast.hpp`

Auditoria do **header AST** que define os tipos de dados produzidos
pelo parser PEGTL (`grammar.hpp`) e consumidos pelo planejador
(`query_planner.hpp`/`query_evaluator.cpp`). Inclui enums (`EntityType`,
`Operator`, `LogicalOp`), `Value` variant, condições (`CompareCondition`,
`CrossesCondition`, `ContainsKmerCondition`), `WhereClause`, queries
(`FindQuery`, `SearchKmerQuery`) e helpers de stringification.

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/query/ast.hpp` (115 linhas, header-only)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 1 | 0 |
| Pontos de melhoria de performance | 0 | 0 |
| Código morto | 1 | 1 (M1) |
| Atributos C++23 / qualifiers | 1 | 0 |
| Estilo / consistência | 1 | 0 |

Severidade geral: **muito baixa**. O header é exemplar no uso de
C++23: `std::to_underlying`, `std::unreachable`, `[[nodiscard("...")]]`
com mensagem, `constexpr` arrays + `static_assert` para invariantes
de enum. Único achado significativo: 5 declarações públicas (3
funções + 2 arrays) que nunca são chamadas em produção.

---

## Bugs / fragilidades

### B1 — `WhereClause::is_valid` aceita estado inconsistente
**Severidade:** muito baixa (latente; PEGTL nunca constrói esse estado)

```cpp
[[nodiscard]] bool is_valid() const noexcept {
    return conditions.empty() || connectors.size() + 1 == conditions.size();
}
```

Para `conditions.empty() == true` e `connectors.size() > 0`, a função
retorna `true`. Mas isto é semanticamente inválido: ter conectores
lógicos sem condições para conectar não faz sentido.

**Fix sugerido:**
```cpp
[[nodiscard]] bool is_valid() const noexcept {
    if (conditions.empty()) {
        return connectors.empty();   // sem conditions → sem connectors
    }
    return connectors.size() + 1 == conditions.size();
}
```

PEGTL garante que apenas `find_stmt` constrói WhereClause via
`grammar.hpp:382-389`, e sempre com pelo menos 1 condition. Mas o
`is_valid` é a documentação executável do invariante — deveria
rejeitar todo estado inconsistente, não só alguns.

---

## Performance

Nenhum ponto crítico. O header é apenas tipos de dados:
- `Value = std::variant<int64_t, double, std::string>` — ~32 B + tag
- `std::vector<ConditionExpr>` em WhereClause — alocação por construção
- `std::optional<uint32_t>` em FindQuery::limit/offset — 8 B
- `std::optional<WhereClause>` em FindQuery::where — heap-free quando nullopt

Possíveis micro-melhorias avaliadas e descartadas:
- **`Value` com `std::string_view` em vez de `std::string`** — quebraria
  semântica (Value precisa lifetime independente do parser input).
- **`SmallVector` para `conditions`/`connectors`** — overhead de
  template > ganho para 1-3 conditions típicas.
- **`std::variant` específico para Value sem string** — alguns predicados
  numéricos só usam int/double; mas API uniforme vence simplicidade.

---

## Código morto

### M1 — 3 funções + 2 arrays nunca usados
**Severidade:** baixa-média (API pública sem callers)

`grep -rn` em todo o codebase (src/, tests/, tools/, include/):

| Símbolo | Definição | Callers | Status |
|---|---|---:|---|
| `entity_to_string` | linha 96 | 0 | **morto** |
| `op_to_string` | linha 101 | 0 | **morto** |
| `logical_op_to_string` | linha 106 | 0 | **morto** |
| `k_entity_names` | linha 87 | 1 (entity_to_string) | morto transitivo |
| `k_op_names` | linha 91 | 1 (op_to_string) | morto transitivo |

Os enums (`EntityType`, `Operator`, `LogicalOp`) **são** amplamente usados
(parser, planner, tests). Mas as utilities de stringification foram
criadas e nunca conectadas — provavelmente projetadas para serialização
futura ou debug que não materializou.

**Decisão:** duas opções defensáveis:

**Opção A — Remover** (~30 LOC):
- Pró: header mais enxuto; menos superfície de manutenção.
- Contra: se algum dia precisar serializar AST para diff/log, terá
  que reimplementar.

**Opção B — Manter como API pública**:
- Pró: pequeno utility header; baixa overhead; arquitetura preparada
  para REPL melhorado, log estruturado, tooling.
- Contra: viola "YAGNI"; código pode estar errado e ninguém percebeu
  porque ninguém usa.

**Recomendação:** **manter** se o roadmap inclui serialização/debug
verbose; **remover** se tem 6+ meses sem uso. Para confirmar a
correctness, adicionar **um teste de unidade** que cubra os 3 helpers
mesmo se eles não são chamados em produção — protege contra regressão
silenciosa.

### M2 — `WhereClause::is_valid` é usado apenas em `grammar.hpp:388`

```cpp
assert(wc.is_valid() && "WHERE: connectors.size() deve ser conditions.size() - 1");
```

Em release build, o `assert` é compilado fora — `is_valid` torna-se
**dead code em release**. Mas é função inline `constexpr` `noexcept`,
zero-cost. Manter (documenta invariante).

---

## Atributos C++23 / qualifiers

### A1 — `logical_op_to_string` usa switch + `unreachable` em vez de array
**Severidade:** zero (cosmético; ambos OK)

```cpp
constexpr std::string_view logical_op_to_string(LogicalOp l) noexcept {
    switch (l) {
        case LogicalOp::AND: return "AND";
        case LogicalOp::OR:  return "OR";
    }
    std::unreachable();
}
```

Inconsistente com `entity_to_string` e `op_to_string` que usam
`array[to_underlying(e)]`. Para 2 elementos, switch é fine — não há
ganho real em uniformizar. Mas se for aplicar M1 (manter), padronizar:

```cpp
constexpr std::array<std::string_view, 2> k_logical_op_names {"AND", "OR"};
static_assert(std::to_underlying(LogicalOp::OR) == 1, ...);

constexpr std::string_view logical_op_to_string(LogicalOp l) noexcept {
    return k_logical_op_names[std::to_underlying(l)];
}
```

### Confirmados em estado bom

- **`[[nodiscard("mensagem")]]`** (C++20) — usado nos 3 helpers de
  stringification com mensagens descritivas.
- **`std::to_underlying`** (C++23) — substitui `static_cast<int>(e)`.
- **`std::unreachable`** (C++23) — em `logical_op_to_string`.
- **`static_assert`** — invariantes de contiguidade de enum
  (linhas 82-85). Excelente: detecta em compile-time se enum for
  reordenado.
- **`constexpr`** — todos os helpers + arrays.
- **`noexcept`** — todos os helpers + `WhereClause::is_valid`.
- **C++23 includes**: `<utility>` para `to_underlying/unreachable`.

---

## Estilo / consistência

### S1 — Comentário não-canônico na declaração `Value`
**Severidade:** zero

```cpp
/// SEARCH KMER "<seq>" [RETURN ...] [LIMIT N]
struct SearchKmerQuery {
```

Comentários `///` (triple slash, Doxygen) são consistentes; OK.

### S2 — Mensagens de `[[nodiscard]]` em português
**Severidade:** zero

```cpp
[[nodiscard("Use o nome retornado para exibição ou serialização")]]
```

Consistente com o resto do projeto (mensagens em pt-BR). OK.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | B1 — Fix `is_valid` para rejeitar `(empty conditions, non-empty connectors)` | bug | mínimo | ✅ aplicado |
| 2 | M1-A — **Remover** helpers + arrays mortos | cleanup | baixo | ✅ aplicado |
| 2' | M1-B — **Adicionar testes** para os helpers (proteção contra regressão silenciosa) | test | baixo | n/a (M1-A escolhido) |
| 3 | A1 — Padronizar `logical_op_to_string` para usar array (com S1 se M1-B) | consistência | mínimo | n/a (resolvido por M1-A) |

### Itens aplicados (2026-05-05) — B1 + M1-A

**B1** — `WhereClause::is_valid` agora rejeita `(empty conditions,
non-empty connectors)`:
```cpp
[[nodiscard]] bool is_valid() const noexcept {
    if (conditions.empty()) {
        return connectors.empty(); // sem conditions → sem connectors
    }
    return connectors.size() + 1 == conditions.size();
}
```
Documentação no comentário atualizada para descrever os dois casos
válidos (WHERE vazio ou cada connector liga 2 conditions consecutivas).

**M1-A** — Removidos 5 símbolos públicos sem callers:
- `entity_to_string` (helper)
- `op_to_string` (helper)
- `logical_op_to_string` (helper)
- `k_entity_names` (array)
- `k_op_names` (array)

Removidos também os 2 `static_assert` que validavam contiguidade dos
enums (estavam ali apenas para o array indexing dos helpers; sem
helpers, não são mais necessários — embora a contiguidade ainda seja
garantida pelo padrão para enums sem valores explícitos).

Includes não-mais-necessários removidos: `<array>`, `<string_view>`,
`<utility>` (`std::to_underlying`/`std::unreachable` eram usados
apenas pelos helpers removidos).

LOC: 115 → 82 (-33 LOC, -29%).

**A1** — Naturalmente resolvido por M1-A: `logical_op_to_string` foi
removido junto com os outros helpers, eliminando a inconsistência
de estilo (switch+unreachable vs array+to_underlying).

17/17 testes passando.

---

## Re-auditoria final pós-cleanup (2026-05-05)

Único cleanup adicional encontrado:

**A2** — Os 3 `enum class` não declaravam underlying type explícito.
Sem `:`, o tipo padrão é `int` (4 bytes). Para enums com poucos
valores (≤256), `: uint8_t` documenta intent e pode ajudar layout
em alguns contextos:
```cpp
enum class EntityType : uint8_t { SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT };
enum class Operator   : uint8_t { EQ, NEQ, GT, LT, GEQ, LEQ };
enum class LogicalOp  : uint8_t { AND, OR };
```

Em CompareCondition (string + enum + Value), o alignment dos vizinhos
mantém o tamanho total inalterado em x86_64 (padding compensa). Mas
em arrays de enums (p.ex. `std::vector<LogicalOp>`), agora cabem 4×
mais por cache line. Para queries com 4+ AND/OR (raro mas possível),
ganho marginal de cache.

Mudança principalmente **documental** — sinaliza ao leitor que estes
enums são limitados a um byte.

### Demais aspectos confirmados em estado bom

- **`[[nodiscard]]`** — `is_valid` (único método com retorno).
- **`noexcept`** — `is_valid` (sem exceções; só compara `vector::size`).
- **`const`** — `is_valid` é const.
- **Defaults explícitos** — `FindQuery::entity = EntityType::SEGMENT`.
  Outros membros usam ctor padrão (string vazia, vector vazio,
  optional nullopt).
- **C++23 features apropriadas:**
  - `std::variant` para soma tipo (`Value`, `ConditionExpr`,
    `GgqlQuery`)
  - `std::optional` para opcionais (`WhereClause`, `limit`,
    `offset`)
  - `std::string`, `std::vector` idiomáticos
  - `enum class` (com underlying type explícito agora)
- **Includes mínimos** — apenas 5 headers essenciais:
  `<cstdint>`, `<optional>`, `<string>`, `<variant>`, `<vector>`.

**Auditoria do `ast.hpp` 100% concluída.** Header é POD-only:
zero funções utility (todas as conversões são feitas no `query_planner.cpp`),
estado e invariante claros, layout otimizado.

17/17 testes passando.

**Recomendação:** começar com **B1** (fix bug latente, sempre seguro).
Para M1, **escolher A ou B** dependendo do roadmap:

- Se há plano de adicionar `--explain` ao REPL ou serialização AST →
  **M1-B** (adicionar testes, padronizar A1).
- Se YAGNI prevalece → **M1-A** (remover; -30 LOC).

---

## Apêndice — itens avaliados e descartados

- **`std::variant<int64_t, double, std::string_view>` em Value** —
  string_view exige lifetime do dado original; AST tem lifetime
  independente do parse input.
- **`SmallVector` em WhereClause** — N=1-3 típico; vetor padrão é OK.
- **`std::flat_map<EntityType, std::string_view>`** em vez do array —
  array é mais rápido, contiguidade de enum garantida.
- **`enum class` com tipos subjacentes explícitos** (`enum class
  EntityType : uint8_t`) — desnecessário para 6 valores; default `int`
  é fine e melhor para optimizer em alguns casos.
- **`std::expected<EntityType, std::string>` em `parse_entity`** —
  invariante (PEGTL valida) torna desnecessário.
- **`<format>` para futura serialização AST** — não há demanda atual.
