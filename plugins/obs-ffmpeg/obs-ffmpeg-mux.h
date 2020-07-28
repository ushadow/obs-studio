#pragma once

struct ffmpeg_muxer {
	obs_output_t *output;
	os_process_pipe_t *pipe;
	int64_t stop_ts;
	uint64_t total_bytes;
	struct dstr path;
	struct dstr muxer_settings;
	bool sent_headers;
	volatile bool active;
	volatile bool stopping;
	volatile bool capturing;

	/* replay buffer */
	struct circlebuf packets;
	int64_t cur_size;
	int64_t cur_time;
	int64_t max_size;
	int64_t max_time;
	int64_t save_ts;
	int keyframes;
	obs_hotkey_id hotkey;

	DARRAY(struct encoder_packet) mux_packets;
	pthread_t mux_thread;
	bool mux_thread_joinable;

	pthread_mutex_t write_mutex;
	os_sem_t *write_sem;
	os_event_t *stop_event;

	volatile bool muxing;

	bool is_network;
	bool threading_buffer;

	int dropped_frames;
	int min_priority;
	int64_t last_dts_usec;
};