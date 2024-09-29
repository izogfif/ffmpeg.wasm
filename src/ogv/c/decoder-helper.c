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

int32_t read_int32(const char **pBuf)
{
  const int data_size = 4;
  int32_t result = -1;
  memcpy(&result, *pBuf, data_size);
  *pBuf += data_size;
  // logCallback("read_int32: got %d\n", result);
  return result;
}

int64_t read_int64(const char **pBuf)
{
  const int data_size = 8;
  int64_t result = -1;
  memcpy(&result, *pBuf, data_size);
  *pBuf += data_size;
  // logCallback("read_int64: got %lld\n", result);
  return result;
}

AVCodecParameters *readCodecParams(const char *paramsData, int paramsDataLength)
{
  AVCodecParameters *pCodecParams = avcodec_parameters_alloc();
  const char *pBuf = paramsData;
  pCodecParams->codec_type = read_int32(&pBuf);
  pCodecParams->codec_id = read_int32(&pBuf);
  pCodecParams->codec_tag = read_int32(&pBuf);
  pCodecParams->extradata_size = read_int32(&pBuf);
  if (pCodecParams->extradata_size)
  {
    pCodecParams->extradata = av_malloc(pCodecParams->extradata_size);
    memcpy(pCodecParams->extradata, pBuf, pCodecParams->extradata_size);
    pBuf += pCodecParams->extradata_size;
  }
  pCodecParams->format = read_int32(&pBuf);
  pCodecParams->bit_rate = read_int64(&pBuf);
  pCodecParams->bits_per_coded_sample = read_int32(&pBuf);
  pCodecParams->bits_per_raw_sample = read_int32(&pBuf);
  pCodecParams->profile = read_int32(&pBuf);
  pCodecParams->level = read_int32(&pBuf);
  pCodecParams->width = read_int32(&pBuf);
  pCodecParams->height = read_int32(&pBuf);
  pCodecParams->sample_aspect_ratio.num = read_int32(&pBuf);
  pCodecParams->sample_aspect_ratio.den = read_int32(&pBuf);
  pCodecParams->field_order = read_int32(&pBuf);
  pCodecParams->color_range = read_int32(&pBuf);
  pCodecParams->color_primaries = read_int32(&pBuf);
  pCodecParams->color_trc = read_int32(&pBuf);
  pCodecParams->color_space = read_int32(&pBuf);
  pCodecParams->chroma_location = read_int32(&pBuf);
  pCodecParams->video_delay = read_int32(&pBuf);
  pCodecParams->sample_rate = read_int32(&pBuf);
  pCodecParams->block_align = read_int32(&pBuf);
  pCodecParams->frame_size = read_int32(&pBuf);
  pCodecParams->initial_padding = read_int32(&pBuf);
  pCodecParams->trailing_padding = read_int32(&pBuf);
  pCodecParams->seek_preroll = read_int32(&pBuf);
  pCodecParams->ch_layout.order = read_int32(&pBuf);
  pCodecParams->ch_layout.nb_channels = read_int32(&pBuf);
  if (pCodecParams->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM)
  {
    logCallback("Unsupported ch_layout.order detected: %d\n", pCodecParams->ch_layout.order);
  }
  else
  {
    pCodecParams->ch_layout.u.mask = read_int64(&pBuf);
  }
  return pCodecParams;
}
