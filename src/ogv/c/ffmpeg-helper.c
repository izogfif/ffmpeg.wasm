#include "ffmpeg-helper.h"
#include <stdarg.h>
#include <stdio.h>

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
