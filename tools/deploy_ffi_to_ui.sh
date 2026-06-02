#!/usr/bin/env bash
# deploy_ffi_to_ui.sh — Fase B (B2) do sistema de versão.
#
# Builda a libggql_ffi.so, copia para o repo da UI (ggql_ui) e LOGA versão +
# git hash + sha256 do que foi copiado — fechando a causa-raiz do caso de
# 2026-06-01 (lib defasada passando silenciosa). Ver docs/plano_versionamento_engine.md.
#
# Uso:
#   tools/deploy_ffi_to_ui.sh                 # usa build/ e o repo UI padrão
#   GGQL_UI_REPO=/caminho/ggql_ui tools/deploy_ffi_to_ui.sh
#   BUILD_DIR=build-mimalloc tools/deploy_ffi_to_ui.sh
#
# Variáveis de ambiente:
#   GGQL_UI_REPO  — raiz do repo Flutter (default: repo irmão ../../flutter_projetos/ggql_ui)
#   BUILD_DIR     — diretório de build do engine (default: build)

set -euo pipefail

# Raiz do engine = pai de tools/ (resolve symlinks).
ENGINE_ROOT="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_PATH="${ENGINE_ROOT}/${BUILD_DIR}"
UI_REPO="${GGQL_UI_REPO:-${ENGINE_ROOT}/../../flutter_projetos/ggql_ui}"
DEST_DIR="${UI_REPO}/.dart_tool/lib"
SO_NAME="libggql_ffi.so"

err() { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }
ok()  { printf '\033[32m✓ %s\033[0m\n' "$*"; }

[ -d "$BUILD_PATH" ] || err "build dir não existe: $BUILD_PATH (configure o CMake primeiro)"
[ -d "$UI_REPO" ]    || err "repo da UI não encontrado: $UI_REPO (defina GGQL_UI_REPO)"

# Canonicaliza para logs limpos (resolve ../.. e symlinks).
UI_REPO="$(cd "$UI_REPO" && pwd)"
DEST_DIR="${UI_REPO}/.dart_tool/lib"

# 1. Build da .so (e do repl, para logar --version do MESMO build).
echo "→ Building ggql_ffi + ggql_repl em ${BUILD_DIR}/ ..."
cmake --build "$BUILD_PATH" --target ggql_ffi ggql_repl -j"$(nproc)" >/dev/null

SRC_SO="${BUILD_PATH}/${SO_NAME}"
[ -f "$SRC_SO" ] || err "não encontrei a .so buildada: $SRC_SO"

# 2. Versão do build (cross-check: repl e .so saem do mesmo ggql_version).
VERSION_LINE="$("${BUILD_PATH}/ggql_repl" --version)"
ok "Build: ${VERSION_LINE}"

# 3. Copia para a UI.
mkdir -p "$DEST_DIR"
cp -f "$SRC_SO" "$DEST_DIR/"
ok "Copiado → ${DEST_DIR}/${SO_NAME}"

# 4. Verifica integridade (origem == destino).
SRC_SHA="$(sha256sum "$SRC_SO"            | cut -d' ' -f1)"
DST_SHA="$(sha256sum "${DEST_DIR}/${SO_NAME}" | cut -d' ' -f1)"
[ "$SRC_SHA" = "$DST_SHA" ] || err "sha256 divergente após cópia (origem=$SRC_SHA destino=$DST_SHA)"
ok "sha256 ${SRC_SHA}"

echo
echo "Pronto. Na UI: regenere o ffigen e confira o gate de ABI no startup."
