#!/usr/bin/env bash
# Build DDNet-Server from this source root and stage deploy/.
set -Eeuo pipefail

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build-linux"
DEPLOY_DIR="${ROOT_DIR}/deploy"

if [[ "${BUILD_SERVER_SKIP_APT:-0}" != "1" ]] && command -v apt-get >/dev/null 2>&1; then
  if [[ "$(id -u)" -eq 0 ]]; then
    APT=(apt-get)
  elif command -v sudo >/dev/null 2>&1; then
    APT=(sudo apt-get)
  else
    echo "==> sudo not found; using preinstalled build dependencies"
    APT=()
  fi
  if ((${#APT[@]})); then
    "${APT[@]}" update
    "${APT[@]}" install -y build-essential cmake ninja-build pkg-config python3 \
      zlib1g-dev libssl-dev libcurl4-openssl-dev libsqlite3-dev libpng-dev
  fi
fi

for tool in cmake ninja c++ python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "Missing build tool: $tool" >&2; exit 127; }
done

echo "==> configuring from $ROOT_DIR"
rm -rf "$BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDEV=ON \
  -DPREFER_BUNDLED_LIBS=OFF \
  -DMYSQL=OFF -DWEBSOCKETS=OFF -DUPNP=OFF -DANTIBOT=OFF \
  -DDOWNLOAD_GTEST=OFF

echo "==> building DDNet-Server"
cmake --build "$BUILD_DIR" --target DDNet-Server --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

[[ -x "$BUILD_DIR/DDNet-Server" ]] || { echo "DDNet-Server was not produced" >&2; exit 1; }
mkdir -p "$DEPLOY_DIR"
install -m 755 "$BUILD_DIR/DDNet-Server" "$DEPLOY_DIR/DDNet-Server"
[[ -f "$DEPLOY_DIR/storage.cfg" ]] || cp "$ROOT_DIR/storage.cfg" "$DEPLOY_DIR/storage.cfg"
mkdir -p "$DEPLOY_DIR/data"

printf '\n==> Linux deploy ready: %s\n' "$DEPLOY_DIR"
file "$DEPLOY_DIR/DDNet-Server" 2>/dev/null || true