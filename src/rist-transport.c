/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "rist-transport.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <stdatomic.h>
#endif

#include <librist/librist.h>

struct irl_rist_transport {
    struct rist_ctx *ctx;
    struct rist_peer *peer;
    struct rist_data_block *current_block;
    size_t current_offset;

#ifdef _WIN32
    volatile LONG stats_seq;
    volatile LONG stats_valid;
#else
    atomic_uint stats_seq;
    atomic_int stats_valid;
#endif
    irl_rist_transport_stats_t latest_stats;
};

/* MSVC still does not provide C11 <stdatomic.h> in ordinary /std:c11 mode.
 * Keep the lock-free seqlock on Windows with Interlocked operations, whose
 * operations are full memory barriers. POSIX builds keep using C11 atomics. */
static void stats_atomic_init(struct irl_rist_transport *t)
{
#ifdef _WIN32
    InterlockedExchange(&t->stats_seq, 0);
    InterlockedExchange(&t->stats_valid, 0);
#else
    atomic_init(&t->stats_seq, 0);
    atomic_init(&t->stats_valid, 0);
#endif
}

static void stats_seq_bump(struct irl_rist_transport *t)
{
#ifdef _WIN32
    (void)InterlockedIncrement(&t->stats_seq);
#else
    (void)atomic_fetch_add_explicit(&t->stats_seq, 1, memory_order_acq_rel);
#endif
}

static unsigned stats_seq_load(const struct irl_rist_transport *t)
{
#ifdef _WIN32
    return (unsigned)InterlockedCompareExchange(
        (volatile LONG *)&t->stats_seq, 0, 0);
#else
    return atomic_load_explicit(&t->stats_seq, memory_order_acquire);
#endif
}

static void stats_valid_set(struct irl_rist_transport *t)
{
#ifdef _WIN32
    InterlockedExchange(&t->stats_valid, 1);
#else
    atomic_store_explicit(&t->stats_valid, 1, memory_order_release);
#endif
}

static int stats_valid_load(const struct irl_rist_transport *t)
{
#ifdef _WIN32
    return InterlockedCompareExchange(
        (volatile LONG *)&t->stats_valid, 0, 0) != 0;
#else
    return atomic_load_explicit(&t->stats_valid, memory_order_acquire) != 0;
#endif
}

static uint64_t monotonic_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ULL +
               (uint64_t)ts.tv_nsec / 1000000ULL;
    return (uint64_t)time(NULL) * 1000ULL;
#endif
}

static int url_query_has_key(const char *url, const char *key)
{
    if (!url || !key || !*key)
        return 0;
    const char *p = strchr(url, '?');
    if (!p)
        return 0;
    p++;
    const size_t key_len = strlen(key);
    while (*p) {
        while (*p == '&' || *p == ';')
            p++;
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=')
            return 1;
        p = strpbrk(p, "&;");
        if (!p)
            break;
    }
    return 0;
}

static int stats_cb(void *arg, const struct rist_stats *container)
{
    struct irl_rist_transport *t = arg;
    if (!t || !container)
        return 0;

    if (container->stats_type == RIST_STATS_RECEIVER_FLOW) {
        const struct rist_stats_receiver_flow *flow =
            &container->stats.receiver_flow;

        /* libRIST reports stats_instant values and zeroes them after callback.
         * Preserve this interval as-is; do not derive deltas later. */
        stats_seq_bump(t);
        t->latest_stats.timestamp_ms = monotonic_ms();
        t->latest_stats.rtt_ms = flow->rtt;
        t->latest_stats.received = flow->received;
        t->latest_stats.missing = flow->missing;
        t->latest_stats.recovered = flow->recovered;
        t->latest_stats.reordered = flow->reordered;
        t->latest_stats.lost = flow->lost;
        t->latest_stats.max_inter_packet_spacing_us =
            flow->max_inter_packet_spacing;
        t->latest_stats.bandwidth_bps = (uint64_t)flow->bandwidth;
        t->latest_stats.retry_bandwidth_bps =
            (uint64_t)flow->retry_bandwidth;
        t->latest_stats.avg_buffer_time_ms = flow->avg_buffer_time;
        t->latest_stats.quality_pct = flow->quality;
        t->latest_stats.flow_id = flow->flow_id;
        t->latest_stats.status = flow->status;
        stats_seq_bump(t);
        stats_valid_set(t);
    }

    /* The application owns the callback container after delivery. */
    rist_stats_free(container);
    return 0;
}

int irl_rist_transport_open(struct irl_rist_transport **out,
                            const irl_rist_transport_config_t *config)
{
    if (!out || !config || !config->url || !config->url[0])
        return -1;

    *out = NULL;
    struct irl_rist_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return -1;

    stats_atomic_init(t);

    /* Parse first so ?profile= can choose the receiver context profile too. */
    struct rist_peer_config *peer_config = NULL;
    if (rist_parse_address2(config->url, &peer_config) < 0 || !peer_config)
        goto fail;

    enum rist_profile profile = (enum rist_profile)config->profile;
#if defined(RIST_PEER_CONFIG_VERSION) && RIST_PEER_CONFIG_VERSION >= 4
    if (peer_config->profile_set)
        profile = peer_config->profile;
#endif

    if (rist_receiver_create(&t->ctx, profile, NULL) != 0)
        goto fail;

    if (config->fifo_packets > 0 &&
        rist_receiver_set_output_fifo_size(t->ctx, config->fifo_packets) != 0)
        goto fail;

#if defined(RIST_API_VERSION) || defined(RIST_PEER_CONFIG_VERSION)
    if (config->cbr_output >= 0) {
        if (rist_receiver_set_cbr_output(t->ctx,
                                         config->cbr_output != 0) != 0)
            goto fail;
    }
#endif

    /* Preserve explicit URL tuning exactly. Inject defaults only when the
     * corresponding RIST query keys were not supplied. */
    const int has_explicit_recovery =
        url_query_has_key(config->url, "buffer") ||
        url_query_has_key(config->url, "buffer-min") ||
        url_query_has_key(config->url, "buffer-max");
    if (config->adaptive_recovery && !has_explicit_recovery) {
        const uint32_t recovery_min = config->recovery_min_ms > 0
            ? config->recovery_min_ms : 250;
        const uint32_t recovery_max = config->recovery_max_ms > recovery_min
            ? config->recovery_max_ms : 1800;
        peer_config->recovery_length_min = recovery_min;
        peer_config->recovery_length_max = recovery_max;

        if (config->rtt_min_ms > 0 &&
            !url_query_has_key(config->url, "rtt-min"))
            peer_config->recovery_rtt_min = config->rtt_min_ms;
        if (config->rtt_max_ms > 0 &&
            !url_query_has_key(config->url, "rtt-max"))
            peer_config->recovery_rtt_max = config->rtt_max_ms;
        if (config->reorder_buffer_ms > 0 &&
            !url_query_has_key(config->url, "reorder-buffer"))
            peer_config->recovery_reorder_buffer = config->reorder_buffer_ms;
    }

    int peer_ret = rist_peer_create(t->ctx, &t->peer, peer_config);
    rist_peer_config_free2(&peer_config);
    peer_config = NULL;
    if (peer_ret != 0)
        goto fail;

    if (config->stats_interval_ms > 0 &&
        rist_stats_callback_set(t->ctx, config->stats_interval_ms,
                                stats_cb, t) != 0)
        goto fail;

    if (rist_start(t->ctx) != 0)
        goto fail;

    *out = t;
    return 0;

fail:
    if (peer_config)
        rist_peer_config_free2(&peer_config);
    irl_rist_transport_close(&t);
    return -1;
}

void irl_rist_transport_close(struct irl_rist_transport **transport)
{
    if (!transport || !*transport)
        return;

    struct irl_rist_transport *t = *transport;
    if (t->current_block)
        rist_receiver_data_block_free2(&t->current_block);
    if (t->ctx)
        rist_destroy(t->ctx);
    free(t);
    *transport = NULL;
}

int irl_rist_transport_read(struct irl_rist_transport *t,
                            uint8_t *dst, int dst_size, int timeout_ms)
{
    if (!t || !dst || dst_size <= 0)
        return -1;

    if (!t->current_block) {
        /* libRIST returns queue depth + 1, NOT bytes read. */
        int ret = rist_receiver_data_read2(t->ctx, &t->current_block,
                                           timeout_ms);
        if (ret <= 0)
            return ret;
        t->current_offset = 0;
    }

    if (!t->current_block || !t->current_block->payload)
        return -1;

    const size_t total = t->current_block->payload_len;
    if (t->current_offset >= total) {
        rist_receiver_data_block_free2(&t->current_block);
        t->current_offset = 0;
        return 0;
    }

    size_t remain = total - t->current_offset;
    size_t copy = remain < (size_t)dst_size ? remain : (size_t)dst_size;
    memcpy(dst,
           (const uint8_t *)t->current_block->payload + t->current_offset,
           copy);
    t->current_offset += copy;

    if (t->current_offset == total) {
        rist_receiver_data_block_free2(&t->current_block);
        t->current_offset = 0;
    }

    return (int)copy;
}

int irl_rist_transport_get_stats(const struct irl_rist_transport *t,
                                 irl_rist_transport_stats_t *out)
{
    if (!t || !out || !stats_valid_load(t))
        return 0;

    for (;;) {
        unsigned before = stats_seq_load(t);
        if (before & 1U)
            continue;
        *out = t->latest_stats;
        unsigned after = stats_seq_load(t);
        if (before == after && !(after & 1U))
            return 1;
    }
}

int irl_rist_transport_get_controller_sample(
    const struct irl_rist_transport *t, irl_rist_sample_t *out)
{
    irl_rist_transport_stats_t stats;
    if (!out || !irl_rist_transport_get_stats(t, &stats))
        return 0;

    memset(out, 0, sizeof(*out));
    out->timestamp_ms = stats.timestamp_ms;
    out->rtt_ms = stats.rtt_ms;
    out->received = stats.received;
    out->missing = stats.missing;
    out->recovered = stats.recovered;
    out->reordered = stats.reordered;
    out->lost = stats.lost;
    out->max_inter_packet_spacing_us = stats.max_inter_packet_spacing_us;
    out->bandwidth_bps = stats.bandwidth_bps;
    out->retry_bandwidth_bps = stats.retry_bandwidth_bps;
    out->avg_buffer_time_ms = stats.avg_buffer_time_ms;
    out->quality_pct = stats.quality_pct;
    return 1;
}

int irl_rist_transport_set_rtt_multiplier(struct irl_rist_transport *t,
                                          int multiplier)
{
    if (!t || !t->ctx || multiplier < 1)
        return -1;
#ifdef IRL_LIBRIST_RUNTIME_RTT_MULTIPLIER
    return rist_recovery_rtt_multiplier_set(t->ctx, multiplier);
#else
    (void)multiplier;
    return -2;
#endif
}
