#!/usr/bin/env bash

set -eu

DEPS_BUILD_ARGS=()
if [ "$(uname -m)" = "arm64" ] || [ "${SPICE_DOCKER_EMULATED:-0}" = "1" ]; then
    DEPS_BUILD_ARGS+=(--build-arg PACMAN_DISABLE_SANDBOX=1)
fi

docker build --pull "$PWD/external/docker" -t spicetools/deps --platform linux/amd64 "${DEPS_BUILD_ARGS[@]}"
docker build --platform linux/amd64 --build-context gitroot="$PWD/../../.git" . -t spicetools/spice:latest

# Interactive TTY if available, so docker build can be Ctrl+C'd
DOCKER_FLAGS=""
[ -t 0 ] && DOCKER_FLAGS="-it"

docker run $DOCKER_FLAGS --rm --platform linux/amd64 -v "$PWD/dist:/src/src/spice2x/dist" -v "$PWD/bin:/src/src/spice2x/bin" -v "$PWD/.ccache:/src/src/spice2x/.ccache" spicetools/spice "$@"
