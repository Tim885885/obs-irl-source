/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "adaptive-rist.h"

#include "rist-transport.h"

#include <stdlib.h>
#include <string.h>

struct irl_adaptive_rist {
    struct irl_rist_transport *transport; /* non-owning */
    irl_network_controller_t controller;
    irl_adaptive_output_t output;
    uint64_t last_sample_timestamp_ms;
};

int irl_adaptive_rist_create(struct irl_adaptive_rist **out,
                             struct irl_rist_transport *transport)
{
    if (!out || !transport)
        return -1;

    *out = NULL;
    struct irl_adaptive_rist *adaptive = calloc(1, sizeof(*adaptive));
    if (!adaptive)
        return -1;

    adaptive->transport = transport;
    irl_network_controller_init(&adaptive->controller);
    *out = adaptive;
    return 0;
}

void irl_adaptive_rist_destroy(struct irl_adaptive_rist **adaptive_ptr)
{
    if (!adaptive_ptr || !*adaptive_ptr)
        return;
    free(*adaptive_ptr);
    *adaptive_ptr = NULL;
}

bool irl_adaptive_rist_update(struct irl_adaptive_rist *adaptive,
                              const irl_rist_sample_t *sample,
                              irl_adaptive_output_t *out,
                              bool *state_changed)
{
    if (state_changed)
        *state_changed = false;
    if (!adaptive || !sample)
        return false;
    if (sample->timestamp_ms == adaptive->last_sample_timestamp_ms)
        return false;

    const irl_net_state_t old_state = adaptive->output.state;
    const uint32_t old_target = adaptive->output.playout_target_ms;

    irl_adaptive_output_t next;
    if (!irl_network_controller_update(&adaptive->controller, sample, &next))
        return false;

    adaptive->output = next;
    adaptive->last_sample_timestamp_ms = sample->timestamp_ms;

    if (state_changed &&
        (old_state != next.state || old_target != next.playout_target_ms))
        *state_changed = true;
    if (out)
        *out = next;
    return true;
}

bool irl_adaptive_rist_poll(struct irl_adaptive_rist *adaptive,
                            irl_adaptive_output_t *out,
                            bool *state_changed)
{
    if (!adaptive || !adaptive->transport)
        return false;

    irl_rist_sample_t sample;
    if (!irl_rist_transport_get_controller_sample(adaptive->transport, &sample))
        return false;
    return irl_adaptive_rist_update(adaptive, &sample, out, state_changed);
}
