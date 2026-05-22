# Integração FFI: Flutter e Python

**Última atualização:** 2026-05-21 (sincroniza struct `GgqlIngestStats`
nos exemplos Dart/Python com o header — remove `elapsed_gbwt_insert_s`
e `elapsed_gbwt_compress_s`, que foram suprimidos no commit `bf9b8fd`
"BREAKING ABI da FFI"; corrige contagens na tabela de cobertura.
Histórico anterior: 2026-05-20 — Rounds 1–3 do refinamento FFI + fix
`thread_local` em `dispatch_bridge` para `NativeCallable.listener`; ver
`docs/ffi_refinement_plan.md`).
**Branch:** `ffi/refine-load-and-introspection`.

A camada `ggql_ffi` expõe o motor C++ via **C ABI estável**, o que a torna
consumível por qualquer linguagem capaz de chamar símbolos de biblioteca
compartilhada — incluindo Dart/Flutter e Python. Este documento descreve as
estratégias de integração para ambos os consumidores, os padrões recomendados e
as armadilhas específicas de cada plataforma.

**Cobertura desta versão** (a referência canônica do C ABI é
`include/ffi/ffi.h`):

| Bloco | Símbolos | Documentado em |
|---|---|---|
| Ingest (legacy + perfil + granular) | `ggql_ingestor_*` (6) + `ggql_profile_*` (2) + `GgqlIngestStats`/`GgqlEvent` | §2.2–§2.4 (Flutter), §3.1–§3.4 (Python) |
| Engine lifecycle | `ggql_engine_{load,create,destroy,last_error}` | §6.1 |
| Query execução | `ggql_engine_{execute,execute_streaming}` | §6.2, §6.7 |
| Introspecção | `ggql_engine_{capabilities,validate,list_entities,describe_entity}` | §6.3–§6.5 |
| Controle de runtime | `ggql_engine_{cancel,set_max_total_hits}` | §6.6 |
| Resultado | `ggql_result_*` (11) + `GgqlCellType` | §6.2 |

---

## 1. Por que um C ABI serve a múltiplos consumidores

A biblioteca compilada (`.so` / `.dylib` / `.dll`) exporta símbolos com linkage
`extern "C"`, sem name-mangling C++. Isso significa:

- **Sem dependência de runtime C++** do lado do consumidor.
- **ABI estável**: adicionar campos novos em `GgqlIngestStats` com um campo
  `size_t struct_size` permite detecção de versão sem recompilar o consumidor.
- **Mesma `.so` compartilhada** entre Flutter e Python no mesmo processo ou
  máquina — zero custo de duplicação.

```
libggql.so
    │
    ├── Flutter (dart:ffi)   ← mobile / desktop
    └── Python  (ctypes/cffi) ← scripts, notebooks, CLI
```

---

## 2. Integração com Flutter via `dart:ffi`

### 2.1 Distribuição da biblioteca

| Plataforma   | Local da `.so` / `.dylib`                       |
|--------------|-------------------------------------------------|
| Android      | `android/src/main/jniLibs/<abi>/libggql.so`     |
| iOS / macOS  | Framework ou `ios/Libraries/libggql.dylib`      |
| Linux desktop| Junto ao executável ou `/usr/local/lib`         |
| Windows      | `libggql.dll` no diretório do executável        |

### 2.2 Mapeamento de tipos

```dart
// ggql_ffi_bindings.dart

import 'dart:ffi';
import 'dart:io';
import 'package:ffi/ffi.dart';

// ── Structs ───────────────────────────────────────────────────────────────────

final class GgqlIngestStats extends Struct {
  @Uint64() external int segmentCount;
  @Uint64() external int linkCount;
  @Uint64() external int nodeCount;
  @Uint64() external int pathCount;

  @Double() external double elapsedTotalS;
  @Double() external double elapsedIo1S;
  @Double() external double elapsedBuildS;
  @Double() external double elapsedIngestS;
  @Double() external double elapsedMetadataS;
  @Double() external double elapsedTopologyS;
  @Double() external double elapsedGbwtS;
  @Double() external double elapsedFmIndexS;
  @Double() external double gbwtCpuAvgS;
  @Double() external double gbwtCpuMaxS;

  @Uint64() external int gbwtTotalNodes;
  @Size()   external int gbwtPeakRssKb;
  @Size()   external int gbwtIndexSizeB;

  @Bool()   external bool success;
  external Pointer<Utf8> errorMessage;
}

final class GgqlEvent extends Struct {
  @Int32()  external int kind;
  @Int32()  external int phaseNumber;
  external Pointer<Utf8> text;
  external Pointer<Utf8> detail;
  @Double() external double percent;
  @Double() external double etaS;
  @Double() external double rateMbs;
}

// ── Typedefs das funções nativas ──────────────────────────────────────────────

typedef _CreateNative  = Pointer<Void> Function();
typedef _CreateDart    = Pointer<Void> Function();

typedef _DestroyNative = Void Function(Pointer<Void>);
typedef _DestroyDart   = void Function(Pointer<Void>);

typedef _EventCbNative = Void Function(Pointer<GgqlEvent>, Pointer<Void>);

typedef _IngestNative  = GgqlIngestStats Function(
  Pointer<Void>, Pointer<Utf8>, Int32, Double,
  Bool, Bool, Bool, Bool, Size, Bool, Pointer<Utf8>,
  Pointer<NativeFunction<_EventCbNative>>, Pointer<Void>,
);
typedef _IngestDart    = GgqlIngestStats Function(
  Pointer<Void>, Pointer<Utf8>, int, double,
  bool, bool, bool, bool, int, bool, Pointer<Utf8>,
  Pointer<NativeFunction<_EventCbNative>>, Pointer<Void>,
);

// ── Loader ────────────────────────────────────────────────────────────────────

class GgqlLib {
  late final DynamicLibrary _lib;
  late final _CreateDart  create;
  late final _DestroyDart destroy;
  late final _IngestDart  ingest;

  GgqlLib() {
    _lib = Platform.isAndroid
        ? DynamicLibrary.open('libggql.so')
        : DynamicLibrary.process();

    create  = _lib.lookupFunction<_CreateNative,  _CreateDart>('ggql_ingestor_create');
    destroy = _lib.lookupFunction<_DestroyNative, _DestroyDart>('ggql_ingestor_destroy');
    ingest  = _lib.lookupFunction<_IngestNative,  _IngestDart>('ggql_ingestor_ingest');
  }
}
```

### 2.3 Isolate + callback de progresso

`ggql_ingestor_ingest` é bloqueante. A abordagem correta no Flutter é executá-la
em um `Isolate` separado e enviar eventos de progresso de volta à thread da UI
via `SendPort`.

```dart
// Rodando a ingestão em background

import 'dart:isolate';
import 'dart:ffi';
import 'package:ffi/ffi.dart';

Future<GgqlIngestStats> ingestInBackground({
  required String filepath,
  required String cacheDir,
  void Function(double percent, double etaS)? onProgress,
}) async {
  final receivePort = ReceivePort();

  // Callback nativo → SendPort (NativeCallable é destruído após o ingest)
  late final NativeCallable<_EventCbNative> nativeCb;

  void handleEvent(Pointer<GgqlEvent> ev, Pointer<Void> _) {
    final e = ev.ref;
    if (e.kind == 9 /* GGQL_EV_PROGRESS */) {
      receivePort.sendPort.send({'percent': e.percent, 'eta': e.etaS});
    }
  }

  nativeCb = NativeCallable.listener(handleEvent);

  await Isolate.spawn((args) {
    final lib    = GgqlLib();
    final handle = lib.create();

    final fp  = args['filepath'].toNativeUtf8();
    final cd  = args['cacheDir'].toNativeUtf8();

    final stats = lib.ingest(
      handle, fp, 4, 1.0,
      false, true, true, false, 0, false, cd,
      args['cbPtr'] as Pointer<NativeFunction<_EventCbNative>>,
      nullptr,
    );

    malloc.free(fp); malloc.free(cd);
    lib.destroy(handle);

    // Envia resultado serializado de volta
    (args['port'] as SendPort).send(stats.success);
  }, {
    'filepath' : filepath,
    'cacheDir' : cacheDir,
    'cbPtr'    : nativeCb.nativeFunction,
    'port'     : receivePort.sendPort,
  });

  await for (final msg in receivePort) {
    if (msg is Map) {
      onProgress?.call(msg['percent'] as double, msg['eta'] as double);
    } else {
      receivePort.close();
      nativeCb.close();
      break;
    }
  }
  // Em produção: retornar o stats real via SendPort (serialização via Map)
}
```

> **Atenção**: strings `text` e `detail` de `GgqlEvent` só são válidas *durante*
> a chamada do callback. Converta imediatamente via `.toDartString()` antes de
> armazenar.

### 2.4 Geração automática de bindings

Use [`package:ffigen`](https://pub.dev/packages/ffigen) para gerar os bindings
Dart diretamente do header `ggql_ffi.h`, eliminando o mapeamento manual:

```yaml
# ffigen.yaml
name: GgqlBindings
description: Auto-generated bindings for libggql
output: 'lib/src/ggql_bindings.dart'
headers:
  entry-points:
    - 'include/ffi/ggql_ffi.h'
silence-enum-warning: true   # GgqlCellType/GgqlIngestProfile
```

```bash
dart run ffigen --config ffigen.yaml
```

### 2.5 Bundling via Dart Hooks (estável desde Dart 3.10)

Em vez de copiar manualmente a `.so` para cada plataforma, registre a
`libggql_ffi.so` produzida pelo CMake do engine como `CodeAsset` via um
`hook/build.dart`. O Flutter então a copia para o bundle automaticamente
(em Linux: `build/linux/<arch>/<mode>/bundle/lib/libggql_ffi.so`).

`pubspec.yaml`:
```yaml
dependencies:
  ffi: ^2.2.0
  hooks: ^2.0.0
  code_assets: ^1.1.0

dev_dependencies:
  ffigen: ^19.0.0
```

`hook/build.dart` (apontando para o `build/` do engine via env var):
```dart
import 'dart:io';
import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    final libFile = _resolveLib(input);
    if (!libFile.existsSync()) {
      throw Exception('libggql_ffi.so não encontrada em ${libFile.path}.');
    }
    output.assets.code.add(
      CodeAsset(
        package: input.packageName,
        name: 'libggql_ffi',
        linkMode: DynamicLoadingBundled(),
        file: libFile.uri,
      ),
    );
  });
}

File _resolveLib(BuildInput input) {
  final env = Platform.environment;
  if ((env['GGQL_FFI_LIB'] ?? '').isNotEmpty) {
    return File(env['GGQL_FFI_LIB']!);
  }
  if ((env['GGQL_ENGINE_DIR'] ?? '').isNotEmpty) {
    return File('${env['GGQL_ENGINE_DIR']}/build/libggql_ffi.so');
  }
  final pkgRoot = input.packageRoot.toFilePath();
  return File('$pkgRoot../../c++/ggql_engine/build/libggql_ffi.so');
}
```

O hook não recompila o engine — apenas referencia a `.so` já produzida
pelo `cmake --build build --target ggql_ffi`. Em runtime, `DynamicLibrary
.open('libggql_ffi.so')` resolve via o bundle do Flutter.

### 2.6 Caveats em Linux desktop

**Allocator + dlopen** (resolvido em 2026-05-21): historicamente
`libggql_ffi.so` era linkada contra `libjemalloc` estático, o que causava
dois problemas em `dlopen()` tardio (Dart `flutter test`, Python ctypes):

1. `cannot allocate memory in static TLS block` (Initial-Exec TLS do
   jemalloc não cabe no surplus do processo já iniciado).
2. `free(): invalid pointer` SIGABRT — jemalloc fazia interposition de
   `free()` no processo inteiro, mas ponteiros já alocados via glibc
   malloc pelo consumer (Dart/Python) caíam no `je_free()` que não os
   reconhecia.

**Fix**: o CMakeLists separa o allocator por target:
- Binários standalone (`ggql_repl`, `bench_*`) → jemalloc-vendor (perf
  máxima).
- `libggql_ffi.so` → **mimalloc** (TLS general-dynamic + fallback
  automático para `free()` da libc em ponteiros não-mimalloc).

Consequência: **nenhum `LD_PRELOAD` é necessário** para `flutter run`,
`flutter test`, ou Python ctypes consumindo `libggql_ffi.so`.

Override (em geral desnecessário): `cmake -DGGQL_FFI_ALLOCATOR=glibc`
desabilita o allocator próprio da .so. Para `jemalloc-system` (ainda
exige LD_PRELOAD), use `-DGGQL_FFI_ALLOCATOR=jemalloc-system` apenas em
deploys server-only onde a .so não é dlopened.

**`NativeCallable.listener` é assíncrono** — o handler Dart roda **após**
o callback nativo retornar. Antes do fix de 2026-05-20, `GgqlEvent` em
`dispatch_bridge` era stack-local, gerando ponteiro inválido quando o
handler eventualmente acessava `evPtr.ref` (sintoma: `kind`=garbage). O
engine agora usa `thread_local GgqlEvent out;`, mantendo o struct vivo
pela duração da thread. Strings `text`/`detail` ainda apontam para
buffers `thread_local` que são reusados entre emissões da mesma thread —
para fidelidade total, leia apenas campos POD (kind, phase_number,
percent, eta_s, rate_mbs) ou copie do lado C++.

---

## 3. Integração com Python via `ctypes` / `cffi`

### 3.1 Carregando a biblioteca

```python
# ggql.py
import ctypes, ctypes.util, pathlib

_lib_path = pathlib.Path(__file__).parent / "libggql.so"
_lib = ctypes.CDLL(str(_lib_path))
```

### 3.2 Mapeamento de structs e callbacks

```python
import ctypes

class GgqlIngestStats(ctypes.Structure):
    _fields_ = [
        ("segment_count",         ctypes.c_uint64),
        ("link_count",            ctypes.c_uint64),
        ("node_count",            ctypes.c_uint64),
        ("path_count",            ctypes.c_uint64),
        ("elapsed_total_s",       ctypes.c_double),
        ("elapsed_io1_s",         ctypes.c_double),
        ("elapsed_build_s",       ctypes.c_double),
        ("elapsed_ingest_s",      ctypes.c_double),
        ("elapsed_metadata_s",    ctypes.c_double),
        ("elapsed_topology_s",    ctypes.c_double),
        ("elapsed_gbwt_s",        ctypes.c_double),
        ("elapsed_fm_index_s",    ctypes.c_double),
        ("gbwt_cpu_avg_s",        ctypes.c_double),
        ("gbwt_cpu_max_s",        ctypes.c_double),
        ("gbwt_total_nodes",      ctypes.c_uint64),
        ("gbwt_peak_rss_kb",      ctypes.c_size_t),
        ("gbwt_index_size_b",     ctypes.c_size_t),
        ("success",               ctypes.c_bool),
        ("error_message",         ctypes.c_char_p),
    ]

class GgqlEvent(ctypes.Structure):
    _fields_ = [
        ("kind",         ctypes.c_int32),
        ("phase_number", ctypes.c_int32),
        ("text",         ctypes.c_char_p),
        ("detail",       ctypes.c_char_p),
        ("percent",      ctypes.c_double),
        ("eta_s",        ctypes.c_double),
        ("rate_mbs",     ctypes.c_double),
    ]

# Tipo do callback: void (*)(const GgqlEvent*, void*)
EventCallbackType = ctypes.CFUNCTYPE(
    None,
    ctypes.POINTER(GgqlEvent),
    ctypes.c_void_p,
)

# Assinaturas das funções exportadas
_lib.ggql_ingestor_create.restype  = ctypes.c_void_p
_lib.ggql_ingestor_create.argtypes = []

_lib.ggql_ingestor_destroy.restype  = None
_lib.ggql_ingestor_destroy.argtypes = [ctypes.c_void_p]

_lib.ggql_ingestor_reset.restype  = None
_lib.ggql_ingestor_reset.argtypes = [ctypes.c_void_p]

_lib.ggql_ingestor_ingest.restype  = GgqlIngestStats
_lib.ggql_ingestor_ingest.argtypes = [
    ctypes.c_void_p,   # handle
    ctypes.c_char_p,   # filepath
    ctypes.c_int,      # num_threads
    ctypes.c_double,   # gamma
    ctypes.c_bool,     # verbose
    ctypes.c_bool,     # save_metadata
    ctypes.c_bool,     # build_topology
    ctypes.c_bool,     # build_fm_index
    ctypes.c_size_t,   # max_gbwt_paths
    ctypes.c_bool,     # keep_gbwt_tmp
    ctypes.c_char_p,   # cache_dir
    EventCallbackType, # event_cb (pode ser None → NULL)
    ctypes.c_void_p,   # user_data
]
```

### 3.3 API de alto nível

```python
from dataclasses import dataclass
from typing import Callable, Optional
import ctypes

@dataclass
class IngestResult:
    segment_count: int
    link_count: int
    node_count: int
    path_count: int
    elapsed_total_s: float
    elapsed_topology_s: float
    elapsed_gbwt_s: float
    gbwt_total_nodes: int
    gbwt_peak_rss_kb: int

class GgqlIngestor:
    def __init__(self):
        self._handle = _lib.ggql_ingestor_create()
        if not self._handle:
            raise RuntimeError("Falha ao criar GgqlIngestor")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self._handle:
            _lib.ggql_ingestor_destroy(self._handle)
            self._handle = None

    def ingest(
        self,
        filepath: str,
        cache_dir: str,
        *,
        num_threads: int = 4,
        gamma: float = 1.0,
        build_topology: bool = True,
        build_fm_index: bool = False,
        max_gbwt_paths: int = 0,
        on_progress: Optional[Callable[[float, float], None]] = None,
    ) -> IngestResult:
        cb = None
        if on_progress:
            def _cb(ev_ptr, _ud):
                ev = ev_ptr.contents
                if ev.kind == 9:  # GGQL_EV_PROGRESS
                    on_progress(ev.percent, ev.eta_s)
            cb = EventCallbackType(_cb)

        stats = _lib.ggql_ingestor_ingest(
            self._handle,
            filepath.encode(),
            num_threads,
            gamma,
            False,         # verbose
            True,          # save_metadata
            build_topology,
            build_fm_index,
            max_gbwt_paths,
            False,         # keep_gbwt_tmp
            cache_dir.encode(),
            cb or EventCallbackType(0),
            None,
        )

        if not stats.success:
            msg = stats.error_message
            raise RuntimeError(msg.decode() if msg else "Erro desconhecido na ingestão")

        return IngestResult(
            segment_count    = stats.segment_count,
            link_count       = stats.link_count,
            node_count       = stats.node_count,
            path_count       = stats.path_count,
            elapsed_total_s  = stats.elapsed_total_s,
            elapsed_topology_s = stats.elapsed_topology_s,
            elapsed_gbwt_s   = stats.elapsed_gbwt_s,
            gbwt_total_nodes = stats.gbwt_total_nodes,
            gbwt_peak_rss_kb = stats.gbwt_peak_rss_kb,
        )
```

### 3.4 Uso em scripts e notebooks

```python
# Uso simples
with GgqlIngestor() as ing:
    result = ing.ingest(
        "/data/graph.gfa",
        "/tmp/ggql_cache",
        build_topology=True,
        on_progress=lambda pct, eta: print(f"  {pct:.1f}%  ETA {eta:.1f}s"),
    )
    print(f"Segmentos: {result.segment_count:,}")
    print(f"Tempo total: {result.elapsed_total_s:.2f}s")
```

```python
# Em um Jupyter notebook — barra de progresso com tqdm
from tqdm.auto import tqdm

with GgqlIngestor() as ing:
    bar = tqdm(total=100, desc="Ingestão GFA")
    last = 0.0

    def on_progress(pct, eta):
        nonlocal last
        bar.update(pct - last)
        last = pct

    result = ing.ingest("graph.gfa", "/tmp/cache", on_progress=on_progress)
    bar.close()
```

### 3.5 Alternativa: `cffi` (mais robusta para projetos maiores)

`cffi` compila o header diretamente, eliminando a declaração manual dos structs
e sendo mais resistente a problemas de alinhamento em arquiteturas diferentes:

```python
import cffi

ffi = cffi.FFI()
ffi.cdef(open("include/ffi/ggql_ffi.h").read()
         .replace("#ifdef __cplusplus", "// #ifdef __cplusplus")
         .replace("#endif", "// #endif")
         .replace("extern \"C\" {", "")
         .replace("}", "", 1))

lib = ffi.dlopen("./libggql.so")
handle = lib.ggql_ingestor_create()
```

Para distribuição limpa, compile via `cffi` ABI mode ou `cdef`+`verify` em
tempo de instalação do pacote Python.

---

## 4. Padrões comuns às duas integrações

### 4.1 Lifecycle do handle

```
create() → ingest() [→ reset() → ingest()...] → destroy()
```

`reset()` limpa estado interno sem realocar; reutilize o handle para processar
múltiplos arquivos em sequência sem custo de criação.

### 4.2 Tratamento de erros

| Condição                    | Comportamento                                                      |
|-----------------------------|--------------------------------------------------------------------|
| `create()` retorna `NULL`   | Não há handle; não chamar nenhuma outra função                    |
| `success == false`          | `error_message` contém descrição; copie imediatamente antes de próxima chamada FFI |
| Exceção C++ interna         | Capturada; nunca atravessa a fronteira ABI                        |

### 4.3 Thread safety do callback

O `EventBridge` já é `thread_local` no lado C++. Do lado do consumidor:

- **Flutter**: use `NativeCallable.listener` (thread-safe por design) e envie
  eventos para o Isolate da UI via `SendPort`.
- **Python**: o GIL protege o callback Python, mas o ingest roda em C++
  (libera o GIL implicitamente). Para callbacks assíncronos, use `threading.Event`
  ou uma `queue.Queue`.

### 4.4 Lifetime das strings em `GgqlEvent`

`text` e `detail` apontam para buffers internos válidos **somente durante a
chamada do callback**. Regra: copie antes de retornar.

```dart
// Dart
final label = ev.ref.text.toDartString(); // copia imediata
```

```python
# Python — ctypes.c_char_p já copia ao ler o atributo
label = ev.contents.text  # bytes object, já copiado
```

---

## 5. Decisão: `ctypes` vs `cffi` vs bindings gerados

| Critério              | `ctypes`             | `cffi`                  | `ffigen` (Dart)       |
|-----------------------|----------------------|-------------------------|-----------------------|
| Setup                 | Zero dependências    | Requer `cffi` instalado | Requer `ffigen` pkg   |
| Manutenção            | Manual (risco drift) | Semi-auto (parseia .h)  | Totalmente automático |
| Alinhamento de struct | Risco em ARM         | Correto (usa clang)     | Correto               |
| Performance overhead  | Baixo                | Baixo                   | Mínimo                |
| Recomendado para      | Scripts rápidos      | Projetos Python sérios  | Flutter/Dart          |

**Recomendação**: use `ffigen` para Flutter (já gera código Dart idiomático a
partir do `ggql_ffi.h`) e `cffi` para o wrapper Python de produção.

---

## 6. API de Query (Rounds 1–3, 2026-05-20)

O motor expõe via FFI três blocos para uso pós-ingest: **lifecycle do
engine**, **execução de query (com paginação ou streaming)** e
**introspecção / controle**. Todos os símbolos seguem o padrão de handle
opaco + erro por chamada (`ggql_engine_last_error`); os structs novos
têm tamanho/layout estável.

### 6.1 Lifecycle do engine

```c
GgqlEngineHandle ggql_engine_load(const char* cache_dir);                  // disco
GgqlEngineHandle ggql_engine_create(GgqlIngestorHandle, const char*);      // de ingestor vivo
void             ggql_engine_destroy(GgqlEngineHandle);
const char*      ggql_engine_last_error(GgqlEngineHandle);                 // thread_local
```

> **Importante (Round 1.A)**: `ggql_engine_load` **NÃO carrega o
> FM-Index** — defere para a primeira `SEARCH KMER` / `WHERE
> contains_kmer`. Em GRCh38-464 (`.idx` ~14 GiB), `engine_load` caiu de
> **~8,5 s para 9 ms** (medido em `ffi_refinement_plan.md §6.A`). UI
> Flutter pode abrir um pangenoma humano instantaneamente; o custo do
> FM-Index (uma única vez) ocorre na primeira query que precisar dele.
> Cancelar via `ggql_engine_cancel` (§6.6) também aborta lazy-loads em
> progresso na próxima verificação.

**Flutter (Dart):**
```dart
typedef _LoadNative = Pointer<Void> Function(Pointer<Utf8>);
typedef _LoadDart   = Pointer<Void> Function(Pointer<Utf8>);

final load    = lib.lookupFunction<_LoadNative, _LoadDart>('ggql_engine_load');
final destroy = lib.lookupFunction<Void Function(Pointer<Void>),
                                   void Function(Pointer<Void>)>('ggql_engine_destroy');

final eng = load('/path/to/cache.idx'.toNativeUtf8());
// ... use o engine ...
destroy(eng);
```

**Python (ctypes):**
```python
lib.ggql_engine_load.argtypes    = [ctypes.c_char_p]
lib.ggql_engine_load.restype     = ctypes.c_void_p
lib.ggql_engine_destroy.argtypes = [ctypes.c_void_p]

eng = lib.ggql_engine_load(b'/path/to/cache.idx')
try:
    ...
finally:
    lib.ggql_engine_destroy(eng)
```

### 6.2 Executar query e ler resultado

```c
GgqlResultHandle ggql_engine_execute(GgqlEngineHandle, const char* query,
                                     uint32_t limit, uint32_t offset);
int64_t      ggql_result_row_count(GgqlResultHandle);
int64_t      ggql_result_col_count(GgqlResultHandle);
int64_t      ggql_result_total_hits(GgqlResultHandle);
bool         ggql_result_total_truncated(GgqlResultHandle);
const char*  ggql_result_col_name(GgqlResultHandle, int32_t col);
GgqlCellType ggql_result_cell_type(GgqlResultHandle, int32_t row, int32_t col);
int64_t      ggql_result_cell_int(...);
double       ggql_result_cell_double(...);
const char*  ggql_result_cell_string(...);
void         ggql_result_free(GgqlResultHandle);
```

> `limit=0` usa o default FFI de 100 linhas. Para counts exatos
> estruturais, `total_hits` já não trunca em 1M (Round 1.#4 do engine);
> `total_truncated` indica truncamento explícito (SEARCH KMER ou cap
> per-engine — §6.6).

**Python (ctypes):**
```python
lib.ggql_engine_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                    ctypes.c_uint32, ctypes.c_uint32]
lib.ggql_engine_execute.restype  = ctypes.c_void_p
# (assumir argtypes/restype dos result_* análogos)

r = lib.ggql_engine_execute(eng, b'FIND SEGMENT WHERE length > 1000 RETURN id, name',
                            100, 0)
if not r:
    err = lib.ggql_engine_last_error(eng)
    raise RuntimeError(err.decode())
rows = lib.ggql_result_row_count(r)
for i in range(rows):
    seg_id = lib.ggql_result_cell_int(r, i, 0)
    name   = lib.ggql_result_cell_string(r, i, 1)
    print(seg_id, name.decode() if name else None)
lib.ggql_result_free(r)
```

**Flutter (Dart)** — typedefs análogos; ver `package:ffi` para conversão
`Pointer<Utf8>.toDartString()` em `cell_string` (copia o conteúdo antes
de `result_free`).

### 6.3 Capabilities — introspecção sem rodar query (Round 1.B)

Diga ao usuário **antes** de tentar uma query: este engine tem
FM-Index? gBWT? snarls? Quantos segmentos?

```c
typedef struct {
    bool has_metadata, has_topology, has_fm_index, has_gbwt, has_snarls;
    uint64_t segment_count, edge_count, path_count;
} GgqlEngineCapabilities;

int32_t ggql_engine_capabilities(GgqlEngineHandle, GgqlEngineCapabilities*);
```

`has_fm_index = is_loaded() OR fs::exists(fm_index.bin)` — reflete "vai
funcionar" mesmo antes do lazy-load.

**Python:**
```python
class Caps(ctypes.Structure):
    _fields_ = [
        ('has_metadata', ctypes.c_bool), ('has_topology', ctypes.c_bool),
        ('has_fm_index', ctypes.c_bool), ('has_gbwt', ctypes.c_bool),
        ('has_snarls',   ctypes.c_bool),
        ('segment_count', ctypes.c_uint64),
        ('edge_count',    ctypes.c_uint64),
        ('path_count',    ctypes.c_uint64),
    ]
lib.ggql_engine_capabilities.argtypes = [ctypes.c_void_p, ctypes.POINTER(Caps)]
lib.ggql_engine_capabilities.restype  = ctypes.c_int32

caps = Caps()
lib.ggql_engine_capabilities(eng, ctypes.byref(caps))
print(f"S={caps.segment_count} E={caps.edge_count} P={caps.path_count} "
      f"fm={caps.has_fm_index} gbwt={caps.has_gbwt} snarls={caps.has_snarls}")
```

**Flutter:** declare `final class Caps extends Struct` com os mesmos
campos e use `Pointer<Caps>` no `lookupFunction`.

**Caso de uso típico** (UI degrade): cinzar botão "Buscar k-mer" quando
`has_fm_index == false`; cinzar "Bolhas" quando `has_snarls == false`.

### 6.4 Validar query (parse-only) — Round 2.D

Para REPL / IDE: marcar erro enquanto o usuário digita, sem executar.

```c
typedef struct {
    bool valid;
    int32_t error_line, error_col;     // -1 atualmente (parser PEGTL)
    const char* error_message;         // thread_local; copie se reter
} GgqlParseResult;

int32_t ggql_engine_validate(GgqlEngineHandle, const char* query,
                             GgqlParseResult* out);
// retorno: 0 = parse OK; 1 = falhou (out preenchido); -1 = args inválidos
```

**Python:**
```python
class ParseResult(ctypes.Structure):
    _fields_ = [('valid', ctypes.c_bool),
                ('error_line', ctypes.c_int32),
                ('error_col',  ctypes.c_int32),
                ('error_message', ctypes.c_char_p)]
lib.ggql_engine_validate.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                     ctypes.POINTER(ParseResult)]
lib.ggql_engine_validate.restype  = ctypes.c_int32

pr = ParseResult()
rc = lib.ggql_engine_validate(eng, b'FIND XYZZY', ctypes.byref(pr))
# rc == 1, pr.valid == False, pr.error_message.decode() = "Erro de sintaxe..."
```

Custo: ~µs em queries comuns — pode rodar a cada keystroke sem lag.

### 6.5 Schema introspection (Round 2.E)

```c
const char* const* ggql_engine_list_entities(int32_t* out_count);
const char* const* ggql_engine_describe_entity(const char* entity_name,
                                               int32_t* out_count);
```

Pointers são literais estáticos (válidos pelo tempo de vida da lib).
Entidades: `SEGMENT, EDGE, PATH, BUBBLE, TIP, REPEAT`. Nome
case-insensitive ASCII; `NULL` se desconhecida.

**Python — popular um dropdown de entidade + colunas:**
```python
lib.ggql_engine_list_entities.argtypes = [ctypes.POINTER(ctypes.c_int32)]
lib.ggql_engine_list_entities.restype  = ctypes.POINTER(ctypes.c_char_p)
lib.ggql_engine_describe_entity.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int32)]
lib.ggql_engine_describe_entity.restype  = ctypes.POINTER(ctypes.c_char_p)

n = ctypes.c_int32(0)
arr = lib.ggql_engine_list_entities(ctypes.byref(n))
entities = [arr[i].decode() for i in range(n.value)]
# ['SEGMENT', 'EDGE', 'PATH', 'BUBBLE', 'TIP', 'REPEAT']

cols_ptr = lib.ggql_engine_describe_entity(b'BUBBLE', ctypes.byref(n))
cols = [cols_ptr[i].decode() for i in range(n.value)]
# ['source', 'sink', 'branch_count', 'internal_node_count', 'has_cycles']
```

**Caso de uso**: gerar campos de autocomplete num REPL, ou colunas de
uma `DataGrid` em Flutter sem hardcoded strings.

### 6.6 Cancel cooperativo + cap per-engine (Round 3.F + G)

```c
void ggql_engine_cancel(GgqlEngineHandle);
void ggql_engine_set_max_total_hits(GgqlEngineHandle, uint64_t cap);  // 0 = default
```

**Cancel** é cooperativo: a thread executando a query checa um
`std::atomic<bool>` em cada hit (~ns overhead). Outra thread (tipicamente
UI) chama `engine_cancel`; a query aborta e retorna **resultado
parcial** (`total_hits` reflete o que foi contado até ali).

> **Comportamento exato do flag** (C6, alinhado em 2026-05-22): o
> `cancel_requested` é **resetado no início de cada `engine_execute`**.
> Armar `engine_cancel` quando nenhuma query está rodando é **no-op** —
> o cancel só morde para a query atualmente em vôo. Design conservador:
> evita cancel "preso" entre queries se o consumer esquecer de re-armar.
> Para parar uma query, o `cancel` precisa ser chamado **enquanto** ela
> roda (típico: thread A em `execute()`, thread B chama `cancel()`).

**Python — botão "Cancelar" no notebook:**
```python
import threading, time
def cancel_after(eng, secs):
    time.sleep(secs)
    lib.ggql_engine_cancel(eng)

t = threading.Thread(target=cancel_after, args=(eng, 0.05))
t.start()
r = lib.ggql_engine_execute(eng, b'SEARCH KMER "AAAAAAAA"', 1000, 0)
t.join()
print(lib.ggql_result_total_hits(r), "hits parciais")  # ~16000 vs scan completo
```

**Flutter:** o `Isolate` que rodou `execute` pode ouvir um `ReceivePort`;
quando a UI envia "cancel", o `Isolate` chama `engine_cancel` (handle
compartilhado via `Pointer<Void>.address`).

**Cap per-engine**: override de `kMaxTotalHitsCount`. Após o Round 1.#4
do engine, counts estruturais já não truncam — o cap só morde
`SEARCH KMER`. Use para limitar varreduras de k-mer DoS-prone num REPL.

```python
lib.ggql_engine_set_max_total_hits(eng, 50)   # SEARCH KMER trunca em ~50
# ...
lib.ggql_engine_set_max_total_hits(eng, 0)    # restaura default
```

### 6.7 Streaming via callback paginado (Round 3.H)

Wrapper sobre `execute()` que re-executa em páginas, chama um callback
por linha, para se o callback retorna `false`. **Não é true streaming**
— cada página materializa até `page_size` linhas (default 1024). Útil
quando o resultado total é grande (chm13 `FIND SEGMENT` = 4,29M
linhas) e o consumer não quer gerenciar offset.

> **Semântica do retorno**: o valor retornado é o número de linhas para
> as quais o callback retornou `true`. A linha que dispara o stop
> (callback retorna `false`) NÃO é contada — só é contado se o cb
> tiver retornado `true` para ela. Logo, para "parar após processar N
> linhas", retorne `true` em todas e mantenha o contador no
> `user_data`; ou retorne `false` na linha N+1, ficando com retorno
> = N.

```c
typedef bool (*GgqlRowCallback)(GgqlResultHandle, int32_t row_in_page,
                                int64_t global_row, void* user_data);
int64_t ggql_engine_execute_streaming(GgqlEngineHandle, const char* query,
                                      uint32_t page_size, GgqlRowCallback,
                                      void* user_data);
```

**Python — exportar para CSV row-a-row:**
```python
import csv
ROW_CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_int32,
                          ctypes.c_int64, ctypes.c_void_p)
lib.ggql_engine_execute_streaming.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                              ctypes.c_uint32, ROW_CB, ctypes.c_void_p]
lib.ggql_engine_execute_streaming.restype  = ctypes.c_int64

with open('segments.csv', 'w') as f:
    writer = csv.writer(f); writer.writerow(['id', 'name', 'length'])
    @ROW_CB
    def emit(r, row, _global, _user):
        writer.writerow([
            lib.ggql_result_cell_int(r, row, 0),
            (lib.ggql_result_cell_string(r, row, 1) or b'').decode(),
            lib.ggql_result_cell_int(r, row, 2),
        ])
        return True
    n = lib.ggql_engine_execute_streaming(
        eng, b'FIND SEGMENT WHERE length > 1000 RETURN id, name, length',
        4096, emit, None)
    print(f"exportadas {n} linhas")
```

**Flutter — converter para `Stream<Row>`**: rodar
`execute_streaming` num `Isolate` que empurra linhas via `SendPort`; a
UI consome como um `Stream<Map<String, Object?>>`. Cancelar = enviar
"stop" → callback retorna `false` na próxima invocação.

### 6.8 Ingest granular (Round 1.C)

Para casos fora dos 4 perfis nomeados (`PANGENOME`/`METAGENOMICS`/
`ASSEMBLY`/`COMPARATIVE`):

```c
GgqlIngestStats ggql_ingestor_ingest_granular(
    handle, filepath, num_threads, gamma, verbose,
    /*save_metadata=*/true, /*build_topology=*/true,
    /*build_fm_index=*/false,
    /*build_gbwt=*/false,   // ← antes acoplado a build_topology
    /*build_snarls=*/true,  // ← idem
    max_gbwt_paths, keep_gbwt_tmp, cache_dir, event_cb, user_data);
```

Útil p.ex. para "topology + snarls sem gBWT" ou "gBWT sem snarls" —
combinações inalcançáveis pelos perfis fixos. Eventos e
`GgqlIngestStats` retornados são os mesmos da `ggql_ingestor_ingest`
legada.

---

## 7. Estado atual da API e roadmap

Após Rounds 1–3, o C ABI cobre o ciclo completo de uso pós-ingest:
ingest, lifecycle, query síncrona, paginação, streaming-paginado,
introspecção de schema/capabilities, cancel cooperativo e cap
per-engine. Total exposto: **31 símbolos** (`nm -D --defined-only
libggql_ffi.so` para verificar).

**Pendências de roadmap** (não-críticas — pagination/cancel cobrem 80%
dos casos):

- **True streaming**: refactor do planner para emitir rows via callback
  diretamente (em vez de materializar páginas). Médio porte; valor
  baixo–médio.
- **Boundary names em FIND BUBBLE**: hoje `RETURN source, sink` dá ID
  interno pós-remap — para o nome GFA original, join com
  `FIND SEGMENT RETURN id, name` (receita em `ggql_vs_vg.md §9.bis`).
  Gap do query layer, não FFI.
- **Export de formato**: GGQL → GFA / FASTA / BED / JSON-Lines.
- **Portabilidade macOS / ARM** (`L5` no plano).
- **`size_t struct_size` em todos os structs**: padrão de detecção de
  versão sem recompilar (proposto na versão anterior deste doc).
  Adicionado, dá ao consumer flexibilidade vs uma lib mais nova.

Para o histórico completo de implementação (medições antes/depois,
bugs descobertos via tests, decisões de design), ver
**`docs/ffi_refinement_plan.md`** §6–§8.

---

## 8. Smoke tests para validação Flutter (Rounds 2–3)

Sequência de validação manual para o cliente Flutter exercitar as
Rounds 2 e 3 da FFI. A ordem vai do mais barato/seguro (introspecção
pura, sem rodar query) ao mais complexo (cancel concorrente), de modo
que uma falha cedo limita o escopo do diagnóstico. Round 1 (ingest +
load lazy) já está validada em produção (`project_ffi_flutter_validated`).

### 8.1 Setup

| Item | Valor |
|---|---|
| `.so` esperada | `libggql_ffi.so` (Linux/Android) ou `.dylib`/`.dll` |
| Símbolos exportados | 31 (confirmar com `nm -D --defined-only libggql_ffi.so \| grep -c ' T ggql_'`) |
| Idx de teste recomendado | `data/chm13_subset_sm.gfa.idx` (≈131k segmentos, tem fm_index + topology + gBWT + snarls — exercita todos os blocos) |
| Pré-requisito Linux | Nenhum — `libggql_ffi.so` usa mimalloc, dlopen-safe (ver §2.6) |

Antes de cada passo: `final eng = lib.load(idxPath.toNativeUtf8());`
`check(eng != nullptr);`. Ao final: `lib.destroy(eng);`.

### 8.2 Sequência (8 passos)

**Passo 1 — `list_entities` (Round 2.E)**
- Chame `ggql_engine_list_entities(&n)`. Esperado: `n == 6`, array
  na ordem `['SEGMENT','EDGE','PATH','BUBBLE','TIP','REPEAT']`.
- Regressão: ponteiro NULL ou `n != 6` indica que `kEntityNames` em
  `src/ffi/ffi.cpp` divergiu — a lista é canônica.

**Passo 2 — `describe_entity` (Round 2.E)**
- `describe_entity("SEGMENT", &n)` → `n == 5`, colunas começando em
  `id`, terminando em `depth`.
- `describe_entity("BUBBLE", &n)` → `n == 5`, primeira coluna
  `source`, última `has_cycles`.
- `describe_entity("segment", &n)` → case-insensitive, `n == 5`.
- `describe_entity("XYZZY", &n)` → retorna `nullptr`, `n == 0`.
- Regressão: usar esses nomes para popular um `DataGrid` no app —
  qualquer column-name mismatch surge aqui.

**Passo 3 — `validate` (Round 2.D)**
- `validate(eng, "FIND SEGMENT", &pr)` → retorno `0`, `pr.valid ==
  true`, `pr.error_message == nullptr`.
- `validate(eng, "FIND XYZZY", &pr)` → retorno `1`, `pr.valid ==
  false`, `pr.error_message != nullptr` (copie imediatamente — é
  `thread_local`).
- `validate(eng, nullptr, &pr)` → retorno `-1`.
- Regressão: chame `validate` a cada keystroke num REPL/IDE — não
  deve adicionar lag perceptível (~µs).

**Passo 4 — `capabilities` (Round 1.B, ainda dentro do mesmo run)**
- `capabilities(eng, &caps)` → retorno `0`. Para chm13_subset_sm:
  `has_metadata=true`, `has_topology=true`, `has_fm_index=true`,
  `segment_count > 0`, `edge_count > 0`.
- Use o resultado para cinzar botões na UI: "Buscar k-mer" só se
  `has_fm_index`, "Bolhas" só se `has_snarls`.
- Regressão: idx sem `fm_index.bin` deve dar `has_fm_index=false`
  sem falhar.

**Passo 5 — `execute` baseline (sanidade do path normal)**
- `execute(eng, "FIND SEGMENT WHERE length > 10000 RETURN id, name,
  length", 5, 0)` → handle não-NULL, `row_count == 5` (em
  chm13_subset_sm; varia por idx).
- Itere as 5 linhas e leia via `cell_type` + `cell_int`/`cell_string`.
- Esse passo confirma que o caminho `execute → result_*` funciona
  antes de partir pro streaming.

**Passo 6 — `execute_streaming` (Round 3.H)**
- Conte um baseline com `execute(..., 1_000_000, 0)` → `row_count =
  N`.
- Rode `execute_streaming(..., page_size=10, cb, ud)` com `cb` que
  retorna `true` sempre e incrementa um contador. Esperado: retorno
  da função `== N`, contador `== N`, `global_row` monotônico
  crescente sem saltos.
- Rode de novo com `cb` que retorna `false` na linha `global_row ==
  2`. Esperado: retorno `== 2`, `cb` chamado 3 vezes. **A linha que
  retornou `false` NÃO conta no retorno** (ver §6.7 — semântica do
  retorno).
- `execute_streaming(..., page_size=0, ...)` → usa default 1024, mesmo
  resultado do baseline.
- Regressão para o cliente Flutter: o Isolate que consome o stream
  deve mapear `cb` para `SendPort.send(row)` e aceitar `stop` da UI.

**Passo 7 — `set_max_total_hits` (Round 3.G)**
- `set_max_total_hits(eng, 5)`. Rode `execute(eng, "SEARCH KMER
  \"AA\" RETURN id", 100, 0)`. Esperado: `total_hits <= 6` (cap+1
  porque `++total_hits` ocorre antes do check), `total_truncated ==
  true`.
- `set_max_total_hits(eng, 0)`. Rode a mesma query. Esperado:
  `total_hits > 6` (default ~1M).
- `set_max_total_hits(nullptr, 100)` deve ser no-op (não crashar).
- Regressão: cap só morde `SEARCH KMER` e queries WHERE complexas
  pós-Round 1.#4 — `FIND SEGMENT` puro NÃO trunca em 5 mesmo com
  cap=5.

**Passo 8 — `cancel` concorrente (Round 3.F)**
- Capture baseline: `execute(eng, "SEARCH KMER \"AA\" RETURN id",
  100, 0)` → `baseline_hits`.
- Dispare uma thread/Isolate que dorme ~50µs e chama
  `cancel(eng)`. Rode `execute` da mesma query no main. Esperado:
  retorno é handle válido (não-NULL), `total_hits <= baseline_hits`.
  Em queries muito rápidas a thread pode chegar tarde — aceitar
  igualdade.
- `cancel(eng)` ANTES de `execute` é resetado no início da query
  (ffi.cpp:684). Logo: `cancel` armado pré-`execute` → próximo run
  completo normal. Esse é o comportamento correto (evita cancel
  "preso" entre queries) e contraria a redação do §6.6 ("o próximo
  execute observa o flag e pode abortar imediatamente") — atualizar
  ou aceitar essa divergência.
- `cancel(nullptr)` é no-op.
- Regressão Flutter: handle do engine compartilhado entre Isolates
  via `Pointer<Void>.address`; UI envia "cancel" ao Isolate worker,
  que chama `cancel(eng)`.

### 8.3 Critérios de regressão

| Sintoma | Suspeita primária |
|---|---|
| `list_entities` devolve `n != 6` | `kEntityNames` em `ffi.cpp` divergiu de `entity_columns.hpp` |
| `describe_entity` retorna ponteiros que segfault ao ler | Build com sanitizer diferente; rebuild limpo |
| `validate` aceita query nonsense | `grammar::parse_ggql` regrediu |
| `capabilities.has_fm_index=false` num idx com `fm_index.bin` | `cache_dir_for_fm` não populado em `engine_load` |
| `execute_streaming` retorno `!= N` para callback sempre-true | Off-by-one no `total_emitted` da ffi |
| `set_max_total_hits` não trunca | `ScopedContext` não instala override (ffi.cpp:685) |
| `cancel` concorrente nunca para | `cancel_requested` não está sendo lido em `Paginator::advance` |

> Após a sequência completa, atualize a memória do projeto
> (`project_ffi_flutter_validated`) com a data e quaisquer
> divergências encontradas para histórico de regressões.

### 8.4 Esqueleto `flutter test`

Coloque em `test/ffi/smoke_round23_test.dart` no projeto Flutter.
Assume bindings gerados via `ffigen` num arquivo
`lib/src/ggql_bindings.dart` com a classe `GgqlBindings` (padrão).
Adapte os nomes se você renomeou.

Linux: rode com `GGQL_TEST_IDX=/path/to/chm13_subset_sm.gfa.idx
flutter test test/ffi/`. `LD_PRELOAD` não é mais necessário desde
2026-05-21 (`libggql_ffi.so` usa mimalloc, dlopen-safe; ver §2.6).

```dart
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:<seu_pkg>/src/ggql_bindings.dart';

void main() {
  late DynamicLibrary lib;
  late GgqlBindings g;
  late Pointer<Void> eng;

  setUpAll(() {
    final libPath = Platform.environment['GGQL_FFI_LIB']
        ?? 'libggql_ffi.so';
    lib = DynamicLibrary.open(libPath);
    g   = GgqlBindings(lib);

    final idx = Platform.environment['GGQL_TEST_IDX']
        ?? (throw StateError('defina GGQL_TEST_IDX'));
    final p = idx.toNativeUtf8();
    eng = g.ggql_engine_load(p.cast()).cast();
    malloc.free(p);
    expect(eng.address, isNot(0), reason: 'engine_load deve devolver handle');
  });

  tearDownAll(() {
    g.ggql_engine_destroy(eng.cast());
  });

  // Passo 1
  test('list_entities → 6 entidades canônicas', () {
    final n = calloc<Int32>();
    final arr = g.ggql_engine_list_entities(n);
    expect(n.value, 6);
    expect(arr[0].cast<Utf8>().toDartString(), 'SEGMENT');
    expect(arr[3].cast<Utf8>().toDartString(), 'BUBBLE');
    expect(arr[5].cast<Utf8>().toDartString(), 'REPEAT');
    calloc.free(n);
  });

  // Passo 2
  test('describe_entity (SEGMENT, BUBBLE, case-insensitive, NULL)', () {
    final n = calloc<Int32>();
    final seg = g.ggql_engine_describe_entity('SEGMENT'.toNativeUtf8().cast(), n);
    expect(n.value, 5);
    expect(seg[0].cast<Utf8>().toDartString(), 'id');
    expect(seg[4].cast<Utf8>().toDartString(), 'depth');

    final bub = g.ggql_engine_describe_entity('BUBBLE'.toNativeUtf8().cast(), n);
    expect(n.value, 5);
    expect(bub[0].cast<Utf8>().toDartString(), 'source');
    expect(bub[4].cast<Utf8>().toDartString(), 'has_cycles');

    g.ggql_engine_describe_entity('segment'.toNativeUtf8().cast(), n);
    expect(n.value, 5, reason: 'case-insensitive');

    final none = g.ggql_engine_describe_entity('XYZZY'.toNativeUtf8().cast(), n);
    expect(none.address, 0);
    expect(n.value, 0);
    calloc.free(n);
  });

  // Passo 3
  test('validate aceita queries válidas e recusa inválidas', () {
    final pr = calloc<GgqlParseResult>();

    var rc = g.ggql_engine_validate(eng.cast(),
        'FIND SEGMENT'.toNativeUtf8().cast(), pr);
    expect(rc, 0);
    expect(pr.ref.valid, true);

    rc = g.ggql_engine_validate(eng.cast(),
        'FIND XYZZY'.toNativeUtf8().cast(), pr);
    expect(rc, 1);
    expect(pr.ref.valid, false);
    expect(pr.ref.error_message.address, isNot(0));

    rc = g.ggql_engine_validate(eng.cast(), nullptr, pr);
    expect(rc, -1);
    calloc.free(pr);
  });

  // Passo 4
  test('capabilities reflete metadata/topology/fm_index', () {
    final caps = calloc<GgqlEngineCapabilities>();
    expect(g.ggql_engine_capabilities(eng.cast(), caps), 0);
    expect(caps.ref.has_metadata, true);
    expect(caps.ref.has_topology, true);
    expect(caps.ref.has_fm_index, true);
    expect(caps.ref.segment_count, greaterThan(0));
    calloc.free(caps);
  });

  // Passo 5
  test('execute baseline: FIND SEGMENT WHERE length > 10000 LIMIT 5', () {
    final q = 'FIND SEGMENT WHERE length > 10000 RETURN id, name, length';
    final r = g.ggql_engine_execute(eng.cast(), q.toNativeUtf8().cast(), 5, 0);
    expect(r.address, isNot(0));
    expect(g.ggql_result_row_count(r), 5);
    expect(g.ggql_result_col_count(r), 3);
    g.ggql_result_free(r);
  });

  // Passo 6
  test('execute_streaming: callback emite todas as linhas; false para', () {
    final q = 'FIND SEGMENT WHERE length > 10000 RETURN id, name, length';
    final baseline = g.ggql_engine_execute(eng.cast(),
        q.toNativeUtf8().cast(), 1000000, 0);
    final expectedRows = g.ggql_result_row_count(baseline);
    g.ggql_result_free(baseline);
    expect(expectedRows, greaterThan(2));

    // Callback que conta tudo
    int count = 0;
    int lastGlobal = -1;
    final cb = NativeCallable<
        Bool Function(Pointer<Void>, Int32, Int64, Pointer<Void>)>.isolateLocal(
      (Pointer<Void> r, int rowInPage, int global, Pointer<Void> ud) {
        expect(global, lastGlobal + 1, reason: 'global_row monotônico');
        lastGlobal = global;
        ++count;
        return true;
      },
      exceptionalReturn: false,
    );
    final emitted = g.ggql_engine_execute_streaming(eng.cast(),
        q.toNativeUtf8().cast(), 10, cb.nativeFunction, nullptr);
    expect(emitted, expectedRows);
    expect(count, expectedRows);
    cb.close();

    // Callback que para na 3ª linha (global_row == 2)
    int callCount = 0;
    final stopper = NativeCallable<
        Bool Function(Pointer<Void>, Int32, Int64, Pointer<Void>)>.isolateLocal(
      (Pointer<Void> r, int rowInPage, int global, Pointer<Void> ud) {
        ++callCount;
        return global < 2;
      },
      exceptionalReturn: false,
    );
    final partial = g.ggql_engine_execute_streaming(eng.cast(),
        q.toNativeUtf8().cast(), 10, stopper.nativeFunction, nullptr);
    expect(partial, 2, reason: 'linha que retornou false não conta');
    expect(callCount, 3);
    stopper.close();
  });

  // Passo 7
  test('set_max_total_hits trunca SEARCH KMER e restaura', () {
    g.ggql_engine_set_max_total_hits(eng.cast(), 5);
    var r = g.ggql_engine_execute(eng.cast(),
        'SEARCH KMER "AA" RETURN id'.toNativeUtf8().cast(), 100, 0);
    expect(g.ggql_result_total_hits(r), lessThanOrEqualTo(6));  // cap+1
    expect(g.ggql_result_total_truncated(r), true);
    g.ggql_result_free(r);

    g.ggql_engine_set_max_total_hits(eng.cast(), 0);
    r = g.ggql_engine_execute(eng.cast(),
        'SEARCH KMER "AA" RETURN id'.toNativeUtf8().cast(), 100, 0);
    expect(g.ggql_result_total_hits(r), greaterThan(6));
    g.ggql_result_free(r);
  });

  // Passo 8 — cancel concorrente via Isolate
  test('cancel cooperativo aborta SEARCH KMER em outra thread', () async {
    final qPtr = 'SEARCH KMER "AA" RETURN id'.toNativeUtf8().cast<Char>();
    final baseline = g.ggql_engine_execute(eng.cast(), qPtr, 100, 0);
    final baselineHits = g.ggql_result_total_hits(baseline);
    g.ggql_result_free(baseline);

    // Dispara cancel em ~50µs via Future.microtask
    final engAddr = eng.address;
    Future.microtask(() async {
      await Future.delayed(const Duration(microseconds: 50));
      g.ggql_engine_cancel(Pointer<Void>.fromAddress(engAddr).cast());
    });
    final r = g.ggql_engine_execute(eng.cast(), qPtr, 100, 0);
    expect(r.address, isNot(0), reason: 'cancel devolve handle parcial');
    expect(g.ggql_result_total_hits(r), lessThanOrEqualTo(baselineHits));
    g.ggql_result_free(r);

    // cancel(NULL) é no-op
    g.ggql_engine_cancel(nullptr);
  });
}
```

### 8.5 Esqueleto da tela de debug no app

Widget standalone para ter um "smoke runner" dentro do app durante
desenvolvimento. Útil para exercitar Isolate/UI thread (algo que o
`flutter test` headless não cobre).

```dart
import 'dart:async';
import 'dart:ffi';
import 'package:flutter/material.dart';
import 'package:ffi/ffi.dart';
import 'package:<seu_pkg>/src/ggql_bindings.dart';

class _Step {
  final String name;
  final Future<void> Function() run;
  _Step(this.name, this.run);
}

enum _Status { pending, running, ok, fail }

class FfiSmokeScreen extends StatefulWidget {
  final String idxPath;
  final GgqlBindings g;
  const FfiSmokeScreen({super.key, required this.idxPath, required this.g});

  @override
  State<FfiSmokeScreen> createState() => _FfiSmokeScreenState();
}

class _FfiSmokeScreenState extends State<FfiSmokeScreen> {
  late Pointer<Void> _eng;
  final Map<String, (_Status, String)> _results = {};
  late final List<_Step> _steps;

  @override
  void initState() {
    super.initState();
    final p = widget.idxPath.toNativeUtf8();
    _eng = widget.g.ggql_engine_load(p.cast()).cast();
    malloc.free(p);
    _steps = _buildSteps();
    for (final s in _steps) { _results[s.name] = (_Status.pending, ''); }
  }

  @override
  void dispose() {
    widget.g.ggql_engine_destroy(_eng.cast());
    super.dispose();
  }

  List<_Step> _buildSteps() => [
    _Step('1. list_entities', () async {
      final n = calloc<Int32>();
      try {
        final arr = widget.g.ggql_engine_list_entities(n);
        if (n.value != 6) throw 'esperado 6, recebido ${n.value}';
        final first = arr[0].cast<Utf8>().toDartString();
        if (first != 'SEGMENT') throw 'esperado SEGMENT, recebido $first';
      } finally { calloc.free(n); }
    }),
    _Step('2. describe_entity SEGMENT', () async {
      final n = calloc<Int32>();
      try {
        final cols = widget.g.ggql_engine_describe_entity(
            'SEGMENT'.toNativeUtf8().cast(), n);
        if (n.value != 5) throw 'esperado 5 cols, recebido ${n.value}';
        if (cols[0].cast<Utf8>().toDartString() != 'id') {
          throw 'esperado id, recebido ${cols[0].cast<Utf8>().toDartString()}';
        }
      } finally { calloc.free(n); }
    }),
    _Step('3. validate (OK + falha)', () async {
      final pr = calloc<GgqlParseResult>();
      try {
        var rc = widget.g.ggql_engine_validate(_eng.cast(),
            'FIND SEGMENT'.toNativeUtf8().cast(), pr);
        if (rc != 0 || !pr.ref.valid) throw 'FIND SEGMENT rejeitado';
        rc = widget.g.ggql_engine_validate(_eng.cast(),
            'FIND XYZZY'.toNativeUtf8().cast(), pr);
        if (rc != 1 || pr.ref.valid) throw 'FIND XYZZY aceito incorretamente';
      } finally { calloc.free(pr); }
    }),
    _Step('4. capabilities', () async {
      final caps = calloc<GgqlEngineCapabilities>();
      try {
        if (widget.g.ggql_engine_capabilities(_eng.cast(), caps) != 0) {
          throw 'capabilities rc != 0';
        }
        if (!caps.ref.has_metadata || !caps.ref.has_topology) {
          throw 'metadata/topology ausente';
        }
      } finally { calloc.free(caps); }
    }),
    _Step('5. execute baseline', () async {
      final q = 'FIND SEGMENT WHERE length > 10000 RETURN id, name, length';
      final r = widget.g.ggql_engine_execute(
          _eng.cast(), q.toNativeUtf8().cast(), 5, 0);
      try {
        if (widget.g.ggql_result_row_count(r) != 5) {
          throw 'row_count != 5';
        }
      } finally { widget.g.ggql_result_free(r); }
    }),
    _Step('6. execute_streaming', () async {
      // Mantenha curto na UI; flutter test cobre os detalhes.
      final q = 'FIND SEGMENT WHERE length > 10000 RETURN id, name, length';
      int count = 0;
      final cb = NativeCallable<
          Bool Function(Pointer<Void>, Int32, Int64, Pointer<Void>)>.isolateLocal(
        (a, b, c, d) { ++count; return true; },
        exceptionalReturn: false,
      );
      final emitted = widget.g.ggql_engine_execute_streaming(_eng.cast(),
          q.toNativeUtf8().cast(), 10, cb.nativeFunction, nullptr);
      cb.close();
      if (emitted != count) throw 'emitted ($emitted) != count ($count)';
    }),
    _Step('7. set_max_total_hits cap=5', () async {
      widget.g.ggql_engine_set_max_total_hits(_eng.cast(), 5);
      final r = widget.g.ggql_engine_execute(_eng.cast(),
          'SEARCH KMER "AA" RETURN id'.toNativeUtf8().cast(), 100, 0);
      try {
        if (widget.g.ggql_result_total_hits(r) > 6) throw 'cap não respeitado';
        if (!widget.g.ggql_result_total_truncated(r)) throw 'truncated=false';
      } finally {
        widget.g.ggql_result_free(r);
        widget.g.ggql_engine_set_max_total_hits(_eng.cast(), 0);
      }
    }),
    _Step('8. cancel concorrente', () async {
      final qPtr = 'SEARCH KMER "AA" RETURN id'.toNativeUtf8().cast<Char>();
      final addr = _eng.address;
      unawaited(Future.delayed(const Duration(microseconds: 50), () {
        widget.g.ggql_engine_cancel(Pointer<Void>.fromAddress(addr).cast());
      }));
      final r = widget.g.ggql_engine_execute(_eng.cast(), qPtr, 100, 0);
      if (r.address == 0) throw 'cancel devolveu NULL';
      widget.g.ggql_result_free(r);
    }),
  ];

  Future<void> _runAll() async {
    for (final s in _steps) {
      setState(() => _results[s.name] = (_Status.running, ''));
      try {
        await s.run();
        setState(() => _results[s.name] = (_Status.ok, 'OK'));
      } catch (e) {
        setState(() => _results[s.name] = (_Status.fail, e.toString()));
      }
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('FFI smoke (Round 2/3)')),
    body: ListView(children: [
      Padding(
        padding: const EdgeInsets.all(12),
        child: ElevatedButton(
          onPressed: _runAll,
          child: const Text('Rodar sequência'),
        ),
      ),
      for (final s in _steps)
        ListTile(
          leading: switch (_results[s.name]!.$1) {
            _Status.pending => const Icon(Icons.circle_outlined),
            _Status.running => const SizedBox(
              width: 24, height: 24, child: CircularProgressIndicator(strokeWidth: 2)),
            _Status.ok      => const Icon(Icons.check_circle, color: Colors.green),
            _Status.fail    => const Icon(Icons.error, color: Colors.red),
          },
          title: Text(s.name),
          subtitle: _results[s.name]!.$2.isEmpty ? null
              : Text(_results[s.name]!.$2,
                  style: const TextStyle(fontFamily: 'monospace')),
        ),
    ]),
  );
}
```

Uso:
```dart
Navigator.push(context, MaterialPageRoute(builder: (_) =>
    FfiSmokeScreen(idxPath: '/path/to/chm13_subset_sm.gfa.idx', g: bindings)));
```
