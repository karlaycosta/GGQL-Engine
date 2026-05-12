# T8 — Decisão: exclusão de `minigraph` da matriz comparativa

> **Tarefa**: T8 do plano `docs/plano_matriz_comparativa.md`
> **Status**: ✅ DECIDIDO em 2026-05-12
> **Decisão**: **Excluir minigraph da matriz §2.1 do `paper_comparisons.md`**.
> **Esforço economizado**: ~30 min de planejamento + 0 dias de execução.

## Contexto

A versão inicial da matriz (`paper_comparisons.md` §2.1) listava 8 ferramentas
para comparação, incluindo `minigraph` (lh3). Durante a revisão da relevância
de cada tool, identificamos que minigraph não é um **query engine** — é um
**graph-based aligner**.

## Análise técnica

### O que minigraph faz

- Pipeline principal: **alinhar** sequências contra um pangenome graph
  (case típico: `minigraph -cx asm graph.gfa contigs.fa > aln.gaf`).
- Outputs: alignments em formato GAF (Graph Alignment Format), não counts ou
  consultas estruturais.
- Indexação interna: sketches minimizer-based para seed alignment, **não
  expostos** como API de consulta.

### O que minigraph **não** oferece

| Funcionalidade GGQL | minigraph |
|---|:-:|
| `FIND SEGMENT count` | ❌ (não expõe count direto) |
| `FIND EDGE count` | ❌ |
| `FIND TIP / REPEAT` | ❌ |
| `FIND BUBBLE / snarls` | ❌ (não computa snarls como output) |
| `FIND PATH` | ❌ (não enumera paths como query) |
| `WHERE` clauses | ❌ |
| `SEARCH KMER` exato | parcial (sketch-based, não exato) |
| Pagination / LIMIT | ❌ |

### Por que minigraph aparecia na matriz inicial

Foi listado por inércia ("ferramentas do ecossistema pangenome") sem
verificar se as **funcionalidades comparáveis** existiam. Reanálise mostrou
sobreposição zero com o caso de uso de GGQL.

## Decisão

**Excluir minigraph da matriz §2.1** do `paper_comparisons.md`. Justificativa:

1. **Paradigma incompatível**: minigraph faz alignment, GGQL faz queries
   estruturais. Casos de uso disjuntos.
2. **Nada para comparar**: nenhuma operação do minigraph corresponde 1:1 a
   uma query GGQL. Tabela ficaria com 100% `❌` ou `—`.
3. **Honestidade científica**: incluir minigraph apenas para parecer "mais
   abrangente" seria padding artificial.

## Posicionamento no paper

Mencionar minigraph em **§7.2 do `paper_comparisons.md`** (já feito) e em
**§1.2 Related work** do `paper_draft.md` como "ferramenta de alignment,
não comparável diretamente".

## Lição

Antes de listar tools na matriz, verificar se há ao menos **5 funcionalidades
overlap** (corretude direta). Tools sem overlap real devem ir para "Related
work" ou ser omitidas.

Critério a aplicar futuramente: se a coluna de uma tool teria >70% de `❌`
em queries comparáveis, **não está na mesma classe** — exclua.

## Output

- ✅ matriz §2.1 reduzida para 6 ferramentas reais (GGQL, vg, gfatools, awk,
  odgi, Python) — feito no commit anterior
- ✅ §7.2 documenta a exclusão com justificativa
- ✅ paper_draft.md §1.2 menciona minigraph em "tools diferentes"
- ✅ T8 fechado neste documento

T8 ✅ CONCLUÍDO.
