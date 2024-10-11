#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ogv-buffer-queue.h"

enum CallbackState
{
  CALLBACK_STATE_NOT_IN_CALLBACK,
  CALLBACK_STATE_IN_SEEK_CALLBACK,
  CALLBACK_STATE_IN_READ_CALLBACK,
};

extern enum CallbackState callbackState;
extern uint64_t fileSize;
extern BufferQueue *bufferQueue;
extern int waitingForInput;

const unsigned int avio_ctx_buffer_size = 4096;

int readCallback(void *userdata, uint8_t *buffer, int length);
int64_t seekCallback(void *userdata, int64_t offset, int whence);

#ifdef __cplusplus
}
#endif