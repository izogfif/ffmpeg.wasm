#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stddef.h>
#include <libavutil/error.h>
#include <libavformat/avio.h>

#ifdef __cplusplus
}
#endif

#include "ffmpeg-helper.h"
#include "ogv-demuxer.h"
#include "io-helper.h"

#include <emscripten.h>

#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
enum CallbackState callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
uint64_t fileSize = -1;
BufferQueue *bufferQueue = NULL;
int waitingForInput = 0;

void requestSeek(int64_t pos)
{
  bq_flush((BufferQueue *)bufferQueue);
  ((BufferQueue *)bufferQueue)->pos = pos;
  uint32_t lower = pos & 0xffffffff;
  uint32_t higher = pos >> 32;
  ogvjs_callback_seek(lower, higher);
}

int readCallback(void *userdata, uint8_t *buffer, int length)
{
  callbackState = CALLBACK_STATE_IN_READ_CALLBACK;
  int64_t can_read_bytes = 0;
  int64_t pos = -1;
  int64_t data_available = -1;
  while (1)
  {
    data_available = bq_headroom((BufferQueue *)userdata);
    // logMessage("readCallback: bytes requested: %d, available: %d\n", length, (int)data_available);
    can_read_bytes = FFMIN(data_available, length);
    pos = ((BufferQueue *)userdata)->pos;
    if (can_read_bytes || !(fileSize - pos))
    {
      break;
    }
    // logMessage(
    //     "readCallback: bytes requested: %d, available: %d, bytes until end of file: %lld, cur pos: %lld, file size: %lld. Waiting for buffer refill: %d.\n",
    //     length, (int)data_available, fileSize - pos, pos, fileSize, waitingForInput);
    if (!waitingForInput)
    {
      waitingForInput = 1;
      logMessage("Requesting seek to %lld\n", pos);

      requestSeek(pos);
    }
    emscripten_sleep(100);
  }
  if (!can_read_bytes)
  {
    logMessage("readCallback: end of file reached. Bytes requested: %d, available: %d, bytes until end of file: %lld. Reporting EOF.\n", length, (int)data_available, fileSize - pos);
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return AVERROR_EOF;
  }
  if (bq_read((BufferQueue *)userdata, (char *)buffer, (size_t)can_read_bytes))
  {
    logMessage("readCallback: bq_red failed. Bytes requested: %d, available: %d, bytes until end of file: %lld. Waiting for buffer refill.\n", length, (int)data_available, fileSize - pos);
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return AVERROR_EOF;
  }
  else
  {
    // success
    // logMessage("readCallback: %d bytes read\n", (int)can_read_bytes);
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return can_read_bytes;
  }
}

int64_t seekCallback(void *userdata, int64_t offset, int whence)
{
  callbackState = CALLBACK_STATE_IN_SEEK_CALLBACK;
  logMessage("seekCallback is being called: offset=%lld, whence=%d\n", offset, whence);
  int64_t pos;
  switch (whence)
  {
  case SEEK_SET:
  {
    pos = offset;
    break;
  }
  case SEEK_CUR:
  {
    pos = ((BufferQueue *)userdata)->pos + offset;
    break;
  }
  case AVSEEK_SIZE:
  {
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return fileSize;
  }
  case SEEK_END: // not implemented
  case AVSEEK_FORCE:
  default:
    callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
    return -1;
  }
  pos = FFMIN(pos, fileSize);
  while (1)
  {
    const int seekRet = bq_seek((BufferQueue *)userdata, pos);
    int64_t data_available = seekRet ? 0 : bq_headroom((BufferQueue *)userdata);
    int64_t bytes_until_end = fileSize - pos;
    if (seekRet || data_available < FFMIN(bytes_until_end, avio_ctx_buffer_size))
    {
      logMessage("FFmpeg demuxer error: buffer seek failure. Error code: %d. Bytes until end: %lld, data available: %lld\n", seekRet, bytes_until_end, data_available);
      requestSeek(pos);
      emscripten_sleep(100);
    }
    else
    {
      logMessage("FFmpeg demuxer: succesfully seeked to %lld.\n", pos);
      callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
      return pos;
    }
  }
}
