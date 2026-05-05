# Análise — `include/ggql/storage/topology.hpp`

Auditoria do header da classe **`GfaCsrTopology`** — núcleo de
armazenamento de grafos em layout colunar (CSR + Elias-Fano + gBWT +
Snarls). Header denso (523 LOC) com ~50% de implementações inline para
hot paths (`find_tips`, `find_repeats`, `out_degree`/`in_degree`,
`out_neighbors`/`in_neighbors`, `decode`, `edge_range_fwd`/`bwd`).

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/storage/topology.hpp` (523 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 2 | 0 |
| Pontos de melhoria de performance | 1 | 0 |
| Código morto | 0 | 0 |
| Atributos C++23 / qualifiers | 18+ getters | 0 |
| Documentação | 0 | 0 |

Severidade geral: **baixa**. Header bem estruturado, com decisões de
design documentadas (inline para hot paths medidos, threshold de
paralelismo, comentários de layout de memória). Único ponto material
é a **ausência massiva de `[[nodiscard]]` em getters/queries** —
oportunidade de proteção do compilador contra retornos ignorados.

Nenhum código morto: `gbwt_offsets_` é usado em `path_nodes`
(busca binária por shard); `is_fwd` em `build_csr` é usado em label
de progresso (`csr_builder.cpp:242`).

---

## Bugs / fragilidades

### B1 — `find_repeats` reserva 25% de N (excessivo)
**Severidade:** baixa (memória prematuramente alocada)

```cpp
std::vector<uint64_t> repeats;
repeats.reserve(N / 4);  // linhas 299, 348
```

Para HPRC (~85M nós), reserva 21M slots de `uint64_t` = **168 MB**
alocados antes de saber se serão usados. CHM13 tem ~5% de repeats
(`out_degree >= 2`), pangenoma humano ~10-20%. Nunca chega a 25%.

No path paralelo, há reserve baseado em `total` real (linha 336-338):
```cpp
if (total.load(std::memory_order_relaxed) > 1'000'000) {
    repeats.reserve(total.load(std::memory_order_relaxed));
}
```
Mas para `total <= 1M` (caso comum), **sem reserve** — realocações
durante `insert`. Inconsistente com o branch serial.

**Fix sugerido:**
```cpp
// Serial: reserva conservadora baseada em estatística real
repeats.reserve(N / 16);  // ~6%, mais realista

// Paralelo: sempre reserva o total exato (já calculado)
repeats.reserve(total.load(std::memory_order_relaxed));  // remove o if > 1M
```

### B2 — `out_degree`/`in_degree` duplicam check de `edge_range_*`
**Severidade:** muito baixa (cosmético; compilador elimina)

```cpp
uint64_t out_degree(uint64_t node) const noexcept {
    if (node >= num_nodes_ || E_fwd_.empty()) {
        return 0;
    }
    auto [lo, hi] = edge_range_fwd(node);
    return hi - lo;
}
```

`edge_range_fwd` (linha 493-503) já tem o mesmo check:
```cpp
std::pair<uint64_t, uint64_t> edge_range_fwd(uint64_t node) const noexcept {
    if (node >= num_nodes_ || E_fwd_.empty()) {
        return {0, 0};
    }
    // ...
}
```

Duplicação inofensiva (compilador inlina e elimina), mas mantém duas
fontes da mesma invariante. Pode simplificar:
```cpp
uint64_t out_degree(uint64_t node) const noexcept {
    auto [lo, hi] = edge_range_fwd(node);
    return hi - lo;
}
```

---

## Performance

### P1 — `tips.insert(tips.end(), v.begin(), v.end())` em loop sequential
**Ganho estimado:** marginal (caso N grande)

Em `find_tips` paralelo (linha 277):
```cpp
std::vector<uint64_t> tips;
tips.reserve(total);
for (auto& v : t_tips) {
    tips.insert(tips.end(), v.begin(), v.end());
}
```

Para 8 threads × ~5M tips/thread = 40M inserts sequenciais. Alternativa:
prefix-sum + flat copy paralelo:
```cpp
std::vector<size_t> off(num_threads + 1, 0);
for (int i = 0; i < num_threads; ++i) off[i+1] = off[i] + t_tips[i].size();
std::vector<uint64_t> tips(off.back());
#pragma omp parallel for schedule(static) num_threads(num_threads)
for (int i = 0; i < num_threads; ++i) {
    std::move(t_tips[i].begin(), t_tips[i].end(), tips.begin() + off[i]);
}
```

Para tips/repeats típicos (5% de N), com 85M nós = 4M results.
Sequential insert é ~1-2ms, prefix-sum + parallel copy seria ~0.3 ms.
**Marginal** — `find_tips` toda dura 94-209 ms; insert é <1% do tempo.
Aceitar como-está.

---

## Código morto

**Nenhum.** Investigado:

| Símbolo | Uso |
|---|---|
| `gbwt_offsets_` | `path_nodes` (graph_algorithms.cpp:649-657: busca binária por shard); `rebuild_offsets_` (csr_builder.cpp:295) |
| `is_fwd` em `build_csr` | label de progresso (`csr_builder.cpp:242`: `is_fwd ? "fwd" : "bwd"`) |
| Outros membros privados | usados em `.cpp` |

---

## Atributos C++23 / qualifiers

### A1 — `[[nodiscard]]` ausente em 18+ getters/queries
**Severidade:** baixa (perda de proteção do compilador)

Métodos que retornam valor sem `[[nodiscard]]`:

**Inline (header):**
- `out_degree` (linha 134) → `uint64_t`
- `in_degree` (linha 143) → `uint64_t`
- `out_neighbors` (linha 158) → `std::span<const uint32_t>`
- `in_neighbors` (linha 166) → `std::span<const uint32_t>`
- `decode` (linha 176) → `EdgeRef` static
- `num_nodes` (linha 219) → `uint64_t`
- `num_edges` (linha 220) → `uint64_t`
- `find_tips` (linha 229) → `std::vector<uint64_t>`
- `find_repeats` (linha 296) → `std::vector<uint64_t>`
- `path_count` (linha 370) → `size_t`
- `path_name` (linha 372) → `std::string_view`
- `has_path_stats` (linha 406) → `bool`
- `path_node_count` (linha 411) → `uint32_t`
- `path_bp_length` (linha 415) → `int64_t`
- `has_snarls` (linha 424) → `bool`
- `snarls` (linha 426) → `std::span<const SnarlRecord>`
- `edge_range_fwd` (linha 493) → `std::pair`
- `edge_range_bwd` (linha 505) → `std::pair`

**Métodos de `GbwtTimings`:**
- `nodes_per_s` (linha 109) → `double`
- `bits_per_node` (linha 110) → `double`

**Out-of-line (sem `[[nodiscard]]` na declaração):**
- `path_nodes(size_t)` (linha 383) — já tem
- `path_nodes(size_t, std::vector&)` (linha 386) — `void`, n/a

**Já corretos:**
- `make_component_remap` (linha 195) — ✓
- `repartition_groups_topological` (linha 205) — ✓
- `memory_bytes` (linha 216) — ✓
- `find_bubbles` (linha 361) — ✓

**Fix sugerido:** adicionar `[[nodiscard]]` em todos os 18+ getters
listados acima. Compilador alertará chamadas como `topo.out_degree(n);`
sem usar o resultado (provavelmente bug semântico).

### A2 — `noexcept` ausente em `GbwtTimings::nodes_per_s`/`bits_per_node`
**Severidade:** mínima

```cpp
double nodes_per_s() const { ... }      // linha 109
double bits_per_node() const { ... }    // linha 110
```

Operações triviais (divisão de double, ternário). Nenhuma operação
pode lançar. Adicionar `noexcept`:

```cpp
[[nodiscard]] double nodes_per_s()  const noexcept { ... }
[[nodiscard]] double bits_per_node() const noexcept { ... }
```

### Confirmados em estado bom

- **`const`** correctness — todos os queries/getters são `const`.
- **`noexcept`** aplicado nos hot paths (out_degree, in_degree,
  out_neighbors, in_neighbors, decode, edge_range_*, num_nodes,
  num_edges, path_count, path_name, has_path_stats, path_node_count,
  path_bp_length, has_snarls, snarls, set_path_stats, set_snarls).
- **`[[likely]]`** em `edge_range_fwd`/`bwd` (offsets_cache check).
- **`mutable`** em `offsets_cache_*` para lazy-build em const methods.
- **`std::span<const T>`** em retornos de `out_neighbors`,
  `in_neighbors`, `snarls`.
- **`static_assert(__cplusplus >= 202302L)`** garante C++23.
- **Move ctor/assign `noexcept`** (declarados; def no .cpp).
- **`= delete`** explícito em copy ctor/assign (gerencia recursos
  grandes).
- **`std::unique_ptr<gbwt::GBWT>`** com forward-decl-style include
  (gbwt.h apenas para tipo completo do destrutor).
- **Comentário arquitetural** linhas 1-51 documenta layout de memória,
  invariantes e decisão de inline vs out-of-line.

---

## Plano sugerido (ordem por valor / risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | A1 — `[[nodiscard]]` em 18+ getters | qualifier | mínimo | ✅ aplicado |
| 2 | A2 — `noexcept` em `nodes_per_s`/`bits_per_node` | qualifier | mínimo | ✅ aplicado |
| 3 | B1 — Reserve mais conservadora em `find_repeats` | bug latente | baixo | ✅ aplicado |
| 4 | B2 — Remover duplicação em `out_degree`/`in_degree` | cleanup | mínimo | ✅ aplicado |

### Itens 1-4 aplicados (2026-05-05)

**A1** — `[[nodiscard]]` adicionado em 20 getters/queries inline:
- Graus: `out_degree`, `in_degree`
- Iteração: `out_neighbors`, `in_neighbors`
- Decode: `decode` (static)
- Dimensões: `num_nodes`, `num_edges`
- Find: `find_tips`, `find_repeats`
- Path: `path_count`, `path_name`, `has_path_stats`, `path_node_count`,
  `path_bp_length`
- Snarls: `has_snarls`, `snarls`
- Ranges: `edge_range_fwd`, `edge_range_bwd` (privados)
- Timings: `nodes_per_s`, `bits_per_node`

**A2** — `nodes_per_s` e `bits_per_node` agora `[[nodiscard]] noexcept`
(operações triviais de divisão de double não podem lançar).

**B1** — `find_repeats` reserve corrigido:
- Serial: `N/4` → `N/16` (ajusta de 25% para ~6%, alinhado com
  ~5-10% de repeats em pangenomas reais; HPRC: 168 MB → 42 MB
  pré-alocados)
- Paralelo: removido `if (total > 1M)`; agora sempre reserva o
  total exato calculado pelos atomic counters (consistente)

**B2** — `out_degree`/`in_degree` simplificados — `edge_range_*`
já contém o check de bounds `node >= num_nodes_ || E_*.empty()`,
duplicação removida. Funcionalidade idêntica (compilador eliminava
duplicação via inline). Mantém uma única fonte da invariante.

17/17 testes passando.

**Auditoria do `topology.hpp` 100% concluída.**

---

## Apêndice — itens avaliados e descartados

- **Mover `find_tips`/`find_repeats` para `.cpp`** — header explícitamente
  documenta (linha 224-226) que inline é necessário para preservar
  especialização do compilador no `kParallelNodeThreshold_`. Out-of-line
  perderia inlining de `offsets_cache_fwd_[i+1] - offsets_cache_fwd_[i]`.
- **Substituir `std::vector<std::unique_ptr<gbwt::GBWT>>` por
  `std::vector<gbwt::GBWT>`** — gbwt::GBWT pode não ter move-noexcept;
  unique_ptr é safer.
- **`std::flat_set` para `snarls_`** — vetor ordenado é equivalente
  e mais rápido para search após carga.
- **Forward-decl `gbwt::GBWT`** (em vez de `<gbwt/gbwt.h>`) —
  tentado em sessão anterior para outros headers; aqui o destrutor
  declarado fora do header já permite, mas comentário (linha 73-74)
  documenta que `<gbwt/utils.h>` é o leve necessário. Já no estado
  ótimo.
- **`std::expected<void, std::string>` em `save`/`load`** — refator
  amplo do retorno; `.cpp` já trata via try/catch.
- **`[[nodiscard]]` em `set_path_stats`/`set_snarls`** — setters,
  `void` return; n/a.
- **Adicionar concept `requires GbwtTimingsLike<T>`** — overhead de
  template para apenas 1 tipo; pular.
- **Removendo o `if` redundante de `out_degree`/`in_degree`** —
  cosmético, sem impacto em hot path (compilador já elimina). Vale
  só por consistência (B2 acima).
