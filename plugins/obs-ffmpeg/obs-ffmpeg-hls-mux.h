#pragma once

#include <obs-module.h>

const char *ffmpeg_hls_mux_getname(void *type);
int hls_stream_dropped_frames(void *data);
void ffmpeg_hls_mux_destroy(void *data);
void *ffmpeg_hls_mux_create(obs_data_t *settings, obs_output_t *output);
bool ffmpeg_hls_mux_start(void *data);
void ffmpeg_hls_mux_data(void *data, struct encoder_packet *packet);
bool hls_deactivate(struct ffmpeg_muxer *stream, int code);