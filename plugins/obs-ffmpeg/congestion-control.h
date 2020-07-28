#pragma once

int hls_stream_dropped_frames(void *data);

void check_to_drop_frames(struct ffmpeg_muxer *stream, bool pframes);