# R13 — Mutation Testing Results

## Campanha executada (2026-05-11)

| Arquivo | Total | Killed | Survived | Build Fail | Score |
|---|---:|---:|---:|---:|:-:|
| `src/query/query_evaluator.cpp` | 30 | 2 | 22 | 6 | **8.3%** (2/24) |
| `src/storage/csr_builder.cpp` | 20 | 2 | 7 | 11 | **22.2%** (2/9) |
| **Total** | 50 | 4 | 29 | 17 | **12.1%** (4/33) |

Ferramenta: `tests/validation/r13_mutate.py` (impl. própria com operadores ROR/LCR/BCR).
Tempo total: ~30 min (build incremental ~30s + ctest subset por mutação).

## Mutação score interpretado

| Tier | Score | Significado |
|---|---|---|
| Excelente | ≥ 80% | Testes detectam vasta maioria dos bugs |
| Bom | 60-80% | Testes razoavelmente fortes |
| Moderado | 40-60% | Algumas lacunas significativas |
| **Fraco** | **< 40%** | Muitas mutações não detectadas |

**Resultado**: 12% — testes ATUAIS detectam apenas 1 em cada 8 mutações.

## Lacunas concretas identificadas (mutações sobreviventes)

### query_evaluator.cpp (R5 deveria cobrir, mas escapou):

| Linha | Operação | Mutação | Por que sobreviveu |
|---|---|---|---|
| 30 | `case NEQ: return l != r` | `==` | Nenhum teste verifica NEQ com l==r |
| 33 | `case GEQ: return l >= r` | `>` | Nenhum teste verifica GEQ com l==r |
| 34 | `case LEQ: return l <= r` | `<` | Nenhum teste verifica LEQ com l==r |
| 115 | `if (sd && ld)` | `\|\|` | Não testamos com `sd == nullptr` |
| 141 | `if (!crosses_cache \|\| !topology_)` | `&&` | Não exercita null pointer |
| 173 | `return false` | `true` | Branch de erro não atingido em testes |
| 194 | `if (!seq_index_ \|\| !metadata_)` | `&&` | Não exercita null pointer |
| 230 | `for (size_t i = 0; i < wc.connectors.size(); ++i)` | `<=` | KILLED ✅ (R5 cobre) |
| 249 | `result_so_far \|\| current` | `&&` | KILLED ✅ (R5 cobre) |

### csr_builder.cpp:

| Linha | Operação | Mutação | Por que sobreviveu |
|---|---|---|---|
| 165 | `for (int d = 0; d < 256; ++d)` | `<=` | KILLED ✅ (iterates exato) |
| 252 | `multiset=true` | `false` | KILLED ✅ |
| 190 | `if (i + kPrefetchAhead < hi)` | `<=` | Prefetch hint, não afeta resultado |
| 266 | `if (--countdown == 0)` | `!=` | Countdown logic survived |
| 286 | `if (..empty() && num_nodes_ > 0)` | `>=` | Edge case num_nodes_==0 |
| 300 | `for (i=0; i <= num_nodes_; ++i)` | `<` | Off-by-one no loop final |
| 221 | `if (m == 0)` | `!=` | Branch m==0 não exercitado |
| 117 | `if (n_passes_real == 0)` | `!=` | Branch raro |

## Ações sugeridas para fechar lacunas

### Categoria A — Boundary tests (alta prioridade)

Adicionar testes que exercitam **l == r** em todos operadores:

```cpp
TEST(query_eval, boundary_LEQ_EQ_GEQ_NEQ) {
    // length == 30 EXACTLY (l == r case)
    assert(run("FIND SEGMENT WHERE length <= 30") inclui o de length=30);
    assert(run("FIND SEGMENT WHERE length >= 30") inclui o de length=30);
    assert(run("FIND SEGMENT WHERE length == 30") só length=30);
    assert(run("FIND SEGMENT WHERE length != 30") exclui length=30);
}
```

Esta adição killaria ~6 das 10 mutações ROR sobreviventes.

### Categoria B — Null pointer safety (média prioridade)

Adicionar testes que passam `topology=nullptr`, `seq_index=nullptr`, `metadata=nullptr`:

```cpp
TEST(query_planner, null_topology_returns_empty) {
    QueryPlanner p(nullptr, &meta, &fm);
    auto r = p.execute(parse("FIND SEGMENT"));
    assert(r->row_count == 0);
}
```

Mataria ~4 das 6 mutações LCR sobreviventes.

### Categoria C — Edge case loops (baixa prioridade)

Testes com `num_nodes_ == 0`, `m == 0`, `n_passes_real == 0` — exercita early-returns
em csr_builder.

## Conclusão

R13 atingiu seu objetivo: **identificou que coverage de linhas (66.8%) é
enganoso** — em termos de detecção de bugs (mutation kill), nossos testes
estão em 12%.

Isso é **típico para projetos C++ sem teste boundary-driven**. Roadmap
para chegar a 60-80% (gold standard):

1. Implementar Categoria A (boundary tests) — ~1 dia, ganho ~+30%
2. Implementar Categoria B (null safety) — ~1 dia, ganho ~+20%
3. Implementar Categoria C (edge cases) — ~2 dias, ganho ~+10%

Total: ~1 semana de trabalho para chegar a ~70% mutation score.

R13 fica marcado como **executado e documentado**, com plano de ação claro
para sessões futuras.
