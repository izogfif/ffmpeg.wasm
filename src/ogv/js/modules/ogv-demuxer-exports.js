const EXPORTED_FUNCTIONS = [
  "_malloc",
  "_free",
  "_ogv_demuxer_init",
  "_ogv_demuxer_receive_input",
  "_ogv_demuxer_process",
  "_ogv_demuxer_destroy",
  "_ogv_demuxer_media_length",
  "_ogv_demuxer_media_duration",
  "_ogv_demuxer_seekable",
  "_ogv_demuxer_keypoint_offset",
  "_ogv_demuxer_seek_to_keypoint",
  "_ogv_demuxer_flush"
];

console.log(EXPORTED_FUNCTIONS.join(","));
