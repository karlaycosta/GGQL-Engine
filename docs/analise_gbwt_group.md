# Análise — `include/ggql/ingest/gbwt_group.hpp`

Auditoria do header de **encoding GBWT**: define `GbwtGroup` (agrupa
sequências de um job de construção do gBWT) e os 3 helpers
`gbwt_encode_node` / `gbwt_decode_node` / `gbwt_decode_orient` que
implementam o shift +1 para reservar valor 0 a `gbwt::ENDMARKER`.

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/ingest/gbwt_group.hpp` (54 linhas, header-only)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 2 | 0 |
| Pontos de melhoria de performance | 0 | 0 |
| Código morto | 0 | 0 |
| Atributos C++23 / qualifiers | 1 | 0 |
| Documentação | 1 | 0 |

Severidade geral: **muito baixa**. Header crítico do hot path da
ingestão (gbwt_encode_node é chamado ~1B vezes para HPRC), bem
estruturado: `inline constexpr noexcept`, comentário explicativo
detalhado sobre o shift +1. Achados são apenas qualifiers e
documentação de pré-condições.

---

## Bugs / fragilidades

### B1 — `gbwt_decode_node` underflow se `encoded == ENDMARKER`
**Severidade:** baixa (latente; pré-condição implícita)

```cpp
inline constexpr uint64_t gbwt_decode_node(uint64_t encoded) noexcept {
    return (encoded >> 1) - 1ull;
}
```

Se `encoded == 0` (= `gbwt::ENDMARKER`):
- `0 >> 1 == 0`
- `0 - 1ull == UINT64_MAX` (wrap-around)

Não é UB para `uint64_t` (subtração é bem-definida módulo 2^64), mas
**resultado sem significado semântico**. Caller deve garantir
`encoded != ENDMARKER` antes de chamar.

`graph_algorithms.cpp:547` e `:671` decodificam values que vêm do gBWT
buffer e checam `if (val == gbwt::ENDMARKER)` antes — corretamente.
`path_index.cpp` provavelmente também.

**Fix sugerido (documentação apenas; não mudar comportamento):**
```cpp
// Pré-condição: encoded > 0 (encoded != gbwt::ENDMARKER).
// Se encoded == 0, retorna UINT64_MAX (caller deve filtrar antes).
inline constexpr uint64_t gbwt_decode_node(uint64_t encoded) noexcept {
    return (encoded >> 1) - 1ull;
}
```

### B2 — `gbwt_encode_node` overflow se `internal_id == UINT64_MAX`
**Severidade:** muito baixa (caso impossível na prática)

```cpp
inline constexpr uint64_t gbwt_encode_node(uint64_t internal_id,
                                           uint64_t orient_bit) noexcept {
    return ((internal_id + 1ull) << 1) | (orient_bit & 1ull);
}
```

Se `internal_id == UINT64_MAX`:
- `UINT64_MAX + 1ull == 0` (wrap)
- `0 << 1 == 0`
- Resultado colide com ENDMARKER!

Caso impossível: nem o pangenoma humano (HPRC, ~10⁹ nós) chega perto
de UINT64_MAX/2. Pré-condição implícita: `internal_id < UINT64_MAX/2`.

**Documentar pré-condição** (não mudar comportamento — adicionar check
custaria 1 branch em loop chamado 10⁹+ vezes):

```cpp
// Pré-condição: internal_id < UINT64_MAX/2 (para evitar overflow do
// shift). Garantido na prática: nenhum dataset realístico chega perto.
```

---

## Performance

Nenhum ponto crítico. As 3 funções são `inline constexpr noexcept` —
inlinadas pelo compilador, single instruction cada (shift + add + or).

`GbwtGroup` é POD-like; default-construct é trivial.

Hot path: `gbwt_encode_node` chamado em `parse_w_line`/`parse_p_line`
(`ingestor.cpp:332`, `:379`) — 1 vez por nó atravessado em um path.
Para HPRC: ~10⁹ chamadas. `inline constexpr` garante que a função é
expandida no caller, sem overhead de chamada.

---

## Código morto

**Nenhum.** Todos os símbolos têm callers:

| Símbolo | Callers (representativos) |
|---|---|
| `GbwtGroup` | `graph_algorithms.cpp:484`, `path_index.cpp:139,145,149` |
| `gbwt_encode_node` | `graph_algorithms.cpp:499`, `ingestor.cpp:332,379` |
| `gbwt_decode_node` | `graph_algorithms.cpp:547,671` |
| `gbwt_decode_orient` | (verificar — pode ter callers em path_index.cpp) |

---

## Atributos C++23 / qualifiers

### A1 — Faltam `[[nodiscard]]` nos 3 helpers de encode/decode
**Severidade:** baixa

```cpp
inline constexpr uint64_t gbwt_encode_node(...) noexcept;
inline constexpr uint64_t gbwt_decode_node(...) noexcept;
inline constexpr uint64_t gbwt_decode_orient(...) noexcept;
```

Funções puras retornando `uint64_t`. Ignorar o resultado é bug
semântico (chamou pra fazer nada). Adicionar `[[nodiscard]]`:

```cpp
[[nodiscard]] inline constexpr uint64_t gbwt_encode_node(...) noexcept;
[[nodiscard]] inline constexpr uint64_t gbwt_decode_node(...) noexcept;
[[nodiscard]] inline constexpr uint64_t gbwt_decode_orient(...) noexcept;
```

### Confirmados em estado bom

- **`constexpr`** — todas as 3 funções; permite avaliação
  compile-time se argumentos são const.
- **`noexcept`** — todas as 3; nenhuma operação pode lançar.
- **`inline`** — redundante (constexpr é implicitly inline desde C++17),
  mas explícito ajuda leitura. Manter.
- **`uint64_t` everywhere** — alinha com `gbwt::size_type`.
- **`orient_bit & 1ull`** mascara defensivamente, OK.
- **`GbwtGroup`** — POD-like; defaults implícitos OK
  (`tmp_path` empty, vectors empty, `sigma = 0`).

---

## Documentação

### D1 — Pré-condições de encode/decode não documentadas
**Severidade:** baixa (documentação)

O header tem um excelente comentário (linhas 22-39) explicando *por
que* o shift +1 é necessário. Mas as **pré-condições das funções
individuais** não estão documentadas:

- `gbwt_encode_node`: `internal_id < UINT64_MAX/2`
- `gbwt_decode_node`: `encoded > 0` (não-ENDMARKER)
- `gbwt_decode_orient`: nenhuma (sempre retorna bit 0)

**Fix sugerido:** adicionar comentário curto antes de cada função
declarando a pré-condição. Útil para readers que vêem só a função
sem o contexto do bloco geral.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | A1 — `[[nodiscard]]` nos 3 helpers | qualifier | mínimo | ✅ aplicado |
| 2 | D1 — Documentar pré-condições | doc | mínimo | ✅ aplicado |
| 3 | B1 — Documentar fall-back de `decode(0)` (parte de D1) | doc | mínimo | ✅ aplicado |
| 4 | B2 — Documentar pré-condição de overflow (parte de D1) | doc | mínimo | ✅ aplicado |

### Itens 1-4 aplicados (2026-05-05)

**A1+D1** — Cada helper agora tem comentário de pré-condição inline
imediatamente antes da declaração + `[[nodiscard]]`:

```cpp
// Codifica node_id + orient_bit em um único uint64_t para gBWT.
// Pré-condição: internal_id < UINT64_MAX/2 (overflow do shift wrap a
// 0 = ENDMARKER). Garantido na prática: nenhum dataset realístico
// (HPRC ~10⁹ nós) chega perto.
[[nodiscard]] inline constexpr uint64_t gbwt_encode_node(...) noexcept;

// Decodifica node_id de um valor encoded.
// Pré-condição: encoded > 0 (encoded != gbwt::ENDMARKER). Caller deve
// filtrar ENDMARKER antes — decode(0) retorna UINT64_MAX (wrap), sem
// significado semântico. Decisão deliberada: evita branch em hot path
// chamado ~10⁹ vezes em HPRC.
[[nodiscard]] inline constexpr uint64_t gbwt_decode_node(...) noexcept;

// Extrai o bit de orientação (0 = forward, 1 = reverse). Sem pré-condição
// — válido para qualquer encoded, incluindo ENDMARKER.
[[nodiscard]] inline constexpr uint64_t gbwt_decode_orient(...) noexcept;
```

LOC: 54 → 65 (+11; comentários documentam contrato sem mudar comportamento).
17/17 testes passando. **Auditoria do `gbwt_group.hpp` 100% concluída.**

**Decisão deliberada de NÃO adicionar checks runtime:**

Adicionar `if (encoded == 0) return UINT64_MAX;` em `gbwt_decode_node`
custaria 1 branch em loop de 10⁹+ chamadas para HPRC. Caller já
filtra ENDMARKER antes (e.g., `graph_algorithms.cpp:547,671`). O
contrato implícito é parte do design de hot path.

---

## Apêndice — itens avaliados e descartados

- **`gbwt::size_type` em vez de `uint64_t`** — `gbwt::size_type` é
  defina por `<gbwt/utils.h>` mas alias para uint64_t. Usar tipo
  explícito é OK e independente de mudanças no GBWT lib.
- **Forward-declaration de `gbwt::vector_type`** — typedef/using
  externos não podem ser fwd-declared; precisa do header completo.
  Aceitar transitive include de `<gbwt/utils.h>`.
- **`std::bit_width` para computar `sigma` automático** — sigma é
  preenchido pelos callers via `std::max(...)`, não pelo struct.
  Manter.
- **Adicionar `static_assert(gbwt::ENDMARKER == 0, ...)`** —
  defensivo contra mudança no GBWT upstream. Pode valer; mas o GBWT
  é stable há anos. Pular.
- **`enum class GbwtGroupKind { InRam, OnDisk }` em vez de
  "in_ram_seqs vazio = on disk"** — refator estrutural; não vale o
  custo para 2 estados que documentação cobre.
- **Mover encoding helpers para um namespace `gbwt_encoding::`** —
  cosmético; manter em `ggql::` é fine.
