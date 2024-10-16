#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include "ogv-decoder-video.h"

#ifdef __cplusplus
}
#endif

#include "decoder-helper.h"
#include "ogv-thread-support.h"
#include <deque>

class DecodedFrame
{
public:
  DecodedFrame(AVFrame *frame) : m_frame(frame) {}
  ~DecodedFrame()
  {
    av_frame_free(&m_frame);
  }
  AVFrame *m_frame;
};

PacketBuffer videoPackets(PACKET_BUFFER_SIZE);
std::deque<DecodedFrame> decodedFrames;
std::deque<int64_t> requestedPts;
bool firstEverPts = true;
int threadCount = -1;

struct FFmpegRamDecoder
{
  AVCodecContext *c_;
  AVFrame *frame_;
  AVPacket *pkt_;
};

static struct FFmpegRamDecoder *ffmpegRamDecoder = NULL;
static AVCodecParameters *pCodecParams = NULL;
static struct SwsContext *pSwsContext = NULL;
int64_t ptsReturned = AV_NOPTS_VALUE;
double decodingStartTime = -1;
AVRational timeBase = {1, 1};
int unreceivedPackets = 0;
double totalSendTime = 0;
double totalReceiveFrameTime = 0;
double totalConversionTime = 0;
const int maxDecodedFrames = 3;
double totalReadInputTime = 0;
double latestReadInputTime = 0;

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
  // Must synchronize with value in ogv-decoder-video.sh:
  // ${FFMPEG_MT:+ -sPTHREAD_POOL_SIZE=4}
  const int max_cores = threadCount;
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
    logMessage("FFmpeg decoder: avcodec_find_decoder_by_name failed\n");
    return -1;
  }
  logMessage("FFmpeg decoder: Found decoder %s (%s)\n", codec->name, codec->long_name);
  if (!(d->c_ = avcodec_alloc_context3(codec)))
  {
    logMessage("FFmpeg decoder: Could not allocate video codec context\n");
    return -1;
  }
  // Fill the codec context based on the values from the supplied codec parameters
  if (avcodec_parameters_to_context(d->c_, pCodecParams) < 0)
  {
    logMessage("FFmpeg decoder: failed to copy codec params to codec context\n");
    return -1;
  }

  d->c_->flags |= AV_CODEC_FLAG_LOW_DELAY;
  // See https://gist.github.com/jimj316/931b8ccbbdbcb5b2d3337879b4829e25#file-main-cpp-L137
  d->c_->flags2 |= AV_CODEC_FLAG2_FAST;
  d->c_->skip_loop_filter = AVDISCARD_DEFAULT; // NB: changing this makes no difference
  d->c_->skip_frame = AVDISCARD_DEFAULT;       //
  d->c_->skip_idct = AVDISCARD_DEFAULT;        // NB: changing this makes no difference
  d->c_->thread_count = get_thread_count();

  // d->c_->workaround_bugs = AV_EF_IGNORE_ERR;
  // d->c_->err_recognition = 0;
  // d->c_->error_concealment = 1;
  // d->c_->debug = 1;
  // See https://stackoverflow.com/a/69025953/156973
  // Try using multi-threading capabilities in this order: slice, then frame
  // Decoding h265 video with frame-threading results in high delay
  if (0)
  {
  }
  else if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
  {
    d->c_->thread_type = FF_THREAD_SLICE;
    printf("Video decoder supports slice-type multi-threading\n");
  }
  else if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
  {
    d->c_->thread_type = FF_THREAD_FRAME;
    printf("Video decoder supports frame-type multi-threading\n");
  }
  else if (codec->capabilities & AV_CODEC_CAP_OTHER_THREADS)
  {
    printf("Video decoder supports multithreading through a method other than slice- or frame-level multithreading. Attempting to use %d threads.\n", d->c_->thread_count);
  }
  else
  {
    // d->c_->thread_count = 1; // don't use multithreading
    printf("Video decoder does not support multi-threading.\n");
  }
  printf("Decoder was configured to use %d thread(s).\n", d->c_->thread_count);
  if (!(d->pkt_ = av_packet_alloc()))
  {
    logMessage("FFmpeg decoder: av_packet_alloc failed\n");
    return -1;
  }

  if (!(d->frame_ = av_frame_alloc()))
  {
    logMessage("FFmpeg decoder: av_frame_alloc failed\n");
    return -1;
  }

  if ((ret = avcodec_open2(d->c_, codec, NULL)) != 0)
  {
    logMessage("FFmpeg decoder: avcodec_open2 failed\n");
    return -1;
  }
  logMessage("FFmpeg decoder: reset ok\n");
#ifdef __EMSCRIPTEN_PTHREADS__
  logMessage("FFmpeg decoder: PTHREADS enabled\n");
#else
  logMessage("FFmpeg decoder: PTHREADS disabled\n");
#endif
  unreceivedPackets = 0;
  totalSendTime = 0;
  totalReceiveFrameTime = 0;
  totalConversionTime = 0;
  requestedPts.clear();
  ptsReturned = AV_NOPTS_VALUE;
  videoPackets.clear();
  decodedFrames.clear();
  firstEverPts = true;
  return 0;
}

static void do_init(const char *paramsData)
{
  printf("FFmpeg decoder: do_init started\n");
  // av_log_set_level(AV_LOG_DEBUG);
  const char *pBuf = paramsData;
  threadCount = readInt32(&pBuf);
  int32_t debugDecoder = readInt32(&pBuf);
  loggingEnabled = debugDecoder ? true : false;
  printf("FFmpeg decoder: do_init: init params requested %d threads, debugDecoder: %d.\n", threadCount, debugDecoder);
  pCodecParams = readCodecParams(pBuf, &timeBase);
  if (!pCodecParams)
  {
    logMessage("FFmpeg decoder: ogv-decoder-video-theora: failed to read codec params\n");
    return;
  }

  if (ffmpegRamDecoder)
  {
    logMessage("FFmpeg decoder: do_init freeing decoder.\n");
    free_decoder(ffmpegRamDecoder);
    free(ffmpegRamDecoder);
    ffmpegRamDecoder = NULL;
  }
  logMessage("FFmpeg decoder: do_init is allocating memory for decoder\n");
  ffmpegRamDecoder = (struct FFmpegRamDecoder *)malloc(sizeof(struct FFmpegRamDecoder));
  if (!ffmpegRamDecoder)
  {
    printf("FFmpeg decoder: failed to allocate memory for decoder\n");
    return;
  }
  ffmpegRamDecoder->c_ = NULL;
  ffmpegRamDecoder->frame_ = NULL;
  ffmpegRamDecoder->pkt_ = NULL;

  logMessage("FFmpeg decoder: do_init resetting decoder\n");

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
    logMessage(
        "FFmpeg decoder: do_init: source video has pixel format %d, initializing conversion context to convert into %d pixel format.\n",
        pCodecParams->format,
        AV_PIX_FMT_YUV420P);
    // Need to convert each input video frame to yuv420p format using sws_scale.
    // Here we're initializing conversion context
    pSwsContext = sws_getContext(
        pCodecParams->width, pCodecParams->height,
        (enum AVPixelFormat)pCodecParams->format,
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
      logMessage("FFmpeg decoder: Failed to initialize converted frame buffer. Error code: %d (%s)\n",
                 bufRet, av_err2str(bufRet));
      return;
    }
    av_frame_free(&pConvertedFrame);
  }
  logMessage("FFmpeg decoder: do_init finished\n");
}

void do_destroy(void)
{
  // TODO: implement
}

static AVFrame *copy_image(const AVFrame *src)
{
  // return src;
  AVFrame *dest = av_frame_alloc();
  if (!dest)
  {
    printf("copy_image failed to allocate frame\n");
    return NULL;
  }
  // Copy src to dest, see https://stackoverflow.com/a/38809306/156973 for details
  dest->format = src->format;
  dest->width = src->width;
  dest->height = src->height;
  // dest->channels = src->channels;
  // dest->channel_layout = src->channel_layout;
  dest->ch_layout = src->ch_layout;
  dest->nb_samples = src->nb_samples;
  int ret = av_frame_get_buffer(dest, 0);
  if (ret)
  {
    printf("av_frame_get_buffer failed. Error code: %d (%s)\n", ret, av_err2str(ret));
    return NULL;
  }
  ret = av_frame_copy(dest, src);
  if (ret)
  {
    printf("av_frame_copy failed. Error code: %d (%s)\n", ret, av_err2str(ret));
    return NULL;
  }
  ret = av_frame_copy_props(dest, src);
  if (ret)
  {
    printf("av_frame_copy_props failed. Error code: %d (%s)\n", ret, av_err2str(ret));
    return NULL;
  }
  return dest;
}

AVFrame *getConvertedFrame(AVFrame *pDecodedFrame)
{
  logMessage("FFmpeg decoder: ogv-decoder-video-theora: getConvertedFrame is being called\n");
  if (pDecodedFrame->format == AV_PIX_FMT_YUV420P ||
      pDecodedFrame->format == AV_PIX_FMT_YUV444P)
  {
    return pDecodedFrame;
  }
  logMessage("FFmpeg decoder: ogv-decoder-video-theora: av_frame_alloc\n");

  AVFrame *pConvertedFrame = av_frame_alloc();
  if (!pConvertedFrame)
  {
    logMessage("FFmpeg decoder: ogv-decoder-video-theora: failed to create frame for conversion");
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
    logMessage("FFmpeg decoder: ogv-decoder-video-theora: failed to allocate buffer for converted frame\n");
    // av_frame_free(&pDecodedFrame);
    return NULL;
  }
  logMessage("FFmpeg decoder: ogv-decoder-video-theora: calling sws_scale\n");
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
    logMessage("FFmpeg decoder: ogv-decoder-video-theora error: scaling failed: sws_scale returned %d, expected %d\n", scaleResult, pConvertedFrame->height);
    // av_frame_free(&pDecodedFrame);
    return NULL;
  }
  logMessage("FFmpeg decoder: ogv-decoder-video-theora: sws_scale returned %d\n", scaleResult);
  // av_frame_free(&pDecodedFrame);
  return pConvertedFrame;
}

static void read_input(const char *data, size_t data_len)
{
  double readInputStart = emscripten_get_now();
  if (!data)
  {
    // NULL data signals syncing the decoder state
    call_main_return(NULL, 1);
    return;
  }

  const char *pBuf = data;
  const int32_t packetCount = readInt32(&pBuf);
  logMessage("FFmpeg decoder: Decoding batch of %d packets\n", packetCount);
  if (firstEverPts)
  {
    // This is the first frame ever requested
    decodingStartTime = emscripten_get_now();
    firstEverPts = false;
  }
  requestedPts.push_back(readInt64(&pBuf));
  logMessage("FFmpeg decoder: Requested pts %lld\n", requestedPts.back());
  // First, add all packets to videoPackets deque (skipping those that are already in deque)
  for (int i = 0; i < packetCount; ++i)
  {
    const int32_t dummy = readInt32(&pBuf);
    if (dummy != 0)
    {
      printf("FFmpeg decoder: Invalid packet signature\n");
      call_main_return(NULL, 0);
      return;
    }
    const int64_t pts = readInt64(&pBuf);
    const int64_t dts = readInt64(&pBuf);
    const int32_t packetSize = readInt32(&pBuf);
    uint8_t *packetData = (uint8_t *)pBuf;
    pBuf += packetSize;
    if (!videoPackets.hasPacketWithPts(pts))
    {
      logMessage("FFmpeg decoder: Adding packet to buffer. pts = %lld, dts = %lld, current buffer size: %d.\n",
                 pts, dts, videoPackets.size());
      videoPackets.emplace_back(pts, dts, packetSize, packetData);
    }
    else
    {
      logMessage("FFmpeg decoder: Skipping packet pts = %lld, dts = %lld, current buffer size: %d.\n",
                 pts, dts, videoPackets.size());
    }
  }
  double readInputEnd = emscripten_get_now();
  latestReadInputTime = readInputEnd - readInputStart;
  totalReadInputTime += latestReadInputTime;
}
static bool try_processing()
{
  logMessage("FFmpeg decoder: try_processing\n");
  if (!requestedPts.empty())
  {
    int64_t oldestRequestedPts = requestedPts.front();
    // Remove all decoded frames that have pts less than requested one
    while (!decodedFrames.empty() && decodedFrames.front().m_frame->pts < oldestRequestedPts)
    {
      logMessage("FFmpeg decoder: Removing decoded frame pts = %lld, dts = %lld, current size of decoded frames buffer: %d, requested pts: %lld.\n",
                 decodedFrames.front().m_frame->pts,
                 decodedFrames.front().m_frame->pkt_dts,
                 decodedFrames.size(), oldestRequestedPts);
      decodedFrames.pop_front();
    }
    if (ptsReturned != oldestRequestedPts)
    {
      // Next, check if we've already decoded a frame with requested pts
      if (!decodedFrames.empty())
      {
        const DecodedFrame &firstFrame = decodedFrames.front();
        if (firstFrame.m_frame->pts == oldestRequestedPts)
        {
          logMessage("FFmpeg decoder: We found requested frame %lld\n", oldestRequestedPts);
          call_main_return((void *)copy_image(firstFrame.m_frame), 1);
          ptsReturned = oldestRequestedPts;
          requestedPts.pop_front();
          return true;
        }
      }
    }
  }
  // Now, attempt to receive a single frame and put it in decodedFrames
  if (decodedFrames.size() < maxDecodedFrames)
  {
    logMessage("FFmpeg decoder: Current size of decoded frames buffer is %d < %d, attempting to receive a frame.\n",
               decodedFrames.size(), maxDecodedFrames);
    double receiveFrameStart = emscripten_get_now();
    int ret = avcodec_receive_frame(ffmpegRamDecoder->c_, ffmpegRamDecoder->frame_);
    double receiveFrameEnd = emscripten_get_now();
    double receiveFrameTime = receiveFrameEnd - receiveFrameStart;
    totalReceiveFrameTime += receiveFrameTime;

    if (ret == AVERROR(EAGAIN))
    {
      logMessage("FFmpeg decoder: Not enough data to receive a frame.\n");
    }
    else if (ret != 0)
    {
      printf("avcodec_receive_frame failed, ret=%d, msg=%s\n", ret, av_err2str(ret));
      call_main_return(NULL, 0);
    }
    else
    {
      logMessage("FFmpeg decoder: Decoded frame with pts %lld (dts %lld) in %.3f ms: %p, d->frame_-:%p, width: %d, height: %d, linesize[0]: %d\n",
                 ffmpegRamDecoder->frame_->pts,
                 ffmpegRamDecoder->frame_->pkt_dts,
                 receiveFrameTime,
                 ffmpegRamDecoder,
                 ffmpegRamDecoder->frame_,
                 ffmpegRamDecoder->frame_->width,
                 ffmpegRamDecoder->frame_->height,
                 ffmpegRamDecoder->frame_->linesize[0]);
      decodedFrames.emplace_back(copy_image(ffmpegRamDecoder->frame_));
      --unreceivedPackets;
      return true;
    }
  }

  if (!videoPackets.empty())
  {
    const DemuxedPacket &packet = videoPackets.front();
    logMessage("FFmpeg decoder: Attempting to send packet with pts %lld (dts %lld), packet size: %d\n", packet.m_pts, packet.m_dts, packet.m_dataSize);

    ffmpegRamDecoder->pkt_->data = packet.m_pData;
    ffmpegRamDecoder->pkt_->size = packet.m_dataSize;
    ffmpegRamDecoder->pkt_->pts = packet.m_pts;
    ffmpegRamDecoder->pkt_->dts = packet.m_dts;

    double sendPacketStart = emscripten_get_now();
    int ret = avcodec_send_packet(ffmpegRamDecoder->c_, ffmpegRamDecoder->pkt_);
    double sendPacketEnd = emscripten_get_now();
    double sendTime = sendPacketEnd - sendPacketStart;
    totalSendTime += sendTime;
    if (ret == AVERROR(EAGAIN))
    {
      logMessage("FFmpeg decoder: avcodec_send_packet's buffer is full\n");
    }
    else if (ret < 0)
    {
      printf("avcodec_send_packet failed, ret=%d, msg=%s\n", ret, av_err2str(ret));
    }
    else
    {
      logMessage("FFmpeg decoder: avcodec_send_packet took %.3f ms\n", sendTime);
      videoPackets.pop_front();
      ++unreceivedPackets;
      return true;
    }
  }
  // There is nothing to do here
  logMessage("FFmpeg decoder: Couldn't receive a frame or send a packet, exiting. Unsent packets: %d, decodedFrames: %d, sent but unreceived packets: %d.\n",
             videoPackets.size(), decodedFrames.size(), unreceivedPackets);
  return false;
}

static int process_frame_return(void *image)
{
  AVFrame *frame = (AVFrame *)image;
  logMessage("FFmpeg decoder: process_frame_return: %p\n", frame);
  if (!frame)
  {
    return 0;
  }
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
  logMessage("FFmpeg decoder: process_frame_return: w: %d, h: %d\n", frame->width, frame->height);
  int oldFormat = frame->format;
  double conversionTime = 0;
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
    AVFrame *pConvertedFrame = getConvertedFrame(frame);
    double conversionEnd = emscripten_get_now();
    conversionTime = conversionEnd - conversionStart;
    totalConversionTime += conversionTime;
    logMessage("FFmpeg decoder: Converted frame from %d to %d in %.3f ms\n", oldFormat, pConvertedFrame->format, conversionTime);
    chromaWidth = pConvertedFrame->width >> 1;
    chromaHeight = height >> 1;
    converted = 1;
    av_frame_free(&frame);
    frame = pConvertedFrame;
  }
  }
  double frameDecodedTime = emscripten_get_now();
  double actualTime = (frameDecodedTime - decodingStartTime) / 1000.0;
  double desiredTime = frame->pts * av_q2d(timeBase);
  double lag = actualTime - desiredTime;
  double totalWorkTime = (totalReceiveFrameTime + totalSendTime + totalConversionTime) / 1000.0;
  double workLagTime = totalWorkTime - desiredTime;
  // To parse this output as JavaScript, copy the output from console
  // and the commented out lines before and after the entire output:
  /*
  cc = [
  */
  printf("[%lld, %.3f, %.3f, %.3f, %.3f, %d, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %d, %d],\n",
         frame->pts,
         desiredTime,
         actualTime,
         lag,
         totalWorkTime,
         unreceivedPackets,
         conversionTime,
         totalReceiveFrameTime,
         totalSendTime,
         totalConversionTime,
         workLagTime,
         latestReadInputTime,
         totalReadInputTime,
         (int)videoPackets.size(),
         (int)decodedFrames.size());
  /*
    ];
    console.log(cc.map(x => x.join('\t')).join('\n'));
  */
  ogvjs_callback_frame(frame->data[0], frame->linesize[0],
                       frame->data[1], frame->linesize[1],
                       frame->data[2], frame->linesize[2],
                       frame->width, height,
                       chromaWidth, chromaHeight,
                       frame->width, frame->height,  // crop size
                       0, 0,                         // crop pos
                       frame->width, frame->height); // render size
  av_frame_free(&frame);
#ifdef __EMSCRIPTEN_PTHREADS__
  // We were given a copy, so free it.
  // vpx_img_free(image); // todo
#else
  // Image will be freed implicitly by next decode call.
#endif
  return 1;
}