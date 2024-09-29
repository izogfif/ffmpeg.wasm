// Callbacks
extern void ogvjs_callback_init_audio(
    int channels,
    int rate,
    const char *videoCodecParams,
    uint32_t videoCodecParamsLength);

extern void ogvjs_callback_init_video(
    int frameWidth, int frameHeight,
    int chromaWidth, int chromaHeight,
    double fps,
    int picWidth, int picHeight,
    int picX, int picY,
    int displayWidth, int displayHeight,
    const char *videoCodecParams,
    uint32_t videoCodecParamsLength);

extern void ogvjs_callback_loaded_metadata(const char *videoCodec, const char *audioCodec);
extern void ogvjs_callback_video_packet(const char *buffer, size_t len, float frameTimestamp, float keyframeTimestamp, int isKeyframe);
extern void ogvjs_callback_audio_packet(const char *buffer, size_t len, float audioTimestamp, double discardPadding);
extern int ogvjs_callback_frame_ready(void);
extern int ogvjs_callback_audio_ready(void);
extern void ogvjs_callback_seek(uint32_t offsetLow, uint32_t offsetHigh);
