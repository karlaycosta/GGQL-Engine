# Baseline de performance — Backend Gbwt vs PathStorage

Documento de referência para medir o impacto das melhorias futuras na
construção do índice Gbwt. Os números abaixo são o estado **antes** das
otimizações, capturados via `bench_gbwt` na branch
`feat/path-backend-coexistence` no commit `a73bf0d`.

Para cada otimização aplicada, adicionar uma nova coluna na tabela
"Comparativo" e uma seção em "Mudanças aplicadas".

---

## Setup

| Item | Valor |
|---|---|
| Data | 2026-05-07 10:50 |
| Branch | `feat/path-backend-coexistence` |
| Commit | `a73bf0d` |
| Compilador | g++-14 (Ubuntu 14.2.0-4ubuntu2~24.04.1) |
| Flags | `-O3 -flto=auto -march=native -fopenmp -funroll-loops -ftree-vectorize` |
| OS | Linux 6.8.0-111-generic x86_64 |
| CPU | Intel Core i7-9750H @ 2.60GHz · 12 cores |
| RAM | 62 GiB |
| OpenMP threads | 8 |
| Dataset | `data/chm13_subset_100w.gfa` (632.89 MB) |
| Dataset stats | 4.294.760 segmentos · 5.025.175 links · 100 paths · 16.033.524 simbolos totais |

## Comando

```bash
./build/bench_gbwt data/chm13_subset_100w.gfa 8 --path-backend <gbwt|pathstorage>
```

---

## Comparativo: PathStorage vs Gbwt (baseline atual)

| Métrica | PathStorage (default) | **Gbwt** (alvo de melhoria) | Razão Gbwt/PathStorage |
|---|---:|---:|---:|
| **Tempo build (wall)** | 190.3 ms | **15.32 s** | **80×** mais lento |
| Tempo CPU médio/thread | 190.3 ms | 15.32 s | 80× |
| Tempo CPU máximo (thread) | 190.3 ms | 15.32 s | 80× |
| Throughput | 84.24 M nós/s | **1.05 M nós/s** | 80× menor |
| **Pico RSS (build isolado)** | 664.32 MB | **1.87 GB** | **2.81×** mais RAM |
| ΔRSS Processo (Total) | 700.73 MB | 941.83 MB | 1.34× |
| **Tamanho do índice (RAM)** | 88.57 MB | **60.63 MB** | 0.68× (32% menor) |
| **`path_stores.bin` (disco)** | 64.135.296 B (61.16 MiB) | **62.571.808 B (59.67 MiB)** | 0.98× (apenas 2.4% menor) |
| **bits/nó** | 46.34 | **31.72** | 0.68× |
| Magic em disco | `GGQLPS\0\0` | `GGQLGB\0\0` | — |

### Tempo total do pipeline (todas as fases incluídas)

| Fase | PathStorage | Gbwt |
|---|---:|---:|
| Fase 1 (MPHF) | 495.8 ms | 537.9 ms |
| Fase 2 (Links/Paths) | 2.41 s | 2.42 s |
| Fase 3 (FM-Index) | 8.76 s | 8.69 s |
| Fase 4 (CSR Topology) | 328.1 ms | 357.6 ms |
| Fase 5 (path index — alvo) | **190.3 ms** | **15.32 s** |
| Snarls | 8.66 s | 8.79 s |
| **Total ingest** | **13.88 s** | **28.90 s** |
| Wall-clock (`time`) | 22.65 s | 37.79 s |

A Fase 5 (path index) é onde está toda a diferença — as outras fases
são essencialmente iguais entre os dois backends.

---

## Análise: por que Gbwt está abaixo do esperado

**Compressão atual: 31.7 bits/nó. Esperado em pangenoma humano: 1–2 bits/nó.**

Causas hipotéticas (ordem aproximada de impacto, a validar com profiling):

1. **Modo serial força 1 GBWTBuilder por shard sequencial.** Em
   `src/storage/path_index.cpp:184` o caminho Gbwt itera shards um a
   um. Cada shard é construído isoladamente e materializado como
   `gbwt::GBWT` antes de prosseguir para o próximo. O cb905db usava 2
   modos (serial / paralelo via OMP) com modelo de custo escolhendo
   entre eles — o port atual perdeu o paralelo.

2. **Dataset tem só 100 paths.** Pangenoma com poucas amostras dá
   pouca redundância para o BWT explorar — a compressão real do gBWT
   só brilha com dezenas a centenas de haplótipos similares (HPRC
   v1 = 28k+ paths). 100 paths em 8 shards = ~12 paths por shard.

3. **Sigma alto por shard mantém alfabeto disperso.** O remap por
   componente que cb905db fazia reduzia sigma, mas nosso path atual
   passa nodes raw para o `GBWTBuilder`.

4. **Builder serial single-thread no insert.** OMP só ataca shards
   diferentes; dentro de um shard, o `GBWTBuilder::insert` é serial.
   Para o shard maior (provavelmente >2M nós no maior componente)
   isso domina o wall.

5. **`gbwt::DynamicGBWT::INSERT_BATCH_SIZE` (100M símbolos) cria
   buffer enorme.** Para dataset de 16M símbolos totais, 100M não faz
   sentido — o batch nunca enche, mas as estruturas de double-buffer
   são alocadas no tamanho cheio.

---

## Oportunidades de melhoria (prioridade decrescente)

### P1 — Re-portar caminho paralelo do cb905db (modo OMP)

**Esperado:** 4–8× speedup com 8 threads (1 builder por shard em paralelo).

Estado atual em `src/storage/path_index.cpp` linha 184: comentário
explícito "Iteracao inicial — modo serial garante correcao
independente de particionamento topologico. Otimizacao paralela
requer garantir invariante 'shards nao compartilham nos'..."

A invariante já é garantida por `repartition_groups_topological`
(usado upstream do build). Basta reativar o caminho paralelo do
cb905db dentro do branch `if (backend == PathBackendKind::Gbwt)`.

### P2 — Sigma reduction local por shard

**Esperado:** ~2× compressão extra (testar empiricamente).

Renumerar nós dentro de cada shard 1..sigma_local antes de inserir
no `GBWTBuilder`. Aproveitar o fato de que cada shard só toca um
componente conexo (já garantido pelo `repartition_topological`).

### P3 — Batch size adaptativo

**Esperado:** menor pico de RAM (impacto em latência menor).

Atualmente fixo em 100M. Adaptar para `min(INSERT_BATCH_SIZE,
total_nodes_shard)`.

### P4 — Mover decode (gbwt_decode_node) para SIMD

**Esperado:** 1.2-1.5× speedup no `extract_into` (afeta queries, não
build).

`extract_into` em `src/gbwt/gbwt_backend.cpp:78` faz loop escalar com
filter de ENDMARKER + decode. Vetorizar com AVX2/AVX-512.

### P5 — Validar paridade contra HPRC v1

Antes de declarar uma melhoria como correta, rodar `bench_gbwt` em
HPRC v1 chm13 para confirmar:
- Compressão chega a ~1-2 bits/nó (esperado para pangenoma denso)
- Build não regride a correção (parity test continua passando)

---

## Mudanças aplicadas

_(adicionar uma seção por otimização aplicada, com data, commit e
delta numérico)_

### (template — preencher conforme avança)

```
### NN — <título da otimização>

Commit: <sha>
Data: YYYY-MM-DD

Mudança: <resumo do que foi feito>

| Métrica | Antes | Depois | Δ |
|---|---:|---:|---:|
| Tempo build | … s | … s | …× |
| Pico RSS | … MB | … MB | …× |
| bits/nó | … | … | …× |
| `path_stores.bin` | … MB | … MB | …× |

Observações: <o que mudou e o que não mudou; surpresas>
```

---

## Saída bruta dos runs (referência)

### Gbwt baseline (15.32 s)

```
● Fase 5: gBWT paralelo · 100 paths em 8 grupos...
│  · GbwtBackend build (serial) · 8 shards · batch 100000000 simbolos
└─ ✔ gBWT construído  100 paths · 60,63 MB · 15,32 s

Tempo Real (Wall-clock)                          15,32 s
Pico RSS (Build Isolado)                         1,87 GB
ΔRSS Processo (Total)                          941,83 MB
Tamanho do Índice                               60,63 MB
Total de Caminhos                                    100
Total de Nós (Símbolos)                       16.033.524
Throughput de Construção                    1.05 M nós/s
Eficiência de Compressão                   31.72 bits/nó
TOTAL Pipeline                                   28,90 s
```

### PathStorage baseline (190 ms)

```
● Fase 5: gBWT paralelo · 100 paths em 8 grupos...
└─ ✔ gBWT construído  100 paths · 88,57 MB · 190,3 ms

Tempo Real (Wall-clock)                         190,3 ms
Pico RSS (Build Isolado)                       664,32 MB
ΔRSS Processo (Total)                          700,73 MB
Tamanho do Índice                               88,57 MB
Total de Caminhos                                    100
Total de Nós (Símbolos)                       16.033.524
Throughput de Construção                   84.24 M nós/s
Eficiência de Compressão                   46.34 bits/nó
TOTAL Pipeline                                   13,88 s
```
