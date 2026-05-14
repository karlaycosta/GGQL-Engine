# Plano de execução — fechar matriz comparativa (§2.2-§2.5 do paper)

> **Documento companion**: `docs/paper_comparisons.md` (§2.2-§2.5)
> **Objetivo**: preencher todas as células `⬜` da matriz comparativa nas 4
> dimensões (C, T, M, D) × 8 ferramentas. Atualmente: ~30/~150 células ✅.
> **Esforço total estimado**: ~3.5-4.5 dias de trabalho líquido (sem
> deadtime de downloads / setups).
> **Última atualização**: 2026-05-12.

---

## §0. Pré-requisitos globais

> **Regra de paridade de threads**: todas as ferramentas comparadas
> (vg, odgi, gfatools, GGQL) devem ser executadas com **a mesma
> quantidade de threads que o `bench_gbwt` usou no T9 = 8**. Comparações
> com contagens de threads divergentes não são apresentáveis no paper.

Antes de iniciar T1-T13, garantir:

### §0.1 Binários
- [x] **GGQL** `build/{bench_gbwt,tests/run_queries}` (release O3+LTO)
- [x] **vg** v1.74.0 em `data/validation/tools/vg`
- [x] **gfatools** em `data/validation/tools/gfatools/gfatools`
- [x] **awk** gawk 5.2.2 (sistema)
- [x] **hyperfine** 1.18 (sistema)
- [x] **/usr/bin/time -v** (sistema, GNU time)
- [ ] **odgi** — pré-requisito T3 (build a partir do source)

### §0.2 Datasets
- [x] `data/chm13_subset_100w.gfa` + `.idx` (4.3M segs, 100 walks)
- [x] `data/validation/tier_b/DRB1-3123.gfa` + `.idx` (5,581 segs)
- [x] `/home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa` + `.idx` (85M segs)
- [x] `data/validation/tier_d/{MT_primate,ecoli_synth}.gfa` + `.idx`

### §0.3 Hardware
- CPU: Intel i7-9750H (6c/12t), DDR4 dual-channel (taskset 0-7 isolado)
- RAM: 62 GiB (suficiente para HPRC full)
- Disco: ~104 GiB livre (HPRC `.idx` ocupa ~22 GiB; vg `.pg` ~50 GiB esperado)

> **Convenção de unidades**: este documento usa GiB/MiB/KiB (binário,
> 1 GiB = 2³⁰ B) consistente com a saída de `du -sh` e
> `/usr/bin/time -v` (RSS em KiB). Conversões: 49.049.813.381 B =
> 45,68 GiB = 49,05 GB decimal.

### §0.4 Scripts existentes a reusar
- `tests/validation/r2_perf_rigorous.sh` — template hyperfine para T
- `tests/validation/r10_hprc_full.sh` — pipeline HPRC
- `tests/validation/r1_determinism.sh` — verificação byte-exact
- Helper `time_query()` já implementado em scripts r10/r11

---

## §1. Roadmap por dia (sequencial, recomendado)

### Dia 1 — Quick wins de memória (~6h)

| Slot | Tarefa | Output |
|---|---|---|
| 09:00-09:30 | **T8** confirmar exclusão de `minigraph` da matriz | Decisão documentada |
| 09:30-10:30 | **T5** re-rodar R2 com `/usr/bin/time -v` capturando RSS | M preenchido para todas queries R2 |
| 10:30-11:00 | **T9** memory peak ingest HPRC com `/usr/bin/time -v` | M ingest GGQL |
| 11:00-12:30 | **T12** gfatools stat em HPRC (paralelo a GGQL FIND SEGMENT) | T+M+D para gfatools |
| 14:00-18:00 | ~~**T1** `vg convert -p` em HPRC full~~ | ⊘ OOM em 2026-05-12 (ver §5.3) |

**Saída do dia (real)**: T5/T8/T9/T12 ✅. T1 abortado por OOM (~1h41min).
Dia 2 redirecionado para T3 (odgi) já que T2 depende de T1.

### Dia 2 — vg em escala HPRC (~8h) — ⊘ CANCELADO

| Slot | Tarefa | Output |
|---|---|---|
| — | ~~**T2** `vg snarls` em HPRC~~ | ⊘ depende de T1 (bloqueado) |
| — | ~~**T10** `vg path -p` vs GGQL FIND PATH~~ | ⊘ depende de `.pg` |

**Plano revisado**: avançar direto para Dia 3 (T3 odgi), que é
independente de vg. T2/T10 poderão ser executados quando houver máquina
≥128 GiB ou após bisseção do GFA HPRC por chromosome.

### Dia 3 — odgi (~6-8h)

| Slot | Tarefa | Output |
|---|---|---|
| 09:00-11:00 | **T3.a** Build odgi do source | Binário `odgi` |
| 11:00-13:00 | **T3.b** `odgi build` em HPRC → `.og` | T+M+D ingest odgi |
| 14:00-16:00 | **T3.c** `odgi stats`, `odgi paths` vs GGQL counts | C+T odgi queries |
| 16:00-17:00 | **T7** `vg find` SEARCH KMER vs GGQL (curto+longo) | T para vg find |
| 17:00-18:00 | **T11** Python set inter como baseline crosses() | T baseline |

**Saída do dia**: odgi completamente integrado, vg find + Python set medidos.

### Dia 4 — Consolidação final (~2-3h)

| Slot | Tarefa | Output |
|---|---|---|
| 09:00-10:00 | **T6** comparação consolidada D: GGQL vs vg .pg vs odgi .og | Tabela D final |
| 10:00-11:00 | **T13** GFA→.idx vs GFA→.pg vs GFA→.og tamanho serializado | D detalhado |

**Saída do dia**: matriz 100% preenchida (sem TileDB-VG — paradigma diferente,
ver `paper_comparisons.md` §7.2).

---

## §2. Plano detalhado por tarefa

### T1 — vg convert -p em HPRC full (T+M+D) — ⊘ BLOQUEADO (OOM)

**Status (2026-05-12)**: tentativa abortada por OOM após ~1h41min de
parsing. Detalhes em §5.3.

**Objetivo original**: medir tempo, RAM e disco do pipeline `vg convert`
para gerar `.pg` a partir do GFA de 45,68 GiB. Preenche 3 células da matriz
§2.2.4.

**Comando**:

```bash
GFA=/home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa
PG=/home/karlay/Downloads/hprc-v1.0-mc-chm13.pg
VG=data/validation/tools/vg

# Cold ingest com medição
/usr/bin/time -v "$VG" convert -g "$GFA" -p > "$PG" 2> tests/validation/T1_vg_convert.log

# Outputs (extrair do .log)
grep -E "Elapsed|Maximum resident|User time|System time" tests/validation/T1_vg_convert.log
du -sh "$PG"
```

**Critério ✅** (§2.5):
- T: 1 run (não 5+10 — é cold ingest, demora demais para repetir)
- M: `Maximum resident set size` em KiB do `/usr/bin/time -v`
- D: `du -sh $PG`

**Risco**: vg convert pode crashar em GFA tão grande (45,68 GiB). Mitigação:
tentar com `--threads N` se default lento; se OOM, dividir GFA por chromosomes.

**Onde gravar**: `tests/validation/r10_results/T1_vg_convert.log` + atualizar
linha `Cold ingest HPRC | vg convert -p` na §2.2.4.

### T2 — vg snarls em HPRC full (T+M+D) — ⊘ BLOQUEADO (depende de T1)

**Status (2026-05-12)**: depende do `.pg` gerado por T1, que falhou por
OOM. Não factível na máquina atual (62 GiB). Mitigações possíveis:
máquina ≥128 GiB, ou bisseção por chromosome do GFA antes do `vg convert`.

**Objetivo original**: cross-check flagship — vg snarls em escala HPRC.
Já estimamos 22min via extrapolação 77×; medir real.

**Comando** (assume T1 já gerou `.pg`):

```bash
PG=/home/karlay/Downloads/hprc-v1.0-mc-chm13.pg

/usr/bin/time -v "$VG" stats -R "$PG" > /tmp/T2_vg_snarls.tsv 2> tests/validation/T2_vg_snarls.log

# Counts
wc -l /tmp/T2_vg_snarls.tsv  # total snarls
awk 'NR>1 && $17>2' /tmp/T2_vg_snarls.tsv | wc -l  # com conteúdo interno
```

**Critério ✅**:
- T: 1 run cold (extrapolação 22min, esperado <2h)
- M: peak RSS via `/usr/bin/time -v`
- D: arquivo `.snarls.tsv` (intermediário)
- **C**: comparar count com GGQL FIND BUBBLE = 27,483,058. Se vg ≠ GGQL,
  documentar diferença semântica em §6.

**Risco**: vg snarls é single-threaded em integrated mode. Pode levar horas.
Mitigação: rodar em background, fazer T3 em paralelo.

### T3 — odgi setup + ingest + queries (C+T+M+D) — ⏱ 4-8h

**T3.a — Build odgi** (1-2h):

Já existe tarball `odgi-v0.9.4` em `data/validation/tools/odgi.tar.gz`
(64 MiB, todas as deps auto-contidas). Não precisa `git clone`. Comando:

```bash
cd data/validation/tools && tar xzf odgi.tar.gz
ODGI=$(pwd)/odgi-v0.9.4
CC=gcc-14 CXX=g++-14 cmake -S "$ODGI" -B "$ODGI/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ODGI/build" -j 8        # 8 threads, paridade com T9
"$ODGI/build/bin/odgi" version          # usar binário in-tree, sem sudo cp
```

**Patch necessário** (aplicado 2026-05-12): comentar
`add_subdirectory(test)` em `deps/sdsl-lite/CMakeLists.txt:151`. O test
target do sdsl-lite usa `STRING(REGEX REPLACE "${CMAKE_CURRENT_BINARY_DIR}/..."`,
e nosso source path `.../c++/ggql_engine/...` tem `++` que é tratado como
metacaractere regex. Os testes do sdsl-lite não são consumidos pelo odgi
— só a lib é. Sem prejuízo funcional.

**T3.b — Ingest HPRC** (2h estimado):

```bash
GFA=/home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa
OG=/home/karlay/Downloads/hprc-v1.0-mc-chm13.og

/usr/bin/time -v odgi build -g "$GFA" -o "$OG" -t 8 2> tests/validation/T3_odgi_build.log
du -sh "$OG"
```

**T3.c — Queries vs GGQL** (1-2h):

```bash
# odgi stats: equivalente a FIND SEGMENT count
/usr/bin/time -v odgi stats -i "$OG" -S | grep "node_count"  # 85,282,047 esperado

# odgi paths: equivalente a FIND PATH
/usr/bin/time -v odgi paths -i "$OG" | wc -l  # 28,784 esperado

# odgi tips
odgi tips -i "$OG" -t 8 | wc -l  # 460,506 esperado

# odgi snarls (se suportado)
odgi snarls -i "$OG" -t 8 | wc -l
```

**Critério ✅**: count exato vs GGQL (Δ=0 esperado para SEGMENT/PATH/TIP).
Para snarls, documentar se diferença semântica.

**Output esperado**: 4-6 linhas de odgi na matriz §2.2 preenchidas.

### T4 — ⊘ DESCARTADO (TileDB-VG paradigma diferente)

TileDB-VG é *storage layer* multidimensional, não query engine declarativo.
Comparação 1:1 não é justa — equivale a benchmark SQLite vs Apache Parquet.
GGQL e TileDB-VG são potencialmente **complementares** (GGQL poderia usar
TileDB-VG como backend), não concorrentes. Justificativa completa em
`paper_comparisons.md` §7.2. Decisão tomada em 2026-05-12.

### T5 — Memory peak nas queries R2 (M completo) — ⏱ 1h

**Objetivo**: re-rodar 13 queries de R2 com `/usr/bin/time -v` para capturar RSS.

**Estratégia**: estender `r2_perf_rigorous.sh` com flag opcional `--with-rss`
que envolve cada execução em `/usr/bin/time -v`. Ou criar `r2_memory.sh`
separado.

**Comando** (rascunho):

```bash
for q in "FIND SEGMENT" "FIND EDGE" "FIND PATH" "FIND BUBBLE" \
         "FIND TIP" "FIND REPEAT" \
         'SEARCH KMER "AAAAAAAA"' 'SEARCH KMER "CGATCGATCG"' \
         'FIND SEGMENT WHERE length > 1000' 'FIND SEGMENT WHERE name = "1"'; do
    label=$(echo "$q" | tr -c 'a-zA-Z0-9' '_')
    for i in 1 2 3; do
        /usr/bin/time -v build/tests/run_queries data/chm13_subset_100w.gfa.idx "$q" 2>&1 \
            | grep "Maximum resident"
    done | awk '{print $NF}' | sort -n | awk 'NR==2 {print}' > tests/validation/r5_memory/${label}.rss_kb
done
```

**Critério ✅**: mediana de 3 runs por query, RSS reportado em MiB.

**Output**: tabela nova em §4.1 com coluna "RSS peak" ao lado de mediana.

### T6 — Disk consolidado (D) — ⏱ 1h (após T1, T3)

**Tabela final** §5.1 com 4 linhas para HPRC:

| Tool | Index size | % de GFA |
|---|---:|---:|
| GGQL .idx | 21,99 GiB | 48,1% |
| vg .pg | (T1) | (T1/45,68 GiB) |
| odgi .og | (T3) | (T3/45,68 GiB) |

**Critério ✅**: `du -sh` em cada índice, tabela atualizada.

### T7 — vg find SEARCH KMER comparativo (T) — ⏱ 2-4h

**Objetivo**: medir tempo de vg find para queries equivalentes ao SEARCH KMER.

**Comando**:

```bash
# vg find -p (path scan) ou vg find -N (node)?  Verificar API
# Provavelmente: vg find -G "ACGT" -x <idx>

# Setup
"$VG" gbwt -G "$GFA" -o /tmp/hprc.gbwt  # GCSA needed for find

# Bench
hyperfine --warmup 2 --runs 5 \
    "$VG find -G AAAAAAAA -x /tmp/hprc.xg | wc -l" \
    "build/tests/run_queries hprc.idx 'SEARCH KMER \"AAAAAAAA\"'"
```

**Critério ✅**: paridade exata em counts + speedup documentado.

**Risco**: vg find requer GCSA2 index que pode demorar horas para HPRC.
Pode pular em escala HPRC, fazer apenas em chm13_100w.

### T8 — minigraph: confirmar exclusão (30min)

**Análise**: minigraph é aligner, não query engine. Não tem comandos diretos
para FIND SEGMENT/EDGE/PATH como queries declarativas. Suporta sketch-based
queries internas para alignment seed, mas não interface comparável.

**Decisão**: documentar em §7.2 (comparações que não fazem sentido) e
**remover linha de minigraph da matriz §2.1** (substituir por `—` se mantida).

**Output**: parágrafo justificando + matriz §2.1 atualizada.

### T9 — Memory peak ingest HPRC GGQL (M) — ⏱ 30min (re-run)

**Comando** (8 threads = baseline para todas as comparações):

```bash
rm -rf /home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa.idx
/usr/bin/time -v build/bench_gbwt /home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa 8 \
    2> tests/validation/T9_ggql_ingest_rss.log
```

**Critério ✅**: Maximum resident reportado. Atualizar linha
`Cold ingest HPRC | GGQL bench_gbwt` na §2.2.4.

### T10 — vg path -p vs FIND PATH (C+T) — ⏱ 1h

```bash
hyperfine --warmup 2 --runs 5 \
    "$VG paths -L -x /tmp/hprc.xg | wc -l" \
    "build/tests/run_queries hprc.idx 'FIND PATH'"
```

**Critério ✅**: count match (28,784) + speedup.

### T11 — Python set inter baseline crosses() (T) — ⏱ 2h

**Cenário**: muitos usuários de bioinformatica usam Python+set para
crosses-equivalent. Demonstrar speedup contra esse baseline.

**Comando** (Python tarefa equivalente):

```python
import sys
walks = {}
with open(sys.argv[1]) as f:
    for line in f:
        if line.startswith("W\t"):
            ...  # parse walk, store node set
target = walks[sys.argv[2]]
count = sum(1 for q in walks if walks[q] & target)
print(count)
```

```bash
hyperfine --warmup 2 --runs 5 \
    "python3 crosses.py chm13_100w.gfa target_path" \
    "build/tests/run_queries chm13_100w.idx 'FIND PATH WHERE crosses(\"target\")'"
```

**Critério ✅**: paridade count + speedup (esperado GGQL >50× pelo CrossesCache).

### T12 — gfatools stat em HPRC (T+M+D) — ⏱ 1h

```bash
GFATOOLS=data/validation/tools/gfatools/gfatools
/usr/bin/time -v "$GFATOOLS" stat /home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa \
    > /tmp/T12_gfatools_stat.txt 2> tests/validation/T12_gfatools_stat.log
```

**Critério ✅**: counts batem com GGQL FIND SEGMENT/EDGE; tempo+RAM medidos.

### T13 — Tamanho serializado GFA→{idx,pg,og} (D) — ⏱ 1h

Tabela final de comparação de tamanho de índice serializado, após T1+T3
gerarem `.pg` e `.og`.

**Comando**:

```bash
du -sh /home/karlay/Downloads/hprc-v1.0-mc-chm13.{gfa,gfa.idx,pg,og}
```

**Output**: tabela §5.1 com 4 colunas (GFA, .idx, .pg, .og) + ratio.

---

## §3. Análise de risco

### §3.1 Riscos por tarefa

| Tarefa | Risco | Mitigação |
|---|---|---|
| T1 vg convert HPRC | OOM ou crash em 45,68 GiB | Tentar `--threads 4`; se falhar, dividir GFA por chrom |
| T2 vg snarls HPRC | Pode demorar 8h+ (vg single-threaded) | Rodar background, fazer T3 em paralelo |
| T3 odgi build | Dependências (jemalloc, htslib) | Usar Docker `pangenome/odgi:latest` |
| T5 RSS | 3 runs em 13 queries = 40min extra | Tolerável |
| T7 vg find | GCSA index demora horas em HPRC | Limitar a chm13_100w |

### §3.2 Risco global

Cronograma realista: **2.5-4 dias úteis** (após descartar T4 TileDB-VG).
A subestimativa original (3.5-4.5h) não contava setup de odgi.

### §3.3 Plano contingente

Se cronograma apertar:
- **Mínimo aceitável**: T1+T2+T3+T5+T6+T9 (~3 dias) — fecha vg+odgi em HPRC
- **Stretch**: T1-T13 (sem T4) (~4 dias) — fecha 100% da matriz justa

---

## §4. Definição de "Pronto" (DoD)

A matriz §2.2 está **pronta para o paper** quando:

1. ✅ Todas as células 🟦 (C+T+M+D) ⬜ viraram ✅ ou ⊘ (com justificativa)
2. ✅ Tabelas §3 (corretude), §4 (perf), §5 (recursos) atualizadas com novos números
3. ✅ §6 (diferenças semânticas) acrescentou notas sobre odgi/vg snarls modes
4. ✅ §7.1 (limitações) reflete o que foi/não foi feito
5. ✅ Scripts em `tests/validation/T{1..13}_*.sh` versionados e reprodutíveis
6. ✅ CI atualizado para rodar T5+T9 em PRs (RSS regression check)

---

## §5. Tracking — Status atual (2026-05-14)

### Concluídos
- ~30 células ✅ corretude (Sprints 1-12, R3, R4, R5, R10, R11)
- ~6 células ✅ tempo (R2 chm13_100w)
- **~12 células ✅ memória** (T5 — 2026-05-12)
- 1 célula ✅ disco (GGQL .idx HPRC)
- ✅ **T8** — minigraph excluído (decisão documentada)
- ✅ **T5** — memory peak medido (13 queries × 3 runs, ~42s)
- ✅ **T9** — memory peak ingest HPRC GGQL (2026-05-12, ver §5.1 abaixo)
- ✅ **T12** — gfatools stat HPRC (2026-05-12, ver §5.2 abaixo)
- ✅ **T3.a** — odgi v0.9.4 buildado (2026-05-12, ver §5.4 abaixo)
- ✅ **T3.b** — odgi build HPRC concluído (2026-05-12, ver §5.5 abaixo)
- ✅ **T3.c** — queries odgi vs GGQL: stats + paths (2026-05-12, ver §5.6 abaixo)
- ✅ **T6 + T13** — tabelas finais de disco consolidadas (2026-05-12, ver §5.7 abaixo)
- ✅ **Fase 5** — robustez adversarial (2026-05-14, ver §5.8 abaixo)
- ✅ **Fase 6** — consolidação para o paper (2026-05-14, ver §5.9 abaixo)

### §5.1 T9 — Resultado cold ingest HPRC (GGQL, 2026-05-12)

Comando: `/usr/bin/time -v ./build/bench_gbwt hprc-v1.0-mc-chm13.gfa 8`

| Métrica | Valor |
|---|---:|
| Wall-clock total | **17,65 min** (1059,7 s) |
| User CPU time | 6.159,40 s (~5,8× wall — ~6 threads efetivas) |
| System CPU time | 166,24 s |
| **Maximum resident (RSS peak)** | **53.168.224 KiB ≈ 50,71 GiB** |
| Pico RSS build gBWT isolado | 20,93 GiB |
| Major page faults | 4.218 |
| Minor page faults | 52.655.982 |
| Tamanho .idx final | **21,99 GiB** (48,1% de GFA 45,68 GiB) |

Breakdown por fase:
- Fase 1 (MPHF): 10,63 s
- Fase 2 (Links/Paths): 2,99 min
- Fase 3 (FM-Index): 2,51 min
- Fase 4 (CSR Topology): 6,22 s
- **Fase 5 (gBWT): 7,18 min** (mais cara, ~41% do total)
- Fase 6 (Snarls): 2,08 min
- Fase 7 (Persistência): 1,10 s

**Notas**:
- Run anterior morreu por OOM (sem log) — `.idx` parcial 7,4 GiB. Re-run
  limpo concluiu com 49 GiB livres no início; pico 50,71 GiB encostou no
  limite da máquina (62 GiB), portanto rodar T9 com outros workloads
  pesados ativos é arriscado.
- Log completo em `tests/validation/T9_T12_results/T9_ggql_ingest.log` +
  stdout em `T9_ggql_ingest.stdout`.

### §5.2 T12 — Resultado gfatools stat HPRC (2026-05-12)

Comando: `/usr/bin/time -v gfatools stat hprc-v1.0-mc-chm13.gfa`

| Métrica | Valor |
|---|---:|
| Wall-clock | **18:58 min** (1138 s) |
| User CPU time | 1.072,79 s (~1× wall → **single-threaded**) |
| System CPU time | 39,60 s |
| **Maximum resident (RSS peak)** | **49.854.152 KiB ≈ 47,54 GiB** |

Counts produzidos:
| Métrica | gfatools | GGQL | Δ |
|---|---:|---:|---:|
| Segments | 85.282.047 | 85.282.047 | **0** ✅ |
| Links | 118.135.187 | 118.135.187 | **0** ✅ |
| Total seg length | 3.324.657.754 bp | — | — |
| Average seg length | 38,984 bp | — | — |
| Max degree | 36 | — | — |

**Insight comparativo crítico**:
- gfatools `stat` (single-threaded, **só conta** segs/links/lengths) usa
  47,54 GiB pico — praticamente o mesmo que GGQL no pipeline completo
  (50,71 GiB inclui CSR + gBWT + FM-Index + Snarls + persistência).
- Wall-clock gfatools (19 min) > GGQL (17,65 min) — apesar de fazer
  drasticamente menos trabalho.
- Conclusão: GGQL entrega ordens de grandeza mais funcionalidade pelo
  mesmo footprint de RAM e menos tempo. Forte argumento para §2.2.

**Risco evitado**: usuário alertou em tempo real que gfatools estava
passando dos 50 GiB. Por sorte o RSS pico do gfatools (47,54 GiB) ficou
abaixo dos 50,71 GiB do GGQL, então não houve concorrência fatal.

Output em `tests/validation/T9_T12_results/T12_gfatools_{stat.txt,time.log}`.

### Bloqueados / Cancelados
- ⊘ **T1** (vg convert HPRC) — OOM após ~1h41min em 2026-05-12 (ver §5.3)
- ⊘ **T2** (vg snarls HPRC) — depende de `.pg` de T1
- ⊘ **T10** (vg path -p HPRC) — depende de `.pg` de T1

### Em progresso
- (nenhum no momento)

### Próximo a executar

**Bloqueador pré-paper** (Fase 6 §4.1):
- ✅ **I-1 (DoS)** — resolvido em 2026-05-14. Fix em
  `src/query/query_planner.cpp:616-625` (boost 16× × cap
  `kSafetyHardLimit`). Validado: `SEARCH KMER "A" LIMIT 10` em
  CHM13-464 caiu de 4 min/13 GiB para 9,22 s/12,3 GiB. Testes de
  regressão em `tests/test_query_planner_idx.cpp:153-160`.

**Atualizações ao `paper_comparisons.md`** (Fase 6 §6.1, 7 itens):
1. §2.1 capability matrix → adicionar coluna "robustez adversarial"
2. §3 Corretude → §3.6 Tier C-CHM13-464 com 13/15 paridades
3. §4 Performance → §4.3 warm queries (F09 23×, F10 1.442×, F12 15×,
   F13 raro 43-53×, F14 18×)
4. §5 Recursos → §5.1 tabela 3-tier + §5.3 multi-thread scaling
5. §6 Diferenças semânticas → §6.5 F07 SEARCH KMER dedup-por-seg
6. §7 Limitações → §7.4 com L1-L9 (especialmente L4 DoS, L9 IUPAC)
7. §8 Resumo executivo → números das 5 fases

**Pendências menores** (não bloqueadoras):
- I-2/I-3/I-4/I-5 — roadmap pós-paper (~30 LOC totais).
- **T7** (vg find SEARCH KMER) — possível em chm13_100w; vg find em HPRC
  requer GCSA index (horas).
- **T11** (Python set inter baseline crosses) — possível em chm13_100w,
  ~2h.
- Para fechar 100% da matriz com vg: requer máquina ≥128 GiB ou
  bisseção do GFA HPRC por chromosome (T1+T2+T10 desbloqueados).

### §5.3 T1 — Tentativa falhada (vg convert HPRC, 2026-05-12)

Comando: `nohup /usr/bin/time -v vg convert -g hprc-v1.0-mc-chm13.gfa -p > hprc-v1.0-mc-chm13.pg 2> tests/validation/T1_vg_convert.log &`

| Métrica | Valor |
|---|---|
| Início | ~11:11 (criação do `.pg`) |
| Fim | ~12:52 (último write no log) |
| Duração | **~1h41min** antes de OOM-kill |
| Output `.pg` | **0 bytes** (vg ainda em pass 1, nenhum flush) |
| Log stderr | 15 linhas — warnings `Duplicate path CHM13.chrN` (parou em chr15) |

**Diagnóstico**:
- `vg convert` carrega o packed graph completo em RAM antes de serializar.
- GFA de 45,68 GiB + RAM peak esperado >50 GiB (extrapolação do T9 do GGQL,
  que usou 50,71 GiB pico) excedem os 62 GiB da máquina.
- Não houve OOM message no log (stderr só recebeu warnings de parsing);
  killer foi o OOM-killer do kernel.

**Decisão**: T1 marcado ⊘ "não factível na máquina atual". Mitigações
para sessão futura, se necessário:
1. Máquina ≥128 GiB de RAM.
2. Bisseção do GFA por chromosome → `vg convert` por chrom → `vg combine`.
3. Substituir T1+T2 por comparação **só em chm13_100w** (4,3M segs),
   reconhecendo limitação em §7.1.

Output em `tests/validation/T1_results/T1_vg_convert.log`. `.pg` vazio
em `/home/karlay/Downloads/hprc-v1.0-mc-chm13.pg` pode ser removido.

### §5.4 T3.a — Build odgi (2026-05-12)

| Métrica | Valor |
|---|---|
| Versão | v0.9.4-0-g5e58a324 |
| Wall-clock | **2 min 50 s** (-j8) |
| User CPU | 1.161,46 s (714% — paralelismo efetivo) |
| RSS pico build | 775 MiB |
| Binário | `data/validation/tools/odgi-v0.9.4/bin/odgi` (13 MiB) |
| Flags | `-Ofast -march=native -pipe -msse4.2 -funroll-all-loops -fPIC` (default odgi) |

**Patch aplicado**: `deps/sdsl-lite/CMakeLists.txt:151` —
`add_subdirectory(test)` comentado (regex bug com `++` no path).

Logs em `tests/validation/T3_odgi_results/T3a_compile.{log,stdout}`.

### §5.5 T3.b — odgi build HPRC (2026-05-12)

Comando: `/usr/bin/time -v odgi build -g hprc-v1.0-mc-chm13.gfa -o hprc-v1.0-mc-chm13.og -t 8 -P`

| Métrica | Valor |
|---|---|
| Wall-clock | **8 min 45 s** (525,24 s) |
| User CPU time | 506,60 s |
| System CPU time | 92,01 s |
| **Percent of CPU** | **113%** (≈ 1.1 thread efetiva — `-t 8` não escalou) |
| **Maximum resident (RSS pico)** | **61.655.640 KiB ≈ 58,80 GiB** |
| Major page faults | 38.207 (vs 4.218 GGQL — muito I/O) |
| Minor page faults | 12.714.781 |
| **Output `.og`** | **14,97 GiB** (32,8% do GFA 45,68 GiB) |

**Comparação direta com GGQL T9** (mesma máquina, mesmo dataset, -t 8):

| Métrica | GGQL `bench_gbwt` | odgi `build` | Razão |
|---|---:|---:|---|
| Wall-clock | 17:39 | **8:45** | odgi 2,02× mais rápido |
| CPU% | 596% | 113% | GGQL escala em 8 threads, odgi não |
| RSS pico | 50,71 GiB | **58,80 GiB** | odgi +16% |
| Output | 21,99 GiB (.idx) | 14,97 GiB (.og) | GGQL +47% |

**Interpretação para o paper (§2.2.4 + §6)**:
- Comparação não é 1:1: GGQL constrói FM-Index + gBWT + snarls + length
  index pré-computados; `odgi build` só monta topologia bidirected +
  paths + sequências.
- GGQL paga o preço da pré-computação no ingest (2× wall) mas entrega
  queries instantâneas (FIND BUBBLE em ms vs `odgi snarls` em min).
- odgi não escala em `-t 8` (single-thread bound em I/O do GFA);
  argumento forte de paralelismo a favor do GGQL.
- odgi RSS encostou no teto (58,80 / 62 GiB) — máquina com menos RAM
  reproduziria OOM como em T1 com vg.

Log em `tests/validation/T3_odgi_results/T3b_build.log` (34 KiB de
progresso + métricas finais).

### §5.6 T3.c — Queries odgi vs GGQL (HPRC, 2026-05-12)

#### §5.6.1 Comandos disponíveis

`odgi v0.9.4` **não suporta `snarls`** como subcomando. Comandos
testados: `stats -S`, `paths -L`. `tips` exige path de referência
(`-r/-R`) e não é equivalente semântico ao GGQL `FIND TIP` (que retorna
nós com in/out degree zero) — descartado da comparação.

#### §5.6.2 Resultados

| Query | Wall-clock | CPU% | RSS pico |
|---|---:|---:|---:|
| `odgi stats -S -t 8` | **1 min 09 s** | 99% | **25,2 GiB** |
| `odgi paths -L -t 8` | 0 min 48 s | 99% | 25,2 GiB |

Apesar de `-t 8`, ambas operações ficam single-thread (CPU% ≈ 99%).
RSS pico ~25 GiB inclui mmap do `.og` (14,97 GiB) + estruturas dinâmicas.

#### §5.6.3 Counts comparados

| Métrica | odgi `stats -S` | GGQL | Δ | Status |
|---|---:|---:|---:|---|
| Nodes | 85.282.047 | 85.282.047 | 0 | ✅ |
| Edges | 118.135.187 | 118.135.187 | 0 | ✅ |
| Paths | **25** | **28.784** | **+28.759** | ⚠ semantic gap |
| Length (bp) | 3.324.657.754 | — | — | (não exposto pelo GGQL) |
| Steps | 50.684.471 | — | — | (interno do odgi) |

#### §5.6.4 Divergência de paths — análise

**Não é bug**: `odgi v0.9.4 build` ingere apenas **P-lines** do GFA
(25 referências CHM13.chr1..chrN). GGQL ingere **W-lines** (28.784
walks/haplotipos do pangenoma HPRC). O T1 (vg convert) confirmou a
estrutura: warnings "Duplicate path CHM13.chrN exists in graph" para
chr1..chr15 — vg também processa P+W e detecta como duplicatas.

GFAs pangenômicos modernos (HPRC, PGGB) usam predominantemente
W-lines. odgi build descarta 99,9% dos caminhos por default.

**Implicação para o paper (§6 — diferenças semânticas)**:
GGQL `FIND PATH` retorna haplotipos individuais; odgi `paths` retorna
referências de coordenada apenas. Comparações de queries que enumeram
ou cruzam paths (FIND PATH WHERE crosses(...), tabelas de presença/
ausência) não são semanticamente comparáveis na escala HPRC.

#### §5.6.5 Trabalho total para uma query simples

Tempo wall-clock para "responder ao usuário: quantos nós tem o grafo?":

| Pipeline | Ingest | Query | **Total** | Repetição |
|---|---:|---:|---:|---|
| GGQL: `bench_gbwt` + `FIND SEGMENT` | 17:39 | ms (FM-Index) | **17:39** | imediato |
| odgi: `build` + `stats -S` | 8:45 | 1:09 | **9:54** | +1:09 cada |
| **Razão (1ª query)** | | | odgi 1,8× mais rápido | |
| **Razão (2ª query)** | 0 | 1:09 | **GGQL >100× mais rápido** | |

odgi paga ~1 min de mmap+scan a cada query. GGQL paga 1 ingest e amortiza
em milhares de queries. Argumento de **amortização forte** para o paper.

Logs em `tests/validation/T3_odgi_results/T3c{1,2}_*.{txt,time.log,count}`.

### §5.7 T6 + T13 — Tamanho de índices consolidado (D, 2026-05-12)

#### §5.7.1 Tabela mestra (3 escalas)

| Dataset | GFA | GGQL `.idx` | odgi `.og` | vg `.pg` |
|---|---:|---:|---:|---:|
| DRB1-3123 (5,5K segs, sem paths) | 429,66 KiB | **581,99 KiB** (135,46%) | — | — |
| DRB1-3123 (com paths) | 429,66 KiB | 697,25 KiB (162,28%) | 971,67 KiB (226,15%) | 395,08 KiB (91,95%) |
| chm13_subset_100w (4,3M segs) | 632,89 MiB | **1.368,70 MiB** (216,27%) | — | — |
| CHM13-464 (data/comparisons) | 3,75 GiB | **15,72 GiB** (419,47%) | — | 1,55 GiB |
| HPRC (85,3M segs) | **45,68 GiB** | **21,99 GiB** (48,13%) | **14,97 GiB** (32,77%) | ⊘ T1 OOM |

Bytes exatos (medidos com `du -sb` em 2026-05-14):
- DRB1-3123 GFA: 439.972 B
- DRB1-3123 `.idx` (tier_b): 595.966 B
- DRB1-3123 `.idx` (comparisons): 713.981 B
- DRB1-3123 `.pg`: 404.559 B
- DRB1-3123 `.og`: 994.987 B
- chm13_subset_100w GFA: 663.629.174 B
- chm13_subset_100w `.idx`: 1.435.194.353 B
- CHM13-464 GFA: 4.025.317.918 B
- CHM13-464 `.idx`: 16.885.045.797 B
- CHM13-464 `.pg`: 1.665.686.979 B
- HPRC GFA: 49.049.813.381 B
- HPRC `.idx` (documentado, removido do filesystem): 23.608.551.929 B
- HPRC `.og`: 16.072.609.535 B

Convenção de unidades: GiB/MiB/KiB binários (1 GiB = 2³⁰ B) consistente
com `du -sh` e `/usr/bin/time -v` (RSS em KiB).

#### §5.7.2 Ratio cresce com a escala — interpretação

Em datasets pequenos, **overhead estrutural** (headers SDSL,
quantização EF, MPHF de mínimo carrying) domina → ratio >100%. Em
escala pangenômica, **redundância de sequências e topologia** permite
compressão real → ratio cai abaixo de 50%. A não-monotonicidade
1,35× → 2,16× → 0,48× evidencia que o GGQL é desenhado para escala
pangenômica e não compete com tools especializados em grafos pequenos.

#### §5.7.3 Breakdown do `.idx` GGQL — HPRC (% relativo)

| Componente | Bytes | % do `.idx` | Propósito |
|---|---:|---:|---|
| `fm_index.bin` | 12,17 GiB | **51,5%** | SEARCH KMER instantâneo (LF-mapping) |
| `metadata_seqs.bin` | 4,09 GiB | 17,3% | Sequências DNA dos segmentos |
| `path_stores.bin` | 1,89 GiB | 8,0% | Paths quentes em RAM-store binário |
| `metadata_strings.bin` | 1,35 GiB | 5,7% | Tabela de nomes (segmentos/paths) |
| `metadata.bin` | 1,02 GiB | 4,3% | Mapas de offsets para metadata |
| `remap.bin` | 682 MiB | 2,9% | Remapping IDs externos → sequenciais |
| `length_index.bin` | 682 MiB | 2,9% | Lengths por nó (FIND ... WHERE length) |
| `snarls.bin` | 660 MiB | 2,8% | FIND BUBBLE pré-computado |
| `E_fwd.bin` + `E_bwd.bin` | 945 MiB | 4,0% | CSR topology bidirected |
| `ef_fwd.sdsl` + `ef_bwd.sdsl` | 66 MiB | 0,28% | Elias-Fano para offsets CSR |
| `mphf.bin` | 39,5 MiB | 0,17% | BBHash para lookup O(1) |
| `topology.bin` | 1,35 MiB | 0,006% | Sumário global (componentes, bins) |
| `path_stats.bin` | 345 KiB | 0,001% | Estatísticas pré-computadas por path |

**51,5% do GGQL `.idx` é FM-Index** — é o que entrega `SEARCH KMER` em
milissegundos. odgi `.og` não tem FM-Index. Argumento direto para
matriz §2.2.6 (capacidade SEARCH KMER): GGQL ✅, odgi ⊘ não suportado.

#### §5.7.4 Bytes por elemento estrutural (HPRC)

Numerador = bytes do índice. Denominador conforme linha:

| Métrica | GGQL `.idx` | odgi `.og` |
|---|---:|---:|
| Por nó | 277 B | 188 B |
| Por edge | 200 B | 136 B |
| Por (nó + edge) | 116 B | 79 B |

odgi `.og` é 32% menor por elemento — mas custa **ausência de
FM-Index, snarls pré-computados, length index, gBWT e suporte a
W-lines** (ver §5.6). Trade-off direto: GGQL paga 88 B/elemento extra
para entregar queries instantâneas em todas as 6 capacidades C+T+M+D.

#### §5.7.5 Tabela final para o paper (§5.1 de `paper_comparisons.md`)

| Tool | Output HPRC | % de GFA | Funcionalidade entregue |
|---|---:|---:|---|
| **GGQL `.idx`** | 21,99 GiB | 48,1% | Indexação completa (FM, gBWT, snarls, length, paths) |
| **odgi `.og`** | 14,97 GiB | 32,8% | Topologia + paths (P-lines apenas) + sequências |
| **vg `.pg`** | ⊘ | — | Não medido (T1 OOM em 62 GiB) |

**Conclusão de §5.7**: GGQL paga ~47% mais bytes que odgi para entregar
funcionalidade pangenômica completa (W-lines + FM-Index + snarls). Em
disco de 2026 ($0.02/GiB/mês em S3 Standard), o overhead de ~7 GiB custa
$0.14/mês — irrelevante frente ao custo de re-rodar `odgi build` ou
buscar k-mers com scan linear a cada query.

### §5.8 Fase 5 — Robustez adversarial (A, 2026-05-14)

Dimensão **A** (robustez a inputs hostis ou degenerados). Doc completo:
`tests/comparisons/fase5_adversarial_results.md`.

| Cenário | GGQL | Veredito |
|---|---|---|
| A1 `SEARCH KMER "A"` em CHM13-464 | timeout 4 min, RSS 12,9 GiB | 🐛 **DoS REAL** |
| A2a `SEARCH KMER` k=1..3 | timeout > 30 s | 🐛 DoS confirmado |
| A2b `SEARCH KMER` k=4-8 | 8-12 s | ⚠️ Lento mas usável |
| A2c `SEARCH KMER` k≥10 | < 100 ms | ✅ Ótimo (sweet spot) |
| A3 `.idx` truncado (`fm_index.bin`) | mensagem clara, sem crash | ✅ Graceful (msg vaga) |
| A4 SEARCH KMER chars IUPAC (R/Y/M) | 0 hits silencioso | ⚠️ Sem warning |
| A5 Sintaxe maliciosa | `[parse error]` no stderr | ✅ |

**Achados críticos**:
1. **DoS real e reproduzível** (`SEARCH KMER "A"` em CHM13-464) — fix em
   `src/query/query_planner.cpp:619` (propagar `max_results` ao FM-Index;
   API já aceita: `fm_index.hpp:62`). Esforço ~10 LOC. Bloqueador
   pré-paper.
2. **Diagnóstico de `load()` silenciado** por `catch (...) {}` em
   `tests/validation/run_queries.cpp:120-122` e `(void)load()` em
   `src/ffi/ffi.cpp:288` + `:308`. Esforço ~10 LOC.
3. **Silenciamento de IUPAC** em SEARCH KMER — chars `R/Y/M/K/B/D/H/V/W/S`
   produzem 0 hits sem warning. UX gap. Fix `cli::note` ~5 LOC.

### §5.9 Fase 6 — Consolidação para o paper (2026-05-14)

Tabela mestra única consolidando Fases 1-5 (corretude C, tempo T, memória
M, disco D, scaling S, robustez A) por funcionalidade × ferramenta. Doc
completo: `tests/comparisons/fase6_paper_summary.md`.

**Score agregado por ferramenta**:

| Ferramenta | Funcionalidades suportadas | Comentário |
|---|---:|---|
| **GGQL** | **20/20 (100%)** | F09 23×, F10 1.442×, F12 15×, F13 raro 50×, F14 18× |
| awk | 12/20 (60%) | trivialmente robusto, sem index |
| Python | 14/20 (70%) | flexível, sem otimização específica |
| vg | 11/20 (55%) | F06 Cactus snarls em escala (se ingest factível) |
| odgi | 7/20 (35%) | ⊘ IDs alfanum; sem snarls; sem WHERE |
| gfatools | 4/20 (20%) | só estatísticas, sem queries |

**Issues priorizadas pré-paper**:

| ID | Descrição | Onde | Esforço |
|---|---|---|---:|
| **I-1 (DoS)** | `execute_search_kmer` ignora `max_results` | `src/query/query_planner.cpp:619` | ~10 LOC |
| I-2 | `catch(...){}` silencia falhas de `load()` | `run_queries.cpp:120-122`, `ffi.cpp:288` + `:308` | ~10 LOC |
| I-3 | IUPAC silenciado em SEARCH KMER | `query_planner.cpp` / `repl/` | ~5 LOC |
| I-4 | F13 k-mer comum: `vector→unordered_set` | `query_planner.cpp:621` | ~10 LOC |
| I-5 | "cache hit" falso positivo em GFA sem paths | `src/bench_gbwt.cpp:131` / `:137` | ~5 LOC |

**Decisão**: aplicar **I-1 (DoS)** + 1 teste de regressão antes de
submeter o paper. I-2/I-3/I-4/I-5 ficam como roadmap pós-paper.

---

## §6. Comandos para iniciar

```bash
cd /home/karlay/Documentos/c++/ggql_engine

# Dia 1 — manhã
echo "=== T8: análise de minigraph ===" > tests/validation/T8_decision.md
echo "Skip — minigraph é aligner, não suporta queries declarativas" \
    >> tests/validation/T8_decision.md

# Dia 1 — T5 memory R2
mkdir -p tests/validation/r5_memory
# (editar script + rodar — ~1h)

# Dia 1 — tarde T1
nohup /usr/bin/time -v data/validation/tools/vg convert \
    -g /home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa -p \
    > /home/karlay/Downloads/hprc-v1.0-mc-chm13.pg \
    2> tests/validation/T1_vg_convert.log &
```

---

## Histórico

- **2026-05-12**: documento criado, mapeando T1-T13 da §2.3 do
  `paper_comparisons.md` em cronograma executável + plano de mitigação
  + DoD. Estimativa global revisada para 3-5 dias (vs 3.5-4.5 anterior).
- **2026-05-12 (tarde)**: T9 ✅ — cold ingest HPRC GGQL medido com
  `/usr/bin/time -v`: 17,65 min wall, 50,71 GiB RSS pico, 21,99 GiB `.idx`.
  Tentativa anterior OOM-killed (`.idx` parcial removido antes do re-run).
  Resultados em §5.1.
- **2026-05-12 (tarde)**: T12 ✅ — gfatools stat HPRC: 18:58 min wall
  (single-thread), 47,54 GiB RSS pico. Counts batem com GGQL (Δ=0 segs+links).
  Insight: gfatools usa RAM equivalente a GGQL fazendo só counts.
  Resultados em §5.2.
- **2026-05-12 (noite)**: T1 ⊘ — `vg convert -p` em HPRC abortado por
  OOM após ~1h41min. `.pg` ficou em 0 bytes (vg ainda em pass 1, parou
  no chr15 dos warnings de duplicate path). T2/T10 ficam bloqueados
  (dependem do `.pg`). Plano redirecionado para T3 (odgi). Detalhes em
  §5.3. Regra de paridade de threads (8 = T9) adicionada em §0.
- **2026-05-12 (noite)**: T3.a ✅ — odgi v0.9.4 buildado em 2 min 50 s
  (gcc-14, -j8). Patch em `deps/sdsl-lite/CMakeLists.txt:151` para
  contornar regex bug com `++` no path. Binário em
  `data/validation/tools/odgi-v0.9.4/bin/odgi`. Detalhes em §5.4.
- **2026-05-12 (noite)**: T3.b ✅ — `odgi build` HPRC: 8 min 45 s wall,
  113% CPU (não escala em -t 8), 58,80 GiB RSS pico (encostou no limite),
  14,97 GiB `.og`. Comparado com GGQL T9: odgi 2× mais rápido em ingest
  básico mas usa mais RAM e entrega menos funcionalidade indexada.
  Detalhes em §5.5.
- **2026-05-12 (noite)**: T3.c ✅ — `odgi stats -S` (1:09, 25,2 GiB RSS,
  single-thread) + `odgi paths -L` (0:48). Counts: nodes/edges Δ=0 vs
  GGQL ✅, **paths 25 vs 28.784** (odgi só ingere P-lines, GGQL ingere
  W-lines). `odgi snarls` não existe em v0.9.4; `odgi tips` exige path
  de referência (semântica diferente do GGQL FIND TIP). Insight forte
  de amortização: GGQL >100× mais rápido em queries repetidas após o
  ingest pago uma vez. Detalhes em §5.6.
- **2026-05-12 (noite)**: T6 + T13 ✅ — tabela final de D consolidada em
  3 escalas (DRB1 / chm13_100w / HPRC). Ratios .idx/GFA: 135% → 216%
  → 48% — comprova que o GGQL é desenhado para escala pangenômica.
  Breakdown: 51,5% do `.idx` HPRC é FM-Index (componente que odgi não
  oferece). Bytes/elemento: GGQL 116 B vs odgi 79 B — overhead de 47%
  justificado pela funcionalidade extra. vg `.pg` permanece ⊘ (T1 OOM).
  Detalhes em §5.7.
- **2026-05-14**: §7.2 (padronização de unidades) executado. Todas as
  ocorrências de GB/MB/KB que se referem a tamanhos de arquivo ou RSS
  reescritas como GiB/MiB/KiB (binário). Correção principal: §5.1
  "45% de GFA 49 GB" → "48,1% de GFA 45,68 GiB" (era erro de divisão
  GiB/GB decimal). Convenção declarada em §0.3.
- **2026-05-14**: §5.8 (Fase 5 — Robustez adversarial) e §5.9 (Fase 6
  — Consolidação para o paper) adicionadas ao tracking. Fase 5 expõe
  7 cenários (A1-A5); achado crítico: DoS real via `SEARCH KMER "A"`
  em CHM13-464 (timeout 4 min, RSS 12,9 GiB). Fix I-1 marcado como
  bloqueador pré-paper. Fase 6 consolida 20 funcionalidades × 6
  ferramentas: GGQL 100% support, próximos 60-70% (awk/Python). Issues
  I-1..I-5 priorizadas com locations verificadas em §7.3. Docs
  completos em `tests/comparisons/fase5_adversarial_results.md` e
  `tests/comparisons/fase6_paper_summary.md`.
- **2026-05-14**: §7.5 (reordenar §5) executado — §5.3 (T1 vg OOM)
  movida de depois de §5.7 para entre §5.2 e §5.4. Ordem cronológica
  agora consistente com a sequência real de execução.
