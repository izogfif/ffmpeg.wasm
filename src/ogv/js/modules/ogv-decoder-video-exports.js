const EXPORTED_FUNCTIONS = [
  "_malloc",
  "_free",
  "_ogv_video_decoder_init",
  "_ogv_video_decoder_async",
  "_ogv_video_decoder_process_header",
  "_ogv_video_decoder_process_frame",
  "_ogv_video_decoder_destroy",
];

console.log(EXPORTED_FUNCTIONS.join(","));
