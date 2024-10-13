#!/bin/bash

set -euo pipefail

CONF_FLAGS=(
  --target-os=none              # disable target specific configs
  --arch=x86_32                 # use x86_32 arch
  --enable-cross-compile        # use cross compile configs
  --disable-asm                 # disable asm
  --disable-stripping           # disable stripping as it won't work
  --disable-programs            # disable ffmpeg, ffprobe and ffplay build
  --disable-doc                 # disable doc build
  --disable-debug               # disable debug mode
  --disable-runtime-cpudetect   # disable cpu detection
  --disable-autodetect          # disable env auto detect

  # assign toolchains and extra flags
  --nm=emnm
  --ar=emar
  --ranlib=emranlib
  --cc=emcc
  --cxx=em++
  --objcc=emcc
  --dep-cc=emcc
  --extra-cflags="$CFLAGS"
  --extra-cxxflags="$CXXFLAGS"

  # start of selective enabling / disabling features

  --disable-iconv
  --disable-schannel
  --disable-symver
  --disable-xlib
  --disable-zlib
  --disable-securetransport
  --disable-d3d11va
  --disable-dxva2
  --disable-vaapi
  --disable-vdpau
  --disable-videotoolbox
  
  --disable-everything
  --disable-all
  --disable-htmlpages
  --disable-manpages
  --disable-podpages
  --disable-txtpages
  --disable-network
  --disable-appkit
  --disable-coreimage
  --disable-metal
  --disable-sdl2
  --disable-securetransport
  --disable-vulkan
  --disable-audiotoolbox
  --disable-v4l2-m2m
  --disable-valgrind-backtrace
  --disable-large-tests
  
  --disable-avdevice
  --disable-avformat
  --disable-avfilter
  --disable-postproc

  --enable-avcodec
  --enable-avformat
  --enable-avutil
  --enable-avcodec
  --enable-swresample
  --enable-swscale

  --enable-demuxer=aa
  --enable-demuxer=aac
  --enable-demuxer=ac3
  --enable-demuxer=ac4
  --enable-demuxer=ace
  --enable-demuxer=amr
  --enable-demuxer=ape
  --enable-demuxer=asf
  --enable-demuxer=asf_o
  --enable-demuxer=ass
  --enable-demuxer=av1
  --enable-demuxer=avi
  --enable-demuxer=avs
  --enable-demuxer=avs2
  --enable-demuxer=avs3
  --enable-demuxer=caf
  --enable-demuxer=cavsvideo
  --enable-demuxer=codec2
  --enable-demuxer=codec2raw
  --enable-demuxer=dash
  --enable-demuxer=daud
  --enable-demuxer=dirac
  --enable-demuxer=dnxhd
  --enable-demuxer=dsf
  --enable-demuxer=dsicin
  --enable-demuxer=dss
  --enable-demuxer=dts
  --enable-demuxer=dtshd
  --enable-demuxer=dv
  --enable-demuxer=dvdvideo
  --enable-demuxer=eac3
  --enable-demuxer=evc
  --enable-demuxer=flac
  --enable-demuxer=flv
  --enable-demuxer=g722
  --enable-demuxer=g723_1
  --enable-demuxer=g726
  --enable-demuxer=g726le
  --enable-demuxer=g729
  --enable-demuxer=gsm
  --enable-demuxer=gxf
  --enable-demuxer=h261
  --enable-demuxer=h263
  --enable-demuxer=h264
  --enable-demuxer=hevc
  --enable-demuxer=hls
  --enable-demuxer=iamf
  --enable-demuxer=ilbc
  --enable-demuxer=imf
  --enable-demuxer=ircam
  --enable-demuxer=kux
  --enable-demuxer=libmodplug
  --enable-demuxer=live_flv
  --enable-demuxer=m4v
  --enable-demuxer=matroska
  --enable-demuxer=mlp
  --enable-demuxer=mov
  --enable-demuxer=mp3
  --enable-demuxer=mpegps
  --enable-demuxer=mpegts
  --enable-demuxer=mpegtsraw
  --enable-demuxer=mpegvideo
  --enable-demuxer=mpjpeg
  --enable-demuxer=nuv
  --enable-demuxer=obu
  --enable-demuxer=ogg
  --enable-demuxer=pcm_alaw
  --enable-demuxer=pcm_f32be
  --enable-demuxer=pcm_f32le
  --enable-demuxer=pcm_f64be
  --enable-demuxer=pcm_f64le
  --enable-demuxer=pcm_mulaw
  --enable-demuxer=pcm_s16be
  --enable-demuxer=pcm_s16le
  --enable-demuxer=pcm_s24be
  --enable-demuxer=pcm_s24le
  --enable-demuxer=pcm_s32be
  --enable-demuxer=pcm_s32le
  --enable-demuxer=pcm_s8
  --enable-demuxer=pcm_u16be
  --enable-demuxer=pcm_u16le
  --enable-demuxer=pcm_u24be
  --enable-demuxer=pcm_u24le
  --enable-demuxer=pcm_u32be
  --enable-demuxer=pcm_u32le
  --enable-demuxer=pcm_u8
  --enable-demuxer=pcm_vidc
  --enable-demuxer=truehd
  --enable-demuxer=wav
  --enable-demuxer=webm_dash_manifest

  --enable-decoder=amv
  --enable-decoder=libdav1d
  # --enable-decoder=libaom_av1
  --enable-decoder=av1
  --enable-decoder=avrn
  --enable-decoder=avrp
  --enable-decoder=cfhd
  --enable-decoder=cinepak
  --enable-decoder=flv
  --enable-decoder=h261
  --enable-decoder=h263
  --enable-decoder=h263i
  --enable-decoder=h263p
  --enable-decoder=h264
  --enable-decoder=libopenh264
  --enable-decoder=hevc
  --enable-decoder=mjpeg
  --enable-decoder=mpeg1video
  --enable-decoder=mpeg2video
  --enable-decoder=mpegvideo
  --enable-decoder=mpeg4
  --enable-decoder=msmpeg4v1
  --enable-decoder=msmpeg4v2
  --enable-decoder=msmpeg4
  --enable-decoder=msvideo1
  --enable-decoder=rv10
  --enable-decoder=rv20
  --enable-decoder=rv30
  --enable-decoder=rv40
  --enable-decoder=theora
  --enable-decoder=camtasia
  --enable-decoder=tscc2
  --enable-decoder=vp3
  --enable-decoder=vp4
  --enable-decoder=vp5
  --enable-decoder=vp6
  --enable-decoder=vp6a
  --enable-decoder=vp6f
  --enable-decoder=vp7
  --enable-decoder=vp8
  --enable-decoder=libvpx
  --enable-decoder=vp9
  --enable-decoder=libvpx-vp9
  --enable-decoder=wmv1
  --enable-decoder=wmv2
  --enable-decoder=wmv3
  --enable-decoder=wmv3image
  --enable-decoder=zlib
  --enable-decoder=aac
  --enable-decoder=aac_fixed
  --enable-decoder=aac_latm
  --enable-decoder=ac3
  --enable-decoder=ac3_fixed
  --enable-decoder=on2avc
  --enable-decoder=eac3
  --enable-decoder=flac
  --enable-decoder=mp1
  --enable-decoder=mp1float
  --enable-decoder=mp2
  --enable-decoder=mp2float
  --enable-decoder=mp3float
  --enable-decoder=mp3
  --enable-decoder=mp3adufloat
  --enable-decoder=mp3adu
  --enable-decoder=mp3on4float
  --enable-decoder=mp3on4
  --enable-decoder=als
  --enable-decoder=opus
  --enable-decoder=libopus
  --enable-decoder=pcm_alaw
  --enable-decoder=pcm_bluray
  --enable-decoder=pcm_dvd
  --enable-decoder=pcm_f16le
  --enable-decoder=pcm_f24le
  --enable-decoder=pcm_f32be
  --enable-decoder=pcm_f32le
  --enable-decoder=pcm_f64be
  --enable-decoder=pcm_f64le
  --enable-decoder=pcm_lxf
  --enable-decoder=pcm_mulaw
  --enable-decoder=pcm_s16be
  --enable-decoder=pcm_s16be_planar
  --enable-decoder=pcm_s16le
  --enable-decoder=pcm_s16le_planar
  --enable-decoder=pcm_s24be
  --enable-decoder=pcm_s24daud
  --enable-decoder=pcm_s24le
  --enable-decoder=pcm_s24le_planar
  --enable-decoder=pcm_s32be
  --enable-decoder=pcm_s32le
  --enable-decoder=pcm_s32le_planar
  --enable-decoder=pcm_s64be
  --enable-decoder=pcm_s64le
  --enable-decoder=pcm_s8
  --enable-decoder=pcm_s8_planar
  --enable-decoder=pcm_sga
  --enable-decoder=pcm_u16be
  --enable-decoder=pcm_u16le
  --enable-decoder=pcm_u24be
  --enable-decoder=pcm_u24le
  --enable-decoder=pcm_u32be
  --enable-decoder=pcm_u32le
  --enable-decoder=pcm_u8
  --enable-decoder=truehd
  --enable-decoder=vorbis
  --enable-decoder=libvorbis
  --enable-decoder=wmalossless
  --enable-decoder=wmapro
  --enable-decoder=wmav1
  --enable-decoder=wmav2
  --enable-decoder=wmavoice

  --enable-parser=aac
  --enable-parser=dca
  --enable-parser=aac_latm
  --enable-parser=mjpeg
  --enable-parser=ac3
  --enable-parser=dolby_e
  --enable-parser=mpeg4video
  --enable-parser=vorbis
  --enable-parser=h261
  --enable-parser=mpegaudio
  --enable-parser=vp3
  --enable-parser=av1
  --enable-parser=dvaudio
  --enable-parser=h263
  --enable-parser=mpegvideo
  --enable-parser=vp8
  --enable-parser=dvbsub
  --enable-parser=h264
  --enable-parser=opus
  --enable-parser=vp9
  --enable-parser=dvd_nav
  --enable-parser=vvc
  --enable-parser=hevc
  --enable-parser=ipu
  --enable-parser=qoi
  --enable-parser=xbm
  --enable-parser=flac
  --enable-parser=rv34
  --enable-parser=xma
  --enable-parser=sbc
  --enable-parser=xwd

  # end of selective enabling / disabling features

  # disable thread when FFMPEG_ST is NOT defined
  ${FFMPEG_ST:+ --disable-pthreads --disable-w32threads --disable-os2threads}
)

# echo "FFMPEG --list-decoders"
# emconfigure ./configure --list-decoders

# echo "FFMPEG --list-demuxers"
# emconfigure ./configure --list-demuxers

# echo "FFMPEG --list-parsers"
# emconfigure ./configure --list-parsers

emconfigure ./configure "${CONF_FLAGS[@]}" $@
emmake make -j
