# GGQL Engine — Análise Realista do Valor para a Biologia

**Data:** 2026-06-01
**Autor:** análise independente (revisão crítica externa)
**Base:** leitura de `README.md`, `docs/ECOSYSTEM.md`, `docs/ggql_vs_vg.md`,
`docs/ggql_vs_odgi.md`, `docs/ggql_vs_gfatools.md` + levantamento do estado da
arte (junho/2026).

> **Propósito deste documento.** Não é marketing nem repetição dos docs
> companion. É um julgamento crítico e sincero sobre (a) se existe ferramenta
> com a mesma proposta e (b) qual o valor real do GGQL para a biologia —
> incluindo onde as alegações de valor são infladas.

---

## 1. O que o GGQL é, de fato

Tirando o discurso de divulgação, o GGQL é uma **camada de consulta
declarativa, interativa e com índice persistente em disco, sobre grafos GFA já
construídos**. Ele **não** constrói pangenomas, **não** alinha reads, **não**
chama variantes. Ele responde — rápido e de forma componível — perguntas sobre
a *estrutura* de um grafo que já existe:

- topologia (tips, repeats, grau)
- bolhas/snarls (pré-computados e cacheados no `.idx`)
- metadados de segmento (`length`, `depth`, `read_count`)
- busca de k-mer (FM-Index lossless)
- paths/haplótipos (gBWT)

A engenharia é genuinamente boa: estruturas sucintas state-of-the-art, ingestão
paralela (mmap + OpenMP + MPHF), cache validado por hash XXH64, FFI maduro
(C/Dart) e — o ponto mais louvável — **validação cruzada rigorosa de
corretude**: `FIND BUBBLE` é um subconjunto *exato* dos snarls do `vg`, com a
diferença caracterizada semanticamente (`Ultrabubble=0` só-vg), não escondida
atrás de "tolerância". Isso é raro e honesto.

---

## 2. Existe ferramenta com a "mesma proposta"?

Esta é a pergunta mais importante, e a resposta honesta é: **quase não existe — e
isso é ao mesmo tempo a maior oportunidade e o maior sinal de alerta do
projeto.**

| Categoria | Ferramentas | Têm a mesma proposta? |
|---|---|---|
| **Toolkits de grafo GFA** | `vg`, `odgi`, `gfatools`, GraTools | ❌ Não. CLIs imperativas, sem linguagem declarativa nem índice de consulta persistente. São **complementares** — o próprio projeto admite corretamente em `ECOSYSTEM.md`. |
| **Indexação/consulta de anotações em pangenoma** | extensões vg+HTSlib para GAF (Llamas et al., 2024) | ⚠️ Parcial. Consulta *anotações/paths* por coordenada/BED, mas é procedural (`vg find`/`chunk` + tabix), sem linguagem. |
| **Grafos de conhecimento genômico** | Neo4j/Cypher, GenomicKB, GeneWeaver, Reactome | ⚠️ Outra camada. Cypher *é* uma linguagem declarativa de grafo madura, mas opera sobre *conhecimento* (gene↔variante↔via), **não** sobre a topologia de um GFA de 85M nós. Ninguém carrega um pangenoma HPRC no Neo4j — não escala. |
| **Visualização** | Bandage, GfaViz, Waragraph | ❌ Visuais, não analíticos em batch. |

**Conclusão do levantamento:** não há ferramenta popular que faça exatamente o
que o GGQL faz — uma DSL tipo-SQL com índice sucinto persistente sobre GFA em
escala HPRC. O posicionamento do projeto ("preenche uma lacuna") é
**factualmente correto**.

O reverso da medalha: **uma lacuna que ninguém preencheu em ~10 anos de
pangenômica pode ser uma lacuna genuína — ou sinal de que a demanda não
existe.** Bioinformatas de pangenoma já dominam `vg`/`odgi` no CLI e em
pipelines Snakemake/Nextflow. A premissa "biólogos querem SQL" precisa ser
**provada, não assumida**.

---

## 3. Onde as alegações de valor são infladas

Três pontos da narrativa interna merecem ceticismo:

### 3.1 "244×–900× mais rápido que o vg" é comparação maçã-com-laranja
O GGQL *lê snarls pré-computados do cache*; o `vg stats -R` *recomputa do zero*.
É uma vitória de engenharia real (amortização via `.idx`), mas compara cargas
de trabalho diferentes. O honesto é dizer: *"pague ~18 min de ingestão uma vez,
depois consultas estruturais custam milissegundos"* — e **não** "somos 900×
mais rápidos", o que sugere superioridade algorítmica que não é o caso.

### 3.2 A linguagem é muito mais limitada do que "SQL-like" sugere
São 6 entidades, `WHERE` com `AND`/`OR` e operadores de comparação simples.
**Não há** `JOIN`, `GROUP BY`, agregação (`COUNT`/`AVG`), subconsultas nem
ordenação. Muitas perguntas biológicas reais ("distribuição de comprimento de
bolhas por cromossomo", "quantos haplótipos passam por cada nó de uma região")
não se expressam — caem de volta no script. É uma **DSL restrita**, não um SQL
de grafos.

### 3.3 Há armadilhas (footguns) que prejudicam a confiança científica
O caso mais grave: `crosses()` fora de `FIND PATH` **retorna 0 linhas
silenciosamente**, sem erro (stub intencional, documentado em `ECOSYSTEM.md
§3.6`). Numa ferramenta científica, um falso-negativo silencioso é **pior que
um crash** — pode gerar uma conclusão biológica errada sem deixar rastro.

---

## 4. Onde o valor é real

- **Exploração interativa em escala.** Para QC de montagem (tips, segmentos
  colapsados por `depth`) e triagem de variação estrutural via bolhas, ter
  respostas em milissegundos sobre um grafo HPRC sem reparsear 49 GB de GFA é
  genuinamente útil no dia a dia.
- **Snarls cacheados.** Recomputar snarls é caro; cachear e consultar é uma
  escolha de design acertada e o caso de uso mais defensável.
- **FFI / integração em apps.** O binding C/Dart validado com Flutter abre um
  nicho que `vg`/`odgi` não atendem: **ferramentas com GUI/app** que precisam
  consultar pangenomas embarcados. Pode ser o diferencial mais subestimado do
  projeto.

---

## 5. Veredito honesto

> O GGQL é uma peça de engenharia de software **acima da média** resolvendo um
> problema **real, porém estreito**, num nicho onde **de fato não há concorrente
> direto** — mas cujo valor científico hoje é **potencial, não comprovado**.

O que determina valor para a biologia não é a qualidade do código (que é alta),
e sim **adoção e descobertas habilitadas**. Nesse eixo, os obstáculos são
grandes e estão **fora do código**:

- **Sem publicação peer-reviewed** → biólogos não citam o que não está validado
  pela comunidade.
- **Linux x64 apenas** → exclui boa parte dos usuários de bancada (Mac).
- **Compete com workflows entrincheirados** (`vg`/`odgi` em Nextflow/Galaxy) — a
  barreira não é técnica, é inércia de ecossistema.
- **A DSL precisa crescer** (agregação, ordenação) para responder perguntas que
  hoje voltam pro script Python.

### Recomendações

1. **Não tentar "vencer o `vg`" em benchmarks** — são complementares, e o
   projeto já sabe disso. Reposicionar os números como amortização, não como
   superioridade bruta.
2. **Dobrar a aposta no nicho sem concorrente:** consulta interativa + FFI
   embarcado em apps (Flutter/Python).
3. **Validar com 2–3 biólogos reais** se a DSL responde perguntas que eles de
   fato fazem — ou se é uma solução elegante à procura de um problema.
4. **Eliminar footguns silenciosos** (`crosses()` deve erro, não 0 linhas).
5. **Publicar um Application Note** (ex.: Bioinformatics) com um caso de uso
   biológico concreto — isso faria mais pela adoção do que qualquer otimização
   de allocator.

---

## 6. Referências

- Llamas B. et al. *Efficient indexing and querying of annotations in a
  pangenome graph.* bioRxiv/PMC, 2024.
  <https://pmc.ncbi.nlm.nih.gov/articles/PMC11507721/>
- *GraTools — user-friendly tool for exploring pangenome variation graphs.*
  bioRxiv, 2025.
- Feng Z. et al. *GenomicKB: a knowledge graph for the human genome.* Nucleic
  Acids Research, 2023. <https://academic.oup.com/nar/article/51/D1/D950/6786196>
- *Cypher — Neo4j declarative graph query language.*
  <https://neo4j.com/docs/getting-started/cypher/>
- Pangenome graphs project (vg/odgi). <https://pangenome.github.io/>
- Docs internos companion: `docs/ECOSYSTEM.md`, `docs/ggql_vs_vg.md`,
  `docs/ggql_vs_odgi.md`, `docs/ggql_vs_gfatools.md`.
