# syntax=docker/dockerfile-upstream:master-labs@sha256:fd5f8eef1b42158f6f148de7ab191aa932a92a3bd5b82f5e0fbf89413d825263
# The line above is required in order for "ADD https://github.com/..." lines to work

# Base emsdk image with environment variables.
FROM --platform=linux/amd64 emscripten/emsdk:3.1.69@sha256:9d6522879357a363ada61862481cc12c5f772d5e9738b8addf95d38490cdc6ea AS emsdk-base
# FROM --platform=linux/amd64 emscripten/emsdk:3.1.40@sha256:c1e807a6e03ac5bd5b37bae2ace3c46c08579e2ddeb951037a3b8dac7067f2cc AS emsdk-base
#FROM --platform=linux/amd64 emscripten/emsdk:3.1.49@sha256:b35d43d2927648cc4e026aecbdc05c5ffdc8a2b72d7e085e0a836510ff2e0004 AS emsdk-base
#FROM --platform=linux/amd64 emscripten/emsdk:3.1.45@sha256:e92eed60a35c9b43588e3fa3dae56c4236b4e7462e6ee5276fae6e98f914b4e4 AS emsdk-base
#FROM  --platform=linux/amd64 emscripten/emsdk:3.1.43@sha256:e856cd59d54fd143366c2df314043d63a1ca23ab43521e0f30f9d9a7ce6d43e0 AS emsdk-base
#FROM  --platform=linux/amd64 emscripten/emsdk:3.1.42@sha256:9f56c533f4da127ecdff0ccca88e7ced2d26b8e9670ed1b90e5fde3207d6e4b9 AS emsdk-base
#FROM  --platform=linux/amd64 emscripten/emsdk:3.1.41@sha256:270ed8b0045b14593817e0c2d751a04ff1da2aea70e17e56a3f36baab6b470e1 AS emsdk-base
ARG EXTRA_CFLAGS
ARG EXTRA_LDFLAGS
ARG FFMPEG_ST
ARG FFMPEG_MT
ENV INSTALL_DIR=/opt
# We cannot upgrade to n6.0 as ffmpeg bin only supports multithread at the moment.
# ENV FFMPEG_VERSION=n5.1.6
ENV FFMPEG_VERSION=n7.1
ENV CFLAGS="-I$INSTALL_DIR/include $CFLAGS $EXTRA_CFLAGS"
ENV CXXFLAGS="$CFLAGS"
ENV LDFLAGS="-L$INSTALL_DIR/lib $LDFLAGS $CFLAGS $EXTRA_LDFLAGS"
ENV EM_PKG_CONFIG_PATH=$EM_PKG_CONFIG_PATH:$INSTALL_DIR/lib/pkgconfig:/emsdk/upstream/emscripten/system/lib/pkgconfig
ENV EM_TOOLCHAIN_FILE=$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake
ENV PKG_CONFIG_PATH=$PKG_CONFIG_PATH:$EM_PKG_CONFIG_PATH
ENV FFMPEG_ST=$FFMPEG_ST
ENV FFMPEG_MT=$FFMPEG_MT
# Uncomment these three lines in order to enable detailed memory debugging
COPY build/emmalloc.c /emsdk/upstream/emscripten/system/lib/emmalloc.c
RUN cat /emsdk/upstream/emscripten/system/lib/emmalloc.c
RUN rm -rf /emsdk/upstream/emscripten/cache
RUN apt-get update && \
  apt-get install -y pkg-config autoconf automake libtool ragel build-essential meson ninja-build

# Build x264
FROM emsdk-base AS x264-builder
ENV X264_BRANCH=4-cores
ADD https://github.com/ffmpegwasm/x264.git#$X264_BRANCH /src
COPY build/x264.sh /src/build.sh
RUN bash -x /src/build.sh

# Build x265
FROM emsdk-base AS x265-builder
ENV X265_BRANCH=4.0
ADD https://bitbucket.org/multicoreware/x265_git.git#$X265_BRANCH /src
COPY build/x265.sh /src/build.sh
COPY build/x265.CMakeLists.txt /src/source/CMakeLists.txt
RUN bash -x /src/build.sh

# Build libvpx
FROM emsdk-base AS libvpx-builder
ENV LIBVPX_BRANCH=v1.13.1
ADD https://github.com/ffmpegwasm/libvpx.git#$LIBVPX_BRANCH /src
COPY build/libvpx.sh /src/build.sh
RUN bash -x /src/build.sh

# Build lame
FROM emsdk-base AS lame-builder
ENV LAME_BRANCH=master
ADD https://github.com/ffmpegwasm/lame.git#$LAME_BRANCH /src
COPY build/lame.sh /src/build.sh
RUN bash -x /src/build.sh

# Build ogg
FROM emsdk-base AS ogg-builder
ENV OGG_BRANCH=v1.3.4
ADD https://github.com/ffmpegwasm/Ogg.git#$OGG_BRANCH /src
COPY build/ogg.sh /src/build.sh
RUN bash -x /src/build.sh

# Build theora
FROM emsdk-base AS theora-builder
COPY --from=ogg-builder $INSTALL_DIR $INSTALL_DIR
ENV THEORA_BRANCH=v1.1.1
ADD https://github.com/ffmpegwasm/theora.git#$THEORA_BRANCH /src
COPY build/theora.sh /src/build.sh
RUN bash -x /src/build.sh

# Build opus
FROM emsdk-base AS opus-builder
ENV OPUS_BRANCH=v1.3.1
ADD https://github.com/ffmpegwasm/opus.git#$OPUS_BRANCH /src
COPY build/opus.sh /src/build.sh
RUN bash -x /src/build.sh

# Build vorbis
FROM emsdk-base AS vorbis-builder
COPY --from=ogg-builder $INSTALL_DIR $INSTALL_DIR
ENV VORBIS_BRANCH=v1.3.3
ADD https://github.com/ffmpegwasm/vorbis.git#$VORBIS_BRANCH /src
COPY build/vorbis.sh /src/build.sh
RUN bash -x /src/build.sh

# Build zlib
FROM emsdk-base AS zlib-builder
ENV ZLIB_BRANCH=v1.2.11
ADD https://github.com/ffmpegwasm/zlib.git#$ZLIB_BRANCH /src
COPY build/zlib.sh /src/build.sh
RUN bash -x /src/build.sh

# Build libwebp
FROM emsdk-base AS libwebp-builder
COPY --from=zlib-builder $INSTALL_DIR $INSTALL_DIR
ENV LIBWEBP_BRANCH=v1.3.2
ADD https://github.com/ffmpegwasm/libwebp.git#$LIBWEBP_BRANCH /src
COPY build/libwebp.sh /src/build.sh
RUN bash -x /src/build.sh

# Build freetype2
FROM emsdk-base AS freetype2-builder
ENV FREETYPE2_BRANCH=VER-2-10-4
ADD https://github.com/ffmpegwasm/freetype2.git#$FREETYPE2_BRANCH /src
COPY build/freetype2.sh /src/build.sh
RUN bash -x /src/build.sh

# Build fribidi
FROM emsdk-base AS fribidi-builder
ENV FRIBIDI_BRANCH=v1.0.9
ADD https://github.com/fribidi/fribidi.git#$FRIBIDI_BRANCH /src
COPY build/fribidi.sh /src/build.sh
RUN bash -x /src/build.sh

# Build harfbuzz
FROM emsdk-base AS harfbuzz-builder
ENV HARFBUZZ_BRANCH=5.2.0
ADD https://github.com/harfbuzz/harfbuzz.git#$HARFBUZZ_BRANCH /src
COPY build/harfbuzz.sh /src/build.sh
RUN bash -x /src/build.sh

# Build libass
FROM emsdk-base AS libass-builder
COPY --from=freetype2-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=fribidi-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=harfbuzz-builder $INSTALL_DIR $INSTALL_DIR
ENV LIBASS_BRANCH=0.15.0
ADD https://github.com/libass/libass.git#$LIBASS_BRANCH /src
COPY build/libass.sh /src/build.sh
RUN bash -x /src/build.sh

# Build zimg
FROM emsdk-base AS zimg-builder
ENV ZIMG_BRANCH=release-3.0.5
RUN apt-get update && apt-get install -y git
RUN git clone --recursive -b $ZIMG_BRANCH https://github.com/sekrit-twc/zimg.git /src
COPY build/zimg.sh /src/build.sh
RUN bash -x /src/build.sh

# Build aom
FROM emsdk-base AS dav1d-builder
ENV DAV1D_BRANCH=1.4.3
RUN apt-get update && apt-get install -y git
RUN git clone --recursive --depth 1 --branch $DAV1D_BRANCH https://code.videolan.org/videolan/dav1d.git /src
# COPY build/dav1d-wasm-simd-cross.txt /src/dav1d-wasm-simd-cross.txt
COPY build/dav1d-wasm-simd-mt-cross.txt /src/dav1d-wasm-simd-mt-cross.txt
COPY build/dav1d.sh /src/build.sh
RUN bash -x /src/build.sh

# Base ffmpeg image with dependencies and source code populated.
FROM emsdk-base AS ffmpeg-base
# RUN apt-get update && apt-get install -y libdav1d-dev
# RUN apt list -a libdav1d-dev
# RUN echo "PKG_CONFIG_PATH is $PKG_CONFIG_PATH"
# RUN echo "failing build" && failbuild
RUN embuilder build sdl2 sdl2-mt
ADD https://github.com/FFmpeg/FFmpeg.git#$FFMPEG_VERSION /src
COPY --from=x264-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=x265-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=libvpx-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=lame-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=opus-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=theora-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=vorbis-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=libwebp-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=libass-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=zimg-builder $INSTALL_DIR $INSTALL_DIR
# COPY --from=aom-builder $INSTALL_DIR $INSTALL_DIR
COPY --from=dav1d-builder $INSTALL_DIR $INSTALL_DIR

# Build ffmpeg
FROM ffmpeg-base AS ffmpeg-builder
COPY build/ffmpeg.sh /src/build.sh
RUN bash -x /src/build.sh \
  --enable-gpl \
  --enable-libx264 \
  --enable-libx265 \
  --enable-libvpx \
  --enable-libmp3lame \
  --enable-libtheora \
  --enable-libvorbis \
  --enable-libopus \
  --enable-zlib \
  --enable-libwebp \
  --enable-libfreetype \
  --enable-libfribidi \
  --enable-libass \
  --enable-libzimg \
  --enable-libdav1d
# --enable-libaom

# Build ogv-demuxer
FROM ffmpeg-builder AS ogv-demuxer-builder
COPY src/ogv /src/src/ogv
COPY build/ogv-demuxer.sh build.sh
# libraries to link
ENV FFMPEG_LIBS \
  -lx264 \
  -lx265 \
  -lvpx \
  -lmp3lame \
  -logg \
  -ltheora \
  -lvorbis \
  -lvorbisenc \
  -lvorbisfile \
  -lopus \
  -lz \
  -lwebpmux \
  -lwebp \
  -lsharpyuv \
  -lfreetype \
  -lfribidi \
  -lharfbuzz \
  -lass \
  -lzimg \
  -ldav1d
# -laom

RUN mkdir -p /src/dist/umd && bash -x /src/build.sh \
  ${FFMPEG_LIBS} \
  -o dist/umd/ogv-demuxer-ogg.js

# Build ogv-decoder-video
FROM ffmpeg-builder AS ogv-decoder-video-builder
COPY src/ogv /src/src/ogv
COPY build/ogv-decoder-video.sh build.sh
# libraries to link
ENV FFMPEG_LIBS \
  -lx264 \
  -lx265 \
  -lvpx \
  -lmp3lame \
  -logg \
  -ltheora \
  -lvorbis \
  -lvorbisenc \
  -lvorbisfile \
  -lopus \
  -lz \
  -lwebpmux \
  -lwebp \
  -lsharpyuv \
  -lfreetype \
  -lfribidi \
  -lharfbuzz \
  -lass \
  -lzimg \
  -ldav1d
# -laom
RUN mkdir -p /src/dist/umd && bash -x /src/build.sh \
  ${FFMPEG_LIBS} \
  -o dist/umd/ogv-decoder-video-theora.js


# Export ffmpeg-core.wasm to dist/, use `docker buildx build -o . .` to get assets
FROM scratch AS exportor
COPY --from=ogv-demuxer-builder /src/dist /dist
COPY --from=ogv-decoder-video-builder /src/dist /dist
