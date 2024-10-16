#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
// #include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixfmt.h>
}
#include "ogv-demuxer.h"
#include "ffmpeg-helper.h"
#include "io-helper.h"
#include <unordered_map>

int endReached = 0;

PacketBuffer videoPackets(PACKET_BUFFER_SIZE);

static bool hasVideo = false;
static int videoStreamIndex = -1;
static const char *videoCodecName = NULL;

static bool hasAudio = false;
static int audioStreamIndex = 0;
static const char *audioCodecName = NULL;

// Time to seek to in milliseconds
static int64_t seekTime;
static unsigned int seekTrack;
static int64_t startPosition;

static double lastKeyframeKimestamp = -1;
static unsigned int LOG_LEVEL_DEFAULT = 0;
static uint8_t *avio_ctx_buffer = NULL;
static AVIOContext *avio_ctx = NULL;
static AVFormatContext *pFormatContext = NULL;
static AVCodecContext *pVideoCodecContext = NULL;
// static AVPacket *pPacket = NULL;
// static struct SwsContext *pSwsContext = NULL;
// static AVFrame *pConvertedFrame = NULL;
static int64_t prev_data_available = 0;
static int retry_count = 0;
const int MAX_RETRY_COUNT = 3;
std::unordered_map<int, AVRational> streamTimeBase;
static int minBufSize = 1 * 1024 * 1024;
const int64_t fakeDtsInitialValue = -1000;
// TODO: reset fakeDtsValue to fakeDtsInitialValue during seeking
int64_t fakeDtsValue = fakeDtsInitialValue;
int64_t previouslyRequestedPts = -1;
int64_t ptsOfLastPacketSent = -1;
int32_t threadCount = -1;
int32_t debugDecoder = 0;
int32_t decodedFrameBufferSize = -1;

enum AppState
{
  STATE_BEGIN,
  STATE_DECODING,
  STATE_SEEKING
} appState;

void printCodecs()
{
  void *data = NULL;
  const AVCodec *item = NULL;
  while ((item = av_codec_iterate(&data)))
  {
    printf("codec: %s\n", item->name);
  }
}
void printDemuxers()
{
  void *data = NULL;
  const AVInputFormat *item = NULL;
  while ((item = av_demuxer_iterate(&data)))
  {
    printf("demuxer: %s\n", item->name);
  }
}
extern "C" void ogv_demuxer_init(const char *initOptions, int len)
{
  printf("FFmpeg demuxer: ogv_demuxer_init started.\n");
  // printCodecs();
  // printDemuxers();
  const char *pBuf = initOptions;
  threadCount = readInt32(&pBuf);
  int debugDemuxer = readInt32(&pBuf);
  loggingEnabled = debugDemuxer ? true : false;
  debugDecoder = readInt32(&pBuf);
  fileSize = readInt64(&pBuf);
  int packetBufferSize = readInt32(&pBuf);
  decodedFrameBufferSize = readInt32(&pBuf);
  printf("FFmpeg demuxer: ogv_demuxer_init with thread count: %d, \
          debugDemuxer: %d, debugDecoder: %d, file size: %lld bytes, \
          packet buffer size: %d, decoded frame buffer size: %d.\n",
         threadCount, debugDemuxer, debugDecoder, fileSize, packetBufferSize, decodedFrameBufferSize);
  appState = STATE_BEGIN;
  videoPackets.clear();
  videoPackets.setMaxSize(packetBufferSize);
  bufferQueue = bq_init();

  pFormatContext = avformat_alloc_context();
  if (!pFormatContext)
  {
    logMessage("FFmpeg demuxer error: could not allocate memory for Format Context\n");
    return;
  }
  avio_ctx_buffer = (uint8_t *)av_malloc(avio_ctx_buffer_size);
  if (!avio_ctx_buffer)
  {
    logMessage("FFmpeg demuxer error: could not allocate memory for AVIO Context buffer");
    return;
  }
  avio_ctx = avio_alloc_context(
      avio_ctx_buffer, avio_ctx_buffer_size,
      0, bufferQueue, &readCallback, NULL,
      &seekCallback);
  if (!avio_ctx)
  {
    logMessage("FFmpeg demuxer error: could not allocate memory for AVIO Context\n");
    return;
  }
  pFormatContext->pb = avio_ctx;
  pFormatContext->flags = AVFMT_FLAG_CUSTOM_IO;
  videoPackets.clear();
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
  logMessage("FFmpeg demuxer: processBegin is being called\n");
  const int openInputRes = avformat_open_input(&pFormatContext, NULL, NULL, NULL);
  if (openInputRes != 0)
  {
    logMessage("FFmpeg demuxer error: could not open input. Error code: %d (%s)\n", openInputRes, av_err2str(openInputRes));
    return -1;
  }
  if (avformat_find_stream_info(pFormatContext, NULL) < 0)
  {
    logMessage("FFmpeg demuxer error: could not get the stream info");
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
    logMessage("AVStream->time_base before open coded %d/%d\n", pStream->time_base.num, pStream->time_base.den);
    logMessage("AVStream->r_frame_rate before open coded %d/%d\n", pStream->r_frame_rate.num, pStream->r_frame_rate.den);
    logMessage("AVStream->start_time %" PRId64 "\n", pStream->start_time);
    logMessage("AVStream->duration %" PRId64 "\n", pStream->duration);
    logMessage("Searching for decoder\n");

    const AVCodec *pLocalCodec = NULL;

    // finds the registered decoder for a codec ID
    // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
    pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

    if (pLocalCodec == NULL)
    {
      logMessage("FFmpeg demuxer error: unsupported codec ID %d!\n", pLocalCodecParameters->codec_id);
      // In this example if the codec is not found we just skip it
      continue;
    }
    streamTimeBase[i] = pStream->time_base;
    // when the stream is a video we store its index, codec parameters and codec
    switch (pLocalCodecParameters->codec_type)
    {
    case AVMEDIA_TYPE_VIDEO:
    {
      logMessage("Stream %d is video stream. Resolution %d x %d. Codec name: %s\n", i, pLocalCodecParameters->width, pLocalCodecParameters->height, pLocalCodec->long_name);
      if (!hasVideo)
      {
        hasVideo = 1;
        videoStreamIndex = i;
        videoCodecName = pLocalCodec->long_name;
        pVideoCodec = pLocalCodec;
        pVideoCodecParameters = pLocalCodecParameters;
      }
      break;
    }
    case AVMEDIA_TYPE_AUDIO:
    {
      logMessage("Stream %d is audio stream. Codec name: %s, sample rate: %d\n", i, pLocalCodec->name, pLocalCodecParameters->sample_rate);
      if (!hasAudio)
      {
        hasAudio = 1;
        audioStreamIndex = i;
        audioCodecName = pLocalCodec->long_name;
        pAudioCodec = pLocalCodec;
        pAudioCodecParameters = pLocalCodecParameters;
      }
      break;
    }
    default:
    {
      logMessage("Stream %d has type %d\n", i, pLocalCodecParameters->codec_type);
      break;
    }
    }
    // print its name, id and bitrate
  }

  // if (video_stream_index == -1)
  // {
  // 	logMessage("File does not contain a video stream!\n");
  // 	return 0;
  // }

  if (hasVideo)
  {
    pVideoCodecContext = avcodec_alloc_context3(pVideoCodec);
    if (!pVideoCodecContext)
    {
      logMessage("failed to allocated memory for AVCodecContext\n");
      hasVideo = 0;
      return 0;
    }
    // Fill the codec context based on the values from the supplied codec parameters
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
    if (avcodec_parameters_to_context(pVideoCodecContext, pVideoCodecParameters) < 0)
    {
      logMessage("failed to copy codec params to codec context\n");
      hasVideo = 0;
      return 0;
    }

    // Initialize the AVCodecContext to use the given AVCodec.
    // https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
    if (avcodec_open2(pVideoCodecContext, pVideoCodec, NULL) < 0)
    {
      logMessage("failed to open codec through avcodec_open2\n");
      return -1;
    }

    // // https://ffmpeg.org/doxygen/trunk/structAVPacket.html
    // pPacket = av_packet_alloc();
    // if (!pPacket)
    // {
    //   logMessage("failed to allocate memory for AVPacket\n");
    //   hasVideo = 0;
    //   return 0;
    // }
    // if (pVideoCodecParameters->format != AV_PIX_FMT_YUV420P)
    // {
    //   logMessage(
    //       "Video pixel format is %d, need %d, initializing scaling context\n",
    //       pVideoCodecParameters->format,
    //       AV_PIX_FMT_YUV420P);
    //   // Need to convert each input video frame to yuv420p format using sws_scale.
    //   // Here we're initializing conversion context
    //   pSwsContext = sws_getContext(
    //       pVideoCodecParameters->width, pVideoCodecParameters->height,
    //       pVideoCodecParameters->format,
    //       pVideoCodecParameters->width, pVideoCodecParameters->height,
    //       AV_PIX_FMT_YUV420P,
    //       SWS_FAST_BILINEAR, NULL, NULL, NULL);
    //   pConvertedFrame = av_frame_alloc();
    //   pConvertedFrame->width = pVideoCodecParameters->width;
    //   pConvertedFrame->height = pVideoCodecParameters->height;
    //   pConvertedFrame->format = AV_PIX_FMT_YUV420P;
    //   av_frame_get_buffer(pConvertedFrame, 0);
    // }

    uint32_t bytesWritten = 0;
    uint32_t allocatedSize = 4 + 4 + 4 + 8 + 32 * 4 + pVideoCodecParameters->extradata_size;
    uint8_t *const pBufStart = (uint8_t *)malloc(allocatedSize);
    uint8_t *pBuf = pBufStart;
    writeInt32(&pBuf, threadCount, &bytesWritten);
    writeInt32(&pBuf, debugDecoder, &bytesWritten);
    writeInt32(&pBuf, decodedFrameBufferSize, &bytesWritten);
    writeInt32(&pBuf, streamTimeBase[videoStreamIndex].num, &bytesWritten);
    writeInt32(&pBuf, streamTimeBase[videoStreamIndex].den, &bytesWritten);

    writeInt32(&pBuf, pVideoCodecParameters->codec_type, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->codec_id, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->codec_tag, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->extradata_size, &bytesWritten);
    if (pVideoCodecParameters->extradata_size)
    {
      memcpy(pBuf, pVideoCodecParameters->extradata, pVideoCodecParameters->extradata_size);
      pBuf += pVideoCodecParameters->extradata_size;
      bytesWritten += pVideoCodecParameters->extradata_size;
    }
    writeInt32(&pBuf, pVideoCodecParameters->format, &bytesWritten);
    writeInt64(&pBuf, pVideoCodecParameters->bit_rate, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->bits_per_coded_sample, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->bits_per_raw_sample, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->profile, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->level, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->width, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->height, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->sample_aspect_ratio.num, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->sample_aspect_ratio.den, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->field_order, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->color_range, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->color_primaries, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->color_trc, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->color_space, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->chroma_location, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->video_delay, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->sample_rate, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->block_align, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->frame_size, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->initial_padding, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->trailing_padding, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->seek_preroll, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->ch_layout.order, &bytesWritten);
    writeInt32(&pBuf, pVideoCodecParameters->ch_layout.nb_channels, &bytesWritten);
    // TODO: find out how to copy this
    if (pVideoCodecParameters->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM)
    {
      logMessage("Unsupported ch_layout.order detected: %d\n", pVideoCodecParameters->ch_layout.order);
    }
    else
    {
      writeInt64(&pBuf, pVideoCodecParameters->ch_layout.u.mask, &bytesWritten);
    }
    if (allocatedSize != bytesWritten)
    {
      logMessage("Allocated and written data size differs! Allocated: %d, written: %d\n", allocatedSize, bytesWritten);
    }

    ogvjs_callback_init_video(
        pVideoCodecParameters->width, pVideoCodecParameters->height,
        pVideoCodecParameters->width >> 1, pVideoCodecParameters->height >> 1, // @todo assuming 4:2:0
        0,                                                                     // @todo get fps
        pVideoCodecParameters->width,
        pVideoCodecParameters->height,
        0, 0,
        pVideoCodecParameters->width, pVideoCodecParameters->height,
        (const char *)pBufStart,
        bytesWritten);
    free(pBufStart);
  }

  if (hasAudio)
  {
    // TODO: implement
    hasAudio = 0;
    // ogvjs_callback_audio_packet((char *)data, len, -1, 0.0);
  }

  logMessage("Loaded metadata. Video codec: %s, audio codec: %s\n", videoCodecName, audioCodecName);
  appState = STATE_DECODING;
  ogvjs_callback_loaded_metadata(
      // Always return "theora" to load modified theora video decoder that's currently in pass-through mode
      videoCodecName ? "theora" : NULL,
      // audioCodecName temporarily disabled
      NULL);

  return 1;
}
void putDemuxedPacketToBuffer(AVPacket const *pPacket, const float frameTimestamp, PacketBuffer &packetBuffer)
{
  while (packetBuffer.isFull())
  {
    packetBuffer.pop_front();
  }
  int64_t dts = pPacket->dts;
  if (dts < 0)
  {
    dts = ++fakeDtsValue;
  }
  packetBuffer.emplace_back(pPacket->pts, dts, pPacket->size, pPacket->data);
  logMessage("FFmpeg demuxer: Adding packet into buffer. Packet pts: %lld, packet size: %d\n", pPacket->pts, pPacket->size);
}
int callVideoCallbackIfBufferIsFull()
{
  if (!videoPackets.isFull() && !endReached)
  {
    logMessage("FFmpeg demuxer: not enough packets in buffer (%d / %d) and haven't reached end yet.\n",
               videoPackets.size(), videoPackets.getMaxSize());
    return 0;
  }
  uint32_t resultBufSize = 4 + 8;
  int64_t requestedPts = videoPackets.getMinPts();
  logMessage("FFmpeg demuxer: iterating over %d packets in buffer\n", videoPackets.size());
  std::deque<DemuxedPacket>::const_iterator itOfFirstPacketToSend = videoPackets.begin();
  if (ptsOfLastPacketSent != -1)
  {
    const auto ptsToSearch = ptsOfLastPacketSent;
    itOfFirstPacketToSend = std::find_if(
        videoPackets.begin(),
        videoPackets.end(),
        [ptsToSearch](const DemuxedPacket &packet)
        { return packet.m_pts == ptsOfLastPacketSent; });
    if (itOfFirstPacketToSend != videoPackets.end())
    {
      ++itOfFirstPacketToSend;
    }
    if (itOfFirstPacketToSend == videoPackets.end())
    {
      // Send everything from the start
      itOfFirstPacketToSend = videoPackets.begin();
    }
  }
  int numberOfPacketsToSend = 0;
  for (auto it = itOfFirstPacketToSend; it != videoPackets.end(); ++it)
  {
    ++numberOfPacketsToSend;
    const auto &packet = *it;
    logMessage("FFmpeg demuxer: pts %lld, dts %lld, size: %d\n", packet.m_pts, packet.m_dts, packet.m_dataSize);
    resultBufSize += 4 + 8 + 8 + 4 + packet.m_dataSize;
  }
  previouslyRequestedPts = requestedPts;
  logMessage("FFmpeg demuxer: writing %d packets into buffer, total size: %d. Requesting pts: %lld\n", numberOfPacketsToSend, resultBufSize, requestedPts);

  uint8_t *const pResultBuf = (uint8_t *)malloc(resultBufSize);
  if (!pResultBuf)
  {
    logMessage("FFmpeg demuxer: failed to allocate result buffer of size %d\n", resultBufSize);
    return 0;
  }
  logMessage("FFmpeg demuxer: allocated %d bytes for packet buffer\n", resultBufSize);
  uint8_t *pBuf = pResultBuf;
  uint32_t bytesWritten = 0;
  writeInt32(&pBuf, numberOfPacketsToSend, &bytesWritten);
  writeInt64(&pBuf, requestedPts, &bytesWritten);
  for (auto it = itOfFirstPacketToSend; it != videoPackets.end(); ++it)
  {
    const auto &packet = *it;
    logMessage("FFmpeg demuxer: writing data of packet with pts %lld into buffer\n", packet.m_pts);
    writeInt32(&pBuf, 0, &bytesWritten);
    writeInt64(&pBuf, packet.m_pts, &bytesWritten);
    writeInt64(&pBuf, packet.m_dts, &bytesWritten);
    writeInt32(&pBuf, packet.m_dataSize, &bytesWritten);
    memcpy(pBuf, packet.m_pData, packet.m_dataSize);
    pBuf += packet.m_dataSize;
    bytesWritten += packet.m_dataSize;
    ptsOfLastPacketSent = packet.m_pts;
  }
  logMessage("FFmpeg demuxer: wrote %d bytes into buffer with size %d\n", bytesWritten, resultBufSize);
  float frameTimestamp = requestedPts * av_q2d(streamTimeBase[videoStreamIndex]);
  logMessage("FFmpeg demuxer: calling ogvjs_callback_video_packet\n");

  ogvjs_callback_video_packet(
      (const char *)pResultBuf,
      bytesWritten,
      frameTimestamp,
      -1,
      0);
  logMessage("FFmpeg demuxer: freeing pResultBuf\n");
  free(pResultBuf);
  logMessage("FFmpeg demuxer: popping video packet\n");
  videoPackets.pop_front();
  logMessage("callVideoCallbackIfBufferIsFull ended\n");
  return 1;
}
static int processDecoding(void)
{
  logMessage("FFmpeg demuxer: processDecoding is being called\n");
  for (;;)
  {
    AVPacket *pPacket = NULL;
    int read_frame_res = av_read_frame(pFormatContext, pPacket);
    if (read_frame_res < 0 && read_frame_res != AVERROR_EOF)
    {
      // Probably need more data
      logMessage("FFmpeg demuxer: av_read_frame returned %d (%s)\n", read_frame_res, av_err2str(read_frame_res));
      return 0;
    }
    if (read_frame_res == AVERROR_EOF)
    {
      logMessage("FFmpeg demuxer: end reached\n");
      endReached = 1;
      if (!videoPackets.empty())
      {
        return callVideoCallbackIfBufferIsFull();
      }
    }
    float frameTimestamp = pPacket->pts * av_q2d(streamTimeBase[pPacket->stream_index]);
    logMessage("FFmpeg demuxer: got packet for stream %d, pts: %lld (%.3f s). Packet size: %d bytes\n",
               pPacket->stream_index, pPacket->pts, frameTimestamp, pPacket->size);

    int ret = 1;
    // logMessage("FFmpeg demuxer: processDecoding successfully read packet. av_read_frame returned %d (%s). Stream index: %d\n", read_frame_res, av_err2str(read_frame_res), pPacket->stream_index);
    // if it's the video stream
    if (hasVideo && pPacket->stream_index == videoStreamIndex)
    {
      logMessage("FFmpeg demuxer: got packet for video stream %d, pts: %lld (%.3f s). Packet size: %d bytes\n",
                 videoStreamIndex, pPacket->pts, frameTimestamp, pPacket->size);
      putDemuxedPacketToBuffer(pPacket, frameTimestamp, videoPackets);
      ret = callVideoCallbackIfBufferIsFull();
    }
    else if (hasAudio && pPacket->stream_index == audioStreamIndex)
    {
      logMessage("FFmpeg demuxer: got packet for audio stream %d\n", audioStreamIndex);
      if (pPacket->size)
      {
        ogvjs_callback_audio_packet((char *)pPacket->buf, pPacket->size, pPacket->pts, 0.0);
      }
      ret = 1;
    }
    av_packet_unref(pPacket);
    if (ret)
    {
      return ret;
    }
  }
}

static int processSeeking(void)
{
  logMessage("FFmpeg demuxer: processSeeking is being called\n");
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
  // 	// logMessage("is seeking processing... FAILED at %lld %lld %lld\n", bufferQueue->pos, bq_start(bufferQueue), bq_end(bufferQueue));
  // }
  // else
  // {
  // 	// We need to go off and load stuff...
  // 	// logMessage("is seeking processing... MOAR SEEK %lld %lld %lld\n", bufferQueue->lastSeekTarget, bq_start(bufferQueue), bq_end(bufferQueue));
  // 	int64_t target = bufferQueue->lastSeekTarget;
  // 	bq_flush(bufferQueue);
  // 	bufferQueue->pos = target;
  // 	ogvjs_callback_seek(target);
  // }
  // // Return false to indicate we need i/o
  // return 0;
}

extern "C" void ogv_demuxer_receive_input(const char *buffer, int bufsize)
{
  logMessage("FFmpeg demuxer: ogv_demuxer_receive_input is being called. bufsize: %d, waiting for input: %d\n", bufsize, waitingForInput);
  if (bufsize > 0)
  {
    waitingForInput = 0;
    bq_append(bufferQueue, buffer, bufsize);
  }
  logMessage("FFmpeg demuxer: ogv_demuxer_receive_input: exited.\n");
}

/**
 * Process previously queued data into packets.
 *
 * return value is 'true' if there
 * are more packets to be processed in the queued data,
 * or 'false' if there aren't.
 */
extern "C" int ogv_demuxer_process(void)
{
  // logMessage("FFmpeg demuxer: ogv_demuxer_process is being called\n");

  const int64_t data_available = bq_headroom(bufferQueue);
  const int64_t bytes_until_end = fileSize - bufferQueue->pos;
  // logMessage("FFmpeg demuxer: buffer got %lld bytes of data in it. Bytes until end: %lld, pos: %lld\n", data_available, bytes_until_end, bufferQueue->pos);
  printf("%lld\n", data_available);

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
    logMessage("FFmpeg demuxer: processDecoding: failed to buffer more data. %lld bytes of data\n", prev_data_available);
  }

  prev_data_available = data_available;
  retry_count = 0;
  if (callbackState != CALLBACK_STATE_NOT_IN_CALLBACK)
  {
    logMessage("FFmpeg demuxer: currently in callback: %d\n", callbackState);
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
      // logMessage("not ready to read the cues\n");
      return 0;
    }
  }
  default:
  {
    // uhhh...
    logMessage("Invalid appState %d in ogv_demuxer_process\n", appState);
    return 0;
  }
  }
}

extern "C" void ogv_demuxer_destroy(void)
{
  // should probably tear stuff down, eh
  // if (pConvertedFrame)
  //   av_frame_free(&pConvertedFrame);
  if (pFormatContext)
    avformat_close_input(&pFormatContext);
  // if (pPacket)
  //   av_packet_free(&pPacket);
  if (pVideoCodecContext)
    avcodec_free_context(&pVideoCodecContext);
  bq_free(bufferQueue);
  bufferQueue = NULL;
}

extern "C" void ogv_demuxer_flush(void)
{
  bq_flush(bufferQueue);
  // we may not need to handle the packet queue because this only
  // happens after seeking and nestegg handles that internally
  // lastKeyframeKimestamp = -1;
}

/**
 * @return segment length in bytes, or -1 if unknown
 */
extern "C" long ogv_demuxer_media_length(void)
{
  // @todo check if this is needed? maybe an ogg-specific thing
  return -1;
}

/**
 * @return segment duration in seconds, or -1 if unknown
 */
extern "C" float ogv_demuxer_media_duration(void)
{
  if (pFormatContext->duration <= 0)
    return -1;
  return pFormatContext->duration / 1000000.0;
}

extern "C" int ogv_demuxer_seekable(void)
{
  // Audio WebM files often have no cues; allow brute-force seeking
  // by linear demuxing through hopefully-cached data.
  return 1;
}

extern "C" long ogv_demuxer_keypoint_offset(long time_ms)
{
  // can't do with nestegg's API; use ogv_demuxer_seek_to_keypoint instead
  return -1;
}

extern "C" int ogv_demuxer_seek_to_keypoint(long time_ms)
{
  appState = STATE_SEEKING;
  seekTime = (int64_t)time_ms;
  if (hasVideo)
  {
    seekTrack = videoStreamIndex;
  }
  else if (hasAudio)
  {
    seekTrack = audioStreamIndex;
  }
  else
  {
    return 0;
  }
  processSeeking();
  return 1;
}
