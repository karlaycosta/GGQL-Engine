# Análise — `include/ggql/ingest/profile.hpp`

Auditoria do header de **perfis de ingestão**: mapeia casos de uso
biológicos (pangenoma, metagenômica, assembly, comparativa) para
flags de construção em `ingest()`. Pequeno header utility (61 LOC,
header-only).

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/ingest/profile.hpp` (61 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 2 | 0 |
| Pontos de melhoria de performance | 0 | 0 |
| Código morto | 0 | 0 |
| Atributos C++23 / qualifiers | 3 | 0 |

Severidade geral: **muito baixa**. Header pequeno e bem estruturado;
todos os símbolos são usados (`bench_gbwt.cpp`, `ffi.cpp`,
`ggql_repl.cpp`, `test_ingestor_profiles.cpp`). Achados são puramente
de qualificadores C++23 e fall-through ambíguo.

---

## Bugs / fragilidades

### B1 — Fall-through silencioso em `flags_for` e `name_of`
**Severidade:** baixa (latente)

```cpp
constexpr ProfileFlags flags_for(IngestProfile p) noexcept {
    switch (p) {
    case IngestProfile::Pangenome:    return {true, true,  true, true,  true};
    case IngestProfile::Metagenomics: return {true, true,  true, false, false};
    case IngestProfile::Assembly:     return {true, false, true, false, true};
    case IngestProfile::Comparative:  return {true, false, true, true,  false};
    }
    return {true, true, true, true, true}; // unreachable, default = pangenome
}

constexpr std::string_view name_of(IngestProfile p) noexcept {
    switch (p) { ... }
    return "pangenome";
}
```

Se `p` for um enum value inválido (cast de int não-mapeado), retorna
silenciosamente "pangenome" (full flags ou string). Mascara bug. PEGTL
não está envolvido; usuários da FFI fazem `static_cast<IngestProfile>(int)`
em `ffi.cpp:242`, então uma int inválida passaria.

**Fix sugerido:**
```cpp
constexpr ProfileFlags flags_for(IngestProfile p) noexcept {
    switch (p) {
    case IngestProfile::Pangenome:    return {true, true,  true, true,  true};
    case IngestProfile::Metagenomics: return {true, true,  true, false, false};
    case IngestProfile::Assembly:     return {true, false, true, false, true};
    case IngestProfile::Comparative:  return {true, true,  true, true,  false};
    }
    std::unreachable();
}
```

`std::unreachable()` (C++23) sinaliza UB, otimiza compilador, e
documenta a precondição. Idêntico ao pattern já aplicado em
`grammar.hpp` e `query_planner.cpp` (`Paginator::advance`).

Mesmo fix para `name_of`.

**Nota:** `ffi.cpp:242` faz cast direto sem validar:
```cpp
const auto pf = flags_for(static_cast<IngestProfile>(profile));
```
Se aplicar `unreachable()`, a FFI deveria validar antes do cast (já tem
em `ggql_profile_from_name` que retorna -1 em nome inválido — o caller
Rust/Dart geralmente usa enum, não int arbitrário).

### B2 — `ProfileFlags` sem default initializers
**Severidade:** muito baixa (defensivo)

```cpp
struct ProfileFlags {
    bool save_metadata;
    bool build_fm_index;
    bool build_topology;
    bool build_gbwt;
    bool build_snarls;
};
```

Sem default values. `ProfileFlags p;` (default ctor) deixa membros
não-inicializados → UB se lido. Atualmente nenhum caller faz isso (todos
recebem retorno de `flags_for`), mas é defensivo:

```cpp
struct ProfileFlags {
    bool save_metadata  = false;
    bool build_fm_index = false;
    bool build_topology = false;
    bool build_gbwt     = false;
    bool build_snarls   = false;
};
```

Custo: zero (defaults usados apenas em default-construct, que ninguém
faz hoje).

---

## Performance

Nenhum ponto crítico. As 3 funções (`flags_for`, `name_of`,
`parse_profile`) são chamadas tipicamente 1× por ingestão — não estão
em hot path.

`flags_for` e `name_of` são `constexpr`, podem ser avaliados em
compile-time se chamados com expressão constante.

---

## Código morto

**Nenhum.** Todos os símbolos têm callers:

| Símbolo | Callers |
|---|---|
| `IngestProfile` enum | `bench_gbwt.cpp`, `ggql_repl.cpp`, `ffi.cpp`, `tests/test_ingestor_profiles.cpp` |
| `ProfileFlags` struct | retorno de `flags_for`, lido pelos callers via `pf.save_metadata` etc. |
| `flags_for` | `bench_gbwt.cpp:84`, `ffi.cpp:242`, `ggql_repl.cpp:181`, tests |
| `name_of` | `bench_gbwt.cpp:89`, `ggql_repl.cpp:186`, tests |
| `parse_profile` | `bench_gbwt.cpp:53`, `ffi.cpp:252`, tests |

`docs/ingest_profiles.md` existe e está referenciado.

---

## Atributos C++23 / qualifiers

### A1 — Faltam `[[nodiscard]]` nas 3 funções
**Severidade:** baixa (perda de proteção do compilador)

`flags_for`, `name_of` e `parse_profile` retornam valor que deve ser
usado. Ignorar é bug semântico.

```cpp
[[nodiscard]] constexpr ProfileFlags flags_for(IngestProfile p) noexcept;
[[nodiscard]] constexpr std::string_view name_of(IngestProfile p) noexcept;
[[nodiscard]] inline std::optional<IngestProfile> parse_profile(std::string_view s) noexcept;
```

### A2 — `IngestProfile` sem underlying type explícito
**Severidade:** baixa (consistência com auditoria de `ast.hpp`)

```cpp
enum class IngestProfile {
    Pangenome, Metagenomics, Assembly, Comparative,
};
```

Apenas 4 valores; cabe em `uint8_t`. Aplicar `: uint8_t` documenta
intent e mantém consistência com `EntityType`/`Operator`/`LogicalOp`
em `ast.hpp` (recém-padronizados).

```cpp
enum class IngestProfile : uint8_t {
    Pangenome, Metagenomics, Assembly, Comparative,
};
```

### A3 — `parse_profile` é `inline` mas poderia ser `constexpr`
**Severidade:** baixa (perda de avaliação compile-time)

```cpp
inline std::optional<IngestProfile> parse_profile(std::string_view s) noexcept { ... }
```

`string_view::operator==` é `constexpr` desde C++17. Marcar
`parse_profile` como `constexpr` permite `static_assert(parse_profile("pangenome").has_value())`
em testes, e o compilador pode resolver chamadas com string literal em
compile-time.

**Fix sugerido:**
```cpp
[[nodiscard]] constexpr std::optional<IngestProfile> parse_profile(std::string_view s) noexcept {
    if (s == "pangenome"    || s == "pan")  { return IngestProfile::Pangenome; }
    // ...
    return std::nullopt;
}
```

`inline` é redundante em função `constexpr` definida no header — pode
remover.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | A1 — `[[nodiscard]]` nas 3 funções | qualifier | mínimo | ✅ aplicado |
| 2 | A2 — `: uint8_t` em IngestProfile | consistência | mínimo | ✅ aplicado |
| 3 | A3 — `constexpr` em `parse_profile` | qualifier | mínimo | ✅ aplicado |
| 4 | B1 — `std::unreachable()` em `flags_for`/`name_of` | bug latente | baixo | ✅ aplicado |
| 5 | B2 — Default initializers em ProfileFlags | bug latente | mínimo | ✅ aplicado |

### Itens 1-5 aplicados (2026-05-05)

Todas as 5 melhorias aplicadas em um único pass:

**A1** — `[[nodiscard]]` adicionado em `flags_for`, `name_of`,
`parse_profile`. Compilador agora alerta se valor de retorno for
ignorado.

**A2** — `IngestProfile : uint8_t` (consistente com `EntityType`,
`Operator`, `LogicalOp` em `ast.hpp`). Adicionado `<cstdint>` aos
includes.

**A3** — `parse_profile` agora `[[nodiscard]] constexpr` (em vez de
apenas `inline`). Permite `static_assert(parse_profile("pangenome").has_value())`
em testes; compilador resolve chamadas com string literal em
compile-time.

**B1** — `flags_for` e `name_of` agora terminam com `std::unreachable()`
(C++23) em vez de retornar valor default silencioso. Sinaliza UB e
otimiza compilador. Adicionado `<utility>` aos includes.

**B2** — `ProfileFlags` struct com default initializers `= false` em
todos os 5 campos. Defensivo: estrutura default-construída agora tem
estado bem-definido (era UB em uso atípico).

LOC: 61 → 65 (+4; defaults + includes adicionais).
17/17 testes passando.

**Auditoria do `profile.hpp` 100% concluída.**

---

## Apêndice — itens avaliados e descartados

- **Mover `flags_for` para `.cpp`** — perderia `constexpr` benefit;
  manter inline.
- **`std::array<ProfileFlags, 4>` para lookup** — switch é mais legível
  e tão rápido quanto.
- **Validar string em `parse_profile` para case-insensitive** — atual
  exige lowercase; convenção de UNIX CLI. Aceitar.
- **Substituir `std::optional<IngestProfile>` por `std::expected`** —
  optional é apropriado para "presente ou não"; expected agregaria
  string de erro desnecessariamente.
- **`<charconv>`** ou similar — n/a.
- **Tabela `static constexpr std::array<std::pair<std::string_view, IngestProfile>, N>`**
  para `parse_profile` — economiza pouco; switch atual é claro.
- **C++23 `std::flat_map`** para name → profile — overhead de criação
  > ganho para 4 valores.
