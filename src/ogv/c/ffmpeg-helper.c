#include "ffmpeg-helper.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
