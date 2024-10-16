#include "decoder-helper.h"

AVCodecParameters *readCodecParams(const char *paramsData, AVRational *pTimeBase)
{
  AVCodecParameters *pCodecParams = avcodec_parameters_alloc();
  const char *pBuf = paramsData;
  pTimeBase->num = readInt32(&pBuf);
  pTimeBase->den = readInt32(&pBuf);
  pCodecParams->codec_type = (AVMediaType)readInt32(&pBuf);
  pCodecParams->codec_id = (AVCodecID)readInt32(&pBuf);
  pCodecParams->codec_tag = readInt32(&pBuf);
  pCodecParams->extradata_size = readInt32(&pBuf);
  if (pCodecParams->extradata_size)
  {
    pCodecParams->extradata = (uint8_t *)av_malloc(pCodecParams->extradata_size);
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
  pCodecParams->field_order = (AVFieldOrder)readInt32(&pBuf);
  pCodecParams->color_range = (AVColorRange)readInt32(&pBuf);
  pCodecParams->color_primaries = (AVColorPrimaries)readInt32(&pBuf);
  pCodecParams->color_trc = (AVColorTransferCharacteristic)readInt32(&pBuf);
  pCodecParams->color_space = (AVColorSpace)readInt32(&pBuf);
  pCodecParams->chroma_location = (AVChromaLocation)readInt32(&pBuf);
  pCodecParams->video_delay = readInt32(&pBuf);
  pCodecParams->sample_rate = readInt32(&pBuf);
  pCodecParams->block_align = readInt32(&pBuf);
  pCodecParams->frame_size = readInt32(&pBuf);
  pCodecParams->initial_padding = readInt32(&pBuf);
  pCodecParams->trailing_padding = readInt32(&pBuf);
  pCodecParams->seek_preroll = readInt32(&pBuf);
  pCodecParams->ch_layout.order = (AVChannelOrder)readInt32(&pBuf);
  pCodecParams->ch_layout.nb_channels = readInt32(&pBuf);
  if (pCodecParams->ch_layout.order == AV_CHANNEL_ORDER_CUSTOM)
  {
    logMessage("Unsupported ch_layout.order detected: %d\n", pCodecParams->ch_layout.order);
  }
  else
  {
    pCodecParams->ch_layout.u.mask = readInt64(&pBuf);
  }
  return pCodecParams;
}