/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * audio-stretch.c — Pitch-preserving tempo adjustment via FFmpeg atempo
 */

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#include "../include/irl-source.h"

static void stretch_meta_reset(struct irl_source *ctx)
{
	ctx->stretch_meta_head = 0;
	ctx->stretch_meta_tail = 0;
	ctx->stretch_meta_count = 0;
	ctx->stretch_next_pts_ns = 0;
	ctx->stretch_next_pts_valid = false;
}

static int stretch_max_frames(const struct irl_source *ctx)
{
	if (ctx->stretch_sample_rate <= 0 || ctx->audio_buf.max_ms <= 0)
		return 0;

	int64_t frames = (int64_t)ctx->stretch_sample_rate *
			 (int64_t)ctx->audio_buf.max_ms / 1000LL;
	if (frames <= 0)
		frames = ctx->stretch_sample_rate;
	if (frames > INT_MAX)
		frames = INT_MAX;
	return (int)frames;
}

static void stretch_meta_push(struct irl_source *ctx, int64_t pts_ns,
			      uint64_t duration_ns, int out_frames)
{
	if (ctx->stretch_meta_count >= IRL_STRETCH_META_MAX) {
		ctx->stretch_meta_tail =
			(ctx->stretch_meta_tail + 1) % IRL_STRETCH_META_MAX;
		ctx->stretch_meta_count--;
	}

	ctx->stretch_meta[ctx->stretch_meta_head].pts_ns = pts_ns;
	ctx->stretch_meta[ctx->stretch_meta_head].duration_ns = duration_ns;
	ctx->stretch_meta[ctx->stretch_meta_head].out_frames = out_frames;
	ctx->stretch_meta[ctx->stretch_meta_head].consumed_frames = 0;
	ctx->stretch_meta_head =
		(ctx->stretch_meta_head + 1) % IRL_STRETCH_META_MAX;
	ctx->stretch_meta_count++;
}

static bool stretch_meta_pop(struct irl_source *ctx, int64_t *pts_ns,
			     uint64_t *duration_ns, int out_frames)
{
	if (ctx->stretch_meta_count <= 0)
		return false;

	struct {
		int index;
		int take;
	} slices[IRL_STRETCH_META_MAX];
	int slice_count = 0;
	int remaining = out_frames;
	uint64_t total_duration = 0;
	bool start_set = false;
	int idx = ctx->stretch_meta_tail;
	int count = ctx->stretch_meta_count;

	while (remaining > 0 && count > 0) {
		struct irl_stretch_meta_entry *m = &ctx->stretch_meta[idx];
		int available = m->out_frames - m->consumed_frames;
		if (available <= 0) {
			idx = (idx + 1) % IRL_STRETCH_META_MAX;
			count--;
			continue;
		}

		int take = remaining < available ? remaining : available;
		if (!start_set) {
			if (pts_ns) {
				*pts_ns =
					m->pts_ns +
					(int64_t)((m->duration_ns *
						   (uint64_t)m->consumed_frames) /
						  (uint64_t)m->out_frames);
			}
			start_set = true;
		}

		total_duration +=
			(m->duration_ns * (uint64_t)take) /
			(uint64_t)m->out_frames;
		slices[slice_count].index = idx;
		slices[slice_count].take = take;
		slice_count++;
		remaining -= take;
		idx = (idx + 1) % IRL_STRETCH_META_MAX;
		count--;
	}

	if (remaining > 0)
		return false;
	if (duration_ns)
		*duration_ns = total_duration;

	for (int i = 0; i < slice_count; i++) {
		struct irl_stretch_meta_entry *m =
			&ctx->stretch_meta[slices[i].index];
		m->consumed_frames += slices[i].take;
	}

	while (ctx->stretch_meta_count > 0) {
		struct irl_stretch_meta_entry *m =
			&ctx->stretch_meta[ctx->stretch_meta_tail];
		if (m->consumed_frames < m->out_frames)
			break;
		ctx->stretch_meta_tail =
			(ctx->stretch_meta_tail + 1) % IRL_STRETCH_META_MAX;
		ctx->stretch_meta_count--;
	}
	return true;
}

static bool stretch_trim_fifo(struct irl_source *ctx)
{
	int max_frames = stretch_max_frames(ctx);
	if (!ctx->stretch_fifo || max_frames <= 0)
		return true;

	int queued = av_audio_fifo_size(ctx->stretch_fifo);
	if (queued <= max_frames)
		return true;

	int trim_frames = queued - max_frames;
	int chunk_frames = trim_frames;
	if (chunk_frames > 4096)
		chunk_frames = 4096;

	float *discard = malloc((size_t)chunk_frames *
				(size_t)ctx->stretch_channels *
				sizeof(float));
	if (!discard)
		return false;

	while (trim_frames > 0) {
		int take = trim_frames < chunk_frames ? trim_frames : chunk_frames;
		void *data[1] = {discard};

		if (!stretch_meta_pop(ctx, NULL, NULL, take) ||
		    av_audio_fifo_read(ctx->stretch_fifo, data, take) != take) {
			free(discard);
			return false;
		}
		trim_frames -= take;
	}

	free(discard);
	return true;
}

static bool stretch_set_speed(struct irl_source *ctx, float speed)
{
	if (!ctx->stretch_graph || !ctx->stretch_tempo_ctx)
		return false;
	if (fabsf(speed - ctx->stretch_speed) < 0.0005f)
		return true;

	char arg[32];
	snprintf(arg, sizeof(arg), "%.5f", (double)speed);
	int ret = avfilter_graph_send_command(ctx->stretch_graph, "tempo",
					      "tempo", arg, NULL, 0, 0);
	if (ret < 0) {
		blog(LOG_WARNING,
		     "[irl-source] Failed to update atempo speed to %.3f",
		     (double)speed);
		return false;
	}

	ctx->stretch_speed = speed;
	return true;
}

void irl_stretch_reset(struct irl_source *ctx)
{
	if (ctx->stretch_fifo) {
		av_audio_fifo_free(ctx->stretch_fifo);
		ctx->stretch_fifo = NULL;
	}
	if (ctx->stretch_graph)
		avfilter_graph_free(&ctx->stretch_graph);

	ctx->stretch_src_ctx = NULL;
	ctx->stretch_tempo_ctx = NULL;
	ctx->stretch_sink_ctx = NULL;
	if (ctx->stretch_layout.nb_channels > 0)
		av_channel_layout_uninit(&ctx->stretch_layout);
	ctx->stretch_sample_rate = 0;
	ctx->stretch_channels = 0;
	ctx->stretch_speed = 1.0f;
	stretch_meta_reset(ctx);
}

bool irl_stretch_configure(struct irl_source *ctx, int sample_rate,
			   int channels)
{
	if (ctx->stretch_graph && ctx->stretch_sample_rate == sample_rate &&
	    ctx->stretch_channels == channels)
		return true;

	irl_stretch_reset(ctx);

	ctx->stretch_graph = avfilter_graph_alloc();
	if (!ctx->stretch_graph)
		return false;

	const AVFilter *abuffer = avfilter_get_by_name("abuffer");
	const AVFilter *atempo = avfilter_get_by_name("atempo");
	const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	if (!abuffer || !atempo || !abuffersink)
		goto fail;

	av_channel_layout_default(&ctx->stretch_layout, channels);
	char layout_desc[128];
	if (av_channel_layout_describe(&ctx->stretch_layout, layout_desc,
				       sizeof(layout_desc)) < 0)
		goto fail;

	char args[256];
	snprintf(args, sizeof(args),
		 "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
		 sample_rate, sample_rate,
		 av_get_sample_fmt_name(AV_SAMPLE_FMT_FLT), layout_desc);

	if (avfilter_graph_create_filter(&ctx->stretch_src_ctx, abuffer, "src",
					 args, NULL, ctx->stretch_graph) < 0)
		goto fail;
	if (avfilter_graph_create_filter(&ctx->stretch_tempo_ctx, atempo,
					 "tempo", "tempo=1.0", NULL,
					 ctx->stretch_graph) < 0)
		goto fail;
	if (avfilter_graph_create_filter(&ctx->stretch_sink_ctx, abuffersink,
					 "sink", NULL, NULL,
					 ctx->stretch_graph) < 0)
		goto fail;

	enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE};
	int sample_rates[] = {sample_rate, -1};
	if (av_opt_set_int_list(ctx->stretch_sink_ctx, "sample_fmts",
				sample_fmts, AV_SAMPLE_FMT_NONE,
				AV_OPT_SEARCH_CHILDREN) < 0)
		goto fail;
	if (av_opt_set_int_list(ctx->stretch_sink_ctx, "sample_rates",
				sample_rates, -1,
				AV_OPT_SEARCH_CHILDREN) < 0)
		goto fail;

	if (avfilter_link(ctx->stretch_src_ctx, 0, ctx->stretch_tempo_ctx, 0) < 0)
		goto fail;
	if (avfilter_link(ctx->stretch_tempo_ctx, 0, ctx->stretch_sink_ctx, 0) <
	    0)
		goto fail;
	if (avfilter_graph_config(ctx->stretch_graph, NULL) < 0)
		goto fail;

	ctx->stretch_fifo =
		av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, channels, sample_rate);
	if (!ctx->stretch_fifo)
		goto fail;

	ctx->stretch_sample_rate = sample_rate;
	ctx->stretch_channels = channels;
	ctx->stretch_speed = 1.0f;
	stretch_meta_reset(ctx);
	return true;

fail:
	irl_stretch_reset(ctx);
	return false;
}

int irl_stretch_available_frames(struct irl_source *ctx)
{
	if (!ctx->stretch_fifo)
		return 0;
	return av_audio_fifo_size(ctx->stretch_fifo);
}

bool irl_stretch_push(struct irl_source *ctx, const float *samples, int frames,
		      float speed, int64_t pts_ns, uint64_t duration_ns)
{
	if (!irl_stretch_configure(ctx, ctx->audio_buf.sample_rate,
				   ctx->audio_buf.channels))
		return false;
	if (!stretch_set_speed(ctx, speed))
		return false;

	AVFrame *in = av_frame_alloc();
	if (!in)
		return false;

	in->nb_samples = frames;
	in->format = AV_SAMPLE_FMT_FLT;
	in->sample_rate = ctx->stretch_sample_rate;
	if (av_channel_layout_copy(&in->ch_layout, &ctx->stretch_layout) < 0)
		goto fail;
	if (av_frame_get_buffer(in, 0) < 0)
		goto fail;

	size_t bytes = (size_t)frames * ctx->stretch_channels * sizeof(float);
	memcpy(in->data[0], samples, bytes);

	if (av_buffersrc_add_frame(ctx->stretch_src_ctx, in) < 0)
		goto fail;

	if (!ctx->stretch_next_pts_valid) {
		ctx->stretch_next_pts_ns = pts_ns;
		ctx->stretch_next_pts_valid = true;
	}
	av_frame_free(&in);

	UNUSED_PARAMETER(duration_ns);

	for (;;) {
		AVFrame *out = av_frame_alloc();
		if (!out)
			return false;

		int ret = av_buffersink_get_frame(ctx->stretch_sink_ctx, out);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			av_frame_free(&out);
			break;
		}
		if (ret < 0) {
			av_frame_free(&out);
			return false;
		}

		if (av_audio_fifo_realloc(
			    ctx->stretch_fifo,
			    av_audio_fifo_size(ctx->stretch_fifo) +
				    out->nb_samples) < 0) {
			av_frame_free(&out);
			return false;
		}

		void *data[1] = {out->data[0]};
		if (av_audio_fifo_write(ctx->stretch_fifo, data, out->nb_samples) <
		    out->nb_samples) {
			av_frame_free(&out);
			return false;
		}

		uint64_t out_stream_duration_ns = 0;
		if (ctx->stretch_sample_rate > 0) {
			double obs_duration_ns =
				(double)out->nb_samples * 1000000000.0 /
				(double)ctx->stretch_sample_rate;
			double stream_duration =
				obs_duration_ns * (double)speed;
			if (stream_duration < 0.0)
				stream_duration = 0.0;
			out_stream_duration_ns =
				(uint64_t)(stream_duration + 0.5);
		}
		stretch_meta_push(ctx, ctx->stretch_next_pts_ns,
				  out_stream_duration_ns, out->nb_samples);
		ctx->stretch_next_pts_ns +=
			(int64_t)out_stream_duration_ns;

		av_frame_free(&out);
	}

	if (!stretch_trim_fifo(ctx))
		return false;

	return true;

fail:
	av_frame_free(&in);
	return false;
}

bool irl_stretch_pop(struct irl_source *ctx, float *out, int out_frames,
		     int64_t *pts_ns, uint64_t *duration_ns)
{
	if (!ctx->stretch_fifo || av_audio_fifo_size(ctx->stretch_fifo) < out_frames)
		return false;
	if (!stretch_meta_pop(ctx, pts_ns, duration_ns, out_frames))
		return false;

	void *data[1] = {out};
	return av_audio_fifo_read(ctx->stretch_fifo, data, out_frames) == out_frames;
}
