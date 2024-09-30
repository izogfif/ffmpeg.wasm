#include <libavcodec/avcodec.h>
#include "ffmpeg-helper.h"

int32_t readInt32(const char **pBuf);
int64_t readInt64(const char **pBuf);
AVCodecParameters *readCodecParams(const char *paramsData);

