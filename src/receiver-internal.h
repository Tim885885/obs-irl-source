#pragma once

#include "../include/irl-source.h"

uint64_t irl_next_audio_timestamp(struct irl_source *ctx, int base_samples,
				  int out_rate);
void irl_reset_stream_timing_state(struct irl_source *ctx);
void irl_reset_audio_timing_state(struct irl_source *ctx);
bool irl_pump_audio_once(struct irl_source *ctx);
void irl_handle_audio_frame(struct irl_source *ctx, AVFrame *frame);
void irl_handle_video_frame(struct irl_source *ctx, AVFrame *frame);
