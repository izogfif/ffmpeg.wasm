#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixfmt.h>
#include "ogv-demuxer.h"
#include "ogv-buffer-queue.h"
#include "ffmpeg-helper.h"
#include <emscripten.h>

static BufferQueue *bufferQueue;

static bool hasVideo = false;
static unsigned int videoTrack = -1;
static const char *videoCodecName = NULL;

static bool hasAudio = false;
static unsigned int audioTrack = 0;
static const char *audioCodecName = NULL;

// Time to seek to in milliseconds
static int64_t seekTime;
static unsigned int seekTrack;
static int64_t startPosition;

static double lastKeyframeKimestamp = -1;
static unsigned int LOG_LEVEL_DEFAULT = 0;
static const unsigned int avio_ctx_buffer_size = 4096;
static uint8_t *avio_ctx_buffer = NULL;
static AVIOContext *avio_ctx = NULL;
static AVFormatContext *pFormatContext = NULL;
static AVCodecContext *pVideoCodecContext = NULL;
static AVPacket *pPacket = NULL;
static struct SwsContext *pSwsContext = NULL;
static AVFrame *pConvertedFrame = NULL;
static int64_t prev_data_available = 0;
static int retry_count = 0;
const int MAX_RETRY_COUNT = 3;
static AVRational videoStreamTimeBase = {1, 1};
static AVRational audioStreamTimeBase = {1, 1};
static char *fileName = NULL;
static uint64_t fileSize = 0;
static int minBufSize = 1 * 1024 * 1024;
static int waitingForInput = 0;

enum CallbackState
{
  CALLBACK_STATE_NOT_IN_CALLBACK,
  CALLBACK_STATE_IN_SEEK_CALLBACK,
  CALLBACK_STATE_IN_READ_CALLBACK,
};

static enum CallbackState callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;

enum AppState
{
  STATE_BEGIN,
  STATE_DECODING,
  STATE_SEEKING
} appState;

struct buffer_data
{
  uint8_t *ptr;
  size_t size; ///< size left in the buffer
};

void copyInt32(uint8_t **pBuf, int32_t value_to_copy, uint32_t *pSizeCounter)
{
  const int data_size = 4;
  memcpy(*pBuf, &value_to_copy, data_size);
  *pBuf += data_size;
  *pSizeCounter += data_size;
  // logCallback("copy_int32: wrote %d\n", value_to_copy);
}

void copyInt64(uint8_t **pBuf, int64_t value_to_copy, uint32_t *pSizeCounter)
{
  const int data_size = 8;
  memcpy(*pBuf, &value_to_copy, data_size);
  *pBuf += data_size;
  *pSizeCounter += data_size;
  // logCallback("copy_int64: wrote %lld\n", value_to_copy);
}

void requestSeek(int64_t pos)
{
  bq_flush(bufferQueue);
  bufferQueue->pos = pos;
  uint32_t lower = pos & 0xffffffff;
  uint32_t higher = pos >> 32;
  ogvjs_callback_seek(lower, higher);
}

static int readCallback(void *userdata, uint8_t *buffer, int length)
{
  callbackState = CALLBACK_STATE_IN_READ_CALLBACK;
  int64_t can_read_bytes = 0;
  int64_t pos = -1;
  int64_t data_available = -1;
  while (1)
  {
    data_available = bq_headroom((BufferQueue *)userdata);
    // logCallback("readCallback: bytes requested: %d, available: %d\n", length, (int)data_available);
    can_read_bytes = FFMIN(data_available, length);
    pos = ((BufferQueue *)userdata)->pos;
    if (can_read_bytes || !(fileSize - pos))
    {
      break;
    }
    // logCallback(
    //     "readCallback: bytes requested: %d, available: %d, bytes until end of file: %lld, cur pos: %lld, file size: %lld. Waiting for buffer refill: %d.\n",
    //     length, (int)data_available, fileSize - pos, pos, fileSize, waitingForInput);
    if (!waitingForInput)
    {
      waitingForInput = 1;
      logCallback("Requesting seek to %lld\n", pos);

      requestSeek(pos);
    }
    emscripten_sleep(100);
  }
  if (!can_read_bytes)
  {
    logCallback("readCallback: end of file reached. Bytes requested: %d, available: %d, bytes until end of file: %lld. Reporting EOF.\n", length, (int)data_available, fileSize - pos);
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return AVERROR_EOF;
  }
  if (bq_read((BufferQueue *)userdata, buffer, can_read_bytes))
  {
    logCallback("readCallback: bq_red failed. Bytes requested: %d, available: %d, bytes until end of file: %lld. Waiting for buffer refill.\n", length, (int)data_available, fileSize - pos);
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return AVERROR_EOF;
  }
  else
  {
    // success
    // logCallback("readCallback: %d bytes read\n", (int)can_read_bytes);
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return can_read_bytes;
  }
}

static int64_t seekCallback(void *userdata, int64_t offset, int whence)
{
  callbackState = CALLBACK_STATE_IN_SEEK_CALLBACK;
  logCallback("seekCallback is being called: offset=%lld, whence=%d\n", offset, whence);
  int64_t pos;
  switch (whence)
  {
  case SEEK_SET:
  {
    pos = offset;
    break;
  }
  case SEEK_CUR:
  {
    pos = ((BufferQueue *)userdata)->pos + offset;
    break;
  }
  case AVSEEK_SIZE:
  {
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return fileSize;
  }
  case SEEK_END: // not implemented
  case AVSEEK_FORCE:
  default:
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return -1;
  }
  pos = FFMIN(pos, fileSize);
  while (1)
  {
    const int seekRet = bq_seek((BufferQueue *)userdata, pos);
    int64_t data_available = seekRet ? 0 : bq_headroom((BufferQueue *)userdata);
    int64_t bytes_until_end = fileSize - pos;
    if (seekRet || data_available < FFMIN(bytes_until_end, avio_ctx_buffer_size))
    {
      logCallback("FFmpeg demuxer error: buffer seek failure. Error code: %d. Bytes until end: %lld, data available: %lld\n", seekRet, bytes_until_end, data_available);
      requestSeek(pos);
      emscripten_sleep(100);
    }
    else
    {
      logCallback("FFmpeg demuxer: succesfully seeked to %lld.\n", pos);
      callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
      return pos;
    }
  }
}

void ogv_demuxer_init(const char *fileSizeAndPath, int len)
{
  const int fileSizeSize = 8;
  memcpy(&fileSize, fileSizeAndPath, fileSizeSize);
  logCallback("ogv_demuxer_init with file size: %lld (fileSizeAndPath has length %d)\n", fileSize, len);
  appState = STATE_BEGIN;
  if (fileName)
  {
    free(fileName);
    fileName = NULL;
  }
  int fileNameLength = len - fileSizeSize;
  const char *inputFilePath = fileSizeAndPath + fileSizeSize;
  if (fileNameLength)
  {
    fileName = malloc(fileNameLength + 1);
    memset(fileName, 0, fileNameLength + 1);
    memcpy(fileName, inputFilePath, fileNameLength);
    logCallback("FFmpeg demuxer: ogv_demuxer_init with fileName %s\n", fileName);
  }

  bufferQueue = bq_init();

  pFormatContext = avformat_alloc_context();
  if (!pFormatContext)
  {
    logCallback("FFmpeg demuxer error: could not allocate memory for Format Context\n");
    return;
  }
  avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
  if (!avio_ctx_buffer)
  {
    logCallback("FFmpeg demuxer error: could not allocate memory for AVIO Context buffer");
    return;
  }
  avio_ctx = avio_alloc_context(
      avio_ctx_buffer, avio_ctx_buffer_size,
      0, bufferQueue, &readCallback, NULL,
      &seekCallback);
  if (!avio_ctx)
  {
    logCallback("FFmpeg demuxer error: could not allocate memory for AVIO Context\n");
    return;
  }
  if (!fileName)
  {
    pFormatContext->pb = avio_ctx;
    pFormatContext->flags = AVFMT_FLAG_CUSTOM_IO;
  }
}

// static int64_t tellCallback(void *userdata)
// {
// 	return bq_tell((BufferQueue *)userdata);
// }

static int readyForNextPacket(void)
{
  return 1; // Always ready
}

static int processBegin(void)
{
  logCallback("FFmpeg demuxer: processBegin is being called\n");
  const int openInputRes = avformat_open_input(&pFormatContext, fileName, NULL, NULL);
  if (openInputRes != 0)
  {
    logCallback("FFmpeg demuxer error: could not open input. Error code: %d (%s)\n", openInputRes, av_err2str(openInputRes));
    return -1;
  }
  if (avformat_find_stream_info(pFormatContext, NULL) < 0)
  {
    logCallback("FFmpeg demuxer error: could not get the stream info");
    return -1;
  }
  AVCodecParameters *pVideoCodecParameters = NULL;
  const AVCodec *pVideoCodec = NULL;
  AVCodecParameters *pAudioCodecParameters = NULL;
  const AVCodec *pAudioCodec = NULL;

  // loop though all the streams and print its main information
  for (int i = 0; i < pFormatContext->nb_streams; i++)
  {
    AVStream *pStream = pFormatContext->streams[i];
    AVCodecParameters *pLocalCodecParameters = NULL;
    pLocalCodecParameters = pStream->codecpar;
    logCallback("AVStream->time_base before open coded %d/%d\n", pStream->time_base.num, pStream->time_base.den);
    logCallback("AVStream->r_frame_rate before open coded %d/%d\n", pStream->r_frame_rate.num, pStream->r_frame_rate.den);
    logCallback("AVStream->start_time %" PRId64 "\n", pStream->start_time);
    logCallback("AVStream->duration %" PRId64 "\n", pStream->duration);
    logCallback("Searching for decoder\n");

    const AVCodec *pLocalCodec = NULL;

    // finds the registered decoder for a codec ID
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
    pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

    if (pLocalCodec == NULL)
    {
      logCallback("FFmpeg demuxer error: unsupported codec ID %d!\n", pLocalCodecParameters->codec_id);
      // In this example if the codec is not found we just skip it
      continue;
    }

    // when the stream is a video we store its index, codec parameters and codec
    switch (pLocalCodecParameters->codec_type)
    {
    case AVMEDIA_TYPE_VIDEO:
    {
      logCallback("Stream %d is video stream. Resolution %d x %d. Codec name: %s\n", i, pLocalCodecParameters->width, pLocalCodecParameters->height, pLocalCodec->long_name);
      if (!hasVideo)
      {
        hasVideo = 1;
        videoTrack = i;
        videoCodecName = pLocalCodec->long_name;
        pVideoCodec = pLocalCodec;
        pVideoCodecParameters = pLocalCodecParameters;
        videoStreamTimeBase = pStream->time_base;
      }
      break;
    }
    case AVMEDIA_TYPE_AUDIO:
    {
      logCallback("Stream %d is audio stream. Codec name: %s, sample rate: %d\n", i, pLocalCodec->name, pLocalCodecParameters->sample_rate);
      if (!hasAudio)
      {
        hasAudio = 1;
        audioTrack = i;
        audioCodecName = pLocalCodec->long_name;
        pAudioCodec = pLocalCodec;
        pAudioCodecParameters = pLocalCodecParameters;
        audioStreamTimeBase = pStream->time_base;
      }
      break;
    }
    default:
    {
      logCallback("Stream %d has type %d\n", i, pLocalCodecParameters->codec_type);
      break;
    }
    }
    // print its name, id and bitrate
  }

  // if (video_stream_index == -1)
  // {
  // 	logCallback("File does not contain a video stream!\n");
  // 	return 0;
  // }

  if (hasVideo)
  {
    pVideoCodecContext = avcodec_alloc_context3(pVideoCodec);
    if (!pVideoCodecContext)
    {
      logCallback("failed to allocated memory for AVCodecContext\n");
      hasVideo = 0;
      return 0;
    }
    // Fill the codec context based on the values from the supplied codec parameters
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
    if (avcodec_parameters_to_context(pVideoCodecContext, pVideoCodecParameters) < 0)
    {
      logCallback("failed to copy codec params to codec context\n");
      hasVideo = 0;
      return 0;
    }

    // Initialize the AVCodecContext to use the given AVCodec.
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
    if (avcodec_open2(pVideoCodecContext, pVideoCodec, NULL) < 0)
    {
      logCallback("failed to open codec through avcodec_open2\n");
      return -1;
    }

    // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
    pPacket = av_packet_alloc();
    if (!pPacket)
    {
      logCallback("failed to allocate memory for AVPacket\n");
      hasVideo = 0;
      return 0;
    }
    if (pVideoCodecParameters->format != AV_PIX_FMT_YUV420P)
    {
      logCallback(
          "Video pixel format is %d, need %d, initializing scaling context\n",
          pVideoCodecParameters->format,
          AV_PIX_FMT_YUV420P);
      // Need to convert each input video frame to yuv420p format using sws_scale.
      // Here we're initializing conversion context
      pSwsContext = sws_getContext(
          pVideoCodecParameters->width, pVideoCodecParameters->height,
          pVideoCodecParameters->format,
          pVideoCodecParameters->width, pVideoCodecParameters->height,
          AV_PIX_FMT_YUV420P,
          SWS_FAST_BILINEAR, NULL, NULL, NULL);
      pConvertedFrame = av_frame_alloc();
      pConvertedFrame->width = pVideoCodecParameters->width;
      pConvertedFrame->height = pVideoCodecParameters->height;
      pConvertedFrame->format = AV_PIX_FMT_YUV420P;
      av_frame_get_buffer(pConvertedFrame, 0);
    }
    // TODO: serialize pVideoCodecParameters

    uint32_t codecParamsSize = 0;
    uint32_t allocatedSize = 32 * 4 + pVideoCodecParameters->extradata_size;
    int8_t *const pVideoCodecParamsCopy = (int8_t *)malloc(allocatedSize);
    int8_t *pVCPBuf = pVideoCodecParamsCopy;
    copyInt32(&pVCPBuf, pVideoCodecParameters->codec_type, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->codec_id, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->codec_tag, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->extradata_size, &codecParamsSize);
    if (pVideoCodecParameters->extradata_size)
    {
      memcpy(pVCPBuf, pVideoCodecParameters->extradata, pVideoCodecParameters->extradata_size);
      pVCPBuf += pVideoCodecParameters->extradata_size;
      codecParamsSize += pVideoCodecParameters->extradata_size;
    }
    copyInt32(&pVCPBuf, pVideoCodecParameters->format, &codecParamsSize);
    copyInt64(&pVCPBuf, pVideoCodecParameters->bit_rate, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->bits_per_coded_sample, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->bits_per_raw_sample, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->profile, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->level, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->width, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->height, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->sample_aspect_ratio.num, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->sample_aspect_ratio.den, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->field_order, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->color_range, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->color_primaries, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->color_trc, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->color_space, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->chroma_location, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->video_delay, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->sample_rate, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->block_align, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->frame_size, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->initial_padding, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->trailing_padding, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->seek_preroll, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->ch_layout.order, &codecParamsSize);
    copyInt32(&pVCPBuf, pVideoCodecParameters->ch_layout.nb_channels, &codecParamsSize);
    // TODO: find out how to copy this
    if (pVideoCodecParameters->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM)
    {
      logCallback("Unsupported ch_layout.order detected: %d\n", pVideoCodecParameters->ch_layout.order);
    }
    else
    {
      copyInt64(&pVCPBuf, pVideoCodecParameters->ch_layout.u.mask, &codecParamsSize);
    }
    if (allocatedSize != codecParamsSize)
    {
      logCallback("Allocated and written data size differs! Allocated: %d, written: %d\n", allocatedSize, codecParamsSize);
    }

    ogvjs_callback_init_video(
        pVideoCodecParameters->width, pVideoCodecParameters->height,
        pVideoCodecParameters->width >> 1, pVideoCodecParameters->height >> 1, // @todo assuming 4:2:0
        0,                                                                     // @todo get fps
        pVideoCodecParameters->width,
        pVideoCodecParameters->height,
        0, 0,
        pVideoCodecParameters->width, pVideoCodecParameters->height,
        pVideoCodecParamsCopy,
        codecParamsSize);
    free(pVideoCodecParamsCopy);
  }

  if (hasAudio)
  {
    // TODO: implement
    hasAudio = 0;
    // ogvjs_callback_audio_packet((char *)data, len, -1, 0.0);
  }

  logCallback("Loaded metadata. Video codec: %s, audio codec: %s\n", videoCodecName, audioCodecName);
  appState = STATE_DECODING;
  ogvjs_callback_loaded_metadata(
      // Always return "theora" to load modified theora video decoder that's currently in pass-through mode
      videoCodecName ? "theora" : NULL,
      // audioCodecName temporarily disabled
      NULL);

  return 1;
}

static int processDecoding(void)
{
  // logCallback("FFmpeg demuxer: processDecoding is being called\n");

  int read_frame_res = av_read_frame(pFormatContext, pPacket);
  if (read_frame_res < 0)
  {
    // Probably need more data
    logCallback("FFmpeg demuxer: av_read_frame returned %d (%s)\n", read_frame_res, av_err2str(read_frame_res));
    return 0;
  }

  // logCallback("FFmpeg demuxer: processDecoding successfully read packet. av_read_frame returned %d (%s). Stream index: %d\n", read_frame_res, av_err2str(read_frame_res), pPacket->stream_index);
  // if it's the video stream
  if (hasVideo && pPacket->stream_index == videoTrack)
  {
    float frameTimestamp = pPacket->pts * av_q2d(videoStreamTimeBase);
    logCallback("FFmpeg demuxer: got packet for video stream %d, pts: %lld (%.3f s). Packet size: %d bytes\n",
                videoTrack, pPacket->pts, frameTimestamp, pPacket->size);
    const int returnSize = pPacket->size + 8 + 4 + 4;
    const char *pResultBuf = malloc(returnSize);
    const char *pBuf = pResultBuf;
    const int32_t packetCount = 1;
    int32_t bytesWritten = 0;
    copyInt32(&pBuf, packetCount, &bytesWritten);
    for (int i = 0; i < packetCount; ++i)
    {
      copyInt64(&pBuf, pPacket->pts, &bytesWritten);
      copyInt32(&pBuf, pPacket->size, &bytesWritten);
      memcpy(pBuf, pPacket->data, pPacket->size);
      bytesWritten += pPacket->size;
      logCallback("Demuxed video packet %d pts: %lld, packet size: %d\n", i, pPacket->pts, pPacket->size);
    }
    ogvjs_callback_video_packet(
        pResultBuf,
        bytesWritten,
        frameTimestamp,
        -1,
        0);
    free(pResultBuf);
  }
  else if (hasAudio && pPacket->stream_index == audioTrack)
  {
    logCallback("FFmpeg demuxer: got packet for audio stream %d\n", audioTrack);
    if (pPacket->size)
    {
      ogvjs_callback_audio_packet((char *)pPacket->buf, pPacket->size, pPacket->pts, 0.0);
    }
  }

  av_packet_unref(pPacket);
  return 1;
}

static int processSeeking(void)
{
  logCallback("FFmpeg demuxer: processSeeking is being called\n");
  bufferQueue->lastSeekTarget = -1;
  int seek_result = avformat_seek_file(pFormatContext, seekTrack, seekTime - 10000, seekTime, seekTime, 0);
  if (seek_result < 0)
  {
    // Something is wrong
    // Return false to indicate we need i/o
    return 0;
  }
  appState = STATE_DECODING;
  // Roll over to packet processing.
  // Return true to indicate we should keep reading.
  return 1;

  // if (bufferQueue->lastSeekTarget == -1)
  // {
  // 	// Maybe we just need more data?
  // 	// logCallback("is seeking processing... FAILED at %lld %lld %lld\n", bufferQueue->pos, bq_start(bufferQueue), bq_end(bufferQueue));
  // }
  // else
  // {
  // 	// We need to go off and load stuff...
  // 	// logCallback("is seeking processing... MOAR SEEK %lld %lld %lld\n", bufferQueue->lastSeekTarget, bq_start(bufferQueue), bq_end(bufferQueue));
  // 	int64_t target = bufferQueue->lastSeekTarget;
  // 	bq_flush(bufferQueue);
  // 	bufferQueue->pos = target;
  // 	ogvjs_callback_seek(target);
  // }
  // // Return false to indicate we need i/o
  // return 0;
}

void ogv_demuxer_receive_input(const char *buffer, int bufsize)
{
  logCallback("FFmpeg demuxer: ogv_demuxer_receive_input is being called. bufsize: %d\n", bufsize);
  if (bufsize > 0)
  {
    waitingForInput = 0;
    bq_append(bufferQueue, buffer, bufsize);
  }
  logCallback("FFmpeg demuxer: ogv_demuxer_receive_input: exited.\n");
}

int ogv_demuxer_process(void)
{
  // logCallback("FFmpeg demuxer: ogv_demuxer_process is being called\n");

  const int64_t data_available = bq_headroom(bufferQueue);
  const int64_t bytes_until_end = fileSize - bufferQueue->pos;
  // logCallback("FFmpeg demuxer: buffer got %lld bytes of data in it. Bytes until end: %lld, pos: %lld\n", data_available, bytes_until_end, bufferQueue->pos);

  if (data_available < minBufSize && bytes_until_end > data_available)
  {
    // Buffer at least 1 megabyte of data first
    if (data_available != prev_data_available)
    {
      prev_data_available = data_available;
      retry_count = 0;
      return 0;
    }
    if (retry_count <= MAX_RETRY_COUNT)
    {
      ++retry_count;
      return 0;
    }
    // No more data available: work with what we have
    logCallback("FFmpeg demuxer: processDecoding: failed to buffer more data. %lld bytes of data\n", prev_data_available);
  }

  prev_data_available = data_available;
  retry_count = 0;
  if (callbackState != CALLBACK_STATE_NOT_IN_CALLBACK)
  {
    logCallback("FFmpeg demuxer: currently in callback: %d\n", callbackState);
    return 0;
  }
  switch (appState)
  {
  case STATE_BEGIN:
  {
    return processBegin();
  }
  case STATE_DECODING:
  {
    return processDecoding();
  }
  case STATE_SEEKING:
  {
    if (readyForNextPacket())
    {
      return processSeeking();
    }
    else
    {
      // need more data
      // logCallback("not ready to read the cues\n");
      return 0;
    }
  }
  default:
  {
    // uhhh...
    logCallback("Invalid appState %d in ogv_demuxer_process\n", appState);
    return 0;
  }
  }
}

void ogv_demuxer_destroy(void)
{
  // should probably tear stuff down, eh
  if (pConvertedFrame)
    av_frame_free(&pConvertedFrame);
  if (pFormatContext)
    avformat_close_input(&pFormatContext);
  if (pPacket)
    av_packet_free(&pPacket);
  if (pVideoCodecContext)
    avcodec_free_context(&pVideoCodecContext);
  bq_free(bufferQueue);
  bufferQueue = NULL;
}

void ogv_demuxer_flush(void)
{
  bq_flush(bufferQueue);
  // we may not need to handle the packet queue because this only
  // happens after seeking and nestegg handles that internally
  // lastKeyframeKimestamp = -1;
}

/**
 * @return segment length in bytes, or -1 if unknown
 */
long ogv_demuxer_media_length(void)
{
  // @todo check if this is needed? maybe an ogg-specific thing
  return -1;
}

/**
 * @return segment duration in seconds, or -1 if unknown
 */
float ogv_demuxer_media_duration(void)
{
  if (pFormatContext->duration <= 0)
    return -1;
  return pFormatContext->duration / 1000000.0;
}

int ogv_demuxer_seekable(void)
{
  // Audio WebM files often have no cues; allow brute-force seeking
  // by linear demuxing through hopefully-cached data.
  return 1;
}

long ogv_demuxer_keypoint_offset(long time_ms)
{
  // can't do with nestegg's API; use ogv_demuxer_seek_to_keypoint instead
  return -1;
}

int ogv_demuxer_seek_to_keypoint(long time_ms)
{
  appState = STATE_SEEKING;
  seekTime = (int64_t)time_ms;
  if (hasVideo)
  {
    seekTrack = videoTrack;
  }
  else if (hasAudio)
  {
    seekTrack = audioTrack;
  }
  else
  {
    return 0;
  }
  processSeeking();
  return 1;
}
