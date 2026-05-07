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

### 01 — Paralelização do build Gbwt via OMP por shard (P1)

Commit: (a preencher após git commit)
Data: 2026-05-07

**Mudança:** Substituído o loop serial em `build_path_storage_parallel`
(branch `if (backend == PathBackendKind::Gbwt)`) por
`#pragma omp parallel for schedule(dynamic) num_threads(omp_threads_gbwt)`.
Cada thread processa um shard independente. `total_nodes_serial` (uint64)
virou `std::atomic<uint64_t> total_nodes_acc` para acumulação thread-safe.
A invariante "shards não compartilham nós" já era garantida upstream por
`repartition_groups_topological`, então não precisa merge cross-shard.

| Métrica | Baseline | **P1** | Δ |
|---|---:|---:|---:|
| **Tempo build (wall)** | 15.32 s | **6.11 s** | **2.51× mais rápido** |
| Throughput | 1.05 M nós/s | **2.62 M nós/s** | 2.50× |
| **Pico RSS (build isolado)** | 1.87 GB | **4.20 GB** | **2.25× MAIS RAM** |
| ΔRSS Processo (Total) | 941.83 MB | 1.15 GB | 1.25× |
| Tamanho do Índice (RAM) | 60.63 MB | 60.63 MB | igual ✓ |
| `path_stores.bin` (disco) | 62.571.808 B | 62.571.808 B | **bit-exato** ✓ |
| bits/nó | 31.72 | 31.72 | igual ✓ |
| **TOTAL Pipeline** | 28.90 s | **20.01 s** | **1.44×** mais rápido |

**O que mudou e o que não mudou:**

- ✅ Tempo cai pela metade (2.5×)
- ✅ Output bit-a-bit idêntico — `path_stores.bin` tem o MESMO tamanho e
  conteúdo (parity test continua passando: `test_path_backend_coexistence`
  em 0.53s)
- ⚠️ Pico de RAM mais que dobrou — era esperado: agora 8 `DynamicGBWT` em
  flight simultaneamente. Cada um pode crescer até `2 × INSERT_BATCH_SIZE
  × node_bytes` (~1.6 GB por thread no pior caso com batch=100M).
- ⚠️ Speedup abaixo do esperado (2.5× vs 4–8× ideal): bottleneck é o shard
  maior (2.267.658 nós). Os outros 7 shards completam rápido mas a thread
  do maior segura o wall-clock. Schedule `dynamic` minimiza a folga, mas
  não pode acelerar o caminho crítico.
- ✅ Compressão (bits/nó) inalterada — paralelismo não muda a estrutura
  do BWT por shard.

**Próximo gargalo identificado:** shard imbalance. Possíveis ataques:
- Sub-shardar o maior componente em 2-4 fatias (mais paralelismo dentro
  do componente, mas precisa garantir invariante de não-compartilhamento)
- Batch adaptativo (P3): reduz batch_size por shard pequeno, libera RAM
  para builders maiores

**Validação de correção:** `test_path_backend_coexistence` (parity
PathStorage vs Gbwt) continua passando — confirma que a paralelização
não introduz divergência em `extract_into`.

### 02 — Batch adaptativo no GBWTBuilder (P3)

Commit: (a preencher após git commit)
Data: 2026-05-07

**Mudança:** Em vez de fixar `batch_size = INSERT_BATCH_SIZE` (100M
símbolos) para todos os shards, calcula o tamanho de cada shard
upfront e passa `batch_adaptive = clamp(shard_size, 1024, 100M)` ao
`GBWTBuilder`. Para shards menores que 100M, evita alocar
`2 × 100M × node_bytes` em buffers que nunca enchem.

Implementação em `src/storage/path_index.cpp` (branch
`if (backend == PathBackendKind::Gbwt)`):

1. Pré-passada paralela computa `shard_size[i]` por shard:
   - `in_ram_seqs`: somatório de `seq.size()`
   - `tmp_path`: `bytes_on_disk * 8 / node_width`
2. Estima pico dos buffers ANTES do loop principal e exibe na CLI
   para diagnóstico.
3. Cada thread aloca `GBWTBuilder` com batch dimensionado ao próprio
   shard.

| Métrica | P1 | **P3 (P1+adaptive)** | Δ vs P1 |
|---|---:|---:|---:|
| **Pico RSS (build isolado)** | 4.20 GB | **2.14 GB** | **−49%** |
| Tempo build (wall) | 6.11 s | **5.89 s** | −3.6% (marginal) |
| Throughput | 2.62 M nós/s | **2.72 M nós/s** | +3.8% |
| ΔRSS Processo (Total) | 1.15 GB | 1.12 GB | −2.6% |
| Tamanho do Índice (RAM) | 60.63 MB | 60.63 MB | igual ✓ |
| `path_stores.bin` (disco) | 62.571.808 B | 62.571.808 B | **bit-exato** ✓ |
| bits/nó | 31.72 | 31.72 | igual ✓ |
| **TOTAL Pipeline** | 20.01 s | **19.37 s** | −3.2% |

### Comparativo cumulativo (Baseline → P1 → P3)

| Métrica | Baseline serial | P1 (paralelo) | **P3 (+adaptive)** | Δ vs Baseline |
|---|---:|---:|---:|---:|
| **Tempo build** | 15.32 s | 6.11 s | **5.89 s** | **2.60× mais rápido** |
| **Pico RSS** | 1.87 GB | 4.20 GB | **2.14 GB** | +14% (apenas) |
| Throughput | 1.05 M/s | 2.62 M/s | **2.72 M/s** | 2.59× |
| Disco | 62.5 MB | 62.5 MB | 62.5 MB | bit-exato |
| bits/nó | 31.72 | 31.72 | 31.72 | igual |
| **Total Pipeline** | 28.90 s | 20.01 s | **19.37 s** | **−33%** |

**O que mudou e o que não mudou:**

- ✅ **Pico de RAM cai pela metade** sem perder o speedup do P1 — recupera
  quase todo o overhead de RAM introduzido pela paralelização
- ✅ Output bit-exato preservado (parity test passa)
- ✅ A msg de diagnóstico mostra "pico estimado dos buffers ~96.21 MB"
  (era ~12.8 GB com batch fixo: 8 threads × 2 buffers × 100M × 8 B);
  redução de ~133× no pico estimado dos buffers do builder. O pico real
  do processo (2.14 GB) inclui o DynamicGBWT crescendo + estruturas
  auxiliares — o limite teórico dos buffers virou ruído no total.
- ⏸️ **Speedup do build praticamente inalterado** — confirma que o
  bottleneck residual é o tempo de processamento do shard maior, não
  alocação/buffer de batch.
- ⏸️ Compressão (bits/nó) inalterada — batch_size não afeta estrutura
  do BWT.

**Limitação:** ainda longe da compressão esperada (1–2 bits/nó). O
ataque seguinte deve focar em sigma reduction local por shard, ou em
sub-shardar o componente maior.

### 02b — Sub-sharding adaptativo (DESCARTADO)

Tentativa: 2026-05-07 (após P3, antes de sigma reduction)
Decisão: descartar. Sub-sharding não é compatível com pangenoma denso.

**Motivação inicial:** o speedup de P1 ficou em 2.5× (em vez dos 4–8×
esperados) porque os shards são desbalanceados — o maior tem 8.97M
símbolos, o menor 28. Com `schedule(dynamic)`, a thread do shard
maior ainda trava o wall-clock. Hipótese: dividir o shard maior em
K sub-fatias de paths consecutivos aumentaria paralelismo real.

**Implementação:** dentro do branch `if (backend == PathBackendKind::Gbwt)`,
adicionada pré-passada que:
1. Calcula `target_per_shard = total_syms / (2 × omp_threads)`
2. Para cada shard com `size > target` e `paths > 1`:
   - Materializa tmp_path em `in_ram_seqs` se necessário (~72 MB extra
     para o maior shard)
   - Divide em K sub-shards contíguos (preserva ordem dos paths)
3. Loop OMP processa o vetor expandido (8 → 22 sub-shards no caso)

**Resultado em chm13_subset_100w:**

| Métrica | P3 (sem sub-shard) | **02b (com sub-shard)** | Δ |
|---|---:|---:|---:|
| Wall build | 5.89 s | **4.69 s** | -20% (1.25×) ✓ |
| **Pico RSS** | 2.14 GB | **5.09 GB** | **+138%** ❌ |
| **Tamanho índice (RAM)** | 60.63 MB | **190.04 MB** | **+213%** ❌ |
| **`path_stores.bin`** | 62.6 MB | **192.5 MB** | **+208%** ❌ |
| **bits/nó** | 31.72 | **99.43** | **piorou 3×** ❌ |

**Por que descartar:** o ganho de tempo (1.25×) é minúsculo comparado à
degradação em todas as outras dimensões (3× pior em RAM, índice e
compressão). O log mostra "8 shards → 22 sub-shards", ou seja, os 2
maiores foram divididos em ~7 sub-fatias cada — cada sub-fatia ficou com
1–2 paths apenas.

**Lição arquitetural:** o gBWT comprime explorando **redundância entre
paths similares dentro do mesmo índice**. Quando você divide os paths
em sub-shards independentes, cada sub-GBWT vê apenas seus 1–2 paths
isolados — não há redundância para o BWT explorar e cada sub-índice
acaba pagando overhead estrutural fixo (header, samples, alfabeto
local). Isso é o oposto do que se deveria fazer com gBWT.

A intuição de paralelismo "divide-and-conquer" do PathStorage (que
arrays disjuntos comprimem trivialmente independente do agrupamento)
**não transfere** para gBWT.

**Onde sub-sharding faria sentido:** apenas para PathStorage, onde a
divisão não afeta compressão. Para Gbwt, o ataque correto à
compressão/wall é via sigma reduction (não divide paths, só renumera
nós dentro do shard) ou validação em pangenoma denso (HPRC v1, onde a
compressão deve naturalmente cair para 1–2 bits/nó sem precisar tocar
no número de shards).

**Reversão:** `git checkout HEAD -- src/storage/path_index.cpp`
descarta a tentativa preservando P1 + P3 intactos. Esta seção fica
documentada para evitar que o problema seja tentado novamente.

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
