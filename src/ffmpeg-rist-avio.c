/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "ffmpeg-rist-avio.h"
#include "adaptive-rist.h"
#include "network-controller.h"
#include "receiver-internal.h"
#include "rist-transport.h"

#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#define IRL_RIST_AVIO_BUFFER_SIZE (64 * 1024)
#define IRL_RIST_READ_POLL_MS 100
#define IRL_RIST_DEFAULT_PROFILE 1 /* RIST_PROFILE_MAIN */

struct irl_direct_rist_io {
    struct irl_source *src;
    struct irl_rist_transport *transport;
    struct irl_adaptive_rist *adaptive;
    AVIOContext *avio;
    uint64_t last_sample_timestamp_ms;
};

static int rist_avio_read(void *opaque, uint8_t *buf, int buf_size)
{
    struct irl_direct_rist_io *io = opaque;
    if (!io || !io->src || !io->transport)
        return AVERROR(EIO);

    while (os_atomic_load_bool(&io->src->thread_active)) {
        /* The stock FFmpeg URL path uses interrupt_cb + io_start_us. With
         * custom AVIO this function is the blocking network boundary, so keep
         * exactly the same dead-uplink deadline. */
        if (io->src->io_start_us != 0 &&
            (uint64_t)av_gettime() - io->src->io_start_us >
                IRL_IO_STALL_TIMEOUT_US)
            return AVERROR(ETIMEDOUT);

        int ret = irl_rist_transport_read(io->transport, buf, buf_size,
                                          IRL_RIST_READ_POLL_MS);
        if (ret > 0)
            return ret;
        if (ret < 0)
            return AVERROR(EIO);
    }
    return AVERROR_EXIT;
}

int irl_open_direct_rist_input(struct irl_source *src,
                               struct irl_direct_rist_io **io_out,
                               int stats_interval_ms,
                               AVDictionary **demuxer_options)
{
    if (!src || !io_out)
        return AVERROR(EINVAL);

    *io_out = NULL;
    struct irl_direct_rist_io *io = av_mallocz(sizeof(*io));
    if (!io)
        return AVERROR(ENOMEM);
    io->src = src;

    irl_rist_transport_config_t cfg = {
        .url = src->config.url,
        .profile = IRL_RIST_DEFAULT_PROFILE,
        .stats_interval_ms = stats_interval_ms,
        .fifo_packets = 4096,
        .adaptive_recovery = 1,
        .recovery_min_ms = 250,
        .recovery_max_ms = 1800,
        .rtt_min_ms = 20,
        .rtt_max_ms = 1200,
        .reorder_buffer_ms = 30,
        .cbr_output = -1,
    };
    if (irl_rist_transport_open(&io->transport, &cfg) != 0) {
        av_free(io);
        return AVERROR(EIO);
    }

    if (irl_adaptive_rist_create(&io->adaptive, io->transport) != 0) {
        irl_rist_transport_close(&io->transport);
        av_free(io);
        return AVERROR(ENOMEM);
    }

    uint8_t *avio_buffer = av_malloc(IRL_RIST_AVIO_BUFFER_SIZE);
    if (!avio_buffer) {
        irl_close_direct_rist_io(&io);
        return AVERROR(ENOMEM);
    }

    io->avio = avio_alloc_context(avio_buffer, IRL_RIST_AVIO_BUFFER_SIZE,
                                  0, io, rist_avio_read, NULL, NULL);
    if (!io->avio) {
        av_free(avio_buffer);
        irl_close_direct_rist_io(&io);
        return AVERROR(ENOMEM);
    }

    src->fmt_ctx = avformat_alloc_context();
    if (!src->fmt_ctx) {
        irl_close_direct_rist_io(&io);
        return AVERROR(ENOMEM);
    }
    src->fmt_ctx->pb = io->avio;
    src->fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

    const AVInputFormat *mpegts = av_find_input_format("mpegts");
    if (!mpegts) {
        avformat_free_context(src->fmt_ctx);
        src->fmt_ctx = NULL;
        irl_close_direct_rist_io(&io);
        return AVERROR_DEMUXER_NOT_FOUND;
    }

    /* No URL here: bytes arrive exclusively through the custom AVIOContext. */
    int ret = avformat_open_input(&src->fmt_ctx, NULL, mpegts,
                                  demuxer_options);
    if (ret < 0) {
        src->fmt_ctx = NULL; /* may be freed by avformat_open_input */
        irl_close_direct_rist_io(&io);
        return ret;
    }

    *io_out = io;
    return 0;
}

void irl_close_direct_rist_io(struct irl_direct_rist_io **io_ptr)
{
    if (!io_ptr || !*io_ptr)
        return;

    struct irl_direct_rist_io *io = *io_ptr;
    irl_adaptive_rist_destroy(&io->adaptive);
    if (io->avio)
        avio_context_free(&io->avio);
    irl_rist_transport_close(&io->transport);
    av_free(io);
    *io_ptr = NULL;
}

struct irl_rist_transport *
irl_direct_rist_transport(struct irl_direct_rist_io *io)
{
    return io ? io->transport : NULL;
}

bool irl_direct_rist_poll(struct irl_direct_rist_io *io,
                          struct irl_adaptive_output *policy_out,
                          struct irl_rist_sample *sample_out,
                          bool *state_changed)
{
    if (state_changed)
        *state_changed = false;
    if (!io || !io->adaptive || !io->transport)
        return false;

    irl_rist_sample_t sample;
    if (!irl_rist_transport_get_controller_sample(io->transport, &sample))
        return false;
    if (sample.timestamp_ms == io->last_sample_timestamp_ms)
        return false;

    irl_adaptive_output_t policy;
    bool changed = false;
    if (!irl_adaptive_rist_update(io->adaptive, &sample, &policy, &changed))
        return false;

    io->last_sample_timestamp_ms = sample.timestamp_ms;
    if (policy_out)
        *policy_out = policy;
    if (sample_out)
        *sample_out = sample;
    if (state_changed)
        *state_changed = changed;
    return true;
}
