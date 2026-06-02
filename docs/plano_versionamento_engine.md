# Versionamento GGQL — Plano de Implementação (lado Engine)

> **Propósito:** plano de implementação do **lado engine** (`ggql_engine`) para o
> sistema de versão proposto pelo cliente Flutter (`ggql_ui`). Espelha e responde
> à proposta da UI, fixando as decisões em aberto (V1–V4) e o passo-a-passo de
> código aterrado no estado real do repositório.
>
> **Documento de origem (cliente):**
> [`../../flutter_projetos/ggql_ui/docs/versionamento_ggql.md`](file:///home/karlay/Documentos/flutter_projetos/ggql_ui/docs/versionamento_ggql.md)
>
> **Status:** ✅ COMPLETO (2026-06-02) — Fases A e B implementadas e validadas.
> Contrato no ar, teste CTest passando, `--version` no REPL e script de deploy
> funcionando. Lado UI também entregue (ffigen + gate de ABI + status bar, ver
> §5 da proposta do cliente). Decisões V1–V4 fechadas (seção
> [§7](#7-decisões-fechadas-respostas-ao-8-da-proposta-ui)).
>
> **Criado:** 2026-06-02 · **Autor:** engine (`ggql_engine`).
>
> **Histórico:**
> - 2026-06-02 — plano criado; decisões V1–V4 fechadas com o cliente.
> - 2026-06-02 — Fase A implementada (A1–A4) e validada: `ggql_ffi_version` é o
>   43º símbolo da `.so`, teste `test_ffi_version` passa, build incremental não
>   recompila `ffi.cpp` (hash fresco sem custo).
> - 2026-06-02 — Fase B implementada (B1–B2): `--version`/`-V` no `ggql_repl` e
>   `tools/deploy_ffi_to_ui.sh` (build + cópia + log + verificação sha256).
> - 2026-06-02 — UI consumiu o contrato: ffigen regenerado, gate de ABI fail-closed
>   no startup e versão real na status bar; 48 testes FFI passando no lado Dart.

---

## 1. Contexto e estado atual (verificado no código)

A UI não tem como saber — em build ou runtime — qual versão do engine carrega,
nem se a ABI esperada bate com a `.so` real. Em 2026-06-01 a UI carregou uma lib
defasada de 24/mai sem nenhum sinal; só foi detectado comparando sha256 na mão.

Estado do engine confirmado em 2026-06-02:

| Item | Estado atual |
|---|---|
| `CMakeLists.txt:2` | `project(ggql_engine LANGUAGES CXX)` — **sem `VERSION`** |
| Header de versão | **não existe** (sem `configure_file`, sem template) |
| Macro de ABI | **não existe** |
| Símbolo de versão na FFI | **não existe** (42 símbolos `ggql_*`, nenhum de versão) |
| Tags git | **nenhuma** — `git describe --tags` cai para short hash |
| Padrão `struct_size` | ✅ já em uso em 6+ structs (`GgqlIngestStats`, `GgqlSurvey`, `GgqlEngineCapabilities`, `GgqlSubgraphDims`…) com validação `< sizeof(...)` → `-2` + echo do `caller_size` |
| Funções handle-free | ✅ `ggql_survey_gfa` já vive no `extern "C"` — modelo direto para `ggql_ffi_version` |

**Ativo a reaproveitar:** toda a proposta fala a língua do padrão `struct_size`
que o engine já adota. A função nova é aditiva e não toca em ABI existente.

---

## 2. Três identidades (alinhado com §2 da proposta UI)

| Identidade | Formato | Fonte no engine | Quando muda |
|---|---|---|---|
| **Product version** | SemVer `MAJOR.MINOR.PATCH` | `project(... VERSION)` no CMake | a cada release |
| **ABI/FFI contract** | `uint32` monotônico | macro `GGQL_FFI_ABI_VERSION` em `ffi.h` | só em quebra/extensão incompatível |
| **Build identity** | short hash + `-dirty` + data | `git rev-parse` em build-time | a cada build |

A **ABI version** governa compatibilidade. A product version é para humano.
A build identity resolve "essa `.so` é a mais recente?".

---

## 3. Fonte da verdade (engine side)

- **SemVer:** `project(ggql_engine VERSION 0.1.0 LANGUAGES CXX)` → CMake expõe
  `PROJECT_VERSION_MAJOR/MINOR/PATCH`.
- **Build identity:** capturada em **build-time** (não só configure-time) via
  `git rev-parse --short HEAD` + `git status --porcelain` (vazio = limpo). O hash
  sai já com sufixo `-dirty` embutido quando há mudanças não commitadas.
- **Header gerado:** `include/ggql/version.h.in` → `build/generated/ggql/version.h`
  via `configure_file(... @ONLY)`. Header gerado é **artefato** (entra no
  `.gitignore`), incluído só internamente pelo `ffi.cpp` (não exposto à UI).
- **Macro de ABI:** `GGQL_FFI_ABI_VERSION` mora **direto em `include/ffi/ffi.h`**
  (não é gerada) — é compile-time, compilada junto da UI via ffigen, e é o número
  que o gate da UI compara.

> **Decisão técnica crítica — hash não pode envelhecer.** Gerar o header só no
> `configure` deixa o hash obsoleto quando se commita e rebuilda sem reconfigurar
> — exatamente a classe de bug de 2026-06-01. Por isso a geração é amarrada a um
> `add_custom_target(ggql_version ALL ...)` do qual `ggql_ffi` depende: todo build
> relê o hash. É a diferença entre "funciona na demo" e "não mente em produção".

---

## 4. Contrato FFI (idêntico ao §4 da proposta UI — zero retrabalho na UI)

Em `include/ffi/ffi.h`, próximo ao topo (após os includes):

```c
#define GGQL_FFI_ABI_VERSION 1u   // bump só em mudança INCOMPATÍVEL de ABI

typedef struct {
    size_t   struct_size;        // caller seta = sizeof(GgqlVersionInfo)
    uint32_t abi_version;        // == GGQL_FFI_ABI_VERSION no build da .so
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    const char* version_str;     // "0.1.0"   (storage estático, não liberar)
    const char* git_hash;        // "43fab89" (+ "-dirty" se aplicável)
    const char* build_date;      // ISO-8601
} GgqlVersionInfo;

// Handle-free: funciona ANTES de engine_load e mesmo se o load falhar.
// Retorna 0 ok, -1 se out NULL, -2 se struct_size insuficiente.
int32_t ggql_ffi_version(GgqlVersionInfo* out);
```

Strings com storage estático (literais vindos das macros) → sem alocação, sem
`free`, igual ao resto da API.

---

## 5. Passo-a-passo de implementação

### Fase A — Contrato (engine-first; desbloqueia a UI) — ✅ IMPLEMENTADA (2026-06-02)

> **Resultado medido:** `ggql_ffi_version` exportado como 43º símbolo da
> `libggql_ffi.so`; `ctest -R test_ffi_version` passa (caminho feliz + `-1` + `-2`);
> saída runtime `version=0.1.0 abi=1 git=43fab89-dirty built=2026-06-02`. Rebuild
> incremental sem mudança **não** recompila `ffi.cpp` (`copy_if_different` preserva
> o mtime do header gerado).
>
> Arquivos tocados: `CMakeLists.txt` (`VERSION 0.1.0` + bloco de geração + wiring
> do target), `include/ggql/version.h.in` (novo), `tools/cmake/GenerateVersionHeader.cmake`
> (novo), `include/ffi/ffi.h`, `src/ffi/ffi.cpp`, `tests/test_ffi_version.cpp` (novo),
> `tests/CMakeLists.txt`.

**A1. CMake + header gerado** — ✅
- `CMakeLists.txt:2`: adicionar `VERSION 0.1.0` ao `project(...)`.
- Criar `include/ggql/version.h.in` (template com placeholders `@PROJECT_VERSION_*@`,
  `@GGQL_VERSION_STR@`, `@GGQL_GIT_HASH@`, `@GGQL_BUILD_DATE@`).
- Criar `tools/cmake/GenerateVersionHeader.cmake`: roda `git rev-parse`/`git status`,
  monta `GGQL_GIT_HASH` (com `-dirty`), `GGQL_BUILD_DATE` (`string(TIMESTAMP ... UTC)`),
  e `configure_file(... @ONLY)` para `build/generated/ggql/version.h`.
- `add_custom_target(ggql_version ALL ...)` que invoca o script via `cmake -P`;
  `add_dependencies(ggql_ffi ggql_version)`.
- `target_include_directories(ggql_ffi PRIVATE ${CMAKE_BINARY_DIR}/generated)`.
- `.gitignore`: nenhuma mudança necessária — `build/` já é ignorado, então
  `build/generated/ggql/version.h` já fica fora do versionamento.
- **Decisão de implementação:** a geração roda em duas etapas — `execute_process`
  no configure (header existe para IntelliSense) **+** `add_custom_target(ggql_version ALL)`
  em build-time (hash sempre fresco). O script usa `copy_if_different` para não
  recompilar `ffi.cpp` quando o header não muda; `GGQL_BUILD_DATE` tem granularidade
  de **dia** (não segundo) pelo mesmo motivo.

**A2. `include/ffi/ffi.h`** — ✅ macro `GGQL_FFI_ABI_VERSION 1u`, struct
`GgqlVersionInfo`, declaração `ggql_ffi_version` (bloco do §4).

**A3. `src/ffi/ffi.cpp`** — ✅ implementado dentro do `extern "C"` (logo após a
abertura, antes de `ggql_ingestor_create`), modelado em `ggql_survey_gfa`.
Inclui `ggql/version.h` gerado. Lógica:

```c
int32_t ggql_ffi_version(GgqlVersionInfo* out) {
    if (!out) return -1;
    if (out->struct_size < sizeof(GgqlVersionInfo)) return -2;
    const size_t caller_size = out->struct_size;   // echo (padrão dos outros structs)
    out->abi_version   = GGQL_FFI_ABI_VERSION;
    out->version_major = GGQL_VERSION_MAJOR;
    out->version_minor = GGQL_VERSION_MINOR;
    out->version_patch = GGQL_VERSION_PATCH;
    out->version_str   = GGQL_VERSION_STR;   // literais → storage estático
    out->git_hash      = GGQL_GIT_HASH;
    out->build_date    = GGQL_BUILD_DATE;
    out->struct_size   = caller_size;
    return 0;
}
```

Vira o **43º símbolo** exportado da `.so`.

**A4. Teste CTest** — ✅ `tests/test_ffi_version.cpp` (handle-free, sem dataset):
valida `rc==0`, `abi_version == GGQL_FFI_ABI_VERSION`, `struct_size` ecoado de
volta, strings não-vazias; e os caminhos `rc==-1` (out NULL) e `rc==-2`
(struct truncado). Registrado no `tests/CMakeLists.txt`.

### Fase B — Ergonomia e deploy — ✅ IMPLEMENTADA (2026-06-02)

**B1. `--version` (e `-V`) no `ggql_repl`** — ✅ saída
`ggql 0.1.0 (43fab89-dirty) · ABI 1 · built 2026-06-02` e sai com 0. O repl agora
inclui o `version.h` gerado (mesmo wiring de target do `ggql_ffi`: `add_dependencies`
+ `target_include_directories`) e `ffi/ffi.h` apenas pela macro `GGQL_FFI_ABI_VERSION`.
Arquivos: `src/repl/ggql_repl.cpp` (`Args::version`, parse, `print_version()`, usage),
`CMakeLists.txt`.

**B2. `tools/deploy_ffi_to_ui.sh`** — ✅ builda `ggql_ffi` + `ggql_repl`, copia a `.so`
para `$GGQL_UI_REPO/.dart_tool/lib/` (default: repo irmão `../../flutter_projetos/ggql_ui`),
loga `--version` do **mesmo build** e verifica `sha256` origem == destino (aborta se
divergir). `set -euo pipefail`, caminhos canonicalizados, `GGQL_UI_REPO`/`BUILD_DIR`
configuráveis por env. Saída real:

```
→ Building ggql_ffi + ggql_repl em build/ ...
✓ Build: ggql 0.1.0 (43fab89-dirty) · ABI 1 · built 2026-06-02
✓ Copiado → .../ggql_ui/.dart_tool/lib/libggql_ffi.so
✓ sha256 440c7805…e952e7ea808
```

Resolve o caso 2026-06-01 (cópia + verificação + log de versão num passo), sem esperar CI.

---

## 6. Política de compatibilidade (alinhada com §6 da proposta UI)

- **Bump** `GGQL_FFI_ABI_VERSION` ao: remover/reordenar campo de struct, mudar
  assinatura, mudar semântica de retorno.
- **Não bump** ao adicionar campo *no fim* de struct com `struct_size` — é o ponto
  do padrão; o caller velho lê só até o offset que conhece.
- Gate da UI é **fail-closed:** ABI divergente bloqueia com erro acionável (erro
  explícito > crash/UB).

---

## 7. Decisões fechadas (respostas ao §8 da proposta UI)

| # | Questão | Decisão | Justificativa |
|---|---|---|---|
| **V1** | De onde sai a SemVer | **Manual no CMake** (`project VERSION`); git hash separado via `git rev-parse` | Não há tags hoje; determinístico; build fora de git-checkout não quebra. |
| **V2** | Granularidade do ABI | **Global** (número único); `struct_size` cobre extensão aditiva | Bate com o padrão já em uso no engine. |
| **V3** | Forma de expor | **Símbolo dedicado handle-free** `ggql_ffi_version` | `capabilities` exige handle e só existe pós-load; versão precisa funcionar no boot. |
| **V4** | Automação da cópia | **`tools/deploy_ffi_to_ui.sh` no engine** agora; CI como evolução futura | Resolve 2026-06-01 imediatamente sem custo de pipeline. |
| — | Versão inicial | **0.1.0** (ABI contract = `1`) | Sinaliza honestamente pré-1.0 / em desenvolvimento. |

---

## 8. Ordem de execução

```
A1 (CMake + header) → A2 (ffi.h) → A3 (ffi.cpp) → A4 (teste)   ← desbloqueia a UI
                                  ↓
B1 (--version no REPL) → B2 (deploy_ffi_to_ui.sh)
```

Engine-first porque a UI consome o contrato: nada na UI compila sem o header novo
(`GGQL_FFI_ABI_VERSION` + `GgqlVersionInfo`). Depois de A, a UI executa os itens
do §5 da sua própria proposta (regenerar ffigen, gate de ABI no startup, tela
"Sobre").

---

## 9. Como isso conserta o caso de 2026-06-01

`git_hash` + `build_date` em `ggql_ffi_version` (e no `--version` do REPL) deixam
a UI exibir/logar exatamente de qual build a `.so` veio. O `deploy_ffi_to_ui.sh`
imprime versão+hash+sha256 no momento da cópia. Divergência fica visível no app e
no terminal — sem comparar binário na mão.
