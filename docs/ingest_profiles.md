# Perfis de Ingestão — GGQL Engine

Cada perfil mapeia um caso de uso biológico a um subconjunto das estruturas de
dados do motor. A escolha correta reduz RAM, tempo de ingestão e tamanho do
índice em disco sem sacrificar as queries relevantes para aquele domínio.

---

## Estruturas de dados disponíveis

| ID | Estrutura | Flag | RAM típica (HPRC 85 M nós) |
|----|-----------|------|---------------------------|
| C1 | **MPHF** — mapeamento nome→ID de segmento | sempre | ~200 MB |
| C2 | **MetadataStore** — lengths, nomes | `save_metadata` | ~350 MB |
| C3 | **FM-Index** — BWT + tabela Occ + SA sampling | `build_fm_index` | ~1 GB |
| C4 | **CSR Topology** — grafo fwd+bwd (Elias-Fano) | `build_topology` | ~1 GB |
| C5 | **Snarls** — algoritmo Billi (SESE bidirected) | `build_snarls` (requer C4) | dominado por biedged graph (2× num_nodes adj.); 24 B por snarl em disco |
| C6 | **Path index** — paths/haplótipos por shard (PathStorage **ou** Gbwt) | `build_gbwt` (requer C4) + `path_backend` | depende do backend (ver §"Backend de paths") |
| C7 | **Path stats** — node_count + bp_length por path | `build_gbwt` | ~12 B × n_paths |

> Os flags `build_gbwt` e `build_snarls` são independentes e validados
> contra o manifest do cache (downgrade automático apaga arquivos órfãos —
> ver §"Downgrade automático").

---

## Queries habilitadas por estrutura

| Query GGQL | Estrutura necessária |
|------------|---------------------|
| `FIND SEGMENT WHERE length …` | C1 + C2 |
| `SEARCH KMER "ACGT…"` | C3 |
| `FIND SEGMENT WHERE contains_kmer(…)` | C3 |
| `FIND EDGE` | C1 + C4 |
| `FIND TIP` | C4 |
| `FIND REPEAT` | C4 |
| `FIND BUBBLE` | C4 + C5 |
| `FIND PATH WHERE …` | C4 + C7 (path_stats; fast path) **ou** C4 + C6 (fallback) |
| `FIND PATH WHERE crosses(…)` | stub atualmente (TODO Phase 2; precisará de C6=Gbwt para latência viável) |

> Caminho rápido de `FIND PATH WHERE length / node_count / name`: lê apenas
> `path_stats.bin` (poucos KB), não toca em C6. C6 só é exercitado em fallback
> (cache sem path_stats) e — futuramente — em queries posicionais como `crosses()`.

---

## Backend de paths (C6) — PathStorage vs Gbwt

A camada C6 tem dois backends intercambiáveis selecionáveis por flag. Ambos
implementam a mesma interface `IPathBackend` (`extract_into`, `num_paths`,
`save`/`load`, etc.), portanto a escolha é transparente para os callers.

| | **PathStorage** (default) | **Gbwt** |
|---|---|---|
| Estrutura | `vector<uint32_t>` concatenado, offsets por path | gBWT (BWT generalizado run-length comprimido) |
| Lib | implementação interna (`include/ggql/gbwt/path_storage.hpp`) | `third_party/gbwt` (jltsiren/gbwt @ MIT) |
| Disco/RAM | **4 B/nó** (uint32 cru) | **~1–2 bits/nó** em pangenoma humano |
| `extract_into` | O(L) memcpy direto | O(L) com LF-stepping (decode + filter ENDMARKER) |
| `nodes_view` zero-copy | sim (`span<const uint32_t>`) | não (precisaria descomprimir para buffer) |
| Build | paralelo (1 PathStorageBuilder por shard via OMP) | serial inicial (1 GBWTBuilder por shard sequencial) — paralelo é refinamento futuro |
| Magic em disco | `GGQLPS\0\0` | `GGQLGB\0\0` |
| Suporta queries posicionais (locate/find) | não | sim (via `gbwt::GBWT::locate` etc., expostas por `GbwtBackend::raw()`) |

**Regra geral:**

- **PathStorage** quando o uso real é só `extract_into` em loop (build_path_stats,
  fallback de `FIND PATH`). Mais rápido para extração, mais simples, sem dependência
  externa em hot path.
- **Gbwt** quando RAM/disco é gargalo (HPRC com muitos haplótipos similares: 16,64
  GiB → ~1 GiB), ou quando o roadmap inclui `crosses()` com latência viável.

**Auto-detect na carga:** o reader de `path_stores.bin` lê 8 bytes de magic por
shard e despacha para `PathStorage::load` ou `GbwtBackend::load` automaticamente.
Mistura de backends no mesmo arquivo é suportada.

**Comportamento default preservado:** PathStorage é o backend padrão em todos
os perfis. A coexistência foi adicionada sem mudar comportamento existente —
cache antigos continuam carregando normalmente.

---

## Perfis

### 1. Metagenômica

**Caso de uso:** GFAs produzidos por montadores metagenômicos (MetaFlye,
MEGAHIT, metaSPAdes). O grafo representa o espaço de sequências de uma
comunidade microbiana — tipicamente sem paths explícitos, com milhares de
componentes desconexos e alto grau de repetição.

**Estruturas necessárias:** C1 · C2 · C3 · C4

**Por que cada uma:**

- **C1 (MPHF):** lookup de segmento por nome é a operação base de qualquer
  query; custo fixo baixo.
- **C2 (MetadataStore):** comprimentos de contigs são essenciais para filtrar
  `FIND SEGMENT WHERE length > X`, a query mais comum para QC de montagem
  metagenômica.
- **C3 (FM-Index):** busca de k-mers é o caso de uso central em metagenômica —
  classificação taxonômica, detecção de genes de resistência, mapeamento de
  reads. `SEARCH KMER` e `contains_kmer()` dependem exclusivamente desta
  estrutura.
- **C4 (CSR Topology):** queries de topologia (`FIND TIP`, `FIND REPEAT`,
  `FIND EDGE`) são usadas para QC do grafo de montagem (identificar dead-ends,
  nós de alto grau que indicam repetições).

**Por que NÃO:**

- **C5 (Snarls):** snarls pressupõem um grafo de variação com haplótipos de
  referência. Em metagenômica o grafo é um espaço de sobreposição — a semântica
  de "bolha" (panbubble/hairpin) não se aplica da mesma forma. Custo de
  build proporcional ao biedged graph (2× num_nodes), sem ganho prático
  em grafos densos sem haplótipos.
- **C6 (gBWT):** GFAs metagenômicos raramente carregam paths. Mesmo quando
  carregam (contigs/unitigs), o índice de haplótipos não acrescenta poder de
  query relevante. O custo domina RAM e tempo de ingestão.
- **C7 (Path stats):** consequência de C6.

---

### 2. Pangenoma

**Caso de uso:** Grafos de pangenoma com haplótipos completos, como os gerados
pelo pipeline minigraph-cactus ou pggb para o HPRC (Human Pangenome Reference
Consortium). Tipicamente 1 grafo principal com dezenas a milhares de haplótipos
representando variação populacional.

**Estruturas necessárias:** C1 · C2 · C3 · C4 · C5 · C6 · C7

**Por que cada uma:**

- **C1–C4:** mesmas razões dos outros perfis; base irredutível.
- **C3 (FM-Index):** busca de sequências em contexto de variação (ex: encontrar
  em quais haplótipos um k-mer de SNP aparece).
- **C5 (Snarls):** pangenomas têm estrutura de snarl bem definida (bolhas de
  SNPs, indels, SVs). `FIND BUBBLE` é a query principal para análise de
  variação estrutural — paridade exata com o binário Billi (algoritmo SESE
  + hairpin detection) e custo de build linear no número de sides do
  biedged graph.
- **C6 (gBWT):** índice de haplótipos é o coração do pangenoma. `FIND PATH
  WHERE crosses(região)`, estatísticas por haplótipo e contagem de frequência
  alélica requerem o gBWT.
- **C7 (Path stats):** node_count e bp_length pré-computados evitam
  re-traversal do gBWT nas queries de `FIND PATH WHERE length > X`
  (fix `path_stats` — 24 min → 18 s no HPRC).

---

### 3. Assembly graph (montagem de genoma único)

**Caso de uso:** Grafo de montagem de uma única amostra produzido por
hifiasm, verkko ou Flye. Paths representam contigs ou unitigs. Foco em QC
da montagem: identificar bolhas não resolvidas, dead-ends, repetições e
estimar completude.

**Estruturas necessárias:** C1 · C2 · C4 · C5

**Por que cada uma:**

- **C1 (MPHF):** base para qualquer lookup.
- **C2 (MetadataStore):** comprimentos de contigs e nomes são indispensáveis
  para N50, L50 e filtros de tamanho.
- **C4 (CSR Topology):** `FIND TIP` detecta dead-ends (indicadores de
  fragmentação), `FIND REPEAT` identifica nós de alto grau (pontos onde a
  montagem colapsa repetições).
- **C5 (Snarls):** `FIND BUBBLE` é a query principal para detectar haplótipos
  não resolvidos ou erros de montagem. O port nativo do algoritmo Billi
  (`find_all_snarls` em `src/storage/snarl_decomposition.cpp`)
  é eficiente em grafos de montagem (panbubbles + hairpins) e tem paridade
  21/21 nos test files oficiais do upstream.

**Por que NÃO:**

- **C3 (FM-Index):** QC de montagem raramente requer busca de k-mers no índice.
  Pode ser ativado opcionalmente se o objetivo for anotação de sequências.
- **C6 (gBWT):** uma única amostra tem poucos paths (contigs/unitigs). O gBWT
  seria trivial mas o overhead de build e RAM não se justifica; `FIND PATH`
  sobre contigs pode ser servido pelo MetadataStore diretamente.
- **C7 (Path stats):** consequência de C6.

---

### 4. Genômica comparativa (multi-espécies)

**Caso de uso:** Alinhamentos de múltiplos genomas de espécies ou strains
diferentes (ex: grafos gerados por minigraph com genomas bacterianos,
fungos ou vertebrados). Paths representam espécies/strains. Sigma é menor
que no pangenoma humano; foco em identificar regiões conservadas e
divergentes entre espécies.

**Estruturas necessárias:** C1 · C2 · C4 · C6 · C7

**Por que cada uma:**

- **C1–C2:** base irredutível.
- **C4 (CSR Topology):** navegação no grafo comparativo; `FIND EDGE` e
  `FIND TIP` para identificar regiões de presença/ausência entre espécies.
- **C6 (gBWT):** paths = espécies/strains; `crosses(espécie)` e queries de
  frequência entre grupos taxonômicos são os casos de uso centrais. Sigma é
  controlado (genoma de uma espécie), tornando o custo de build razoável.
- **C7 (Path stats):** bp_length por path (= por espécie) é essencial para
  normalizar queries comparativas.

**Por que NÃO:**

- **C3 (FM-Index):** buscas de k-mers em contexto comparativo são geralmente
  feitas offline (BLAST, minimap2). O FM-Index do GGQL é útil para queries
  interativas pontuais — pode ser ativado opcionalmente.
- **C5 (Snarls):** bolhas em grafos multi-espécie representam regiões de
  divergência, mas a análise tipicamente é feita por ferramentas dedicadas
  (HAL, Cactus). Custo elevado para benefício marginal neste perfil.

---

## Resumo de flags

| Flag | Default | Descrição |
|------|:-------:|-----------|
| `save_metadata` | `true` | Grava MetadataStore (C2) |
| `build_fm_index` | `false` | Constrói FM-Index (C3) |
| `build_topology` | `false` | Constrói CSR + Elias-Fano (C4) |
| `build_snarls` | `true` | Detecta snarls via algoritmo Billi/SESE (C5) — requer C4 |
| `build_gbwt` | `true` | Constrói índice de paths C6+C7 — requer C4 |
| `path_backend` | `PathStorage` | Backend de C6: `PathStorage` (default), `Gbwt`, `Auto` (= PathStorage hoje) |

> `build_gbwt` e `build_snarls` requerem `build_topology=true`. Quando
> `build_gbwt=true` mas o GFA não contém W/P-lines, nada é gravado (o
> `path_stores.bin` é omitido sem erro). `build_snarls=true` em GFAs sem
> componentes 2-edge-connected significativos resulta em `snarls.bin`
> com 0 registros.

> `path_backend` é ortogonal a `build_gbwt`: o primeiro escolhe **como** os
> paths são armazenados (PathStorage cru ou Gbwt comprimido); o segundo
> apenas decide **se** a Camada 2 é construída.

| Perfil | `save_metadata` | `build_fm_index` | `build_topology` | `build_snarls` | `build_gbwt` | `path_backend` |
|--------|:-:|:-:|:-:|:-:|:-:|:-:|
| Metagenômica | ✓ | ✓ | ✓ | — | — | n/a |
| Pangenoma | ✓ | ✓ | ✓ | ✓ | ✓ | PathStorage |
| Assembly graph | ✓ | opt | ✓ | ✓ | — | n/a |
| Comparativa | ✓ | opt | ✓ | — | ✓ | PathStorage |

> O default `PathStorage` em todos os perfis preserva o comportamento
> atual. Para deploys com pressão de RAM (HPRC v1 com 4,47 G símbolos
> ocupa 16,64 GiB no PathStorage vs ~1 GiB no Gbwt), troque para
> `Gbwt` via flag — sem alterar o perfil.

### CLI

`bench_gbwt` aceita `--path-backend|-b <pathstorage|gbwt|auto>`:

```bash
./build/bench_gbwt arquivo.gfa 8 --profile pangenome --path-backend gbwt
./build/bench_gbwt arquivo.gfa 8 --profile pangenome --path-backend pathstorage  # default
```

API C++:

```cpp
ingestor.ingest(filepath, threads, gamma, verbose,
                save_metadata, build_topology, build_fm_index,
                max_paths, keep_gbwt_tmp, cache_dir,
                build_gbwt, build_snarls,
                ggql::PathBackendKind::Gbwt);  // <-- backend
```

---

## Upgrade incremental de cache

Subir um perfil entre execuções (ex: começou com `metagenomics` e agora quer
`pangenome`) é tratado de forma diferenciada conforme **o que falta no cache**:

| Falta | Estratégia | Custo HPRC |
|-------|-----------|:----------:|
| Apenas `snarls.bin` | **Fase 1**: deriva da CSR já carregada via `find_all_snarls`, sem reler o GFA. Grava só `snarls.bin`. | ~5 s |
| `gbwts.bin` (com ou sem snarls) | **Fase 2**: Pass2 paths-only sobre o GFA — pula MPHF/CSR/metadata/FM-Index, só coleta W/P-lines e constrói gBWT (build serial — ver §"Por que a Fase 2 força build serial do gBWT" abaixo). Se snarls também faltam, encadeia Fase 1 depois. | ~30 s |

**Ganho estimado vs reingestão completa (~165 s no HPRC): 5–33×.**

**Casos cobertos pela Fase 1 (só snarls):**
- `comparative` → `pangenome` (cache tem gBWT, falta apenas snarls)
- `metagenomics` → `assembly` (falta apenas snarls; gBWT desabilitado em ambos)

**Casos cobertos pela Fase 2 (gBWT, com ou sem snarls):**
- `metagenomics` → `comparative` (falta gBWT)
- `metagenomics` → `pangenome` (falta gBWT + snarls; Fase 2 + Fase 1)
- `assembly` → `pangenome` (falta gBWT)
- `assembly` → `comparative` (falta gBWT, downgrade snarls é noop em disco)

A detecção é automática em `try_cache_fast` / `try_cache_hash` via
verificação de `gbwts.bin` / `snarls.bin` no `.idx/`. Implementação:
`complete_with_snarls()` e `complete_with_gbwt()` em `ingestor.cpp`;
`save_snarls_only()` e `save_gbwt_only()` em `topology` para gravar
incrementalmente sem reescrever CSR/EF/metadata.

### Por que a Fase 2 força build serial do gBWT

`build_path_index_parallel` em modo paralelo executa N `GBWTBuilder`
locais por thread e faz um merge final via `gbwt::GBWT(partial_indexes)`.
O merge requer a invariante **"dois GBWTs parciais não podem compartilhar
nenhum nó"** — se for violada o libgbwt aborta com `GBWT::GBWT(): Sources
X and Y both have node Z`.

No fluxo completo (Pass1+Pass2), essa invariante é garantida por
`repartition_groups_topological(groups, node_component, ...)`, que usa o
mapa `node_component[]` (nó → ID de componente conexo) produzido pelo
union-find do Pass1 para redistribuir paths de modo que cada grupo toque
apenas um componente.

Na Fase 2, Pass1 já rodou em uma execução anterior — `node_component`
não está em RAM e **não é persistido no cache**. Reconstruí-lo
percorreria o CSR de novo (O(V+E)), anulando o ganho de pular Pass1.
Sem o repartition, paths que atravessam múltiplos componentes (típicos
de walks de metaFlye, hifiasm-meta) fazem dois jobs paralelos inserir o
mesmo nó → merge falha.

**Solução:** forçar build serial no Pass2 paths-only. Um único
`GBWTBuilder` processa todos os grupos sequencialmente e produz 1 GBWT
final — sem merge, a invariante simplesmente não se aplica.

**Por que o ganho do paralelismo é marginal aqui:** a Fase 2 só roda
quando Pass1+CSR+metadata+FM-Index já estão em cache. O custo total dela
é dominado por Pass2 paths-only (re-parse das W/P-lines, ~10–20 s no
HPRC) e I/O de gravação. O gBWT build em si é da ordem de 10–60 s no
HPRC; mesmo com speedup paralelo limitado por Amdahl (componente maior),
a diferença absoluta é poucos segundos sobre uma operação que já é
"incremental" por construção. Trade-off: poucos segundos de paralelismo
em troca de correção garantida em todos os GFAs (incluindo aqueles com
paths atravessando componentes).

## Downgrade automático de cache

Quando o usuário ingere com um perfil mais restrito que o cache existente
(ex: `pangenome` → `metagenomics` no mesmo `.idx/`), o motor agora detecta
o downgrade e **apaga os arquivos órfãos** automaticamente:

| Flag rebaixado | Arquivos removidos |
|----------------|--------------------|
| `build_gbwt` true→false | `gbwts.bin`, `path_stats.bin` |
| `build_snarls` true→false | `snarls.bin` |
| `build_fm_index` true→false | `fm_index.bin` |
| `save_metadata` true→false | `metadata.bin`, `metadata_seqs.bin`, `metadata_strings.bin`, `length_index.bin` |

Sem esse trim, o `topology->load()` carregaria todas as estruturas em RAM
mesmo quando o usuário pediu um perfil que não as usa — anulando a economia
de RAM esperada.

A detecção é feita comparando os flags do manifest persistido (campo
`flag_*=0|1`) com os flags solicitados na chamada atual. Se houver
divergência onde o cache tinha `true` e a chamada pede `false`, os arquivos
correspondentes são removidos e o manifest é regravado com os novos flags.

### Manifest v4

O `manifest.txt` foi estendido com os campos:

```
version=4
profile=pangenome              # nome canônico (ou "custom")
indexed_at=2026-04-30T14:23:43Z # ISO-8601 UTC
flag_save_metadata=1
flag_build_fm_index=1
flag_build_topology=1
flag_build_gbwt=1
flag_build_snarls=1
```

Manifests v3 ou anteriores são tratados como "flags desconhecidos" — não
disparam downgrade (assumimos comportamento conservador).

A API pública `GfaIngestorMmapParallel::read_cached_flags(dir)` retorna
um `optional<CachedFlags>` para inspeção (auditoria/UI).

## Pendências

- **Pass2 paths-only ainda faz scan completo:** o `process_data_parallel`
  varre todo o arquivo procurando W/P-lines mesmo quando `collect_records`
  e `collect_links` são false. Otimização possível com índice de offsets de
  linhas W/P (gravado durante a primeira ingestão), mas o ganho prático é
  limitado — o scan sequencial do mmap é dominado por bandwidth e não por
  CPU.
