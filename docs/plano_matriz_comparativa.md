# Plano de execução — fechar matriz comparativa (§2.2-§2.5 do paper)

> **Documento companion**: `docs/paper_comparisons.md` (§2.2-§2.5)
> **Objetivo**: preencher todas as células `⬜` da matriz comparativa nas 4
> dimensões (C, T, M, D) × 8 ferramentas. Atualmente: ~30/~150 células ✅.
> **Esforço total estimado**: ~3.5-4.5 dias de trabalho líquido (sem
> deadtime de downloads / setups).
> **Última atualização**: 2026-05-12.

---

## §0. Pré-requisitos globais

Antes de iniciar T1-T13, garantir:

### §0.1 Binários
- [x] **GGQL** `build/{bench_gbwt,tests/run_queries}` (release O3+LTO)
- [x] **vg** v1.74.0 em `data/validation/tools/vg`
- [x] **gfatools** em `data/validation/tools/gfatools/gfatools`
- [x] **awk** gawk 5.2.2 (sistema)
- [x] **hyperfine** 1.18 (sistema)
- [x] **/usr/bin/time -v** (sistema, GNU time)
- [ ] **odgi** — pré-requisito T3 (build a partir do source)
- [ ] **TileDB-VG** — pré-requisito T4 (Python + tiledb)

### §0.2 Datasets
- [x] `data/chm13_subset_100w.gfa` + `.idx` (4.3M segs, 100 walks)
- [x] `data/validation/tier_b/DRB1-3123.gfa` + `.idx` (5,581 segs)
- [x] `/home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa` + `.idx` (85M segs)
- [x] `data/validation/tier_d/{MT_primate,ecoli_synth}.gfa` + `.idx`

### §0.3 Hardware
- CPU: 12-core Intel (taskset 0-7 isolado)
- RAM: 62 GB (suficiente para HPRC full)
- Disco: ~104 GB livre (HPRC .idx ocupa 23 GB; vg .pg ~50 GB esperado)

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
| 14:00-18:00 | **T1** `vg convert -p` em HPRC full (~2-4h) | T+M+D para vg ingest |

**Saída do dia**: 4 linhas inteiras de M preenchidas, vg ingest comparado.

### Dia 2 — vg em escala HPRC (~8h)

| Slot | Tarefa | Output |
|---|---|---|
| 09:00-17:00 | **T2** `vg snarls` em HPRC (espera 2-8h) | T+M+D para vg snarls em HPRC |
| 17:00-18:00 | **T10** `vg path -p` vs GGQL FIND PATH | C+T para path extraction |

**Saída do dia**: comparação flagship `vg snarls` na escala real (HPRC),
não mais só extrapolação.

### Dia 3 — odgi (~6-8h)

| Slot | Tarefa | Output |
|---|---|---|
| 09:00-11:00 | **T3.a** Build odgi do source | Binário `odgi` |
| 11:00-13:00 | **T3.b** `odgi build` em HPRC → `.og` | T+M+D ingest odgi |
| 14:00-16:00 | **T3.c** `odgi stats`, `odgi paths` vs GGQL counts | C+T odgi queries |
| 16:00-17:00 | **T7** `vg find` SEARCH KMER vs GGQL (curto+longo) | T para vg find |
| 17:00-18:00 | **T11** Python set inter como baseline crosses() | T baseline |

**Saída do dia**: odgi completamente integrado, vg find + Python set medidos.

### Dia 4 (opcional) — TileDB-VG (~1-2 dias)

| Slot | Tarefa | Output |
|---|---|---|
| Dia 4+ | **T4** Setup TileDB-VG + ingest + 3 queries | C+T+M+D TileDB-VG |
| | **T6** comparação consolidada D: GGQL vs vg .pg vs odgi .og | Tabela D final |
| | **T13** GFA→.idx vs GFA→.pg vs GFA→.og tamanho serializado | D detalhado |

**Saída do dia**: matriz 100% preenchida.

---

## §2. Plano detalhado por tarefa

### T1 — vg convert -p em HPRC full (T+M+D) — ⏱ 2-4h

**Objetivo**: medir tempo, RAM e disco do pipeline `vg convert` para gerar
`.pg` a partir do GFA de 49 GB. Preenche 3 células da matriz §2.2.4.

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
- M: `Maximum resident set size` em KB do `/usr/bin/time -v`
- D: `du -sh $PG`

**Risco**: vg convert pode crashar em GFA tão grande (49 GB). Mitigação:
tentar com `--threads N` se default lento; se OOM, dividir GFA por chromosomes.

**Onde gravar**: `tests/validation/r10_results/T1_vg_convert.log` + atualizar
linha `Cold ingest HPRC | vg convert -p` na §2.2.4.

### T2 — vg snarls em HPRC full (T+M+D) — ⏱ 2-8h

**Objetivo**: cross-check flagship — vg snarls em escala HPRC. Já estimamos
22min via extrapolação 77×; medir real.

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

```bash
# Pode requerer Python dev + libffi
sudo apt install -y python3-dev libffi-dev
git clone --recursive https://github.com/pangenome/odgi.git
cd odgi && cmake -H. -Bbuild -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
sudo cp bin/odgi /usr/local/bin/
odgi version
```

**T3.b — Ingest HPRC** (2h estimado):

```bash
GFA=/home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa
OG=/home/karlay/Downloads/hprc-v1.0-mc-chm13.og

/usr/bin/time -v odgi build -g "$GFA" -o "$OG" -t 12 2> tests/validation/T3_odgi_build.log
du -sh "$OG"
```

**T3.c — Queries vs GGQL** (1-2h):

```bash
# odgi stats: equivalente a FIND SEGMENT count
/usr/bin/time -v odgi stats -i "$OG" -S | grep "node_count"  # 85,282,047 esperado

# odgi paths: equivalente a FIND PATH
/usr/bin/time -v odgi paths -i "$OG" | wc -l  # 28,784 esperado

# odgi tips
odgi tips -i "$OG" -t 12 | wc -l  # 460,506 esperado

# odgi snarls (se suportado)
odgi snarls -i "$OG" -t 12 | wc -l
```

**Critério ✅**: count exato vs GGQL (Δ=0 esperado para SEGMENT/PATH/TIP).
Para snarls, documentar se diferença semântica.

**Output esperado**: 4-6 linhas de odgi na matriz §2.2 preenchidas.

### T4 — TileDB-VG (C+T+M+D) — ⏱ 1-2 dias (opcional)

**Setup mais complexo**. Requer:

```bash
pip install tiledbvg
# Documentação: https://github.com/TileDB-Inc/TileDB-VG
```

**Pipeline**:
1. Import GFA → TileDB array group
2. Query SEGMENT count
3. Query PATH count
4. Query SEARCH KMER (TileDB-VG suporta)

**Critério ✅**: 3 queries básicas funcionam + counts batem com GGQL.

**Recomendação**: pular T4 se cronograma apertado — TileDB-VG é tool
distribuída, comparação justa em laptop é difícil.

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

**Critério ✅**: mediana de 3 runs por query, RSS reportado em MB.

**Output**: tabela nova em §4.1 com coluna "RSS peak" ao lado de mediana.

### T6 — Disk consolidado (D) — ⏱ 1h (após T1, T3)

**Tabela final** §5.1 com 4 linhas para HPRC:

| Tool | Index size | % de GFA |
|---|---:|---:|
| GGQL .idx | 23 GB | 47% |
| vg .pg | (T1) | (T1/49) |
| odgi .og | (T3) | (T3/49) |
| TileDB-VG | (T4 opt) | — |

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

**Comando**:

```bash
rm -rf /home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa.idx
/usr/bin/time -v build/bench_gbwt /home/karlay/Downloads/hprc-v1.0-mc-chm13.gfa 12 \
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
| T1 vg convert HPRC | OOM ou crash em 49 GB | Tentar `--threads 4`; se falhar, dividir GFA por chrom |
| T2 vg snarls HPRC | Pode demorar 8h+ (vg single-threaded) | Rodar background, fazer T3 em paralelo |
| T3 odgi build | Dependências (jemalloc, htslib) | Usar Docker `pangenome/odgi:latest` |
| T4 TileDB-VG | Setup complexo + curva de aprendizado | Pular se cronograma apertado |
| T5 RSS | 3 runs em 13 queries = 40min extra | Tolerável |
| T7 vg find | GCSA index demora horas em HPRC | Limitar a chm13_100w |

### §3.2 Risco global

Cronograma realista: **3-5 dias úteis** (não 3.5-4.5h como na §2.4 do
paper_comparisons). A subestimativa anterior não contava setup de odgi e
TileDB-VG. Plano atual é realista.

### §3.3 Plano contingente

Se cronograma apertar:
- **Mínimo aceitável**: T1+T2+T3+T5+T6+T9 (~3 dias) — fecha vg+odgi em HPRC
- **Stretch**: T1-T13 (~5 dias) — fecha 100% da matriz
- **Pular**: T4 (TileDB-VG) — adiar para sessão pós-aceitação

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

## §5. Tracking — Status atual (2026-05-12)

### Concluídos
- ~30 células ✅ corretude (Sprints 1-12, R3, R4, R5, R10, R11)
- ~6 células ✅ tempo (R2 chm13_100w)
- 0 células ✅ memória (todas ⬜)
- 1 célula ✅ disco (GGQL .idx HPRC)

### Em progresso
- (nenhum item no momento — aguardando início do plano)

### Próximo a executar
Recomendação: começar pelo **T8** (decisão, 30min) + **T5** (memory R2, 1h).
São quick wins que destravam todo o resto do dia 1.

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
