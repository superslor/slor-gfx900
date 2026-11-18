#!/bin/bash
# Rebuild Ubuntu 26.04's own rocBLAS 7.1.0 source package (with its debian/patches) for gfx900 only:
# Ubuntu's librocblas5 ships no gfx900 kernels, but the source still has the vega10 Tensile logic.
# Output: rocblas-gfx900/lib (librocblas.so*) and rocblas-gfx900/library (kernels), next to this script.
set -euo pipefail
cd "$(dirname "$0")"
HERE=$PWD
URL=https://launchpad.net/ubuntu/+archive/primary/+sourcefiles/rocblas/7.1.0-1ubuntu4

mkdir -p work && cd work
while read -r sum f; do
    [ -f "$f" ] || curl -fsSL -o "$f" "$URL/$f"
    echo "$sum  $f" | sha256sum -c --quiet -
done <<'EOF'
54f38222d0e58344cf5c86f151d418c071b59145297fd2ed953bb561df1e12c3 rocblas_7.1.0.orig.tar.gz
853b92723750ee2249d8f7aedb1e367a97fb3b9fe4b3741d67e8c1bee7cd97cb rocblas_7.1.0.orig-tensile.tar.gz
536df82dc8996dbd6a5a8dd2001148d66411c6fb99afc0dab1b92e9ef73cbd4c rocblas_7.1.0-1ubuntu4.debian.tar.xz
EOF

rm -rf src && mkdir -p src/tensile
tar xzf rocblas_7.1.0.orig.tar.gz -C src --strip-components=1
tar xzf rocblas_7.1.0.orig-tensile.tar.gz -C src/tensile --strip-components=1
tar xJf rocblas_7.1.0-1ubuntu4.debian.tar.xz -C src
cd src
while read -r p; do
    case "$p" in ''|\#*) continue ;; esac
    patch -p1 --forward --quiet < "debian/patches/$p"
done < debian/patches/series

# the flags of Ubuntu's debian/rules, with GPU_TARGETS=gfx900 and no clients
export CXX=hipcc CC=hipcc TENSILE_ROCM_ASSEMBLER_PATH=/usr/bin/clang++ TENSILE_ROCM_OFFLOAD_BUNDLER_PATH=/usr/bin/clang-offload-bundler
export PATH=$PATH:/usr/lib/llvm-21/bin
cmake -S . -B build -DROCTX_PATH=/usr/lib/x86_64-linux-gnu -DCMAKE_BUILD_TYPE=Release -DCMAKE_SKIP_INSTALL_RPATH=ON \
  -DROCM_SYMLINK_LIBS=OFF -DBUILD_CLIENTS_BENCHMARKS=OFF -DBUILD_CLIENTS_TESTS=OFF -DBUILD_FILE_REORG_BACKWARD_COMPATIBILITY=OFF \
  -DGPU_TARGETS=gfx900 -DBUILD_WITH_HIPBLASLT=OFF -DROCBLAS_TENSILE_LIBRARY_DIR=/usr/lib/x86_64-linux-gnu/rocblas/5.1.0 \
  -DTensile_COMPILER=hipcc -DTensile_TEST_LOCAL_PATH="$PWD/tensile" -DTensile_ROOT="$PWD/tensile/Tensile" -DTensile_LOGIC=asm_full \
  -DTensile_LIBRARY_FORMAT=msgpack -DTensile_LAZY_LIBRARY_LOADING=ON -DTensile_SEPARATE_ARCHITECTURES=ON \
  -DBUILD_WITH_PIP=false -DOptions=--ignore-asm-cap-cache -DRUN_HEADER_TESTING=OFF -DBUILD_OFFLOAD_COMPRESS=ON
cmake --build build -j"$(nproc)"

rm -rf "$HERE/rocblas-gfx900" && mkdir -p "$HERE/rocblas-gfx900/lib"
cp -a build/library/src/librocblas.so* "$HERE/rocblas-gfx900/lib/"
cp -a build/Tensile/library "$HERE/rocblas-gfx900/"
echo "done: sudo cp -a $HERE/rocblas-gfx900 /opt/"
