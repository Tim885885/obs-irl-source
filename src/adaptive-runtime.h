/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_ADAPTIVE_RUNTIME_H
#define IRL_ADAPTIVE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "network-controller.h"

struct irl_source;

/* Apply a transport-derived playout target without ever going below the user's
 * configured Target Buffer. Returns true if the effective buffer changed. */
bool irl_adaptive_runtime_apply(struct irl_source *ctx,
                                const irl_adaptive_output_t *policy,
                                const irl_rist_sample_t *sample);

/* Reclaim controller-added cushion before a reconnect/new RIST session. */
void irl_adaptive_runtime_restore_user_target(struct irl_source *ctx);

/* Called when OBS hot-applies a new user Target Buffer. */
void irl_adaptive_runtime_set_user_target(struct irl_source *ctx,
                                          long target_ms);

#endif
