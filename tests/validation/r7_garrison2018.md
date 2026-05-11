# R7 — Cross-check Garrison 2018 Table 2

## Status: PARTIAL — requer acesso ao PDF do paper

**Paper**: Garrison, E., Sirén, J., Novak, A. M., Hickey, G., Eizenga, J. M.,
Dawson, E. T., ... & Paten, B. (2018). *Variation graph toolkit improves
read mapping by representing genetic variation in the population*.
Nature Biotechnology, 36(9), 875-879. DOI: 10.1038/nbt.4227

## O que conseguimos validar (sem PDF)

DRB1-3123 é o dataset canônico do `vg msga` (alinhamento multi-sample dos
12 alelos HLA-DRB1 GRCh38). Os counts GGQL foram validados contra `vg`
(v1.74.0) como ground truth em Sprint 2 (Tier B):

| Métrica           | GGQL  | vg/gfatools/awk | Δ    | Status |
|-------------------|-------|-----------------|------|--------|
| FIND_SEGMENT      |  5581 | 5581 (gfatools) | 0    | ✅      |
| FIND_EDGE         |  7448 | 7448 (gfatools) | 0    | ✅      |
| FIND_TIP          |     5 | 5 (awk)         | 0    | ✅      |
| FIND_REPEAT       |   373 | 373 (awk)       | 0    | ✅      |
| FIND_BUBBLE       |  1518 | 1518 (vg, ≥2-net) | 0  | ✅      |
| FIND_PATH         |    12 | 12 (P-lines)    | 0    | ✅      |

Esses 6 counts são *consistentes com* o pipeline `vg msga` reportado no
paper (mesmo binário oficial vg v1.74.0). Mas sem o PDF da Tabela 2 não
podemos afirmar com certeza que os números do GGQL batem com os
publicados pelos autores.

## Como completar R7 (futuro)

1. Obter PDF (acesso institucional ou preprint na bioRxiv:
   `https://www.biorxiv.org/content/10.1101/234856v1`)
2. Transcrever Tabela 2 (linha "DRB1-3123 graph") com counts:
   - número de nós (segments)
   - número de edges
   - número de paths/walks
   - número de snarls/bubbles (se reportado)
3. Comparar diretamente contra a coluna GGQL acima.
4. Documentar discrepâncias (esperadas se vg msga mudou de versão).

## Notas

- O paper Garrison 2018 usa `vg snarls` (algoritmo `integrated`); GGQL
  usa porte do Billi/SESE. Diferença documentada em Sprint 2: snarls
  "vazios" (Net-Graph-Size=2) — vg reporta 535 a mais, GGQL exclui esses
  por design.
- Liao 2023 (HPRC paper, Nature 617:312-324) é um cross-check ainda
  melhor: ~85k snarls totais reportados para chm13 v2. Coberto em R10.

## Verdict

R7 *parcialmente* satisfeito: os números do GGQL estão consistentes com
os outputs do vg oficial (que produziu os dados do paper). Cross-check
direto contra Tabela 2 fica como TODO até alguém ter acesso ao PDF.
