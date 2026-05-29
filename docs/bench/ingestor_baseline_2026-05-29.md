# Baseline ingestor — 2026-05-29

- Commit: `442a82b` (ingestor: **clean**)
- Kernel: 6.8.0-117-generic · RAM: 63 GiB · Threads: 8 · Profile: metagenomics
- COLD reps: 3 (mediana) · survey iters: 6
- Evicção de cache: posix_fadvise(DONTNEED) por arquivo (sem root)

## WARM — bench_survey (isola CPU de parsing · baseline H2)

| Arquivo | Tamanho | Scan médio warm (s) | Throughput (MB/s) |
|---|---:|---:|---:|
| bench_1M.gfa | 127M | 0.0186 | 6808.2 |
| chm13_subset_10w.gfa | 439M | 0.0580 | 7566.0 |
| chm13_subset_100w.gfa | 633M | 0.0896 | 7063.5 |

## COLD — bench_gbwt cache-evicted (isola I/O multi-passada · baseline H1)

Mediana de 3 reps. `fs_in` = File system inputs (blocos de 512 B
lidos do disco, via getrusage ru_inblock). `fs_in×512 / tamanho` ≈ nº de
leituras do arquivo: ~1.0 = page cache persiste entre passadas (sem re-read).

| Arquivo | Tamanho | Wall total (s) | fs_in (blocos) | fs_in/arquivo | Peak RSS (MB) |
|---|---:|---:|---:|---:|---:|
| bench_1M.gfa | 127M | 0.94 | 259344 | 1.00 | 426 |
| chm13_subset_10w.gfa | 439M | 11.08 | 898720 | 1.00 | 2536 |
| chm13_subset_100w.gfa | 633M | 14.45 | 1296152 | 1.00 | 2914 |

_Gerado por `tools/bench/ingestor_bench.sh`. Re-rode após os fixes e compare a mesma tabela._

## H2 — baseline isolado de `next_token` (scalar vs memchr)

Micro-bench `tools/bench/bench_tokenize.cpp` (compilar com
`g++-14 -O3 -march=native -std=c++23`). Replica FIELMENTE o loop de S-lines do
Pass1 (`ingestor.cpp:92-107`): após `S\t`, `tokenize` lê nome + **sequência** e
soma `tokens[1].size()`. Compara o `next_token` escalar atual com a variante
memchr proposta no H2. **Single-thread**, cache quente, mediana de iters
(iter 0 descartado). `seqbytes` idênticos entre variantes ⇒ equivalência.

| Arquivo | avg seqlen | SCALAR | MEMCHR | Speedup |
|---|---:|---:|---:|---:|
| chm13_subset_100w (633M) | ~54 B | 2359 MB/s (0,268 s) | 3824 MB/s (0,166 s) | **1,62×** |
| **CHM13-464 (3,8G real)** | ~2840 B (max 4,67 MB) | 2036 MB/s (1,885 s) | 12124 MB/s (0,317 s) | **5,95×** |

**Leitura honesta:**
- **Isolado, H2 é um ganho grande** em GFAs reais de pangenoma com segmentos
  longos: o scan escalar gasta **1,89 s** só delimitando tokens no CHM13-464;
  memchr faz em **0,32 s** (5,95×). Confirma que o comentário do header
  ("GCC vetoriza com AVX2" / "dois memchr é mais lento") está errado.
- **End-to-end, o ganho é diluído.** O Pass1/Pass2 é paralelo (até 12 threads
  aqui) — o scan de 1,89 s single-thread vira ~0,24 s de wall com 8 threads.
  E o wall total é dominado por outras fases: no CHM13-464 com profile
  `metagenomics` (que **constrói FM-Index**), Fase 1 (MPHF) = 752 ms,
  Fase 2 (Links/Paths) = 1,33 s, **Fase 3 (FM-Index) = 3,40 min**, peak RSS
  41,5 GB. H2 economizaria < 1 s de minutos → desprezível no ingest completo.

**Veredito:** H2 vale como melhoria barata e correta (ganho real no caminho
parse-bound: survey-like, perfis sem FM-Index/gBWT, baixo nº de threads — e o
comentário precisa ser corrigido de qualquer forma), mas **não move a agulha**
num ingest completo dominado por FM-Index. Aplicar pelo valor de
corretude + casos parse-bound, não esperando speedup end-to-end.
