#include <obs-module.h>
#include <obs-avc.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/circlebuf.h>
#include <util/pipe.h>
#include <util/threading.h>

#include "obs-ffmpeg-mux.h"

int hls_stream_dropped_frames(void *data)
{
	struct ffmpeg_muxer *stream = data;
	return stream->dropped_frames;
}

static void drop_frames(struct ffmpeg_muxer *stream, int highest_priority)
{
	int num_frames_dropped = 0;
    int i = 0;
    //printf("\ndrop_frames: entered\n");
    printf("\ndrop_frames: number of frames in array: %d\n", stream->mux_packets.num);
	while (i < stream->mux_packets.num) {
        // is this safe w.r.t runtime // 
		struct encoder_packet* packet;
        packet = &stream->mux_packets.array[i];

		/* do not drop audio data or video keyframes */
		if (!(packet->type == OBS_ENCODER_AUDIO ||
		    packet->drop_priority >= highest_priority)) {
            printf("drop_frames: found frame to drop\n");
			num_frames_dropped++;
			obs_encoder_packet_release(packet);
            da_erase_item(stream->mux_packets, packet);
            stream->dropped_frames++;
            i--;
		}
        i++;
	}
    printf("drop_frames: number of frames in array after frames dropped: %d\n", stream->mux_packets.num);

	if (stream->min_priority < highest_priority)
		stream->min_priority = highest_priority;
	if (!num_frames_dropped)
		return;

    printf("drop_frames: %d frames dropped\n", num_frames_dropped);
	stream->dropped_frames += num_frames_dropped;
}

static bool find_first_video_packet(struct ffmpeg_muxer *stream,
				    struct encoder_packet *first)
{
    //printf("\nfind_first_video_packet: entered\n");
    //printf("\nfind_first_video_packet: grabbed mutex\n");
	for (size_t i = 0; i < stream->mux_packets.num; i++) {
        //printf("find_first_video_packet: one iteration of forloop\n");
		struct encoder_packet *cur =
			&stream->mux_packets.array[i];
		if (cur->type == OBS_ENCODER_VIDEO && !cur->keyframe) {
			*first = *cur;
			return true;
		}
	}
    //printf("find_first_video_packet: about to unlock mutex\n");
	return false;
}

void check_to_drop_frames(struct ffmpeg_muxer *stream, bool pframes)
{
    //printf("\ncheck_to_drop_frames: entered\n");
	struct encoder_packet first;
	int64_t buffer_duration_usec;
	int priority = pframes ? OBS_NAL_PRIORITY_HIGHEST
			       : OBS_NAL_PRIORITY_HIGH;
	int64_t drop_threshold = 2 * stream->keyframes;

    //printf("check_to_drop_frames: before find_first_video_packet\n");
	if (!find_first_video_packet(stream, &first)) {
        //printf("check_to_drop_frames: NOT find_first_video_packet\n");
		return;
    }
    //printf("check_to_drop_frames: after find_first_video_packet\n");
	/* if the amount of time stored in the buffered packets waiting to be
	 * sent is higher than threshold, drop frames */
	buffer_duration_usec = stream->last_dts_usec - first.dts_usec;

	if (buffer_duration_usec > drop_threshold) {
        printf("\n******************************************************\n");
        printf("check_to_drop_frames: threshold exceeded\n");
		drop_frames(stream, priority);
        printf("check_to_drop_frames: after drop_frames called\n");
        printf("******************************************************\n");
	}
}

static const char *ffmpeg_hls_mux_getname(void *type)
{
	UNUSED_PARAMETER(type);
	return obs_module_text("FFmpegHlsMuxer");
}

static void ffmpeg_hls_mux_destroy(void *data)
{
	printf("ffmpeg_hls_mux_destroy: entered\n");
	struct ffmpeg_muxer *stream = data;

	if (stream) {
		if (stream->mux_thread_joinable)
			pthread_join(stream->mux_thread, NULL);

		if (stream->active) {
			// we get leaks when deactivate not called but destroy is and stream
			// isnt active, ask what to do at meeting
			hls_deactivate(stream, 0);
		}

		pthread_mutex_destroy(&stream->write_mutex);
		os_sem_destroy(stream->write_sem);
		os_event_destroy(stream->stop_event);

		os_process_pipe_destroy(stream->pipe);
		dstr_free(&stream->path);
		dstr_free(&stream->muxer_settings);
		bfree(data);
	}
}
