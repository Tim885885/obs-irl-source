/*
 * obs-irl-source Adaptive RIST experiment
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef IRL_LIVE_EDGE_RUNTIME_H
#define IRL_LIVE_EDGE_RUNTIME_H

#include <stdbool.h>
#include "network-controller.h"

struct irl_source;

/* Opt-in only. IRL_ADAPTIVE_LIVE_EDGE=balanced|aggressive enables destructive
 * live-edge reclaim. Unset/invalid means OFF. */
void irl_live_edge_runtime_init(struct irl_source *ctx);
void irl_live_edge_runtime_reset_session(struct irl_source *ctx);

/* Publish the current transport policy to the video thread and, when the
 * opt-in mode allows it, trim a genuinely excessive plugin-side audio backlog.
 * Returns true only when audio content was actually reclaimed. */
bool irl_live_edge_runtime_update(struct irl_source *ctx,
                                  const irl_adaptive_output_t *policy);

#endif
