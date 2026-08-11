#!/bin/bash
# Build the DDNet-TankTrouble server for Linux and stage a deploy folder.
# Tested on Ubuntu/Debian. Run from the linux/ directory:
#   cd linux && ./build_server.sh
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build-linux"
DEPLOY_DIR="$REPO_DIR/linux/deploy"

echo "==> installing build dependencies (Ubuntu/Debian)"
sudo apt update
sudo apt install -y build-essential cmake ninja-build pkg-config \
    zlib1g-dev libssl-dev libcurl4-openssl-dev libsqlite3-dev

echo "==> configuring cmake (Release, server only)"
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMYSQL=OFF \
    -DWEBSOCKETS=OFF \
    -DUPNP=OFF \
    -DANTIBOT=OFF \
    -DDOWNLOAD_GTEST=OFF \
    "$REPO_DIR"

echo "==> building DDNet-Server"
cmake --build "$BUILD_DIR" --target DDNet-Server -j"$(nproc)"

echo "==> staging deploy folder"
rm -rf "$DEPLOY_DIR"
mkdir -p "$DEPLOY_DIR/data/maps"
cp "$BUILD_DIR/DDNet-Server" "$DEPLOY_DIR/"
cp "$REPO_DIR/storage.cfg" "$DEPLOY_DIR/"
cp "$REPO_DIR/autoexec.cfg" "$DEPLOY_DIR/autoexec.cfg"
cp "$REPO_DIR/data/maps/tkt_test.map" "$DEPLOY_DIR/data/maps/"
cp -r "$REPO_DIR/room_config" "$DEPLOY_DIR/room_config"

# boot the test map: on a fresh server the mega map does not exist yet and
# the engine loads the map before the controller can generate it; the first
# round end then switches to the generated mega map permanently
sed -i 's/^sv_map ".*"/sv_map "tkt_test"/' "$DEPLOY_DIR/autoexec.cfg"

echo ""
echo "==> deploy ready at: linux/deploy"
echo "    edit autoexec.cfg (sv_rcon_password, sv_port, ...) then run:"
echo "    cd linux/deploy && ./DDNet-Server"
echo "    (first round generates the mega map, afterwards the server switches to it)"
