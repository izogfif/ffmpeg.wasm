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

static BufferQueue *bufferQueue;

static bool hasVideo = false;
static unsigned int videoTrack = -1;
static int videoCodec = -1;
static char *videoCodecName = NULL;

static bool hasAudio = false;
static unsigned int audioTrack = 0;
static int audioCodec = -1;
static char *audioCodecName = NULL;

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
static AVFrame *pFrame = NULL;
static AVPacket *pPacket = NULL;
static struct SwsContext *ptImgConvertCtx = NULL;
static AVFrame *pConvertedFrame = NULL;

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

static int readCallback(void *userdata, uint8_t *buffer, int length)
{
 int64_t data_available = bq_headroom((BufferQueue *)userdata);
 int64_t can_read_bytes = FFMIN(data_available, length);
 if (!can_read_bytes)
 {
		// End of stream. Demuxer can recover from this if more data comes in!
		return AVERROR(EAGAIN);
 }

 if (bq_read((BufferQueue *)userdata, buffer, can_read_bytes))
 {
		// error
		return AVERROR_EOF;
 }
 else
 {
		// success
		return can_read_bytes;
 }
}

static int64_t seekCallback(void *userdata, int64_t offset, int whence)
{
 int64_t pos;
 switch (whence)
 {
 case SEEK_SET:
		pos = offset;
		break;
 case SEEK_CUR:
		pos = ((BufferQueue *)userdata)->pos + offset;
		break;
 case SEEK_END:				// not implemented
 case AVSEEK_SIZE: // not implemented
 case AVSEEK_FORCE:
 default:
		return -1;
 }
 if (bq_seek((BufferQueue *)userdata, pos))
 {
		printf("Buffer seek failure in ffmpeg demuxer\n");
		return -1;
 }
 else
 {
		return 0;
 }
}

void ogv_demuxer_init(void)
{
 appState = STATE_BEGIN;
 bufferQueue = bq_init();

 pFormatContext = avformat_alloc_context();
 if (!pFormatContext)
 {
		printf("ERROR could not allocate memory for Format Context");
		return;
 }
 avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
 if (!avio_ctx_buffer)
 {
		printf("ERROR could not allocate memory for AVIO Context buffer");
		return;
 }
 avio_ctx = avio_alloc_context(
					avio_ctx_buffer, avio_ctx_buffer_size,
					0, bufferQueue, &readCallback, NULL, &seekCallback);
 if (!avio_ctx)
 {
		printf("ERROR could not allocate memory for AVIO Context");
		return;
 }
 pFormatContext->pb = avio_ctx;
 pFormatContext->flags = AVFMT_FLAG_CUSTOM_IO;

 if (avformat_open_input(&pFormatContext, NULL, NULL, NULL) != 0)
 {
		printf("ERROR could not open input");
		return;
 }
}

static void logCallback(unsigned int severity, char const *format, ...)
{
 if (severity >= LOG_LEVEL_DEFAULT)
 {
		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
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
 printf("ffmpeg processBegin\n");
 if (avformat_find_stream_info(pFormatContext, NULL) < 0)
 {
		printf("ERROR could not get the stream info");
		return 0;
 }
 AVCodecParameters *pVideoCodecParameters = NULL;
 AVCodec *pVideoCodec = NULL;
 AVCodecParameters *pAudioCodecParameters = NULL;
 AVCodec *pAudioCodec = NULL;

 // loop though all the streams and print its main information
 for (int i = 0; i < pFormatContext->nb_streams; i++)
 {
		AVCodecParameters *pLocalCodecParameters = NULL;
		pLocalCodecParameters = pFormatContext->streams[i]->codecpar;
		printf("AVStream->time_base before open coded %d/%d", pFormatContext->streams[i]->time_base.num, pFormatContext->streams[i]->time_base.den);
		printf("AVStream->r_frame_rate before open coded %d/%d", pFormatContext->streams[i]->r_frame_rate.num, pFormatContext->streams[i]->r_frame_rate.den);
		printf("AVStream->start_time %" PRId64, pFormatContext->streams[i]->start_time);
		printf("AVStream->duration %" PRId64, pFormatContext->streams[i]->duration);

		printf("finding the proper decoder (CODEC)");

		AVCodec *pLocalCodec = NULL;

		// finds the registered decoder for a codec ID
		// https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga19a0ca553277f019dd5b0fec6e1f9dca
		pLocalCodec = avcodec_find_decoder(pLocalCodecParameters->codec_id);

		if (pLocalCodec == NULL)
		{
			printf("ERROR unsupported codec!");
			// In this example if the codec is not found we just skip it
			continue;
		}

		// when the stream is a video we store its index, codec parameters and codec
		if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			if (hasVideo)
			{
				hasVideo = 1;
				videoTrack = i;
				videoCodec = pLocalCodec->id;
				videoCodecName = pLocalCodec->long_name;
				pVideoCodec = pLocalCodec;
				pVideoCodecParameters = pLocalCodecParameters;
			}

			printf("Video Codec: resolution %d x %d", pLocalCodecParameters->width, pLocalCodecParameters->height);
		}
		else if (pLocalCodecParameters->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			printf("Audio Codec: %d channels, sample rate %d", pLocalCodecParameters->channels, pLocalCodecParameters->sample_rate);
			if (!hasAudio)
			{
				hasAudio = 1;
				audioTrack = i;
				audioCodec = pLocalCodec->id;
				audioCodecName = pLocalCodec->long_name;
				pAudioCodec = pLocalCodec;
				pAudioCodecParameters = pLocalCodecParameters;
			}
		}

		// print its name, id and bitrate
		printf("\tCodec %s ID %d bit_rate %lld", pLocalCodec->name, pLocalCodec->id, pLocalCodecParameters->bit_rate);
 }

 // if (video_stream_index == -1)
 // {
	// 	printf("File does not contain a video stream!");
	// 	return 0;
 // }

 if (hasVideo)
 {
		pVideoCodecContext = avcodec_alloc_context3(pVideoCodec);
		if (!pVideoCodecContext)
		{
			printf("failed to allocated memory for AVCodecContext");
			hasVideo = 0;
			return 0;
		}
		// Fill the codec context based on the values from the supplied codec parameters
		// https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#gac7b282f51540ca7a99416a3ba6ee0d16
		if (avcodec_parameters_to_context(pVideoCodecContext, pVideoCodecParameters) < 0)
		{
			printf("failed to copy codec params to codec context");
			hasVideo = 0;
			return 0;
		}

		// Initialize the AVCodecContext to use the given AVCodec.
		// https://ffmpeg.org/doxygen/trunk/group__lavc__core.html#ga11f785a188d7d9df71621001465b0f1d
		if (avcodec_open2(pVideoCodecContext, pVideoCodec, NULL) < 0)
		{
			printf("failed to open codec through avcodec_open2");
			return -1;
		}

		// https://ffmpeg.org/doxygen/trunk/structAVFrame.html
		pFrame = av_frame_alloc();
		if (!pFrame)
		{
			printf("failed to allocate memory for AVFrame");
			hasVideo = 0;
			return 0;
		}
		// https://ffmpeg.org/doxygen/trunk/structAVPacket.html
		pPacket = av_packet_alloc();
		if (!pPacket)
		{
			printf("failed to allocate memory for AVFrame");
			hasVideo = 0;
			return 0;
		}
		if (pVideoCodecParameters->format != AV_PIX_FMT_YUV420P)
		{
			// Need to convert each input video frame to yuv420p format using sws_scale.
			// Here we're initializing conversion context
			ptImgConvertCtx = sws_getContext(
							pVideoCodecParameters->width, pVideoCodecParameters->height,
							pVideoCodecParameters->format, // (source format)
							pVideoCodecParameters->width, pVideoCodecParameters->height,
							AV_PIX_FMT_YUV420P, // (dest format)
							SWS_BICUBIC, NULL, NULL, NULL);
			pConvertedFrame = av_frame_alloc();
			pConvertedFrame->width = pVideoCodecParameters->width;
			pConvertedFrame->height = pVideoCodecParameters->height;
			pConvertedFrame->format = AV_PIX_FMT_YUV420P;
			av_frame_get_buffer(pConvertedFrame, 0);
		}
		ogvjs_callback_init_video(pVideoCodecParameters->width, pVideoCodecParameters->height,
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

 appState = STATE_DECODING;
 ogvjs_callback_loaded_metadata(FFMPEG_CODEC_NAME, FFMPEG_CODEC_NAME);

 return 1;
}

// static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame)
// {
//  // Supply raw packet data as input to a decoder
//  // https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga58bc4bf1e0ac59e27362597e467efff3
//  int response = avcodec_send_packet(pCodecContext, pPacket);

//  if (response < 0)
//  {
// 		printf("Error while sending a packet to the decoder: %s", av_err2str(response));
// 		return response;
//  }

//  while (response >= 0)
//  {
// 		// Return decoded output data (into a frame) from a decoder
// 		// https://ffmpeg.org/doxygen/trunk/group__lavc__decoding.html#ga11e6542c4e66d3028668788a1a74217c
// 		response = avcodec_receive_frame(pCodecContext, pFrame);
// 		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF)
// 		{
// 			break;
// 		}
// 		else if (response < 0)
// 		{
// 			printf("Error while receiving a frame from the decoder: %s", av_err2str(response));
// 			return response;
// 		}

// 		if (response >= 0)
// 		{
// 			printf(
// 							"Frame %d (type=%c, size=%d bytes, format=%d) pts %d key_frame %d [DTS %d]",
// 							pCodecContext->frame_number,
// 							av_get_picture_type_char(pFrame->pict_type),
// 							pFrame->pkt_size,
// 							pFrame->format,
// 							pFrame->pts,
// 							pFrame->key_frame,
// 							pFrame->coded_picture_number);

// 			//   char frame_filename[1024];
// 			//   snprintf(frame_filename, sizeof(frame_filename), "%s-%d.pgm", "frame", pCodecContext->frame_number);
// 			// Check if the frame is a planar YUV 4:2:0, 12bpp
// 			// That is the format of the provided .mp4 file
// 			// RGB formats will definitely not give a gray image
// 			// Other YUV image may do so, but untested, so give a warning
// 			if (pFrame->format != AV_PIX_FMT_YUV420P)
// 			{
// 				printf("Warning: the generated file may not be a grayscale image, but could e.g. be just the R component if the video format is RGB");
// 			}
// 			AVFrame *outFrame = av_frame_alloc();
// 			outFrame->width = pFrame->width;
// 			outFrame->height = pFrame->height;
// 			outFrame->format = AV_PIX_FMT_YUV420P;
// 			av_frame_get_buffer(outFrame, 0);

// 			// int size = av_image_get_buffer_size(outFrame->format, width, height, 1);
// 			// AVBufferRef *dataref = av_buffer_alloc(size);
// 			// av_image_fill_arrays(outFrame->data, outFrame->linesize, dataref->data, outFrame->format, outFrame->width, outFrame->height, 1);
// 			// outFrame->buf[0] = dataref;
// 			// if (av_image_alloc(outFrame->data, outFrame->linesize, pFrame->width, pFrame->height, AV_PIX_FMT_YUV420P, 1) < 0) {
// 			//   printf("Failed to call av_image_alloc");
// 			//   exit(-1);
// 			// }
// 			// Maybe this is not needed
// 			// av_image_fill_arrays(outFrame->data, outFrame->linesize, NULL, AV_PIX_FMT_YUV420P, outFrame->width, outFrame->height, 1);
// 			ConvertImage1(pFrame, outFrame);
// 			// save a grayscale frame into a .pgm file
// 			// save_gray_frame(pFrame->data[0], pFrame->linesize[0], pFrame->width, pFrame->height, frame_filename);
// 			// save_gray_frame(outFrame->data[0], outFrame->linesize[0], outFrame->width, outFrame->height, frame_filename);
// 			// av_freep(outFrame->data);
// 			av_frame_free(&outFrame);
// 		}
//  }
//  return 0;
// }

static int processDecoding(void)
{
 printf("ffmpeg processDecoding: reading next packet...\n");
 int read_frame_res = av_read_frame(pFormatContext, pPacket);
 if (read_frame_res < 0)
 {
		// Probably need more data
		printf("av_read_frame returned %d", read_frame_res);
		return 0;
 }

 printf("ffmpeg processDecoding: got packet?\n");
 // if it's the video stream
 if (hasVideo && pPacket->stream_index == videoTrack)
 {
		// printf("AVPacket->pts %" PRId64, pPacket->pts);
		// int response = decode_packet(pPacket, pVideoCodecContext, pFrame);
		// if (response < 0)
		// 	break;
		ogvjs_callback_video_packet((char *)pPacket->buf, pPacket->size, pPacket->pts, -1, 0);
 }
 else if (hasAudio && pPacket->stream_index == audioTrack)
 {
		ogvjs_callback_audio_packet((char *)pPacket->buf, pPacket->size, pPacket->pts, 0.0);
 }

 // https://ffmpeg.org/doxygen/trunk/group__lavc__packet.html#ga63d5a489b419bd5d45cfd09091cbcbc2
 av_packet_unref(pPacket);
 return 1;
}

static int processSeeking(void)
{
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
 // 	// printf("is seeking processing... FAILED at %lld %lld %lld\n", bufferQueue->pos, bq_start(bufferQueue), bq_end(bufferQueue));
 // }
 // else
 // {
 // 	// We need to go off and load stuff...
 // 	// printf("is seeking processing... MOAR SEEK %lld %lld %lld\n", bufferQueue->lastSeekTarget, bq_start(bufferQueue), bq_end(bufferQueue));
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
 if (bufsize > 0)
 {
		bq_append(bufferQueue, buffer, bufsize);
 }
}

int ogv_demuxer_process(void)
{
 if (appState == STATE_BEGIN)
 {
		return processBegin();
 }
 else if (appState == STATE_DECODING)
 {
		return processDecoding();
 }
 else if (appState == STATE_SEEKING)
 {
		if (readyForNextPacket())
		{
			return processSeeking();
		}
		else
		{
			// need more data
			// printf("not ready to read the cues\n");
			return 0;
		}
 }
 else
 {
		// uhhh...
		// printf("Invalid appState in ogv_demuxer_process\n");
		return 0;
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
 if (pFrame)
		av_frame_free(&pFrame);
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
