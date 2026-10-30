#!/usr/bin/env bash
# Builds the TV app in the toolchain container and writes out/TizenSteamLink.wgt.
#   scripts/build-tizen.sh                  build (creates the toolchain image the first time)
#   scripts/build-tizen.sh --rebuild-image  rebuild the toolchain image first
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=tizen-steam-link-toolchain
# Tizen Studio and Samsung's Emscripten ship x86-64 binaries only. Pin the platform so
# ARM hosts (Apple Silicon) build and run the image under emulation instead of
# producing an ARM image where the installers fail.
if [[ "${1:-}" == "--rebuild-image" ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build --platform linux/amd64 --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t "$IMAGE" docker/
fi

docker run --platform linux/amd64 --rm -v "$PWD":/src -w /src "$IMAGE" bash scripts/package-in-container.sh
echo "Package: out/TizenSteamLink.wgt"
