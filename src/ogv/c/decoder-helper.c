#include "decoder-helper.h"

#define DEBUG_ENABLED 1

void logCallback(char const *format, ...)
{
  if (DEBUG_ENABLED)
  {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
  }
}

int32_t readInt32(const char **pBuf)
{
  const int data_size = 4;
  int32_t result = -1;
  memcpy(&result, *pBuf, data_size);
  *pBuf += data_size;
  // logCallback("readInt32: got %d\n", result);
  return result;
}

int64_t readInt64(const char **pBuf)
{
  const int data_size = 8;
  int64_t result = -1;
  memcpy(&result, *pBuf, data_size);
  *pBuf += data_size;
  // logCallback("readInt64: got %lld\n", result);
  return result;
}

AVCodecParameters *readCodecParams(const char *paramsData, int paramsDataLength)
{
  AVCodecParameters *pCodecParams = avcodec_parameters_alloc();
  const char *pBuf = paramsData;
  pCodecParams->codec_type = readInt32(&pBuf);
  pCodecParams->codec_id = readInt32(&pBuf);
  pCodecParams->codec_tag = readInt32(&pBuf);
  pCodecParams->extradata_size = readInt32(&pBuf);
  if (pCodecParams->extradata_size)
  {
    pCodecParams->extradata = av_malloc(pCodecParams->extradata_size);
    memcpy(pCodecParams->extradata, pBuf, pCodecParams->extradata_size);
    pBuf += pCodecParams->extradata_size;
  }
  pCodecParams->format = readInt32(&pBuf);
  pCodecParams->bit_rate = readInt64(&pBuf);
  pCodecParams->bits_per_coded_sample = readInt32(&pBuf);
  pCodecParams->bits_per_raw_sample = readInt32(&pBuf);
  pCodecParams->profile = readInt32(&pBuf);
  pCodecParams->level = readInt32(&pBuf);
  pCodecParams->width = readInt32(&pBuf);
  pCodecParams->height = readInt32(&pBuf);
  pCodecParams->sample_aspect_ratio.num = readInt32(&pBuf);
  pCodecParams->sample_aspect_ratio.den = readInt32(&pBuf);
  pCodecParams->field_order = readInt32(&pBuf);
  pCodecParams->color_range = readInt32(&pBuf);
  pCodecParams->color_primaries = readInt32(&pBuf);
  pCodecParams->color_trc = readInt32(&pBuf);
  pCodecParams->color_space = readInt32(&pBuf);
  pCodecParams->chroma_location = readInt32(&pBuf);
  pCodecParams->video_delay = readInt32(&pBuf);
  pCodecParams->sample_rate = readInt32(&pBuf);
  pCodecParams->block_align = readInt32(&pBuf);
  pCodecParams->frame_size = readInt32(&pBuf);
  pCodecParams->initial_padding = readInt32(&pBuf);
  pCodecParams->trailing_padding = readInt32(&pBuf);
  pCodecParams->seek_preroll = readInt32(&pBuf);
  pCodecParams->ch_layout.order = readInt32(&pBuf);
  pCodecParams->ch_layout.nb_channels = readInt32(&pBuf);
  if (pCodecParams->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM)
  {
    logCallback("Unsupported ch_layout.order detected: %d\n", pCodecParams->ch_layout.order);
  }
  else
  {
    pCodecParams->ch_layout.u.mask = readInt64(&pBuf);
  }
  return pCodecParams;
}