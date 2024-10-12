#include <libavcodec/avcodec.h>
#include "ffmpeg-helper.h"

#ifdef __cplusplus
extern "C" {
#endif

int32_t readInt32(const char **pBuf);
int64_t readInt64(const char **pBuf);
AVCodecParameters *readCodecParams(const char *paramsData, AVRational *pTimeBase);

#ifdef __cplusplus
}
#endif
