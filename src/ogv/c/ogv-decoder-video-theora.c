#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>

#include "ogv-decoder-video.h"
#include "decoder-helper.h"

static int display_width = 0;
static int display_height = 0;
static AVCodecContext *pVideoCodecContext = NULL;
static AVPacket *pPacket = NULL;
static struct SwsContext *ptImgConvertCtx = NULL;
static AVFrame *pConvertedFrame = NULL;
static AVCodecParameters *pCodecParams = NULL;

void ogv_video_decoder_init(const char *paramsData, int paramsDataLength)
{
  logCallback("ogv-decoder-video-theora is being initialized with params length %d\n", paramsDataLength);
  pCodecParams = readCodecParams(paramsData, paramsDataLength);
  if (!pCodecParams)
  {
    logCallback("ogv-decoder-video-theora: failed to read codec params\n");
    return;
  }
  const AVCodec *pVideoCodec = avcodec_find_decoder(pCodecParams->codec_id);

  pVideoCodecContext = avcodec_alloc_context3(pVideoCodec);
  if (!pVideoCodecContext)
  {
    logCallback("failed to allocated memory for AVCodecContext\n");
    return;
  }
  // Fill the codec context based on the values from the supplied codec parameters
  // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
  if (avcodec_parameters_to_context(pVideoCodecContext, pCodecParams) < 0)
  {
    logCallback("failed to copy codec params to codec context\n");
    return;
  }

  // Initialize the AVCodecContext to use the given AVCodec.
  // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
  if (avcodec_open2(pVideoCodecContext, pVideoCodec, NULL) < 0)
  {
    logCallback("failed to open codec through avcodec_open2\n");
    return;
  }

  // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
  pPacket = av_packet_alloc();
  if (!pPacket)
  {
    logCallback("failed to allocate memory for AVFrame\n");
    return;
  }
  // TODO: consider YUV444P and other formats. They might be used without
  // using sws_scale. Check https://github.com/21pages/ogv.js/blob/ffmpeg/src/c/ogv-decoder-video-ffmpeg.c for example
  if (pCodecParams->format != AV_PIX_FMT_YUV420P)
  {
    logCallback(
        "Video pixel format is %d, need %d, initializing scaling context\n",
        pCodecParams->format,
        AV_PIX_FMT_YUV420P);
    // Need to convert each input video frame to yuv420p format using sws_scale.
    // Here we're initializing conversion context
    ptImgConvertCtx = sws_getContext(
        pCodecParams->width, pCodecParams->height,
        pCodecParams->format,
        pCodecParams->width, pCodecParams->height,
        AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);
    pConvertedFrame = av_frame_alloc();
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
  }
}

int ogv_video_decoder_async(void)
{
  return 0;
}

int ogv_video_decoder_process_header(const char *data, size_t data_len)
{
  logCallback("ogv-decoder-video-theora: ogv_video_decoder_process_header is being called. data=%p, data size=%lld\n", data, data_len);
  return 1;
}

int ogv_video_decoder_process_frame(const char *data, size_t data_len)
{
  // printf("ogv-decoder-video-theora: ogv_video_decoder_process_frame is being called. data size=%d\n", data_len);
  if (!data_len)
  {
    return 1;
  }
  const char *pBuf = data;
  int width = read_int32(&pBuf);
  int height = read_int32(&pBuf);
  int linesize0 = read_int32(&pBuf);
  int linesize1 = read_int32(&pBuf);
  int linesize2 = read_int32(&pBuf);
  const int datasize0 = linesize0 * height;
  const int datasize1 = linesize1 * height / 2;
  const int datasize2 = linesize2 * height / 2;
  printf("ogv-decoder-video-theora: width=%d, height=%d, \
	 linesize0=%d, linesize1=%d, linesize2=%d, \
		datasize0=%d, datasize1=%d, datasize2=%d\n",
         width, height,
         linesize0, linesize1, linesize2,
         datasize0, datasize1, datasize2);

  ogvjs_callback_frame(
      pBuf, linesize0,
      pBuf + datasize0, linesize1,
      pBuf + datasize0 + datasize1, linesize2,
      width, height,
      width / 2, height / 2,
      width, height,
      0, 0,
      width, height);
  return 1;
}

void ogv_video_decoder_destroy(void)
{
  avcodec_parameters_free(&pCodecParams);
}
