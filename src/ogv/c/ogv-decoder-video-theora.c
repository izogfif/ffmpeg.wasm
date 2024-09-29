#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>

#include "ogv-decoder-video.h"

static int display_width = 0;
static int display_height = 0;

void ogv_video_decoder_init(void)
{
}

int ogv_video_decoder_async(void)
{
 return 0;
}

int ogv_video_decoder_process_header(const char *data, size_t data_len)
{
 printf("ogv-decoder-video-theora: ogv_video_decoder_process_header is being called. data=%p, data size=%lld\n", data, data_len);
 return 1;
}

int read_int(const char **pBuf)
{
 int result = -1;
 memcpy(&result, *pBuf, 4);
 *pBuf += 4;
	return result;
}

int ogv_video_decoder_process_frame(const char *data, size_t data_len)
{
 // printf("ogv-decoder-video-theora: ogv_video_decoder_process_frame is being called. data size=%d\n", data_len);
 if (!data_len)
 {
		return 1;
 }
 char *pBuf = data;
 int width = read_int(&pBuf);
 int height = read_int(&pBuf);
 int linesize0 = read_int(&pBuf);
 int linesize1 = read_int(&pBuf);
 int linesize2 = read_int(&pBuf);
 const int datasize0 = linesize0 * height;
 const int datasize1 = linesize1 * height / 2;
 const int datasize2 = linesize2 * height / 2;
 printf("ogv-decoder-video-theora: width=%d, height=%d, \
	 linesize0=%d, linesize1=%d, linesize2=%d, \
		datasize0=%d, datasize1=%d, datasize2=%d\n",
								width, height,
								linesize0, linesize1, linesize2,
								datasize0, datasize1, datasize2);

 ogvjs_callback_frame(
					pBuf, linesize0,
					pBuf + datasize0, linesize1,
					pBuf + datasize0 + datasize1, linesize2,
					width, height,
					width / 2, height / 2,
					width, height,
					0, 0,
					width, height);
 return 1;
}

void ogv_video_decoder_destroy(void)
{
}
