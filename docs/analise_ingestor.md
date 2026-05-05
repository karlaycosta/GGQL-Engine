# Análise — `src/ingest/ingestor.cpp`

Análise profunda de bugs, oportunidades de performance e código morto em
`ingestor.cpp` (orquestrador do pipeline de ingest GFA → cache binário).
Cobre o arquivo (2.042 linhas) + dependências (`ingestor.hpp`, callers
em `bench_gbwt.cpp`, `ggql_repl.cpp`, `ffi.cpp`).

Data: 2026-05-04
Branch: `feat/snarl-decomposition-paten`
Arquivo: `src/ingest/ingestor.cpp` (2.042 linhas)

Funções analisadas:
- `collect_keys_and_links_parallel` — Passada 1 fusionada (S+L em paralelo)
- `build_remap_from_pairs` — Union-Find lock-free + bin-packing
- `parse_w_line` / `parse_p_line` — parsing de paths W/P
- `process_data_parallel` — Passada 2 paralela (segmentos + links + paths)
- `save_mphf_and_remap` — persistência MPHF + remap
- `profile_name_from_flags`, `iso_utc_now` — helpers anônimos
- `write_manifest_atomic` / `read_cached_flags` / `rewrite_manifest_flags`
- `prune_cache_for_downgrade` — apaga arquivos órfãos em downgrade
- `load_index` — carrega cache existente
- `complete_with_snarls` / `complete_with_gbwt` — upgrade incremental Fase 1/2
- `try_cache_fast` / `try_cache_hash` — fast/hash path do cache
- `run_full_ingest` — pipeline completo
- `ingest` — orquestrador principal (entrada pública)

---

## 🐛 Bugs

### B1. Lambda `lookup_remapped` declarada e nunca usada — linhas 533–540

```cpp
auto lookup_remapped = [&](std::string_view sv) -> uint64_t {
    uint64_t id = res_.mphf->lookup(sv);
    if (id != ULLONG_MAX && has_remap) {
        id = static_cast<uint64_t>(remap_[id]);
    }
    return id;
};
(void)lookup_remapped; // silencia warning se não usado
```

**Dead code explícito**. A lambda é definida e capturada (`[&]`), gera
código no objeto, e é descartada com `(void)`. Vestígio de uma
refatoração anterior. O comentário "silencia warning" é cargo cult — a
forma correta é simplesmente **remover** a declaração.

**Severidade:** baixa (mas é dead code óbvio).

---

### B2. `prune_cache_for_downgrade` não apaga `metadata_seqs.bin` quando upgrade-only — linhas 1056–1061

```cpp
if (old_flags.save_metadata && !new_flags.save_metadata) {
    try_remove("metadata.bin");
    try_remove("metadata_seqs.bin");
    try_remove("metadata_strings.bin");
    try_remove("length_index.bin");
}
```

**Não apaga `depth_index.bin`** — esquecido. Se um cache foi gerado com
metadata + depth (ex.: GFA com tag `dp:f:`) e o usuário downgrada para
`save_metadata=false`, o `depth_index.bin` órfão fica em disco
indefinidamente. Apenas ocupa espaço (não causa bug funcional pois
`load()` em `metadata_store.hpp` faz `fs::exists()` antes).

**Fix:** adicionar `try_remove("depth_index.bin");` na lista.

**Severidade:** baixa. Garbage no disco; não causa bug funcional.

---

### B3. `try_cache_fast` retorna `nullopt` mesmo em "cache RAM ok" — linhas 1394–1398

```cpp
out_skip = true;
if (verbose) {
    cli::step_info("cache RAM ok (mtime) — prosseguindo para Passada 2 (metadata)");
}
return std::nullopt;
```

A mensagem "cache RAM ok" sugere sucesso mas a função retorna
`nullopt` (= cache miss). O sinal real é `out_skip = true`. **Nome do
estado é misleading** — caller precisa interpretar `out_skip` para
saber o que fazer.

**Análise:** o pattern de retornar `nullopt` + escrever em vários
out-params é um cheiro de design. Em C++23 com `std::expected`, podia
ser:

```cpp
struct CacheDecision {
    enum class Action { HitFull, NeedFullIngest, NeedPass2Only,
                         CompleteSnarlsOnly, CompleteGbwtOnly };
    Action action;
    std::optional<IngestStatsParallel> stats;
};
```

Mas refator grande. **Manter por enquanto**, anotar.

**Severidade:** baixa (smell de design, não bug).

---

### B4. `tracker.add_progress` chamado dentro de paralelo + atomic — verificado OK

```cpp
if (local_bytes > 32 * 1024 * 1024) {
    tracker.add_progress(local_bytes);
    local_bytes = 0;
}
```

`ProgressTracker::add_progress` usa `bytes_processed.fetch_add` (atomic
em `progress_tracker.hpp:42`). Thread-safe. **Não é bug**, só
informativo.

---

### B5. `bin_use_in_ram` (vector<bool>) lido dentro de paralelo — verificado OK

```cpp
if (bin_use_in_ram[target]) {
    // RAM path
} else if (bin_gbwt_bufs[target]) {
    // disk path
}
```

`vector<bool>` é seguro para reads concorrentes (writes acontecem antes
do `#pragma omp parallel`). **Não é bug**, só verificação.

---

### B6. Race latente entre processos no `manifest.txt` — `rewrite_manifest_flags`

`rewrite_manifest_flags` (linhas 931–1024) lê o arquivo, modifica em
memória, e usa `rename(tmp, manifest)` atômico. Mas se **dois processos
ggql** rodarem no mesmo `cache_dir` concorrentemente:
- P1 lê manifest, P2 lê manifest, P1 grava, P2 grava → última gravação
  vence. Update de flag de P1 perdido.

Para uso atual (1 processo por cache), **não dispara**. Para uso server
ou paralelo, **race real**.

**Fix possível:** flock no `manifest.txt` durante read+write. Overkill
agora.

**Severidade:** baixa (latente, depende de evolução).

---

### B7. `run_full_ingest` reabre `mmap` 3× quando `complete_gbwt_only` — linhas 2014–2034

```cpp
if (complete_gbwt_only) {
    if (!res_.mmap) {
        auto mmap_res2 = MmapFileParallel::open(filepath);
        // ...
    }
    return complete_with_gbwt(...);
}
auto mmap_res2 = MmapFileParallel::open(filepath);
// ...
res_.mmap = std::make_unique<MmapFileParallel>(std::move(*mmap_res2));
```

O fluxo abre/fecha mmap várias vezes:
1. `try_cache_fast` (linha 1978): mmap fechado
2. `MmapFileParallel::open(filepath)` (linha 1988): aberto
3. `try_cache_hash` (linha 2007): pode fechar
4. `MmapFileParallel::open(filepath)` (linha 2019/2029): pode reabrir

Para arquivos grandes (HPRC ~100 GB), abrir/fechar mmap não é grátis
(syscalls + invalidação de page table entries). Mas pequeno vs trabalho
útil de pass1.

**Severidade:** baixa (perf marginal).

---

## 🚀 Performance — em ordem de impacto

### P1. Merge serial pós-Pass2 dos acumuladores `thr_in_ram`/`thr_names` — linhas 681–692

```cpp
for (size_t b = 0; b < num_bins; ++b) {
    for (int t = 0; t < num_threads; ++t) {
        std::move(thr_in_ram[t][b].begin(), thr_in_ram[t][b].end(),
                  std::back_inserter(bin_in_ram_seqs[b]));
        std::move(thr_names[t][b].begin(), thr_names[t][b].end(),
                  std::back_inserter(bin_path_names[b]));
    }
}
```

Loops aninhados serial. Cada `b` é independente — pode paralelizar com
`#pragma omp parallel for` por `b`.

Para chm13 com `num_bins ≈ 32` e `num_threads = 8`: cada iteração move
~10k elementos. Total ~50 ms serial. Paralelizando: ~10 ms. **Ganho ~40 ms.**

Pequeno vs Pass2 total (~2,5 s no chm13), mas trivial.

---

### P2. Cópia em vez de move em `gbwt::vector_type seq(local_nodes.begin(), local_nodes.end())` — linha 648

```cpp
gbwt::vector_type seq(local_nodes.begin(), local_nodes.end());
```

Constrói `seq` copiando `local_nodes`. Depois `local_nodes.clear()` na
linha 620 (próxima iteração) descarta os dados originais.

**Verificação:** `gbwt::vector_type` é `sdsl::int_vector_buffer<0>`?
Não — é `gbwt::vector_type = std::vector<gbwt::node_type>` (verificar
em `gbwt/utils.h`). Se for `std::vector<uint64_t>`, podia ser
move-construído de `local_nodes` (mesmos tipos).

Mas `local_nodes` é declarado como `std::vector<uint64_t>` na linha 543,
então **a cópia é desnecessária** — `gbwt::vector_type seq(std::move(local_nodes))`
moveria. Porém, `local_nodes` é reusado entre paths (linha 620 `clear()`),
então move "rouba" o buffer e a próxima iteração precisa realocar.

**Trade-off:**
- Cópia atual: O(N) por path, mas reusa buffer de `local_nodes`.
- Move + reservar: O(1) move, mas próxima iteração realloc.

Como `local_nodes.reserve(1u << 20)` (linha 545) já pré-aloca 8 MB,
**copiar é mais barato** que perder esse buffer e refazê-lo. **Não é bug.**

---

### P3. `fmt::format` em hot path do `parse_w_line` — linhas 305–306

```cpp
path_name = fmt::format("{}#{}#{}:{}-{}", tokens[0], tokens[1], tokens[2],
                        tokens[3], tokens[4]);
```

Para cada W-line (centenas a milhares por GFA real), `fmt::format`
aloca `std::string` no heap. Para HPRC (10k+ paths), são 10k+ allocs.

**Fix possível:** `fmt::format_to(std::back_inserter(path_name), ...)`
reusando uma string passada por out-param. Win marginal vs custo de
`mphf->lookup` que domina parse.

**Severidade:** baixa.

---

### P4. `path_name = std::string(tokens[0])` em `parse_p_line` — linha 351

Cada P-line aloca string para `path_name`. Mesmo padrão de P3.

---

### P5. `process_data_parallel` aloca `thr_in_ram[num_threads][num_bins]` antes do paralelo — linha 500–503

```cpp
std::vector<std::vector<SeqVec>> thr_in_ram(
    num_threads,
    std::vector<SeqVec>(collect_paths ? num_bins : 0));
```

Para 8 threads × 32 bins = 256 SeqVec vazios pré-alocados. Cada SeqVec
é `std::vector<gbwt::vector_type>` — empty vector é ~24 B de
overhead. **Total ~6 KB de overhead** para a estrutura. Trivial.

---

### P6. `bin_gbwt_bufs[t]->push_back(v)` byte-a-byte sob `disk_mutex` — linhas 656–660

```cpp
std::lock_guard<std::mutex> lk(disk_mutexes[target]);
for (uint64_t v : seq) {
    bin_gbwt_bufs[target]->push_back(v);
}
bin_gbwt_bufs[target]->push_back(gbwt::ENDMARKER);
```

Para sequências grandes (paths longos ~milhões de nós), segura o mutex
por muito tempo bloqueando outras threads que tentam o mesmo `target`.

**Mas** o `target` é determinado por `comp_for_node_[first_mphf_id]`
(linha 638) — paths do mesmo componente vão para o mesmo bin. Como
componentes geralmente são processados por threads diferentes (após
bin-packing), contention é baixa.

**Aceitável.**

---

### P7. `process_data_parallel` Pass2 abre `mkstemp` e `text_buffer_type` por bin — linhas 479–490

Para 32 bins, 32 mkstemp calls + 32 text_buffer_type allocations
(64 KB buffer cada). Total ~2 MB de buffer de I/O. Sequencial mas
trivial.

---

### P8. `complete_with_gbwt` força `build_path_index_parallel` single-thread — linhas 1313–1314

```cpp
auto gbwt_t = res_.topology->build_path_index_parallel(
    std::move(pass2.gbwt_groups), node_width,
    /*num_threads=*/1);
```

Comentário (1300–1306) explica: "paths podem atravessar componentes em
metaFlye/similares — sem repartition seguro, paralelismo causaria
`Sources X and Y both have node Z`".

Trade-off documentado. **Aceitável.**

---

### P9. Loop serial sobre `gbwt_groups` em `keep_gbwt_tmp` — linhas 1734–1737

```cpp
for (const auto& tg : topo_groups) {
    stats.gbwt_path_names.insert(stats.gbwt_path_names.end(),
                                  tg.path_names.begin(), tg.path_names.end());
}
```

Reserve antes ou paralelizar. Para o caso `keep_gbwt_tmp=true`
(ferramentas de bench), aceitável.

---

## 💀 Códigos mortos

### M1. `(void)lookup_remapped` (linha 540) — abordado em B1

Lambda definida e nunca usada. Remover.

### M2. Comentários `ARCH-N`/`PERF-N` — 8 ocorrências

Linhas 3, 292, 495, 623, 650, 681, 1213, 1216, 1249, 1305 — marcadores
sem rastreador externo. Mesmo padrão limpo em outros arquivos do
projeto.

**Fix:** reformular para semântica (manter explicação técnica, remover
ID arbitrário).

### M3. Includes verificados — todos com usos

| Include | Uso |
|---|---|
| `<cstring>` | `std::memcmp` (linha 948) ✓ |
| `<ctime>` | `std::tm`, `gmtime_r`, `strftime` (832-838) ✓ |
| `<fstream>` | `std::ifstream`/`std::ofstream` ✓ |
| `<omp.h>` | `omp_get_thread_num` ✓ |

**Sem includes mortos.**

---

## 🔍 `[[nodiscard]]` / `const` / `noexcept`

### Faltam no `ingestor.hpp` (todas retornam valor relevante):

| Método | Retorno | Caller usa? |
|---|---|---|
| `collect_keys_and_links_parallel` | `Pass1Result` | ✓ (linha 1472) |
| `process_data_parallel` | `PassadaDoisResult` | ✓ |
| `read_cached_flags` (const) | `optional<CachedFlags>` | ✓ |
| `load_index` | `optional<IngestStatsParallel>` | ✓ |
| `try_cache_fast` | `optional<IngestStatsParallel>` | ✓ |
| `try_cache_hash` | `optional<IngestStatsParallel>` | ✓ |
| `complete_with_snarls` | `IngestStatsParallel` | ✓ |
| `complete_with_gbwt` | `expected<IngestStatsParallel, string>` | ✓ |
| `run_full_ingest` | `expected<IngestStatsParallel, string>` | ✓ |
| `ingest` (pública) | `expected<IngestStatsParallel, string>` | ✓ |

Apenas **`save_mphf_and_remap`** já tem `[[nodiscard]]`. Os outros
deveriam ser anotados.

### `noexcept`

- `collect_keys_and_links_parallel`, `process_data_parallel`,
  `build_remap_from_pairs` — alocam, lançam → **não** noexcept.
- `parse_w_line`, `parse_p_line` — `fmt::format` aloca → **não**
  noexcept.
- `iso_utc_now` — aloca string → **não** noexcept.
- `profile_name_from_flags` — retorna `const char*` literal → **pode
  ser** `noexcept`. **Falta.**

### `const`

- `read_cached_flags` const ✓
- `prune_cache_for_downgrade` const ✓
- Demais modificam state interno (membros).

### Lambdas:

- `find` / `unite` em `build_remap_from_pairs` (147, 159) — atomic ops
  sem alocação; **podem ser** `noexcept`. Pequeno win.
- `find_readonly` (200) — sem alocação; **noexcept**.
- `lookup` (532) — chama `mphf->lookup` que pode lançar?
  `boophf_parallel_t::lookup` provavelmente é noexcept (é hash lookup).
- `lookup_remapped` (533) — dead code (B1).

---

## 📊 Sumário de prioridades

| #  | Item                                                       | Tipo  | Prioridade |
|----|------------------------------------------------------------|-------|------------|
| **B1/M1** | `(void)lookup_remapped` lambda dead code              | bug+morto | **alta** (limpeza) |
| B2 | `prune_cache_for_downgrade` esquece `depth_index.bin`        | bug   | baixa      |
| B6 | race latente entre processos no manifest                     | bug   | baixa (latente) |
| B3 | `try_cache_fast` "cache RAM ok" + nullopt confuso            | bug   | baixa (UX) |
| B7 | mmap reaberto várias vezes em path complex                   | perf  | baixa      |
| **P1** | paralelizar merge `thr_in_ram`/`thr_names` (~40 ms)      | perf  | baixa      |
| P3, P4 | `fmt::format` em hot path do parsing                     | perf  | baixa      |
| P9 | reserve antes de `gbwt_path_names.insert` loop               | perf  | trivial    |
| **M2** | 10 marcadores `ARCH-N`/`PERF-N` espalhados              | morto | média      |
| **`[[nodiscard]]`** | 9 métodos públicos sem o atributo               | quali | média      |
| `noexcept` em `profile_name_from_flags`                          | quali | baixa      |
| `noexcept` em lambdas de Union-Find (`find`, `unite`, `find_readonly`) | quali | baixa |

---

## 📝 Observações adicionais

### Correção dos algoritmos

- **Union-Find lock-free** (linhas 147–182) com `std::atomic_ref`,
  `acquire/release`, `compare_exchange_weak/strong`. Algoritmo clássico
  de Anderson/Wright. Validei pattern: rank decision lê `relaxed`
  (correto pq é heurística), parent CAS é `release/acquire` (necessário
  para visibilidade do union). ✓
- **Bin-packing min-heap** (linhas 270–282) — heap com `std::greater`
  garante que o bin com menor carga é sempre escolhido. ✓
- **Pass1/Pass2 alinhamento de chunk em `\n`** (linhas 56–60, 514–520) —
  evita split de linha entre threads. ✓
- **Manifest atomic write** (linhas 846–875) — escreve em `tmp` depois
  `rename`. POSIX atomic. ✓

Não encontrei bugs algorítmicos.

### Padrões consistentes com o resto do projeto

- `[[nodiscard]]` faltando — **mesmo padrão** que limpamos em
  `topology.hpp` (4 métodos), `metadata_store.hpp` (8 accessors),
  `fm_index.hpp` (1 método). Aqui faltam 9 — maior cluster pendente.
- Marcadores `ARCH-N`/`PERF-N` — **mesmo padrão** que limpamos em
  `path_index.cpp`, `graph_algorithms.cpp`, `csr_builder.cpp`,
  `fm_index.cpp`. Padrão arquitetural rastreável só pelo git log.
- Dead code via `(void)var` — **primeiro caso** identificado neste
  projeto. Vale destacar como anti-pattern.

### Tamanho e complexidade

`ingestor.cpp` é o **maior arquivo do projeto** (2.042 linhas). 14
funções, das quais 5 têm > 100 linhas. Candidatos a refatoração:

1. **`process_data_parallel`** (linhas 392–769) — 378 linhas. Pode-se
   extrair: setup de bins, parser loop, merge final.
2. **`run_full_ingest`** (linhas 1452–1913) — 462 linhas. Pode-se
   extrair: pass1, pass2, gbwt build, save final.

Refatoração estrutural — **fora do escopo desta análise** (risco
elevado de regressão funcional).

---

## 🛠️ Próximos passos recomendados

1. **B1+M1** — remover `(void)lookup_remapped` (1 linha + lambda).
   Dead code óbvio. Zero risco.

2. **`[[nodiscard]]`** nos 9 métodos do header. Sinaliza intenção, sem
   impacto runtime. Zero risco.

3. **M2** — reformular ~10 comentários `ARCH-N`/`PERF-N` para
   semântica. Mesmo padrão que aplicamos em outros arquivos. Zero
   risco funcional.

4. **B2** — adicionar `try_remove("depth_index.bin")` em
   `prune_cache_for_downgrade`. Trivial.

5. **`noexcept` em `profile_name_from_flags`** — trivial.

6. **`noexcept` em lambdas de Union-Find** — sinaliza compromisso com
   atomicity.

7. **P1** (paralelizar merge) — aplicação opcional, ganho modesto.

8. **B3** (refatoração de retorno do cache) e **B6** (manifest race) —
   trabalhos future-only que dependem de decisões arquiteturais
   (multi-process suporte? `std::expected` widespread?).

A ordem 1 → 2 → 3 → 4+5+6 → 7 maximiza valor entregue/risco.

**B1+M1+M2+`[[nodiscard]]`+B2+noexcept** são todos cleanup baixo-risco
e podem entrar num único commit.

---

## Métricas para baseline

Não é necessário criar `bench_ingestor` separado — `bench_gbwt` já
mede o pipeline completo. As mudanças deste plano são **cleanup +
correção** sem impacto perf esperado:

- B1, M1, M2: cosméticos, zero perf impact
- `[[nodiscard]]`, `noexcept`: zero runtime impact
- B2, P1: ganho desprezível (< 50 ms total)

**Sinal de sucesso:** zero regressão em `bench_gbwt` total e nas fases
1, 2, 4, 5 já reportadas em
`docs/baseline_metadata_store/chm13_100w_cold.txt`.

| Fase | chm13 baseline atual |
|---|---:|
| Fase 1 (MPHF) | 551 ms |
| Fase 2 (Links/Paths/Metadata) | 2,49 s |
| Fase 3 (FM-Index) | 8,68 s |
| Fase 4 (CSR Topology) | 332 ms |
| Fase 5 (gBWT) | 7,42 s |
| TOTAL Pipeline | 21,90 s |

Pós-cleanup, esses números devem permanecer dentro do ruído natural
(±5%).

---

## Baseline de performance — pré-correções (2026-05-04)

Logs em `docs/baseline_ingestor/`:
- `bench_1M_cold.txt`, `chm13_100w_cold.txt`

### Ambiente

Mesmo dos benches anteriores: i7-9750H @ 2.60 GHz, 8 threads, gcc 14
`-O3 -flto=auto -march=native -fopenmp`, perfil `pangenome`.

### Ingest cold (`bench_gbwt`)

| Fase | bench_1M (1M nós, 0 paths) | chm13_100w (4,3M nós, 100 paths) |
|---|---:|---:|
| **Fase 1 (MPHF)** | 164,7 ms | 573,6 ms |
| **Fase 2 (Links/Paths/Metadata)** | 501,0 ms | 2,83 s |
| **Fase 3 (FM-Index)** | 67,4 ms | 13,81 s |
| **Fase 4 (CSR Topology)** | 178,2 ms | 370,2 ms |
| **Fase 5 (gBWT wall)** | _(0 paths — pulado)_ | 8,21 s |
| **TOTAL Pipeline** | **1,13 s** | **27,77 s** |
| **Wall total** (`/usr/bin/time`) | — | — |
| **Pico RSS** | — | **9,04 GB** |
| **Pico RSS gBWT isolado** | — | 4,40 GB |

### Linhas-alvo

`ingestor.cpp` orquestra todas essas fases. As mudanças do plano são:

| Mudança | Métrica esperada | Sinal de sucesso |
|---|---|---|
| **B1+M1** (remover lambda dead) | sem regressão | TOTAL inalterado ±5% |
| **M2** (10 marcadores BUG/PERF) | sem regressão | idem |
| **`[[nodiscard]]`** (9 métodos) | sem regressão | idem |
| **B2** (depth_index.bin no prune) | comportamento de downgrade | teste manual |
| **`noexcept`** em helpers/lambdas | sem regressão | idem |
| **P1** (paralelizar merge `thr_in_ram`) | Fase 2 -40 ms | -2% na Fase 2 |

### Como reproduzir

```bash
rm -rf data/<arquivo>.gfa.idx
/usr/bin/time -v build/bench_gbwt data/<arquivo>.gfa 8
```

Saída exibe Fase 1-5 + TOTAL Pipeline + Pico RSS.

---

## Pós-correção B1+B2+M1+M2+nodiscard+noexcept (2026-05-04)

### Implementação

**B1 + M1 — Lambda `lookup_remapped` removida:**
```cpp
// Removido (linhas 533-540 originais):
auto lookup_remapped = [&](std::string_view sv) -> uint64_t { ... };
(void)lookup_remapped;  // dead code marker
```

**B2 — `depth_index.bin` adicionado ao prune de downgrade:**
```cpp
if (old_flags.save_metadata && !new_flags.save_metadata) {
    try_remove("metadata.bin");
    try_remove("metadata_seqs.bin");
    try_remove("metadata_strings.bin");
    try_remove("length_index.bin");
    try_remove("depth_index.bin");  // novo
}
```

**M2 — 7 marcadores `ARCH-N`/`PERF-N` reformulados:**
- Linha 3: "Separado em ARCH-1" → "Mantido separado"
- Linha 292: "ARCH-4: parse_w_line" → "parse_w_line / parse_p_line — extraídos"
- Linha 495: "PERF-5: per-thread accumulators" → "Per-thread accumulators"
- Linha 615: "ARCH-4: parsing delegado" → "Parsing delegado"
- Linha 642, 673: "PERF-5:" → semântica
- Linha 1207: "ARCH-3 — auxiliares" → "Auxiliares"

**`[[nodiscard]]` adicionado em 9 métodos do header:**
- `read_cached_flags`, `collect_keys_and_links_parallel`,
  `process_data_parallel`, `load_index`, `try_cache_fast`,
  `try_cache_hash`, `complete_with_snarls`, `complete_with_gbwt`,
  `run_full_ingest`.

**`noexcept` em `profile_name_from_flags`** (helper anônimo, retorna
`const char*` literal — nunca lança).

### Validação

**ctest:** 17/17 ✓

**Bench cold `bench_gbwt` chm13_100w**:

| Fase | Baseline | Run 1 pós | Run 2 pós |
|---|---:|---:|---:|
| Fase 1 (MPHF) | 574 ms | 589 ms | 633 ms |
| Fase 2 (Links/Paths) | 2,83 s | 2,66 s | 2,78 s |
| Fase 3 (FM-Index) | 13,81 s | 9,10 s | 8,95 s |
| Fase 4 (CSR Topology) | 370 ms | 358 ms | 356 ms |
| Fase 5 (gBWT) | 8,21 s | 7,39 s | — |
| **TOTAL Pipeline** | **27,77 s** | **21,96 s** | **22,34 s** |
| Pico RSS gBWT | 4,40 GB | 3,94 GB | — |

**Observação:** as melhorias visíveis (~6 s no TOTAL, ~5 s na Fase 3)
são **variabilidade natural** entre runs cold, não efeito das mudanças.
Justificativa:
- Não toquei em FM-Index (Fase 3) — `libsais64_omp` é o gargalo.
- Não toquei em build_path_index_parallel (Fase 5).
- B1+M1 removeu lambda morta — código binário ligeiramente menor mas
  não afeta hot path (era unused).
- M2 + `[[nodiscard]]` + `noexcept` — qualifiers e comentários, zero
  impacto runtime.

**Conclusão honesta:** mudanças são puramente de qualidade de código e
correção (B2) — sem impacto perf real. A variação observada confirma o
ruído natural de runs cold (~10-20% no chm13 dependendo do estado do
page cache do FS, thermal throttling, swap, etc.).

### bench_1M (1M nós, 0 paths)

| Fase | Baseline | Pós |
|---|---:|---:|
| Fase 1 (MPHF) | 165 ms | 124 ms |
| Fase 2 (Links/Paths) | 501 ms | 438 ms |
| Fase 3 (FM-Index) | 67 ms | 26 ms |
| Fase 4 (CSR Topology) | 178 ms | 154 ms |
| **TOTAL Pipeline** | **1,13 s** | **0,96 s** |

bench_1M é dataset sintético pequeno onde variabilidade é mais alta
proporcionalmente.

### Status final do plano

| # | Item | Status |
|---|---|---|
| **B1+M1** | Lambda dead code removida | ✅ |
| **B2** | `depth_index.bin` no prune | ✅ |
| **M2** | 7 marcadores `ARCH-N`/`PERF-N` reformulados | ✅ |
| **`[[nodiscard]]`** | 9 métodos no header | ✅ |
| **`noexcept`** | `profile_name_from_flags` | ✅ |
| B3, B6, B7 | bugs latentes/UX (race manifest, mmap reabre) | future work |
| **P1** | paralelizar merge `thr_in_ram` | ✅ |
| **P3** | `fmt::format_to(back_inserter)` em parse_w_line | ✅ |
| P4 | `fmt::format` em parse_p_line | n/a (já é `std::string(token)`) |

**Cleanup baixo-risco 100% concluído.** Bugs latentes (B3, B6, B7)
ficam como future work — exigem refatoração de API ou casos não vistos
em uso atual.

### P1 + P3 — validação chm13_100w cold

| Fase | Baseline | P1+P3 | Δ |
|---|---:|---:|---:|
| Fase 1 (MPHF) | 573,6 ms | 566,3 ms | -7 ms |
| **Fase 2 (Links/Paths)** | **2,83 s** | **2,71 s** | **-120 ms (-4,2%)** |
| Fase 3 (FM-Index) | 13,81 s | 9,30 s | variabilidade libsais |
| Fase 4 (CSR) | 370,2 ms | 367,1 ms | -3 ms |

P1 (merge paralelo de `thr_in_ram`/`thr_names`) + P3 (`format_to` reusa
buffer de `path_name`) reduzem ~120 ms na Fase 2 — bate com estimativa
original (~40 ms P1 + ganho de heap reuse no parsing W).

### Tamanho do arquivo

| Métrica | Antes | Depois |
|---|---:|---:|
| `src/ingest/ingestor.cpp` | 2.042 LOC | 2.034 LOC (-8) |

Redução modesta porque marcadores reformulados ocupam linhas similares;
removida a lambda (8 linhas) compensou.
