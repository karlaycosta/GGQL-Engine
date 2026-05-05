# Análise — `include/ggql/ingest/ingestor.hpp`

Auditoria do **header** `ingestor.hpp` (547 LOC após cleanup pós-P1+P3+re-auditoria).
Diferente do `.cpp` (já auditado em `analise_ingestor.md`), este foco é o
header: declarações de classe, structs públicas (`MmapFileParallel`,
`StringViewHasherParallel`, `IngestStatsParallel`, `CachedFlags`), funções
inline (`parse_optional_tags`, `tokenize`, `next_token`, `calc_node_width`),
e contrato de API exposto a callers.

Data: 2026-05-05
Branch: `feat/snarl-decomposition-paten`
Arquivo: `include/ggql/ingest/ingestor.hpp` (547 linhas)

---

## Resumo executivo

| Categoria | Encontrados | Críticos |
|---|---:|---:|
| Bugs / fragilidades | 5 | 1 (B1) |
| Pontos de melhoria de performance | 4 | 0 |
| Código morto (campos write-only ou nunca-setado) | 4 | 1 (M1) |
| Atributos C++23 ausentes / qualifiers | 3 | 0 |

Severidade geral: **baixa**. O header é maduro; os achados mais
relevantes são fields documentados como populados mas que nunca
recebem valor, e fragilidades de contrato (`MmapFileParallel` para
arquivos vazios).

---

## Bugs / fragilidades

### B1 — `MmapFileParallel::open()` para arquivo vazio retorna estado válido com `data=nullptr`
**Severidade:** média (latente)

```cpp
size_t new_size = static_cast<size_t>(st.st_size);
if (new_size == 0) {
    ::close(fd);
    return MmapFileParallel{};  // data=nullptr, size=0
}
```

Resultado: caller recebe `MmapFileParallel` "válido" mas `data == nullptr`.

Em `process_data_parallel`:
```cpp
const char* data_start = res_.mmap->data;     // nullptr
const char* data_end = data_start + total_size; // nullptr + 0 = UB pelo standard
```

Em `try_cache_hash`:
```cpp
out_hash = XXH64(res_.mmap->data, res_.mmap->size, 0); // XXH64(nullptr, 0, 0)
```

Na prática, glibc retorna 0 para `nullptr + 0` e XXH64 lida com tamanho zero,
mas é UB segundo o C++ standard. Para um GFA vazio (0 bytes), `pass1.keys`
ficaria vazio e o caller já retorna erro adiante — então o bug é mascarado.
Mas é fragilidade.

**Fix sugerido:** `MmapFileParallel::open()` deve retornar
`std::unexpected("arquivo GFA vazio")` quando `st.st_size == 0`, ou pelo
menos garantir `data` aponta para um buffer válido (ex.: estático vazio).

### B2 — `MmapFileParallel` declara só move-ctor; move-assign está implicitamente deletado
**Severidade:** baixa (limitação de API)

```cpp
MmapFileParallel(const MmapFileParallel&) = delete;
MmapFileParallel& operator=(const MmapFileParallel&) = delete;
MmapFileParallel(MmapFileParallel&& o) noexcept : data(o.data), size(o.size) { ... }
// faltando: MmapFileParallel& operator=(MmapFileParallel&&) noexcept;
```

Conforme rule-of-5, declarar copy=delete + move-ctor faz move-assign
ser implicitamente *deletado* (não default-generated). Por isso código
como `m = MmapFileParallel::open(...).value()` não compilaria. Atualmente
o código contorna usando `std::make_unique<MmapFileParallel>(std::move(*ex))`,
o que funciona, mas seria mais idiomático declarar move-assign.

**Fix sugerido:** adicionar move-assign explícito que faz `munmap` do
estado antigo antes de assumir o novo, ou marcar `= delete` explícito
para deixar a intenção clara.

### B3 — `IngestStatsParallel::gbwt_tmp_path` documentado mas **nunca setado**
**Severidade:** média (contrato de API quebrado)

Header (linha 156-162):
```cpp
// Campos opcionais — preenchidos apenas quando keep_gbwt_tmp=true em ingest().
std::string gbwt_tmp_path;                // caminho do arquivo temporário em disco
std::vector<std::string> gbwt_path_names; // nomes dos paths
uint8_t gbwt_node_width = 0;
```

Header (linha 220-229) afirma:
```cpp
//   keep_gbwt_tmp   — se true, não exclui o arquivo temporário do gBWT
//                     após a construção; preenche gbwt_tmp_path,
//                     gbwt_path_names e gbwt_node_width em
//                     IngestStatsParallel.
```

Mas no `.cpp` linhas 1723-1727, com `keep_gbwt_tmp=true`, o código apenas
preenche `gbwt_node_width` e `gbwt_path_names` — **`gbwt_tmp_path` nunca
recebe valor**. Como a refatoração de bin-packing dividiu paths em
múltiplos tmp files (um por bin), o conceito de "single tmp path"
ficou obsoleto, mas o campo não foi atualizado/removido.

`grep -rn 'gbwt_tmp_path\s*=' src/` não retorna setters.

**Fix sugerido:** ou remover o campo (e documentação) — paths agora
estão distribuídos entre múltiplos arquivos — ou expor `std::vector<std::string>
gbwt_tmp_paths`. Como nenhum caller atual depende deste campo (FFI passa-o
mas Rust não consome em prod), a remoção é o caminho mais honesto.

### B4 — `parse_optional_tags` aceita inputs malformados silenciosamente
**Severidade:** baixa

```cpp
const char c0 = static_cast<char>(sv[0] & ~0x20);  // bit-trick uppercase
const char c1 = static_cast<char>(sv[1] & ~0x20);
```

Bit-trick `& ~0x20` converte `a..z` para `A..Z` mas também aplica em
qualquer char não-letra (p.ex. `'\n' & ~0x20 = '\n'`, `'1' & ~0x20 = '1'`,
mas `'0' & ~0x20 = '0'`). Funciona para ASCII letters, mas inputs com
chars não-letra geram falsos negativos silenciosos (`from_chars` falha,
`out_*` não atualizado). Para input controlado (manifest gerado pelo
binário), OK; para GFA externo malformado, bug consome a tag sem warning.

**Fix sugerido:** validar que `sv[0]` e `sv[1]` são letras antes do bit-trick:
```cpp
if (!std::isalpha(static_cast<unsigned char>(sv[0])) ||
    !std::isalpha(static_cast<unsigned char>(sv[1]))) continue;
```

### B5 — `get_mtime_ns` usa `try/catch(...)` para `last_write_time`
**Severidade:** baixa (defensivo correto)

```cpp
static int64_t get_mtime_ns(const std::string& path) noexcept {
    try { ... } catch (...) { return 0; }
}
```

OK, mas catch-all engole exceções específicas. Em uso normal não é
problema; só se `last_write_time` lançar tipos não documentados
(unlikely). Aceitar como-está.

---

## Performance

### P1 — `parse_optional_tags` é função inline grande (40 LOC) no header
**Ganho estimado:** marginal

Função `static` definida no header. Vantagem: inlinable em cada call site
(chamada uma vez por linha S em Pass2 — hot path). Desvantagem: força
recompilação de todo arquivo que faz `#include "ingestor.hpp"` quando
muda. Trade-off correto: inline é a escolha certa para hot path.

**Sugestão:** manter inline, mas medir se mover para `.cpp` com `inline`
(não-static) e LTO produz código equivalente. Se sim, header bloat
diminui. Investigação requer compilar com `-flto=thin` e diffar binários.

### P2 — `parse_optional_tags` repete teste `c0 == 'D' && c1 == 'P'` em ramos consecutivos
**Ganho estimado:** zero (compilador já elimina)

```cpp
} else if (c0 == 'D' && c1 == 'P' && type == 'f') { ... }
} else if (c0 == 'D' && c1 == 'P' && type == 'i') { ... }
```

Compilador combina o teste comum em forma de switch. Apenas cosmético —
poderia agrupar com sub-switch:
```cpp
} else if (c0 == 'D' && c1 == 'P') {
    if (type == 'f') { ... } else if (type == 'i') { ... }
}
```

Mas a forma atual é mais legível para auditoria das tags. **Manter.**

### P3 — `IngestStatsParallel` tem 24 campos POD; layout pode ter padding suboptimal
**Ganho estimado:** zero em hot path (struct é construída poucas vezes)

Mistura `uint64_t`, `double`, `size_t`, `uint8_t`, `std::string`. Em x86_64
com 8 B alignment natural, `uint8_t gbwt_node_width` provavelmente gera
7 bytes de padding. Mas a struct é instanciada raramente (~1× por
ingestão), então padding é irrelevante. **Não alterar.**

### P4 — `tokenize` usa `#pragma GCC unroll 8`
**Ganho estimado:** já aplicado

Loop unroll para até 8 tokens. Heurística razoável (S-line tem ≤8 fields,
L-line tem ≤4, W-line tem ≤6). Funciona com GCC; clang ignora o pragma
mas seu otimizador faz auto-unroll similar. **OK.**

---

## Código morto

### M1 — Campos `elapsed_gbwt_insert_s` / `elapsed_gbwt_compress_s` **nunca setados**
**Severidade:** média (campos zumbi visíveis na FFI)

```cpp
double elapsed_gbwt_insert_s = 0.0;   // Tempo acumulado em DynamicGBWT::insert()
double elapsed_gbwt_compress_s = 0.0; // Tempo acumulado em GBWT(dynamic) compress
```

Buscas:
- `grep '\.elapsed_gbwt_insert_s' src/` → nada
- `grep '\.elapsed_gbwt_compress_s' src/` → nada

Os campos são propagados pela FFI (`include/ffi/ffi.h:28-29` + `ffi.cpp:175-176`)
e expostos a callers Rust como sempre `0.0`. Eram preenchidos numa versão
antiga do `build_path_index` (provavelmente single-threaded, instrumentado
com timers granulares). A migração para parallel/multi-bin substituiu por
`gbwt_t.cpu_avg_s`/`gbwt_t.cpu_max_s` (no `BuildResult` retornado por
`topology->build_path_index_parallel`).

**Ação:** remover ambos os campos do header E da estrutura FFI (breaking
change; coordenar). Alternativa conservadora: marcar `[[deprecated("sempre 0
desde v3.0; usar gbwt_cpu_avg_s/max_s")]]`.

### M2 — `comp_sigma_` é write-only
**Severidade:** baixa

```cpp
std::vector<uint64_t> comp_sigma_;  // declared, hpp:277
```

Em `ingestor.cpp:257-259`, é preenchido em `build_remap_from_pairs`:
```cpp
comp_sigma_.resize(K);
for (uint32_t c = 0; c < K; ++c) {
    comp_sigma_[comp_rank[c]] = comp_size[c];
}
```

E zerado mais tarde em `run_full_ingest:1583-1585`:
```cpp
{ std::vector<uint64_t> tmp; comp_sigma_.swap(tmp); }
```

**Mas nunca é lido** entre o write e o swap-clear. `bin_sigma_` (campo
similar) é lido em linha 449. `comp_sigma_` provavelmente sobrou de
uma versão anterior do bin-packing que usava sigma por componente em
vez de por bin.

**Ação:** remover o campo + as 4 linhas de write/swap.

### M3 — `Otimizações implementadas` lista 10 itens fixos no header (linhas 14-25)
**Severidade:** baixa (documentação stale)

```cpp
// Otimizações implementadas:
//   1. Paralelização com OpenMP (Passada 1 e 2)
//   2. posix_fadvise(POSIX_FADV_SEQUENTIAL) antes do mmap
//   ...
//  10. fmt::format para nomes de paths (C++23)
```

Lista útil para newcomers, mas desatualizada (não menciona Union-Find
lock-free com `std::atomic_ref`, bin-packing, P1/P3, etc.). Decay típico
de docs em código.

**Ação:** ou atualizar lista (custo de manutenção contínua) ou remover
e linkar para `docs/analise_ingestor.md` (single source of truth).

### M4 — Comentário em linha 175 menciona "C++23: constexpr node_width" mas é redundante
```cpp
// C++23: constexpr node_width calculation
```

`constexpr` está em C++11. C++23 traz `constexpr` ainda mais expandido,
mas isso aqui é trivial. Cosmético — remover ou substituir por descrição
funcional.

---

## Atributos C++23/20 ausentes

### A1 — `IngestStatsParallel` não tem `[[nodiscard]]` em métodos pois é POD sem métodos
**Status:** n/a — POD struct, getters são acessos diretos a fields públicos.
Apenas se transformarmos em classe com getters. Manter como POD.

### A2 — Faltam `noexcept` em utilitários estáticos
**Severidade:** baixa

Os métodos estáticos de parsing já têm `noexcept`:
- `skip_line` ✓
- `next_token` ✓
- `tokenize` ✓
- `align_to_line_start` ✓
- `parse_optional_tags` ✓ (linha 384, sem noexcept — `from_chars` é noexcept)

**Achado:** `parse_optional_tags` poderia ter `noexcept` adicionado —
todas as ops internas são noexcept (`from_chars`, `string_view::substr`,
ariths). Mas pode lançar se alguém modificar para usar `std::string`
construído. **Adicionar `noexcept` aumenta segurança de contrato.**

### A3 — `MmapFileParallel` destrutor não é declarado `noexcept`
**Severidade:** zero (destrutores são implicitamente noexcept)

Destrutores são `noexcept(true)` por padrão em C++11+. Adicionar
explícito é redundante. **Não alterar.**

---

## Plano sugerido (ordem por risco)

| # | Item | Tipo | Risco | Status |
|---|---|---|---|---|
| 1 | M2 — Remover `comp_sigma_` (write-only) | cleanup | mínimo | ✅ aplicado |
| 2 | M3 — Atualizar/remover lista "Otimizações" | doc | mínimo | ✅ aplicado |
| 3 | A2 — `noexcept` em `parse_optional_tags` | qualifier | mínimo | ✅ já existia |
| 4 | M4 — Remover comentário "C++23: constexpr" stale | doc | mínimo | ✅ aplicado |
| 5 | B1 — `MmapFileParallel::open()` rejeitar arquivo vazio com erro | bug | baixo | ✅ aplicado |
| 6 | B2 — Adicionar move-assign explícito em `MmapFileParallel` | API | baixo | ✅ aplicado |
| 7 | B4 — Validar `isalpha` em `parse_optional_tags` | bug | baixo | ✅ aplicado |
| 8 | M1 — Remover `elapsed_gbwt_*_s` (campos zumbi) | API/FFI | médio (FFI) | ✅ aplicado |
| 9 | B3 — Remover `gbwt_tmp_path` (nunca setado) | API | médio (breaking) | ✅ aplicado |

### Itens 1-4 aplicados (2026-05-05)

LOC: 2.580 → 2.559 (-21).
- `.hpp`: 547 → 534 (-13)
- `.cpp`: 2.033 → 2.025 (-8)

17/17 testes passando.

### Itens 5-7 aplicados (2026-05-05)

**B1** — `MmapFileParallel::open()` para arquivo vazio agora retorna
`std::unexpected("mmap: arquivo vazio (0 bytes): <path>")` em vez de
`MmapFileParallel{}` com `data=nullptr`. Elimina UB latente em
`data_start + total_size` e `XXH64(nullptr, 0, 0)`.

**B2** — Adicionado `MmapFileParallel& operator=(MmapFileParallel&&) noexcept`
explícito que faz `munmap` do estado antigo antes de assumir o novo.
Move-ctor já existia; antes só copy-assign era explicitamente deletado
e move-assign era implicitamente deletado. Padroniza rule-of-5.

**B4** — `parse_optional_tags` agora valida que `sv[0]` e `sv[1]` são
letras ASCII (a-z/A-Z) antes de aplicar o bit-trick `& ~0x20`. Antes,
chars não-letra resultavam em comparações incorretas; agora a tag é
rejeitada silenciosamente como antes mas sem o risco de falso positivo
em `c0 == 'D' && c1 == 'P'` para chars binários.

17/17 testes passando.

### Itens 8-9 aplicados (2026-05-05) — **BREAKING ABI**

**M1** — Removidos `elapsed_gbwt_insert_s` e `elapsed_gbwt_compress_s`
de `IngestStatsParallel` (`ingestor.hpp`), `GgqlIngestStats` (`ffi.h`)
e dos assignments em `ffi.cpp`. Esses campos eram zumbi: declarados,
inicializados a 0.0, propagados pela FFI a callers Rust/Dart, mas
**nunca setados** no pipeline. Migrar callers para `gbwt_cpu_avg_s`
e `gbwt_cpu_max_s` (que de fato carregam timing por-thread do
`build_path_index_parallel`).

**B3** — Removido `gbwt_tmp_path` de `IngestStatsParallel`. Documentado
como populado quando `keep_gbwt_tmp=true` mas **nunca recebia valor**:
o refator de bin-packing dividiu paths em múltiplos tmp files (um por
bin) que ficam encapsulados no ingestor. O campo single-string
representava conceito obsoleto. `keep_gbwt_tmp` flag e os campos
`gbwt_path_names`/`gbwt_node_width` permanecem (estes últimos são de
fato preenchidos). Comentário do parâmetro `keep_gbwt_tmp` atualizado
para refletir a realidade.

**Impacto ABI:** `GgqlIngestStats` perdeu 2 campos `double` no meio da
struct — qualquer caller (Dart binding, Rust crate) precisa recompilar
contra `ffi.h` atualizado. Versões antigas de bindings lerão valores
deslocados.

17/17 testes passando.

**Itens 1-4** (cleanup baixo-risco, ~10 min) podem ser aplicados imediatamente.

**Itens 5-7** (bugs latentes baixo-impacto) também são seguros.

**Itens 8-9** mexem em FFI — coordenar com consumidores Rust antes.

---

## Apêndice — itens avaliados e descartados

- `std::ranges` em loops internos do header — n/a, header não tem loops próprios.
- `std::print` C++23 — não há `fmt::print` no header.
- `std::flat_map` C++23 — não há mapa neste arquivo.
- `if constexpr` — não há código template aqui.
- `std::string_view` literais (`"prefix"sv`) — sem ganho real.
- Mover `parse_optional_tags` para `.cpp` — perderia inline, performance regrediria.
