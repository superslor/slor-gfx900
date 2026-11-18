#!/bin/bash
# Clone the pinned llama.cpp base, apply patches/, build llama-server for HIP gfx900.
set -euo pipefail
cd "$(dirname "$0")"

REPO=https://github.com/PrismML-Eng/llama.cpp
TAG=prism-b10709-9a9394a
COMMIT=9a9394a895b96003ca842a6041cb28ac49a108f7
SRC=${SRC:-llama.cpp}
BUILD=${BUILD:-build-gfx900}

if [ ! -d "$SRC" ]; then
    git clone --depth 1 --branch "$TAG" "$REPO" "$SRC"
    [ "$(git -C "$SRC" rev-parse HEAD)" = "$COMMIT" ] || { echo "unexpected base commit" >&2; exit 1; }
    for p in patches/*.patch; do
        git -C "$SRC" apply "$PWD/$p"
    done
fi

HIPCXX="$(hipconfig -l)/clang" HIP_PATH="$(hipconfig -R)" \
cmake -S "$SRC" -B "$SRC/$BUILD" -DCMAKE_BUILD_TYPE=Release -DGGML_HIP=ON -DGPU_TARGETS=gfx900 \
      -DGGML_CUDA_FA_ALL_QUANTS=ON -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=ON \
      -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TESTS=ON
cmake --build "$SRC/$BUILD" --target llama-server test-backend-ops -j"$(nproc)"
echo "built: $SRC/$BUILD/bin"
