#define FFMPEG_CODEC_NAME "ffmpeg"
#define DEBUG_ENABLED 1

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

void logCallback(char const *format, ...);
void copyInt32(uint8_t **pBuf, int32_t value_to_copy, uint32_t *pSizeCounter);
void copyInt64(uint8_t **pBuf, int64_t value_to_copy, uint32_t *pSizeCounter);
#ifdef __cplusplus
}
#endif
