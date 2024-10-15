#!/bin/bash
# `-o <OUTPUT_FILE_NAME>` must be provided when using this build script.
# ex:
#     bash ffmpeg-wasm.sh -o ffmpeg.js

set -euo pipefail

EXPORT_NAME="OGVDecoderVideoTheora"

CONF_FLAGS=(
  -I. 
  -I./src/fftools 
  -I$INSTALL_DIR/include 
  -L$INSTALL_DIR/lib 
  -Llibavcodec 
  -Llibavdevice 
  -Llibavfilter 
  -Llibavformat 
  -Llibavutil 
  -Llibpostproc 
  -Llibswresample 
  -Llibswscale 
  -lavcodec 
  #-lavdevice 
  #-lavfilter 
  -lavformat 
  -lavutil 
  #-lpostproc 
  -lswresample 
  -lswscale 
  -Wno-deprecated-declarations 
  $LDFLAGS 
  -sENVIRONMENT=worker
  -sWASM_BIGINT                            # enable big int support
  -sUSE_SDL=2                              # use emscripten SDL2 lib port
  -sMODULARIZE                             # modularized to use as a library
  -s VERBOSE=1
  ${FFMPEG_MT:+ -sINITIAL_MEMORY=512MB}   # ALLOW_MEMORY_GROWTH is not recommended when using threads, thus we use a large initial memory
  # ${FFMPEG_MT:+ -sPTHREAD_POOL_SIZE=2}
  ${FFMPEG_ST:+ -sINITIAL_MEMORY=128MB -sALLOW_MEMORY_GROWTH -sTOTAL_STACK=100MB} # Use just enough memory as memory usage can grow
  -sEXPORT_NAME="$EXPORT_NAME"             # required in browser env, so that user can access this module from window object
#  -sEXPORTED_FUNCTIONS=$(node src/bind/ffmpeg/export.js) # exported functions
  -sEXPORTED_FUNCTIONS=$(node src/ogv/js/modules/ogv-decoder-video-exports.js)
#  -sEXPORTED_RUNTIME_METHODS=$(node src/bind/ffmpeg/export-runtime.js) # exported built-in functions
  -lworkerfs.js
  --js-library src/ogv/js/modules/ogv-decoder-video-callbacks.js
  --pre-js src/ogv/js/modules/ogv-module-pre.js
  --post-js src/ogv/js/modules/ogv-decoder-video.js
  # -sASYNCIFY
  # -sASYNCIFY_STACK_SIZE=8192
  src/ogv/c/ffmpeg-helper.cpp
  src/ogv/c/decoder-helper.cpp
  src/ogv/c/ogv-decoder-video-ffmpeg.cpp
)

emcc "${CONF_FLAGS[@]}" $@
