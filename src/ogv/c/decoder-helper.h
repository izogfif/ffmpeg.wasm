#include <libavcodec/avcodec.h>

int32_t read_int32(const char **pBuf);
int64_t read_int64(const char **pBuf);
AVCodecParameters *readCodecParams(const char *paramsData, int paramsDataLength);
void logCallback(char const *format, ...);