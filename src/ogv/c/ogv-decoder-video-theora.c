#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include "ogv-decoder-video.h"
#include "decoder-helper.h"

static int display_width = 0;
static int display_height = 0;
static AVCodecContext *pVideoCodecContext = NULL;
static AVPacket *pPacket = NULL;
static struct SwsContext *pSwsContext = NULL;
static AVFrame *pConvertedFrame = NULL;
static AVCodecParameters *pCodecParams = NULL;

void ogv_video_decoder_init(const char *paramsData, int paramsDataLength)
{
  logMessage("ogv-decoder-video-theora is being initialized with params length %d\n", paramsDataLength);
  pCodecParams = readCodecParams(paramsData);
  if (!pCodecParams)
  {
    logMessage("ogv-decoder-video-theora: failed to read codec params\n");
    return;
  }
  const AVCodec *pVideoCodec = avcodec_find_decoder(pCodecParams->codec_id);

  pVideoCodecContext = avcodec_alloc_context3(pVideoCodec);
  if (!pVideoCodecContext)
  {
    logMessage("failed to allocated memory for AVCodecContext\n");
    return;
  }

  // Fill the codec context based on the values from the supplied codec parameters
  if (avcodec_parameters_to_context(pVideoCodecContext, pCodecParams) < 0)
  {
    logMessage("failed to copy codec params to codec context\n");
    return;
  }

  if (avcodec_open2(pVideoCodecContext, pVideoCodec, NULL) < 0)
  {
    logMessage("failed to open codec through avcodec_open2\n");
    return;
  }

  pPacket = av_packet_alloc();
  if (!pPacket)
  {
    logMessage("failed to allocate memory for AVFrame\n");
    return;
  }
  // TODO: consider YUV444P and other formats. They might be used without
  // using sws_scale. Check https://github.com/21pages/ogv.js/blob/ffmpeg/src/c/ogv-decoder-video-ffmpeg.c for example
  if (pCodecParams->format != AV_PIX_FMT_YUV420P)
  {
    logMessage(
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
    pConvertedFrame = av_frame_alloc();
    pConvertedFrame->width = pCodecParams->width;
    pConvertedFrame->height = pCodecParams->height;
    pConvertedFrame->format = AV_PIX_FMT_YUV420P;
    int bufRet = av_frame_get_buffer(pConvertedFrame, 0);
    if (bufRet)
    {
      logMessage("Failed to initialize converted frame buffer. Error code: %d (%s)\n",
                  bufRet, av_err2str(bufRet));
      return;
    }
  }
  pPacket = av_packet_alloc();
  if (!pPacket)
  {
    logMessage("Failed to allocate memory for video packet");
  }
}

int ogv_video_decoder_async(void)
{
  return 0;
}

int ogv_video_decoder_process_header(const char *data, size_t data_len)
{
  logMessage("ogv-decoder-video-theora: ogv_video_decoder_process_header is being called. data=%p, data size=%lld\n", data, data_len);
  return 1;
}

AVFrame *getConvertedFrame(AVFrame *pDecodedFrame)
{
  logMessage("ogv-decoder-video-theora: getConvertedFrame is being called\n");
  if (pDecodedFrame->format == AV_PIX_FMT_YUV420P)
  {
    return pDecodedFrame;
  }
  logMessage("ogv-decoder-video-theora: av_frame_alloc\n");

  AVFrame *pConvertedFrame = av_frame_alloc();
  if (!pConvertedFrame)
  {
    logMessage("ogv-decoder-video-theora: failed to create frame for conversion");
    av_frame_free(&pDecodedFrame);
    return NULL;
  }
  pConvertedFrame->width = pDecodedFrame->width;
  pConvertedFrame->height = pDecodedFrame->height;
  pConvertedFrame->format = AV_PIX_FMT_YUV420P;
  pConvertedFrame->pts = pDecodedFrame->pts;
  int get_buffer_res = av_frame_get_buffer(pConvertedFrame, 0);
  if (get_buffer_res)
  {
    logMessage("ogv-decoder-video-theora: failed to allocate buffer for converted frame\n");
    av_frame_free(&pDecodedFrame);
    return NULL;
  }
  logMessage("ogv-decoder-video-theora: calling sws_scale\n");
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
    logMessage("ogv-decoder-video-theora error: scaling failed: sws_scale returned %d, expected %d\n", scaleResult, pConvertedFrame->height);
    av_frame_free(&pDecodedFrame);
    return NULL;
  }
  logMessage("ogv-decoder-video-theora: sws_scale returned %d\n", scaleResult);
  av_frame_free(&pDecodedFrame);
  return pConvertedFrame;
}

int onDecodedFrame(AVFrame *pDecodedFrame)
{
  logMessage("ogv-decoder-video-theora: onDecodedFrame is being called\n");
  if (!pDecodedFrame)
  {
    logMessage("ogv-decoder-video-theora: pDecodedFrame is NULL\n");
    return 0;
  }
  AVFrame *pConvertedFrame = getConvertedFrame(pDecodedFrame);
  logMessage("ogv-decoder-video-theora: calling ogvjs_callback_frame width=%d, height=%d, \
	 linesize0=%d, linesize1=%d, linesize2=%d\n",
              pConvertedFrame->width,
              pConvertedFrame->height,
              pConvertedFrame->linesize[0],
              pConvertedFrame->linesize[1],
              pConvertedFrame->linesize[2]);

  ogvjs_callback_frame(
      pConvertedFrame->data[0], pConvertedFrame->linesize[0],
      pConvertedFrame->data[1], pConvertedFrame->linesize[1],
      pConvertedFrame->data[2], pConvertedFrame->linesize[2],
      pConvertedFrame->width, pConvertedFrame->height,
      pConvertedFrame->width / 2, pConvertedFrame->height / 2,
      pConvertedFrame->width, pConvertedFrame->height,
      0, 0,
      pConvertedFrame->width, pConvertedFrame->height);

  av_frame_free(&pConvertedFrame);
  return 1;
}

int decodeVideoPacket(AVPacket *pPacket, AVCodecContext *pCodecContext)
{
  logMessage("ogv-decoder-video-theora: calling avcodec_send_packet\n");
  // Supply raw packet data as input to a decoder
  // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga58bc4bf1e0ac59e27362597e467efff3
  int response = avcodec_send_packet(pCodecContext, pPacket);
  logMessage("ogv-decoder-video-theora: avcodec_send_packet returned %d (%s)\n", response, av_err2str(response));

  if (response < 0)
  {
    logMessage("ogv-decoder-video-theora error: while sending a packet to the decoder: %s", av_err2str(response));
  }
  int framesDecoded = 0;
  while (response >= 0)
  {
    // Return decoded output data (into a frame) from a decoder
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga11e6542c4e66d3028668788a1a74217c
    AVFrame *pDecodedFrame = av_frame_alloc();
    if (!pDecodedFrame)
    {
      logMessage("ogv-decoder-video-theora error: could not allocate video frame\n");
      return framesDecoded;
    }

    logMessage("ogv-decoder-video-theora: calling avcodec_receive_frame\n");
    response = avcodec_receive_frame(pCodecContext, pDecodedFrame);
    logMessage("ogv-decoder-video-theora: avcodec_receive_frame returned %d (%s)\n", response, av_err2str(response));
    if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
    {
      logMessage("ogv-decoder-video-theora error: avcodec_receive_frame needs more data %s\n", av_err2str(response));
      return framesDecoded;
    }
    else if (response < 0)
    {
      logMessage("ogv-decoder-video-theora error: while receiving a frame from the decoder: %s\n", av_err2str(response));
      return framesDecoded;
    }

    if (response >= 0)
    {
      logMessage(
          "Frame %d (type=%c, size=%d bytes, format=%d) pts %lld key_frame %d [DTS %d]\n",
          pCodecContext->frame_number,
          av_get_picture_type_char(pDecodedFrame->pict_type),
          pDecodedFrame->pkt_size,
          pDecodedFrame->format,
          pDecodedFrame->pts,
          pDecodedFrame->key_frame,
          pDecodedFrame->coded_picture_number);
      if (!pDecodedFrame)
      {
        logMessage("ogv-decoder-video-theora error: something is wrong: %d", (int)pDecodedFrame);
      }
      framesDecoded += onDecodedFrame(pDecodedFrame);
    }
  }
  return framesDecoded;
}

int ogv_video_decoder_process_frame(const char *data, size_t data_len)
{
  logMessage("ogv-decoder-video-theora: ogv_video_decoder_process_frame is being called. data size=%d\n", data_len);
  if (!data_len)
  {
    return 1;
  }
  pPacket->data = (uint8_t *)data;
  pPacket->size = data_len;

  int result = decodeVideoPacket(pPacket, pVideoCodecContext);

  return result;
}

void ogv_video_decoder_destroy(void)
{
  avcodec_parameters_free(&pCodecParams);
  if (pPacket)
  {
    av_packet_free(&pPacket);
  }
}
