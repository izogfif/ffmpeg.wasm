#!/bin/bash

set -euo pipefail

CM_FLAGS=(
  -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR           # assign lib and include install path
  -DCMAKE_TOOLCHAIN_FILE=$EM_TOOLCHAIN_FILE     # use emscripten toolchain file
  -DBUILD_SHARED_LIBS=0                         # disable shared library build
  # -DAOM_TARGET_CPU=generic                      # use generic cpu
  -DENABLE_DOCS=0                               # disable docs
  -DENABLE_TESTS=0                              # disable tests
  -DENABLE_EXAMPLES=0                           # disable examples
  -DENABLE_TOOLS=0                              # disable tools
  -DCONFIG_RUNTIME_CPU_DETECT=0                 # disable cpu detect
  -DCONFIG_WEBM_IO=0                            # disable libwebm support
  -DENABLE_TESTDATA=0
  -DCONFIG_MULTITHREAD=0
  -DCMAKE_BUILD_TYPE=Release
  # Taken from ogv.js/buildscripts/compileDav1dWasm.sh
  # See https://github.com/videolan/dav1d/blob/master/meson_options.txt
  -Denable_asm=true
  -Denable_tests=false
  -Denable_tools=false
  # -Dbitdepths='["8"]'
  -Ddefault_library=static
)

CMBUILD_DIR=cmbuild
rm -rf $CMBUILD_DIR
mkdir -p $CMBUILD_DIR
cd $CMBUILD_DIR

# These commands can be added after "meson /src"
  # -Dbitdepths='["8"]' \

CFLAGS=-pthread \
  LDFLAGS=-pthread \
  meson /src \
  --prefix=$INSTALL_DIR \
  --cross-file=/src/dav1d-wasm-simd-mt-cross.txt \
  -Denable_asm=true \
  -Denable_tests=false \
  -Denable_tools=false \
  -Ddefault_library=static \
  --buildtype release && \
  ninja -v && \
  ninja install

# emmake cmake .. \
#   -DAOM_EXTRA_C_FLAGS="$CFLAGS" \
#   -DAOM_EXTRA_CXX_FLAGS="$CFLAGS" \
#   ${CM_FLAGS[@]}
# emmake make install -j
