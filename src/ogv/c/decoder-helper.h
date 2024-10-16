#include "ffmpeg-helper.h"

#ifdef __cplusplus
extern "C"
{
#endif

#include <libavcodec/avcodec.h>

  AVCodecParameters *readCodecParams(const char *paramsData, AVRational *pTimeBase);

#ifdef __cplusplus
}
#endif
