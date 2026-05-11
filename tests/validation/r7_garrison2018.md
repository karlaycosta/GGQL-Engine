# R7 — Cross-check Garrison 2018: PREMISSA REVISADA

## Status: ENCERRADO — premissa original incorreta

**Paper auditado**: Garrison, E. et al. (2018). *Variation graph toolkit improves
read mapping by representing genetic variation in the population*. Nature
Biotechnology 36(9): 875-879. DOI: 10.1038/nbt.4227.
Preprint bioRxiv: https://www.biorxiv.org/content/10.1101/234856v1.

## Achado da auditoria

Após obter o PDF (v1 do preprint, 24 páginas, Dec 2017), procurei por:

- "DRB1-3123" → **não encontrado**
- "Table 2" / "Tabela 2" → **não encontrado**
- Counts numéricos de S/L/Path/Snarls por dataset → **não tabulados**

O paper foca em **performance de read mapping** (ROC curves, kmer indexing,
GCSA2 alignment), não em estatísticas estruturais de pangenome graphs.

Os únicos números quantitativos do paper:
  - 1000GP human variation graph: 4.76 GB serialized, 38 GB index memory
  - 7-yeast strain pangenome (Yue et al data) — sem counts S/L/Paths reportados
  - NA24385 mapping: 60s para carregar índices, 75 GB RAM

## Conclusão

A premissa original de R7 ("cross-check contra Tabela 2 do paper") era
**baseada em informação incorreta** da minha reanálise. DRB1-3123 vem do
`vg test corpus` (produzido por `vg msga` sobre FASTA HLA-DRB1.fa), mas
seus counts **não foram publicados** no paper Garrison 2018.

## O que efetivamente validamos

Em vez de "Tabela 2 do paper", nossos counts foram validados contra:

| Ground Truth | Origem | Status |
|---|---|---|
| `vg` v1.74.0 binário oficial | vgteam, release Mar 2025 | ✅ paper-grade |
| `gfatools` lh3 | Heng Li, mantido ativamente | ✅ paper-grade |
| `awk` sobre GFA raw | comportamento determinístico | ✅ ground truth canônico |
| Python set ops (paths) | implementação direta da definição | ✅ ground truth canônico |

`vg` é o binário produzido pelos próprios autores do paper, e foi usado para
gerar os experimentos publicados. Validar contra `vg` é **mais forte** que
validar contra counts publicados (que ficam congelados em tempo de submissão
e podem virar obsoletos com novas versões).

## Cross-checks de paper que **fazem sentido** (substitutos de R7)

Se o objetivo é "cross-check contra paper", os candidatos reais são:

1. **Liao 2023** (HPRC year-1, Nature 617:312-324) — ~85k snarls totais para
   chm13 v2 pangenome. Coberto por R10 (HPRC full, ainda pendente).

2. **Sirén et al. 2021** (Pangenomics-from-Vertebrates, J Comp Biol) — counts
   de GBWT em pangenomes humanos. Mas não tem DRB1.

3. **Garrison & Sirén 2024** (vg snarls integrated algorithm) — talvez tabela
   de snarls por pangenome. Não auditado.

## Decisão

R7 é encerrado como **inaplicável** (premissa incorreta). O cross-check
contra paper é redirecionado para **R10** (Liao 2023 HPRC), que é o paper
adequado para validar GGQL em escala pangenome humano completo.

## Histórico

- **2026-05-10** (criação inicial): marcado como parcial assumindo Tabela 2
  no paper.
- **2026-05-11** (auditoria): após obter PDF, descobri que o paper não tem
  essa tabela. R7 encerrado, substituído por R10 como fonte de cross-check
  com paper publicado.
