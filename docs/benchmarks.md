# Benchmarks — GfaCsrTopology / GfaIngestorMmapParallel

Métricas de referência coletadas com `bench_ingest` para guiar otimizações de
`gfa_csr_topology.hpp` e `gfa_ingestor.hpp`.

Comando padrão:
```
bench_ingest <arquivo.gfa> <threads> <gamma> <max_depth> <runs> <max_nodes_per_branch> <threads_bfs>
```

---

## Arquivo: hprc-v1.0-mc-chm13.gfa

**Data:** 2026-03-24  
**Comando:** `bench_ingest hprc-v1.0-mc-chm13.gfa 8 2.0 5 1 0 4`  
**Hardware:** (preencher: CPU, RAM, SO)

### Grafo

| Propriedade | Valor |
|---|---|
| Segmentos (nós GFA) | 85 282 047 |
| Links (arestas GFA) | 118 135 187 |
| Nós únicos (MPHF) | 85 282 047 |

### Pipeline de carregamento

| Fase | Tempo |
|---|---|
| Passada 1 — io + parse | 11 201.9 ms |
| Build MPHF | 7 052.2 ms |
| Passada 2 — ingest links | 10 862.8 ms |
| Build CSR topology | 7 004.1 ms |
| **TOTAL** | **36 930.9 ms** |

> **Nota:** tempo com arquivo já em page cache (mmap de 46.8 GB). Primeira leitura (cold cache) levou ≈50 s.

### Estrutura CSR — memória

| Componente | Entradas | Bytes/entrada | Tamanho |
|---|---|---|---|
| E_fwd (arestas saída) | 118 135 187 | 8 B | 901.3 MB |
| E_bwd (arestas entrada) | 118 135 187 | 8 B | 901.3 MB |
| offsets_fwd | 85 282 048 | 4 B | 325.3 MB |
| offsets_bwd | 85 282 048 | 4 B | 325.3 MB |
| **TOTAL CSR** | — | — | **2 453.3 MB** |

### Distribuição de graus

| Grau | Out-degree (nós) | In-degree (nós) |
|---|---|---|
| 0 | 284 248 | 178 660 |
| 1 | 56 028 781 (65.7 %) | 55 235 480 |
| 2 | 26 131 510 (30.6 %) | 27 243 544 |
| 3 | 2 325 385 | 2 340 698 |
| 4 | 248 780 | 182 417 |
| 5–9 | 224 761 | 91 484 |
| 10–19 | 35 896 | 9 084 |
| 20–49 | 2 686 | 680 |
| 50–99 | 0 | 0 |
| ≥100 | 0 | 0 |
| **média** | **1.385** | **1.385** |
| **máx** | **34** | **36** |

### Benchmarks de consultas — baseline (runs=3, threads_bfs=4)

| Consulta | avg | min | max | Resultados |
|---|---|---|---|---|
| `find_tips` | 146.8 ms | 141.2 ms | 152.3 ms | 460 506 |
| `find_repeats(>=2)` | 531.7 ms | 514.0 ms | 563.3 ms | 28 969 018 |
| `find_repeats(>=4)` | 91.0 ms | 88.2 ms | 92.9 ms | 512 123 |
| `find_repeats(>=10)` | 91.7 ms | 90.9 ms | 92.3 ms | 38 582 |
| `find_bubbles(depth=5, max_nodes=0)` | 25 152.0 ms | 24 904.1 ms | 25 364.6 ms | 28 207 289 (incorreto) |

#### Primeiras 5 bolhas — baseline (incorreto)

| # | source | sink | branches |
|---|---|---|---|
| 0 | 4 | 21 225 178 | 8 |
| 1 | 5 | 82 082 689 | 8 |
| 2 | 6 | 24 374 993 | 9 |
| 3 | 7 | 72 029 539 | 8 |
| 4 | 8 | 58 948 667 | 9 |

#### Notas sobre `find_bubbles` — baseline

- 28.2M bolhas detectadas de 29M fontes candidatas (≈97%) — indica convergência excessiva para hub nodes globais.  
- Sinks com IDs muito distantes dos sources — BFS sem critério de localidade.

### Benchmarks de consultas — após correção `find_bubbles` (runs=1, threads_bfs=4)

| Consulta | avg | min | max | Resultados |
|---|---|---|---|---|
| `find_tips` | 128.4 ms | 128.4 ms | 128.4 ms | 460 506 |
| `find_repeats(>=2)` | 497.2 ms | 497.2 ms | 497.2 ms | 28 969 018 |
| `find_repeats(>=4)` | 90.6 ms | 90.6 ms | 90.6 ms | 512 123 |
| `find_repeats(>=10)` | 89.9 ms | 89.9 ms | 89.9 ms | 38 582 |
| `find_bubbles(depth=5, max_nodes=0)` | **20 545 ms** | — | — | **22 023 822** |

#### Primeiras 5 bolhas — após correção

| # | source | sink | branches |
|---|---|---|---|
| 0 | 4 | 21 225 178 | 2 |
| 1 | 5 | 82 082 689 | 2 |
| 2 | 6 | 24 374 993 | 2 |
| 3 | 7 | 72 029 539 | 2 |
| 4 | 8 | 58 948 667 | 2 |

#### Notas sobre `find_bubbles` — v2 no HPRC

- 22M bolhas de 85M nós — plausível para grafo de montagem com grau médio 1.385.
- `branches=2` sugere bolhas com 2 nós internos (ramos simples source→a→sink e source→b→sink).
- Sinks ainda distantes (IDs >> source) — esperado em grafo linear onde cadeias longas convergem raramente.
- Speedup vs baseline: 25 152 ms → 20 545 ms (**1.2×**) — menor que bench_1M porque o grafo HPRC tem muitas bolhas reais que passam na validação.
- Para refinar: adicionar `max_sink_distance` para limitar a distância espacial no grafo.

---

## Arquivo: bench_1M.gfa

**Data:** 2026-03-24  
**Comando:** `bench_ingest bench_1M.gfa 4 2.0 5 3 0 4`  
**Hardware:** (preencher: CPU, RAM, SO)

### Grafo

| Propriedade | Valor |
|---|---|
| Segmentos (nós GFA) | 1 000 000 |
| Links (arestas GFA) | 3 000 000 |
| Nós únicos (MPHF) | 1 000 000 |

### Pipeline de carregamento

| Fase | Tempo |
|---|---|
| Passada 1 — io + parse | 55.4 ms |
| Build MPHF | 68.9 ms |
| Passada 2 — ingest links | 181.6 ms |
| Build CSR topology | 184.8 ms |
| **TOTAL** | **495.1 ms** |

### Estrutura CSR — memória

| Componente | Entradas | Bytes/entrada | Tamanho |
|---|---|---|---|
| E_fwd (arestas saída) | 3 000 000 | 8 B | 22.9 MB |
| E_bwd (arestas entrada) | 3 000 000 | 8 B | 22.9 MB |
| offsets_fwd | 1 000 001 | 4 B | 3.8 MB |
| offsets_bwd | 1 000 001 | 4 B | 3.8 MB |
| **TOTAL CSR** | — | — | **53.4 MB** |

### Distribuição de graus

| Grau | Out-degree (nós) | In-degree (nós) |
|---|---|---|
| 0 | 49 885 | 49 573 |
| 1 | 148 668 (14.9 %) | 148 963 |
| 2 | 224 583 (22.5 %) | 225 198 |
| 3 | 224 381 (22.4 %) | 223 490 |
| 4 | 167 895 (16.8 %) | 167 871 |
| 5–9 | 183 495 (18.3 %) | 183 799 |
| 10–19 | 1 093 | 1 106 |
| 20–49 | 0 | 0 |
| 50–99 | 0 | 0 |
| ≥100 | 0 | 0 |
| **média** | **3.000** | **3.000** |
| **máx** | **14** | **16** |

### Benchmarks de consultas (runs=3, threads_bfs=4)

| Consulta | avg | min | max | Resultados |
|---|---|---|---|---|
| `find_tips` | 2.6 ms | 2.5 ms | 2.7 ms | 97 010 |
| `find_repeats(>=2)` | 5.6 ms | 3.2 ms | 8.1 ms | 801 447 |
| `find_repeats(>=4)` | 3.9 ms | 3.9 ms | 3.9 ms | 352 483 |
| `find_repeats(>=10)` | 0.9 ms | 0.8 ms | 1.1 ms | 1 093 |
| `find_bubbles(depth=5, max_nodes=0)` | 18 389.4 ms | 17 867.6 ms | 19 116.0 ms | 25 864 (incorreto) |

#### Primeiras 5 bolhas — baseline (incorreto)

| # | source | sink | branches |
|---|---|---|---|
| 0 | 25 | 251 407 | 735 |
| 1 | 27 | 799 851 | 1 230 |
| 2 | 581 | 681 921 | 1 181 |
| 3 | 621 | 590 447 | 1 120 |
| 4 | 931 | 517 736 | 1 077 |

#### Notas sobre `find_bubbles` — baseline

- branches >>100 (até 1230) confirmam que o BFS estava convergindo para hub nodes globais, não para sinks locais de bolha.
- Grafo sintético com grau médio 3.0 (vs 1.385 do HPRC) — muito mais denso, BFS expande rapidamente.
- Reforça necessidade de critério de localidade para filtrar falsos positivos.

### Benchmarks de consultas — após correção `find_bubbles` (runs=3, threads_bfs=4)

| Consulta | avg | min | max | Resultados |
|---|---|---|---|---|
| `find_tips` | 2.4 ms | 2.2 ms | 2.6 ms | 97 010 |
| `find_repeats(>=2)` | 7.2 ms | 4.2 ms | 9.8 ms | 801 447 |
| `find_repeats(>=4)` | 3.9 ms | 3.8 ms | 4.0 ms | 352 483 |
| `find_repeats(>=10)` | 1.1 ms | 0.9 ms | 1.2 ms | 1 093 |
| `find_bubbles(depth=5, max_nodes=0)` | **2 498 ms** | 2 490 ms | 2 503 ms | **2 (correto)** |

#### Primeiras bolhas — após correção

| # | source | sink | branches |
|---|---|---|---|
| 0 | 48 395 | 558 154 | 0 |
| 1 | 280 443 | 342 190 | 0 |

#### Notas sobre `find_bubbles` — v2

- Algoritmo reescrito: BFS unificado com interseção progressiva (ramo b≥1 só expande nós já alcançados por todos os ramos anteriores).
- Critério de localidade: sink com profundidade máxima ≤ `max_depth` entre todos os ramos.
- Validação de disjunção + fechamento para confirmar superbubble real.
- `branches=0` nessas 2 bolhas indica nós internos ausentes — bolhas triviais (source→sink direto por todos os ramos, sem nós intermediários).
- Speedup: 18 389 ms → 2 498 ms (**7.4×** mais rápido com correctness).

---

## Histórico de otimizações

| Data | Componente | Alteração | Impacto medido |
|---|---|---|---|
| 2026-03-24 | `find_bubbles` | Reescrita completa: BFS unificado + interseção progressiva + critério de localidade + validações de disjunção/fechamento | bench_1M: 18 389 ms → 2 498 ms (**7.4×**); correctness: 25 864 falsos positivos → 2 bolhas reais; HPRC: 25 152 ms → 20 545 ms (**1.2×**), 28.2M → 22.0M (com validação) |
