all: dev

MT_FLAGS := -sUSE_PTHREADS -pthread

DEV_ARGS := --progress=plain

DEV_CFLAGS := --profiling
DEV_MT_CFLAGS := $(DEV_CFLAGS) $(MT_FLAGS)
# PROD_CFLAGS := -O3 -msimd128 -mavx -sMALLOC=mimalloc

# --profiling-funcs
# PROD_CFLAGS := -msimd128 -mavx \
# 	-O0 --profiling -g -gsource-map \
# 	-sASSERTIONS=2 -sSAFE_HEAP=1 -sSTACK_OVERFLOW_CHECK=2 \
# 	-sMALLOC=emmalloc-memvalidate-verbose 

# This one works
# PROD_CFLAGS := -msimd128 -mavx \
# 	-O0 --profiling -g -gsource-map \


PROD_CFLAGS := -msimd128 -mavx \
	-O0 --profiling -g -gsource-map \
	-fsanitize=undefined \
	
# This enables ASAN. It doesn't work together with SAFE_HEAP 
# and is currently causing this error:
# streaming compile failed: CompileError: WebAssembly.instantiateStreaming(): Compiling function #3639:"ff_h264_filter_mb_fast" failed: local count too large
# -fsanitize=address

# -sASSERTIONS=2 -sSAFE_HEAP=1 -sSTACK_OVERFLOW_CHECK=2 \

PROD_MT_CFLAGS := $(PROD_CFLAGS) $(MT_FLAGS)

clean:
	rm -rf ./packages/core$(PKG_SUFFIX)/dist

.PHONY: build
build:
	make clean PKG_SUFFIX="$(PKG_SUFFIX)"
	EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
	EXTRA_LDFLAGS="$(EXTRA_LDFLAGS)" \
	FFMPEG_ST="$(FFMPEG_ST)" \
	FFMPEG_MT="$(FFMPEG_MT)" \
		docker buildx build \
			--progress=plain \
			--build-arg EXTRA_CFLAGS \
			--build-arg EXTRA_LDFLAGS \
			--build-arg FFMPEG_MT \
			--build-arg FFMPEG_ST \
			-o ./packages/core$(PKG_SUFFIX) \
			$(EXTRA_ARGS) \
			. \
			--no-cache-filter=ogv-decoder-video-builder,ogv-demuxer-builder,exportor

#   --no-cache \
#		--no-cache-filter=ogv-decoder-video-builder,ogv-demuxer-builder,exportor \

build-st:
	make build \
		FFMPEG_ST=yes

build-mt:
	make build \
		FFMPEG_MT=yes \

#		PKG_SUFFIX=-mt \

dev:
	make build-st EXTRA_CFLAGS="$(DEV_CFLAGS)" EXTRA_ARGS="$(DEV_ARGS)"

dev-mt:
	make build-mt EXTRA_CFLAGS="$(DEV_MT_CFLAGS)" EXTRA_ARGS="$(DEV_ARGS)"

prd:
#	make build-mt EXTRA_CFLAGS="$(PROD_MT_CFLAGS)"
	make build-st EXTRA_CFLAGS="$(PROD_CFLAGS)"

prd-mt:
	make build-mt EXTRA_CFLAGS="$(PROD_MT_CFLAGS)"
