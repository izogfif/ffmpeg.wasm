#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#ifdef __cplusplus
}
#endif

#include "ogv-decoder-video.h"
#include "ogv-thread-support.h"
#include "decoder-helper.h"

struct FFmpegRamDecoder
{
  AVCodecContext *c_;
  AVFrame *frame_;
  AVPacket *pkt_;
};

static struct FFmpegRamDecoder *ffmpegRamDecoder = NULL;
static AVCodecParameters *pCodecParams = NULL;
static struct SwsContext *pSwsContext = NULL;

static void free_decoder(struct FFmpegRamDecoder *d)
{
  if (d->frame_)
    av_frame_free(&d->frame_);
  if (d->pkt_)
    av_packet_free(&d->pkt_);
  if (d->c_)
    avcodec_free_context(&d->c_);

  d->frame_ = NULL;
  d->pkt_ = NULL;
  d->c_ = NULL;
}

static int get_thread_count()
{
#ifdef __EMSCRIPTEN_PTHREADS__
  const int max_cores = 8; // max threads for UHD tiled decoding
  int cores = emscripten_num_logical_cores();
  if (cores == 0)
  {
    // Safari 15 does not report navigator.hardwareConcurrency...
    // Assume at least two fast cores are available.
    cores = 2;
  }
  else if (cores > max_cores)
  {
    cores = max_cores;
  }
  return cores;
#else
  return 1;
#endif
}

static int reset(struct FFmpegRamDecoder *d)
{
  free_decoder(d);
  const AVCodec *codec = NULL;
  int ret;
  if (!(codec = avcodec_find_decoder(pCodecParams->codec_id)))
  {
    logCallback("avcodec_find_decoder_by_name failed\n");
    return -1;
  }
  logCallback("Found decoder %s (%s)\n", codec->name, codec->long_name);
  if (!(d->c_ = avcodec_alloc_context3(codec)))
  {
    logCallback("Could not allocate video codec context\n");
    return -1;
  }
  // Fill the codec context based on the values from the supplied codec parameters
  if (avcodec_parameters_to_context(d->c_, pCodecParams) < 0)
  {
    logCallback("failed to copy codec params to codec context\n");
    return -1;
  }

  d->c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  // See https://gist.github.com/jimj316/931b8ccbbdbcb5b2d3337879b4829e25#file-main-cpp-L137
  d->c_->flags2 |= AV_CODEC_FLAG2_FAST;
  d->c_->skip_loop_filter = AVDISCARD_DEFAULT; // NB: changing this makes no difference
  d->c_->skip_frame = AVDISCARD_DEFAULT;       //
  d->c_->skip_idct = AVDISCARD_DEFAULT;        // NB: changing this makes no difference
  // d->c_->thread_count = get_thread_count();
  // See https://stackoverflow.com/a/69025953/156973
  if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
    d->c_->thread_type = FF_THREAD_FRAME;
  else if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
    d->c_->thread_type = FF_THREAD_SLICE;
  else
    d->c_->thread_count = 1; // don't use multithreading

  if (!(d->pkt_ = av_packet_alloc()))
  {
    logCallback("av_packet_alloc failed\n");
    return -1;
  }

  if (!(d->frame_ = av_frame_alloc()))
  {
    logCallback("av_frame_alloc failed\n");
    return -1;
  }

  if ((ret = avcodec_open2(d->c_, codec, NULL)) != 0)
  {
    logCallback("avcodec_open2 failed\n");
    return -1;
  }
  logCallback("reset ok\n");
#ifdef __EMSCRIPTEN_PTHREADS__
  logCallback("PTHREADS enabled\n");
#else
  logCallback("PTHREADS disabled\n");
#endif
  return 0;
}

static void do_init(const char *paramsData)
{
  logCallback("ogv-decoder-video-theora is being initialized\n");
  pCodecParams = readCodecParams(paramsData);
  if (!pCodecParams)
  {
    logCallback("ogv-decoder-video-theora: failed to read codec params\n");
    return;
  }

  logCallback("do init 1\n");
  if (ffmpegRamDecoder)
  {
    free_decoder(ffmpegRamDecoder);
    free(ffmpegRamDecoder);
    ffmpegRamDecoder = NULL;
  }
  logCallback("do init 2\n");
  ffmpegRamDecoder = malloc(sizeof(struct FFmpegRamDecoder));
  if (!ffmpegRamDecoder)
  {
    return;
  }
  ffmpegRamDecoder->c_ = NULL;
  ffmpegRamDecoder->frame_ = NULL;
  ffmpegRamDecoder->pkt_ = NULL;

  logCallback("do init 3\n");

  if (reset(ffmpegRamDecoder) != 0)
  {
    free_decoder(ffmpegRamDecoder);
    free(ffmpegRamDecoder);
    ffmpegRamDecoder = NULL;
    return;
  }

  if (pCodecParams->format != AV_PIX_FMT_YUV420P &&
      pCodecParams->format != AV_PIX_FMT_YUV444P)
  {
    logCallback(
        "Video pixel format is %d, need %d, initializing scaling context\n",
        pCodecParams->format,
        AV_PIX_FMT_YUV420P);
    // Need to convert each input video frame to yuv420p format using sws_scale.
    // Here we're initializing conversion context
    pSwsContext = sws_getContext(
        pCodecParams->width, pCodecParams->height,
        pCodecParams->format,
        pCodecParams->width, pCodecParams->height,
        AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    // Just for testing purposes
    AVFrame *pConvertedFrame = av_frame_alloc();
    pConvertedFrame->width = pCodecParams->width;
    pConvertedFrame->height = pCodecParams->height;
    pConvertedFrame->format = AV_PIX_FMT_YUV420P;
    int bufRet = av_frame_get_buffer(pConvertedFrame, 0);
    if (bufRet)
    {
      logCallback("Failed to initialize converted frame buffer. Error code: %d (%s)\n",
                  bufRet, av_err2str(bufRet));
      return;
    }
    av_frame_free(&pConvertedFrame);
  }
  logCallback("do init ok\n");
}

void do_destroy(void)
{
  // should tear instance down, but meh
}

static int do_decode()
{
  int ret;
  bool decoded = false;
  double sendPacketStart = emscripten_get_now();
  ret = avcodec_send_packet(ffmpegRamDecoder->c_, ffmpegRamDecoder->pkt_);
  double sendPacketEnd = emscripten_get_now();
  if (ret < 0)
  {
    logCallback("avcodec_send_packet failed, ret=%d, msg=%s\n", ret, av_err2str(ret));
    return ret;
  }
  logCallback("avcodec_send_packet took %.3f ms\n", sendPacketEnd - sendPacketStart);
  printf("[%lld, %.3f], \n", ffmpegRamDecoder->pkt_->pts, sendPacketEnd - sendPacketStart);

  while (ret >= 0)
  {
    double decodeStart = emscripten_get_now();
    if ((ret = avcodec_receive_frame(ffmpegRamDecoder->c_, ffmpegRamDecoder->frame_)) != 0)
    {
      if (ret != AVERROR(EAGAIN))
      {
        logCallback("avcodec_receive_frame failed,  ret=%d, msg=%s\n", ret, av_err2str(ret));
      }
      break;
    }
    double decodeEnd = emscripten_get_now();
    decoded = true;
    logCallback("Decoded frame in %.3f ms: %p, d->frame_-:%p, width: %d, height: %d, linesize[0]: %d\n",
                decodeEnd - decodeStart,
                ffmpegRamDecoder,
                ffmpegRamDecoder->frame_,
                ffmpegRamDecoder->frame_->width,
                ffmpegRamDecoder->frame_->height,
                ffmpegRamDecoder->frame_->linesize[0]);
    break;
  }
  av_packet_unref(ffmpegRamDecoder->pkt_);
  return decoded ? 0 : -1;
}

static int decode(const uint8_t *data, int length)
{
  int ret = -1;
  if (!data || !length)
  {
    logCallback("illegal decode parameter\n");
    return -1;
  }
  const uint8_t *pBuf = data;
  const int32_t packetCount = readInt32(&pBuf);
  for (int i = 0; i < packetCount; ++i)
  {
    const int64_t pts = readInt64(&pBuf);
    const int32_t packetSize = readInt32(&pBuf);
    ffmpegRamDecoder->pkt_->data = pBuf;
    ffmpegRamDecoder->pkt_->size = packetSize;
    ffmpegRamDecoder->pkt_->pts = pts;
    logCallback("decoding packet #%d of %d, packet size: %d\n", i, packetCount, packetSize);
    ret = do_decode();
    // TODO: implement asynchronous decoding of other packets
    return ret;
  }
}

static AVFrame *copy_image(AVFrame *src)
{
  // AVFrame* dest = av_frame_alloc();
  // if (!dest) {
  // 	return NULL;
  // }
  // // copy src to dest
  // dest->format = src->format;
  // dest->width = src->width;
  // dest->height = src->height;
  // dest->channels = src->channels;
  // dest->channel_layout = src->channel_layout;
  // dest->nb_samples = src->nb_samples;
  // av_frame_get_buffer(dest, 32); // 32 ??
  // av_frame_copy(dest, src);
  // av_frame_copy_props(dest, src);
  // return dest;
  return src;
}

AVFrame *getConvertedFrame(AVFrame *pDecodedFrame)
{
  logCallback("ogv-decoder-video-theora: getConvertedFrame is being called\n");
  if (pDecodedFrame->format == AV_PIX_FMT_YUV420P ||
      pDecodedFrame->format == AV_PIX_FMT_YUV444P)
  {
    return pDecodedFrame;
  }
  logCallback("ogv-decoder-video-theora: av_frame_alloc\n");

  AVFrame *pConvertedFrame = av_frame_alloc();
  if (!pConvertedFrame)
  {
    logCallback("ogv-decoder-video-theora: failed to create frame for conversion");
    // av_frame_free(&pDecodedFrame);
    return NULL;
  }
  pConvertedFrame->width = pDecodedFrame->width;
  pConvertedFrame->height = pDecodedFrame->height;
  pConvertedFrame->format = AV_PIX_FMT_YUV420P;
  pConvertedFrame->pts = pDecodedFrame->pts;
  int get_buffer_res = av_frame_get_buffer(pConvertedFrame, 0);
  if (get_buffer_res)
  {
    logCallback("ogv-decoder-video-theora: failed to allocate buffer for converted frame\n");
    // av_frame_free(&pDecodedFrame);
    return NULL;
  }
  logCallback("ogv-decoder-video-theora: calling sws_scale\n");
  int scaleResult = sws_scale(
      pSwsContext,
      (const uint8_t *const *)pDecodedFrame->data,
      pDecodedFrame->linesize,
      0,
      pDecodedFrame->height,
      (uint8_t *const *)pConvertedFrame->data,
      pConvertedFrame->linesize);
  if (scaleResult != pConvertedFrame->height)
  {
    logCallback("ogv-decoder-video-theora error: scaling failed: sws_scale returned %d, expected %d\n", scaleResult, pConvertedFrame->height);
    // av_frame_free(&pDecodedFrame);
    return NULL;
  }
  logCallback("ogv-decoder-video-theora: sws_scale returned %d\n", scaleResult);
  // av_frame_free(&pDecodedFrame);
  return pConvertedFrame;
}

static void process_frame_decode(const char *data, size_t data_len)
{
  if (!data)
  {
    // NULL data signals syncing the decoder state
    call_main_return(NULL, 1);
    return;
  }

  int ret = decode((const uint8_t *)data, data_len);
  logCallback("Right after decode:%p, ffmpegRamDecoder->frame_:%p, width: %d, height: %d, linesize[0]: %d\n", ffmpegRamDecoder, ffmpegRamDecoder->frame_, ffmpegRamDecoder->frame_->width, ffmpegRamDecoder->frame_->height, ffmpegRamDecoder->frame_->linesize[0]);
  if (ret != 0)
  {
    call_main_return(NULL, 0);
    return;
  }
  logCallback("ffmpegRamDecoder:%p, ffmpegRamDecoder->frame_:%p, width: %d, height: %d, linesize[0]: %d\n", ffmpegRamDecoder, ffmpegRamDecoder->frame_, ffmpegRamDecoder->frame_->width, ffmpegRamDecoder->frame_->height, ffmpegRamDecoder->frame_->linesize[0]);
  AVFrame *frame = ffmpegRamDecoder->frame_;

  // send back to the main thread for extraction.
#ifdef __EMSCRIPTEN_PTHREADS__
  // Copy off main thread and send asynchronously...
  // This allows decoding to continue without waiting
  // for the main thread.
  call_main_return(copy_image(frame), 0);
#else
  call_main_return(frame, 1);
#endif
}

static int process_frame_return(void *image)
{
  AVFrame *frame = (AVFrame *)image;
  logCallback("process_frame_return: %p\n", frame);
  if (frame)
  {
    // image->h is inexplicably large for small sizes.
    // don't both copying the extra, but make sure it's chroma-safe.
    int height = frame->height;
    if ((height & 1) == 1)
    {
      // copy one extra row if need be
      // not sure this is even possible
      // but defend in depth
      height++;
    }

    int converted = 0;
    int chromaWidth, chromaHeight;
    logCallback("process_frame_return: w: %d, h: %d\n", frame->width, frame->height);
    int oldFormat = frame->format;
    switch (frame->format)
    {
    case AV_PIX_FMT_YUV420P:
      chromaWidth = frame->width >> 1;
      chromaHeight = height >> 1;
      break;
    case AV_PIX_FMT_YUV444P:
      chromaWidth = frame->width;
      chromaHeight = height;
      break;
    default:
    {
      double conversionStart = emscripten_get_now();
      frame = getConvertedFrame(frame);
      double conversionEnd = emscripten_get_now();
      logCallback("Converted frame from %d to %d in %.3f ms\n", oldFormat, frame->format, conversionEnd - conversionStart);
      chromaWidth = frame->width >> 1;
      chromaHeight = height >> 1;
      converted = 1;
    }
    }
    ogvjs_callback_frame(frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1],
                         frame->data[2], frame->linesize[2],
                         frame->width, height,
                         chromaWidth, chromaHeight,
                         frame->width, frame->height,  // crop size
                         0, 0,                         // crop pos
                         frame->width, frame->height); // render size
    if (converted)
    {
      av_frame_free(&frame);
    }
#ifdef __EMSCRIPTEN_PTHREADS__
    // We were given a copy, so free it.
    // vpx_img_free(image); // todo
#else
    // Image will be freed implicitly by next decode call.
#endif
    return 1;
  }
  else
  {
    return 0;
  }
}