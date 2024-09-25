#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/pixfmt.h>
#include "ogv-demuxer.h"
#include "ogv-buffer-queue.h"
#include "ffmpeg_helper.h"
#include <emscripten.h>

static BufferQueue *bufferQueue;

static bool hasVideo = false;
static unsigned int videoTrack = -1;
static const char *videoCodecName = NULL;

static bool hasAudio = false;
static unsigned int audioTrack = 0;
static const char *audioCodecName = NULL;

// Time to seek to in milliseconds
static int64_t seekTime;
static unsigned int seekTrack;
static int64_t startPosition;

static double lastKeyframeKimestamp = -1;
static unsigned int LOG_LEVEL_DEFAULT = 0;
static const unsigned int avio_ctx_buffer_size = 4096;
static uint8_t *avio_ctx_buffer = NULL;
static AVIOContext *avio_ctx = NULL;
static AVFormatContext *pFormatContext = NULL;
static AVCodecContext *pVideoCodecContext = NULL;
static AVPacket *pPacket = NULL;
static struct SwsContext *ptImgConvertCtx = NULL;
static AVFrame *pConvertedFrame = NULL;
static int64_t prev_data_available = 0;
static int retry_count = 0;
const int MAX_RETRY_COUNT = 3;
static AVRational videoStreamTimeBase = {1, 1};
static AVRational audioStreamTimeBase = {1, 1};
static char *fileName = NULL;
static uint64_t fileSize = 0;
static int minBufSize = 1 * 1024 * 1024;
static int waitingForInput = 0;

enum CallbackState
{
 CALLBACK_STATE_NOT_IN_CALLBACK,
 CALLBACK_STATE_IN_SEEK_CALLBACK,
 CALLBACK_STATE_IN_READ_CALLBACK,
};

static enum CallbackState callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;

enum AppState
{
 STATE_BEGIN,
 STATE_DECODING,
 STATE_SEEKING
} appState;

struct buffer_data
{
 uint8_t *ptr;
 size_t size; ///< size left in the buffer
};

void copy_int(uint8_t **pBuf, int value_to_copy)
{
 memcpy(*pBuf, &value_to_copy, 4);
 *pBuf += 4;
}

static void logCallback(char const *format, ...)
{
 if (true)
 {
		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
 }
}

void requestSeek(int64_t pos)
{
 bq_flush(bufferQueue);
 bufferQueue->pos = pos;
 uint32_t lower = pos & 0xffffffff;
 uint32_t higher = pos >> 32;
 ogvjs_callback_seek(lower, higher);
}

static int readCallback(void *userdata, uint8_t *buffer, int length)
{
 callbackState = CALLBACK_STATE_IN_READ_CALLBACK;
 int64_t can_read_bytes = 0;
 int64_t pos = -1;
 int64_t data_available = -1;
 while (1)
 {
		data_available = bq_headroom((BufferQueue *)userdata);
		logCallback("readCallback: bytes requested: %d, available: %d\n", length, (int)data_available);
		can_read_bytes = FFMIN(data_available, length);
		pos = ((BufferQueue *)userdata)->pos;
		if (can_read_bytes || !(fileSize - pos))
		{
			break;
		}
		logCallback(
						"readCallback: bytes requested: %d, available: %d, bytes until end of file: %lld, cur pos: %lld, file size: %lld. Waiting for buffer refill: %d.\n",
						length, (int)data_available, fileSize - pos, pos, fileSize, waitingForInput);
		if (!waitingForInput)
		{
			waitingForInput = 1;
			requestSeek(pos);
		}
		emscripten_sleep(100);
 }
 if (!can_read_bytes)
 {
		logCallback("readCallback: end of file reached. Bytes requested: %d, available: %d, bytes until end of file: %lld. Reporting EOF.\n", length, (int)data_available, fileSize - pos);
		callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
		return AVERROR_EOF;
 }
 if (bq_read((BufferQueue *)userdata, buffer, can_read_bytes))
 {
		logCallback("readCallback: bq_red failed. Bytes requested: %d, available: %d, bytes until end of file: %lld. Waiting for buffer refill.\n", length, (int)data_available, fileSize - pos);
		callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
		return AVERROR_EOF;
 }
 else
 {
		// success
		logCallback("readCallback: %d bytes read\n", (int)can_read_bytes);
		callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
		return can_read_bytes;
 }
}

static int64_t seekCallback(void *userdata, int64_t offset, int whence)
{
 callbackState = CALLBACK_STATE_IN_SEEK_CALLBACK;
 logCallback("seekCallback is being called: offset=%lld, whence=%d\n", offset, whence);
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
			logCallback("FFmpeg demuxer error: buffer seek failure. Error code: %d. Bytes until end: %lld, data available: %lld\n", seekRet, bytes_until_end, data_available);
			requestSeek(pos);
			emscripten_sleep(100);
		}
		else
		{
			logCallback("FFmpeg demuxer: succesfully seeked to %lld.\n", pos);
			callbackState = CALLBACK_STATE_NOT_IN_CALLBACK;
			return pos;
		}
 }
}

void ogv_demuxer_init(const char *fileSizeAndPath, int len)
{
 const int fileSizeSize = 8;
 memcpy(&fileSize, fileSizeAndPath, fileSizeSize);
 logCallback("ogv_demuxer_init with file size: %lld (fileSizeAndPath has length %d)\n", fileSize, len);
 appState = STATE_BEGIN;
 if (fileName)
 {
		free(fileName);
		fileName = NULL;
 }
 int fileNameLength = len - fileSizeSize;
 const char *inputFilePath = fileSizeAndPath + fileSizeSize;
 if (fileNameLength)
 {
		fileName = malloc(fileNameLength + 1);
		memset(fileName, 0, fileNameLength + 1);
		memcpy(fileName, inputFilePath, fileNameLength);
		logCallback("FFmpeg demuxer: ogv_demuxer_init with fileName %s\n", fileName);
 }

 bufferQueue = bq_init();

 pFormatContext = avformat_alloc_context();
 if (!pFormatContext)
 {
		logCallback("FFmpeg demuxer error: could not allocate memory for Format Context\n");
		return;
 }
 avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
 if (!avio_ctx_buffer)
 {
		logCallback("FFmpeg demuxer error: could not allocate memory for AVIO Context buffer");
		return;
 }
 avio_ctx = avio_alloc_context(
					avio_ctx_buffer, avio_ctx_buffer_size,
					0, bufferQueue, &readCallback, NULL,
					&seekCallback);
 if (!avio_ctx)
 {
		logCallback("FFmpeg demuxer error: could not allocate memory for AVIO Context\n");
		return;
 }
 if (!fileName)
 {
		pFormatContext->pb = avio_ctx;
		pFormatContext->flags = AVFMT_FLAG_CUSTOM_IO;
 }
}

// static int64_t tellCallback(void *userdata)
// {
// 	return bq_tell((BufferQueue *)userdata);
// }

static int readyForNextPacket(void)
{
 return 1; // Always ready
}

static int processBegin(void)
{
 logCallback("FFmpeg demuxer: processBegin is being called\n");
 const int openInputRes = avformat_open_input(&pFormatContext, fileName, NULL, NULL);
 if (openInputRes != 0)
 {
		logCallback("FFmpeg demuxer error: could not open input. Error code: %d (%s)\n", openInputRes, av_err2str(openInputRes));
		return -1;
 }
 if (avformat_find_stream_info(pFormatContext, NULL) < 0)
 {
		logCallback("FFmpeg demuxer error: could not get the stream info");
		return -1;
 }
 AVCodecParameters *pVideoCodecParameters = NULL;
 const AVCodec *pVideoCodec = NULL;
 AVCodecParameters *pAudioCodecParameters = NULL;
 const AVCodec *pAudioCodec = NULL;

 // loop though all the streams and print its main information
 for (int i = 0; i < pFormatContext->nb_streams; i++)
 {
		AVStream *pStream = pFormatContext->streams[i];
		AVCodecParameters *pLocalCodecParameters = NULL;
		pLocalCodecParameters = pStream->codecpar;
		logCallback("AVStream->time_base before open coded %d/%d\n", pStream->time_base.num, pStream->time_base.den);
		logCallback("AVStream->r_frame_rate before open coded %d/%d\n", pStream->r_frame_rate.num, pStream->r_frame_rate.den);
		logCallback("AVStream->start_time %" PRId64 "\n", pStream->start_time);
		logCallback("AVStream->duration %" PRId64 "\n", pStream->duration);
		logCallback("Searching for decoder\n");

		const AVCodec *pLocalCodec = NULL;

		// finds the registered decoder for a codec ID
		// https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
		pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

		if (pLocalCodec == NULL)
		{
			logCallback("FFmpeg demuxer error: unsupported codec ID %d!\n", pLocalCodecParameters->codec_id);
			// In this example if the codec is not found we just skip it
			continue;
		}

		// when the stream is a video we store its index, codec parameters and codec
		switch (pLocalCodecParameters->codec_type)
		{
		case AVMEDIA_TYPE_VIDEO:
		{
			logCallback("Stream %d is video stream. Resolution %d x %d. Codec name: %s\n", i, pLocalCodecParameters->width, pLocalCodecParameters->height, pLocalCodec->long_name);
			if (!hasVideo)
			{
				hasVideo = 1;
				videoTrack = i;
				videoCodecName = pLocalCodec->long_name;
				pVideoCodec = pLocalCodec;
				pVideoCodecParameters = pLocalCodecParameters;
				videoStreamTimeBase = pStream->time_base;
			}
			break;
		}
		case AVMEDIA_TYPE_AUDIO:
		{
			logCallback("Stream %d is audio stream. Codec name: %s, sample rate: %d\n", i, pLocalCodec->name, pLocalCodecParameters->sample_rate);
			if (!hasAudio)
			{
				hasAudio = 1;
				audioTrack = i;
				audioCodecName = pLocalCodec->long_name;
				pAudioCodec = pLocalCodec;
				pAudioCodecParameters = pLocalCodecParameters;
				audioStreamTimeBase = pStream->time_base;
			}
			break;
		}
		default:
		{
			logCallback("Stream %d has type %d\n", i, pLocalCodecParameters->codec_type);
			break;
		}
		}
		// print its name, id and bitrate
 }

 // if (video_stream_index == -1)
 // {
 // 	logCallback("File does not contain a video stream!\n");
 // 	return 0;
 // }

 if (hasVideo)
 {
		pVideoCodecContext = avcodec_alloc_context3(pVideoCodec);
		if (!pVideoCodecContext)
		{
			logCallback("failed to allocated memory for AVCodecContext\n");
			hasVideo = 0;
			return 0;
		}
		// Fill the codec context based on the values from the supplied codec parameters
		// https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
		if (avcodec_parameters_to_context(pVideoCodecContext, pVideoCodecParameters) < 0)
		{
			logCallback("failed to copy codec params to codec context\n");
			hasVideo = 0;
			return 0;
		}

		// Initialize the AVCodecContext to use the given AVCodec.
		// https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
		if (avcodec_open2(pVideoCodecContext, pVideoCodec, NULL) < 0)
		{
			logCallback("failed to open codec through avcodec_open2\n");
			return -1;
		}

		// https://ffmpeg.org/doxygen/trunk/structAVPacket.html
		pPacket = av_packet_alloc();
		if (!pPacket)
		{
			logCallback("failed to allocate memory for AVFrame\n");
			hasVideo = 0;
			return 0;
		}
		if (pVideoCodecParameters->format != AV_PIX_FMT_YUV420P)
		{
			logCallback(
							"Video pixel format is %d, need %d, initializing scaling context\n",
							pVideoCodecParameters->format,
							AV_PIX_FMT_YUV420P);
			// Need to convert each input video frame to yuv420p format using sws_scale.
			// Here we're initializing conversion context
			ptImgConvertCtx = sws_getContext(
							pVideoCodecParameters->width, pVideoCodecParameters->height,
							pVideoCodecParameters->format,
							pVideoCodecParameters->width, pVideoCodecParameters->height,
							AV_PIX_FMT_YUV420P,
							SWS_FAST_BILINEAR, NULL, NULL, NULL);
			pConvertedFrame = av_frame_alloc();
			pConvertedFrame->width = pVideoCodecParameters->width;
			pConvertedFrame->height = pVideoCodecParameters->height;
			pConvertedFrame->format = AV_PIX_FMT_YUV420P;
			av_frame_get_buffer(pConvertedFrame, 0);
		}
		ogvjs_callback_init_video(
						pVideoCodecParameters->width, pVideoCodecParameters->height,
						pVideoCodecParameters->width >> 1, pVideoCodecParameters->height >> 1, // @todo assuming 4:2:0
						0,																																																																					// @todo get fps
						pVideoCodecParameters->width,
						pVideoCodecParameters->height,
						0, 0,
						pVideoCodecParameters->width, pVideoCodecParameters->height);
 }

 if (hasAudio)
 {
		// TODO: implement
		hasAudio = 0;
		// ogvjs_callback_audio_packet((char *)data, len, -1, 0.0);
 }

 logCallback("Loaded metadata. Video codec: %s, audio codec: %s\n", videoCodecName, audioCodecName);
 appState = STATE_DECODING;
 ogvjs_callback_loaded_metadata(
					// Always return "theora" to load modified theora video decoder that's currently in pass-through mode
					videoCodecName ? "theora" : NULL,
					// audioCodecName temporarily disabled
					NULL);

 return 1;
}

AVFrame *getConvertedFrame(AVFrame *pDecodedFrame)
{
 logCallback("FFmpeg demuxer: getConvertedFrame is being called\n");
 if (pDecodedFrame->format == AV_PIX_FMT_YUV420P)
 {
		return pDecodedFrame;
 }
 logCallback("FFmpeg demuxer: av_frame_alloc\n");

 AVFrame *pConvertedFrame = av_frame_alloc();
 if (!pConvertedFrame)
 {
		logCallback("FFmpeg demuxer: failed to create frame for conversion");
		av_frame_free(&pDecodedFrame);
		return NULL;
 }
 pConvertedFrame->width = pDecodedFrame->width;
 pConvertedFrame->height = pDecodedFrame->height;
 pConvertedFrame->format = AV_PIX_FMT_YUV420P;
 pConvertedFrame->pts = pDecodedFrame->pts;
 int get_buffer_res = av_frame_get_buffer(pConvertedFrame, 0);
 if (get_buffer_res)
 {
		logCallback("FFmpeg demuxer: failed to allocate buffer for converted frame\n");
		av_frame_free(&pDecodedFrame);
		return NULL;
 }
 logCallback("FFmpeg demuxer: calling sws_scale\n");
 int scaleResult = sws_scale(
					ptImgConvertCtx,
					(const uint8_t *const *)pDecodedFrame->data,
					pDecodedFrame->linesize,
					0,
					pDecodedFrame->height,
					(uint8_t *const *)pConvertedFrame->data,
					pConvertedFrame->linesize);
 if (scaleResult != pConvertedFrame->height)
 {
		logCallback("FFmpeg demuxer error: scaling failed: sws_scale returned %d, expected %d\n", scaleResult, pConvertedFrame->height);
		av_frame_free(&pDecodedFrame);
		return NULL;
 }
 logCallback("FFmpeg demuxer: sws_scale returned %d\n", scaleResult);
 av_frame_free(&pDecodedFrame);
 return pConvertedFrame;
}

void onDecodedFrame(AVFrame *pDecodedFrame, AVRational timeBase)
{
 logCallback("FFmpeg demuxer: onDecodedFrame is being called\n");
 if (!pDecodedFrame)
 {
		logCallback("FFmpeg demuxer: pDecodedFrame is NULL\n");
		return;
 }
 AVFrame *pConvertedFrame = getConvertedFrame(pDecodedFrame);
 const int int_size = 4;
 const int header_size = int_size * 5;
 const int datasize0 = pConvertedFrame->linesize[0] * pConvertedFrame->height;
 const int datasize1 = pConvertedFrame->linesize[1] * pConvertedFrame->height / 2;
 const int datasize2 = pConvertedFrame->linesize[2] * pConvertedFrame->height / 2;
 const int total_size = header_size + datasize0 + datasize1 + datasize2;
 uint8_t *const pBuf = (uint8_t *)malloc(total_size);
 memset(pBuf, 0, total_size);
 if (!pBuf)
 {
		logCallback("FFmpeg demuxer: failed to allocate memory for buffer for ogvjs_callback_video_packet\n");
		av_frame_free(&pConvertedFrame);
		return;
 }
 uint8_t *pCur = pBuf;
 copy_int(&pCur, pConvertedFrame->width);
 copy_int(&pCur, pConvertedFrame->height);
 copy_int(&pCur, pConvertedFrame->linesize[0]);
 copy_int(&pCur, pConvertedFrame->linesize[1]);
 copy_int(&pCur, pConvertedFrame->linesize[2]);
 memcpy(pCur, pConvertedFrame->data[0], datasize0);
 pCur += datasize0;
 memcpy(pCur, pConvertedFrame->data[1], datasize1);
 pCur += datasize1;
 memcpy(pCur, pConvertedFrame->data[2], datasize2);
 logCallback("FFmpeg demuxer: calling ogvjs_callback_video_packet with %d bytes of payload. \
	 width=%d, height=%d, \
	 linesize0=%d, linesize1=%d, linesize2=%d, \
		datasize0=%d, datasize1=%d, datasize2=%d\n",
													total_size,
													pConvertedFrame->width, pConvertedFrame->height,
													pConvertedFrame->linesize[0], pConvertedFrame->linesize[1], pConvertedFrame->linesize[2],
													datasize0, datasize1, datasize2);
 ogvjs_callback_video_packet((const char *)pBuf, total_size, pConvertedFrame->pts * av_q2d(timeBase), -1, 0);

 free(pBuf);
 av_frame_free(&pConvertedFrame);
}

void decodeVideoPacket(AVPacket *pPacket, AVCodecContext *pCodecContext, AVRational streamTimeBase)
{
 logCallback("FFmpeg demuxer: calling avcodec_send_packet\n");
 // Supply raw packet data as input to a decoder
 // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga58bc4bf1e0ac59e27362597e467efff3
 int response = avcodec_send_packet(pCodecContext, pPacket);
 logCallback("FFmpeg demuxer: avcodec_send_packet returned %d (%s)\n", response, av_err2str(response));

 if (response < 0)
 {
		logCallback("FFmpeg demuxer error: while sending a packet to the decoder: %s", av_err2str(response));
 }
 while (response >= 0)
 {
		// Return decoded output data (into a frame) from a decoder
		// https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga11e6542c4e66d3028668788a1a74217c
		AVFrame *pDecodedFrame = av_frame_alloc();
		if (!pDecodedFrame)
		{
			logCallback("FFmpeg demuxer error: could not allocate video frame\n");
			return;
		}

		logCallback("FFmpeg demuxer: calling avcodec_receive_frame\n");
		response = avcodec_receive_frame(pCodecContext, pDecodedFrame);
		logCallback("FFmpeg demuxer: avcodec_receive_frame returned %d (%s)\n", response, av_err2str(response));
		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
		{
			logCallback("FFmpeg demuxer error: avcodec_receive_frame needs more data %s\n", av_err2str(response));
			return;
		}
		else if (response < 0)
		{
			logCallback("FFmpeg demuxer error: while receiving a frame from the decoder: %s\n", av_err2str(response));
			return;
		}

		if (response >= 0)
		{
			logCallback(
							"Frame %d (type=%c, size=%d bytes, format=%d) pts %lld key_frame %d [DTS %d]\n",
							pCodecContext->frame_number,
							av_get_picture_type_char(pDecodedFrame->pict_type),
							pDecodedFrame->pkt_size,
							pDecodedFrame->format,
							pDecodedFrame->pts,
							pDecodedFrame->key_frame,
							pDecodedFrame->coded_picture_number);
			if (!pDecodedFrame)
			{
				logCallback("FFmpeg demuxer error: something is wrong: %d", (int)pDecodedFrame);
			}
			onDecodedFrame(pDecodedFrame, streamTimeBase);
		}
 }
}

static int processDecoding(void)
{
 logCallback("FFmpeg demuxer: processDecoding is being called\n");

 int read_frame_res = av_read_frame(pFormatContext, pPacket);
 if (read_frame_res < 0)
 {
		// Probably need more data
		logCallback("FFmpeg demuxer: av_read_frame returned %d (%s)\n", read_frame_res, av_err2str(read_frame_res));
		return 0;
 }

 logCallback("FFmpeg demuxer: processDecoding successfully read packet. av_read_frame returned %d (%s)\n", read_frame_res, av_err2str(read_frame_res));
 // if it's the video stream
 if (hasVideo && pPacket->stream_index == videoTrack)
 {
		logCallback("FFmpeg demuxer: got packet for video stream %d, pts: %lld\n", videoTrack, pPacket->pts);
		decodeVideoPacket(pPacket, pVideoCodecContext, videoStreamTimeBase);
		av_packet_unref(pPacket);
		return 1;
 }
 else if (hasAudio && pPacket->stream_index == audioTrack)
 {
		logCallback("FFmpeg demuxer: got packet for audio stream %d\n", audioTrack);
		if (pPacket->size)
		{
			ogvjs_callback_audio_packet((char *)pPacket->buf, pPacket->size, pPacket->pts, 0.0);
		}
		av_packet_unref(pPacket);
		return 1;
 }

 // https://ffmpeg.org/doxygen/trunk/group__lavc__packet.html#ga63d5a489b419bd5d45cfd09091cbcbc2
 av_packet_unref(pPacket);
 return 1;
}

static int processSeeking(void)
{
 logCallback("FFmpeg demuxer: processSeeking is being called\n");
 bufferQueue->lastSeekTarget = -1;
 int seek_result = avformat_seek_file(pFormatContext, seekTrack, seekTime - 10000, seekTime, seekTime, 0);
 if (seek_result < 0)
 {
		// Something is wrong
		// Return false to indicate we need i/o
		return 0;
 }
 appState = STATE_DECODING;
 // Roll over to packet processing.
 // Return true to indicate we should keep reading.
 return 1;

 // if (bufferQueue->lastSeekTarget == -1)
 // {
 // 	// Maybe we just need more data?
 // 	// logCallback("is seeking processing... FAILED at %lld %lld %lld\n", bufferQueue->pos, bq_start(bufferQueue), bq_end(bufferQueue));
 // }
 // else
 // {
 // 	// We need to go off and load stuff...
 // 	// logCallback("is seeking processing... MOAR SEEK %lld %lld %lld\n", bufferQueue->lastSeekTarget, bq_start(bufferQueue), bq_end(bufferQueue));
 // 	int64_t target = bufferQueue->lastSeekTarget;
 // 	bq_flush(bufferQueue);
 // 	bufferQueue->pos = target;
 // 	ogvjs_callback_seek(target);
 // }
 // // Return false to indicate we need i/o
 // return 0;
}

void ogv_demuxer_receive_input(const char *buffer, int bufsize)
{
 logCallback("FFmpeg demuxer: ogv_demuxer_receive_input is being called. bufsize: %d\n", bufsize);
 if (bufsize > 0)
 {
		waitingForInput = 0;
		bq_append(bufferQueue, buffer, bufsize);
 }
 logCallback("FFmpeg demuxer: ogv_demuxer_receive_input: exited.\n");
}

int ogv_demuxer_process(void)
{
 logCallback("FFmpeg demuxer: ogv_demuxer_process is being called\n");

 const int64_t data_available = bq_headroom(bufferQueue);
 const int64_t bytes_until_end = fileSize - bufferQueue->pos;
 logCallback("FFmpeg demuxer: buffer got %lld bytes of data in it. Bytes until end: %lld, pos: %lld\n", data_available, bytes_until_end, bufferQueue->pos);

 if (data_available < minBufSize && bytes_until_end > data_available)
 {
		// Buffer at least 1 megabyte of data first
		if (data_available != prev_data_available)
		{
			prev_data_available = data_available;
			retry_count = 0;
			return 0;
		}
		if (retry_count <= MAX_RETRY_COUNT)
		{
			++retry_count;
			return 0;
		}
		// No more data available: work with what we have
		logCallback("FFmpeg demuxer: processDecoding: failed to buffer more data. %lld bytes of data\n", prev_data_available);
 }

 prev_data_available = data_available;
 retry_count = 0;
 if (callbackState != CALLBACK_STATE_NOT_IN_CALLBACK)
 {
		logCallback("FFmpeg demuxer: currently in callback: %d\n", callbackState);
		return 0;
 }
 switch (appState)
 {
 case STATE_BEGIN:
 {
		return processBegin();
 }
 case STATE_DECODING:
 {
		return processDecoding();
 }
 case STATE_SEEKING:
 {
		if (readyForNextPacket())
		{
			return processSeeking();
		}
		else
		{
			// need more data
			// logCallback("not ready to read the cues\n");
			return 0;
		}
 }
 default:
 {
		// uhhh...
		logCallback("Invalid appState %d in ogv_demuxer_process\n", appState);
		return 0;
 }
 }
}

void ogv_demuxer_destroy(void)
{
 // should probably tear stuff down, eh
 if (pConvertedFrame)
		av_frame_free(&pConvertedFrame);
 if (pFormatContext)
		avformat_close_input(&pFormatContext);
 if (pPacket)
		av_packet_free(&pPacket);
 if (pVideoCodecContext)
		avcodec_free_context(&pVideoCodecContext);
 bq_free(bufferQueue);
 bufferQueue = NULL;
}

void ogv_demuxer_flush(void)
{
 bq_flush(bufferQueue);
 // we may not need to handle the packet queue because this only
 // happens after seeking and nestegg handles that internally
 // lastKeyframeKimestamp = -1;
}

/**
 * @return segment length in bytes, or -1 if unknown
 */
long ogv_demuxer_media_length(void)
{
 // @todo check if this is needed? maybe an ogg-specific thing
 return -1;
}

/**
 * @return segment duration in seconds, or -1 if unknown
 */
float ogv_demuxer_media_duration(void)
{
 if (pFormatContext->duration <= 0)
		return -1;
 return pFormatContext->duration / 1000000.0;
}

int ogv_demuxer_seekable(void)
{
 // Audio WebM files often have no cues; allow brute-force seeking
 // by linear demuxing through hopefully-cached data.
 return 1;
}

long ogv_demuxer_keypoint_offset(long time_ms)
{
 // can't do with nestegg's API; use ogv_demuxer_seek_to_keypoint instead
 return -1;
}

int ogv_demuxer_seek_to_keypoint(long time_ms)
{
 appState = STATE_SEEKING;
 seekTime = (int64_t)time_ms;
 if (hasVideo)
 {
		seekTrack = videoTrack;
 }
 else if (hasAudio)
 {
		seekTrack = audioTrack;
 }
 else
 {
		return 0;
 }
 processSeeking();
 return 1;
}
