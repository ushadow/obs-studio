#include <obs-module.h>
#include <obs-avc.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/circlebuf.h>
#include <util/pipe.h>
#include <util/threading.h>

#include "obs-ffmpeg-mux.h"
#include "obs-ffmpeg-hls-mux.h"

#define do_log(level, format, ...)                  \
	blog(level, "[ffmpeg muxer: '%s'] " format, \
	     obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)

const char *ffmpeg_hls_mux_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegHlsMuxer");
}

int hls_stream_dropped_frames(void *data)
{
	struct ffmpeg_muxer *stream = data;
	return stream->dropped_frames;
}

int hls_deactivate(struct ffmpeg_muxer *stream, int code)
{
	int ret = -1;

	if (active(stream)) {
		ret = os_process_pipe_destroy(stream->pipe);
		stream->pipe = NULL;

		os_atomic_set_bool(&stream->active, false);
		os_atomic_set_bool(&stream->sent_headers, false);

		info("Output of file '%s' stopped",
		     stream->printable_path.array);
	}

	if (code) {
		obs_output_signal_stop(stream->output, code);
	} else if (stopping(stream)) {
		obs_output_end_data_capture(stream->output);
	}

	os_atomic_set_bool(&stream->stopping, false);

	if (stream->mux_thread_joinable) {
		os_event_signal(stream->stop_event);
		os_sem_post(stream->write_sem);
		pthread_join(stream->mux_thread, NULL);
		stream->mux_thread_joinable = false;
	}

	struct encoder_packet *packet;
	size_t i;

	pthread_mutex_lock(&stream->write_mutex);
	for (i = 0; i < stream->mux_packets.num; i++) {
		packet = &stream->mux_packets.array[i];
		obs_encoder_packet_release(packet);
	}
	da_free(stream->mux_packets);
	pthread_mutex_unlock(&stream->write_mutex);

	return ret;
}

void ffmpeg_hls_mux_destroy(void *data)
{
	struct ffmpeg_muxer *stream = data;

	if (stream) {
		if (stream->mux_thread_joinable)
			pthread_join(stream->mux_thread, NULL);

		hls_deactivate(stream, 0);

		pthread_mutex_destroy(&stream->write_mutex);
		os_sem_destroy(stream->write_sem);
		os_event_destroy(stream->stop_event);

		os_process_pipe_destroy(stream->pipe);
		dstr_free(&stream->path);
		dstr_free(&stream->printable_path);
		dstr_free(&stream->stream_key);
		dstr_free(&stream->muxer_settings);
		bfree(data);
	}
}

void *ffmpeg_hls_mux_create(obs_data_t *settings, obs_output_t *output)
{
	struct ffmpeg_muxer *stream = bzalloc(sizeof(*stream));
	pthread_mutex_init_value(&stream->write_mutex);
	stream->output = output;

	/* init mutex, semaphore and event */
	if (pthread_mutex_init(&stream->write_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&stream->stop_event, OS_EVENT_TYPE_AUTO) != 0)
		goto fail;
	if (os_sem_init(&stream->write_sem, 0) != 0)
		goto fail;

	if (obs_output_get_flags(output) & OBS_OUTPUT_SERVICE)
		stream->is_network = true;

	UNUSED_PARAMETER(settings);
	return stream;

fail:
	pthread_mutex_destroy(&stream->write_mutex);
	os_sem_destroy(stream->write_sem);
	os_event_destroy(stream->stop_event);
	bfree(stream);
	return NULL;
}

static int process_packet(struct ffmpeg_muxer *stream)
{
	struct encoder_packet *packet;
	int remaining;

	pthread_mutex_lock(&stream->write_mutex);
	remaining = stream->mux_packets.num;
	if (remaining) {
		packet = &stream->mux_packets.array[0];
		write_packet(stream, packet);
		obs_encoder_packet_release(packet);
		da_erase(stream->mux_packets, 0);
	}
	pthread_mutex_unlock(&stream->write_mutex);

	return 0;
}

static void *write_thread(void *data)
{
	struct ffmpeg_muxer *stream = data;
	stream->active = true;

	while (os_sem_wait(stream->write_sem) == 0) {

		if (os_event_try(stream->stop_event) == 0)
			break;

		int ret = process_packet(stream);
		if (ret != 0) {
			int code = OBS_OUTPUT_ERROR;

			if (ret == -ENOSPC)
				code = OBS_OUTPUT_NO_SPACE;

			obs_output_signal_stop(stream->output, code);
			hls_deactivate(stream, 0);
			break;
		}
	}

	stream->active = false;
	return NULL;
}

bool ffmpeg_hls_mux_start(void *data)
{
	struct ffmpeg_muxer *stream = data;
	obs_service_t *service;
	const char *path_str;
	const char *stream_key;
	struct dstr path = {0};
	obs_encoder_t *vencoder;
	obs_data_t *settings;
	int keyint_sec;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	service = obs_output_get_service(stream->output);
	if (!service)
		return false;
	path_str = obs_service_get_url(service);
	stream_key = obs_service_get_key(service);
	dstr_copy(&stream->stream_key, stream_key);
	dstr_copy(&path, path_str);
	dstr_replace(&path, "{stream_key}", stream_key);
	dstr_init(&stream->muxer_settings);
	dstr_catf(&stream->muxer_settings,
		  "http_user_agent=libobs/%s method=PUT http_persistent=1",
		  OBS_VERSION);
	dstr_catf(&stream->muxer_settings, " ignore_io_errors=1");

	vencoder = obs_output_get_video_encoder(stream->output);
	settings = obs_encoder_get_settings(vencoder);
	keyint_sec = obs_data_get_int(settings, "keyint_sec");
	if (keyint_sec)
		dstr_catf(&stream->muxer_settings, " hls_time=%d", keyint_sec);

	obs_data_release(settings);

	start_pipe(stream, path.array);
	dstr_free(&path);

	if (!stream->pipe) {
		obs_output_set_last_error(
			stream->output, obs_module_text("HelperProcessFailed"));
		warn("Failed to create process pipe");
		return false;
	}

	/* write headers and start capture */
	os_atomic_set_bool(&stream->active, true);
	os_atomic_set_bool(&stream->capturing, true);
	os_atomic_set_bool(&stream->threading_buffer, true);
	stream->total_bytes = 0;
	stream->dropped_frames = 0;
	stream->min_priority = 0;

	obs_output_begin_data_capture(stream->output, 0);

	dstr_copy(&stream->printable_path, path_str);
	info("Writing to path '%s'...", stream->printable_path.array);

	stream->mux_thread_joinable = pthread_create(&stream->mux_thread, NULL,
						     write_thread, stream) == 0;
	return (stream->mux_thread_joinable);
}

static bool write_packet_to_array(struct ffmpeg_muxer *stream,
				  struct encoder_packet *packet)
{
	da_push_back(stream->mux_packets, packet);
	return true;
}

static void drop_frames(struct ffmpeg_muxer *stream, int highest_priority)
{
	int num_frames_dropped = 0;
	int i = 0;
	while (i < stream->mux_packets.num) {
		struct encoder_packet *packet;
		packet = &stream->mux_packets.array[i];

		/* do not drop audio data or video keyframes */
		if (!(packet->type == OBS_ENCODER_AUDIO ||
		      packet->drop_priority >= highest_priority)) {
			num_frames_dropped++;
			obs_encoder_packet_release(packet);
			da_erase_item(stream->mux_packets, packet);
			stream->dropped_frames++;
			i--;
		}
		i++;
	}

	if (stream->min_priority < highest_priority) {
		stream->min_priority = highest_priority;
	}
	if (!num_frames_dropped)
		return;

	stream->dropped_frames += num_frames_dropped;
}

static bool find_first_video_packet(struct ffmpeg_muxer *stream,
				    struct encoder_packet *first)
{
	for (size_t i = 0; i < stream->mux_packets.num; i++) {
		struct encoder_packet *cur = &stream->mux_packets.array[i];
		if (cur->type == OBS_ENCODER_VIDEO && !cur->keyframe) {
			*first = *cur;
			return true;
		}
	}
	return false;
}

void check_to_drop_frames(struct ffmpeg_muxer *stream, bool pframes)
{
	struct encoder_packet first;
	int64_t buffer_duration_usec;
	int priority = pframes ? OBS_NAL_PRIORITY_HIGHEST
			       : OBS_NAL_PRIORITY_HIGH;
	int64_t drop_threshold = 2 * stream->keyframes;

	if (!find_first_video_packet(stream, &first))
		return;

	buffer_duration_usec = stream->last_dts_usec - first.dts_usec;

	if (buffer_duration_usec > drop_threshold)
		drop_frames(stream, priority);
}

static bool add_video_packet(struct ffmpeg_muxer *stream,
			     struct encoder_packet *packet)
{
	check_to_drop_frames(stream, false);
	check_to_drop_frames(stream, true);

	/* if currently dropping frames, drop packets until it reaches the
	 * desired priority */
	if (packet->drop_priority < stream->min_priority) {
		stream->dropped_frames++;
		return false;
	} else {
		stream->min_priority = 0;
	}

	stream->last_dts_usec = packet->dts_usec;
	return write_packet_to_array(stream, packet);
}

void ffmpeg_hls_mux_data(void *data, struct encoder_packet *packet)
{
	struct ffmpeg_muxer *stream = data;
	struct encoder_packet new_packet;
	struct encoder_packet tmp_packet;
	bool added_packet = false;

	if (!active(stream)) {
		return;
	}

	/* encoder failure */
	if (!packet) {
		hls_deactivate(stream, OBS_OUTPUT_ENCODE_ERROR);
		return;
	}

	if (!stream->sent_headers) {
		if (!send_headers(stream))
			return;
		stream->sent_headers = true;
	}

	if (stopping(stream)) {
		if (packet->sys_dts_usec >= stream->stop_ts) {
			hls_deactivate(stream, 0);
			return;
		}
	}

	if (packet->type == OBS_ENCODER_VIDEO) {
		obs_parse_avc_packet(&tmp_packet, packet);
		packet->drop_priority = tmp_packet.priority;
		obs_encoder_packet_release(&tmp_packet);
	}
	obs_encoder_packet_ref(&new_packet, packet);

	pthread_mutex_lock(&stream->write_mutex);

	if (stream->active) {
		added_packet =
			(packet->type == OBS_ENCODER_VIDEO)
				? add_video_packet(stream, &new_packet)
				: write_packet_to_array(stream, &new_packet);
	}

	pthread_mutex_unlock(&stream->write_mutex);

	if (added_packet)
		os_sem_post(stream->write_sem);
	else
		obs_encoder_packet_release(&new_packet);
}