/*
 * obs-irl-source — IRL streaming source plugin for OBS
 * https://irlserver.com
 *
 * Copyright (C) 2026 Thomas Lekanger
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * settings.c — OBS properties UI for the IRL source
 */

#include "../include/irl-source.h"

/* ── Defaults ─────────────────────────────────────────────── */

void irl_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "url", "");
	obs_data_set_default_int(settings, "reconnect_delay",
				 IRL_DEFAULT_RECONNECT_DELAY);
	obs_data_set_default_int(settings, "network_buffer_mb",
				 IRL_DEFAULT_NETWORK_BUFFER_MB);

	obs_data_set_default_int(settings, "buffer_target_ms",
				 IRL_DEFAULT_BUFFER_TARGET_MS);
	obs_data_set_default_int(settings, "buffer_min_ms",
				 IRL_DEFAULT_BUFFER_MIN_MS);
	obs_data_set_default_int(settings, "buffer_max_ms",
				 IRL_DEFAULT_BUFFER_MAX_MS);
	obs_data_set_default_bool(settings, "adaptive_speed",
				  IRL_DEFAULT_ADAPTIVE_SPEED);
	obs_data_set_default_double(settings, "speed_min",
				    IRL_DEFAULT_SPEED_MIN);
	obs_data_set_default_double(settings, "speed_max",
				    IRL_DEFAULT_SPEED_MAX);

	obs_data_set_default_int(settings, "small_gap_ms",
				 IRL_DEFAULT_SMALL_GAP_MS);
	obs_data_set_default_int(settings, "large_gap_ms",
				 IRL_DEFAULT_LARGE_GAP_MS);

	obs_data_set_default_string(settings, "ffmpeg_options", "");
	obs_data_set_default_int(settings, "hw_decode", IRL_DEFAULT_HW_DECODE);
	obs_data_set_default_bool(settings, "wait_for_keyframe",
				  IRL_DEFAULT_WAIT_KEYFRAME);
}

/* ── Properties ───────────────────────────────────────────── */

obs_properties_t *irl_source_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	/* ── General ───────────────────────────────────────── */

	obs_properties_add_text(props, "url",
				obs_module_text("URL"),
				OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "reconnect_delay",
			       obs_module_text("Reconnect Delay (s)"), 1, 60,
			       1);
	obs_properties_add_int(props, "network_buffer_mb",
			       obs_module_text("Network Buffer (MB)"), 0, 16,
			       1);

	/* ── Audio Buffer ──────────────────────────────────── */

	obs_properties_add_int(props, "buffer_target_ms",
			       obs_module_text("Target Buffer (ms)"), 20, 500,
			       10);
	obs_properties_add_int(props, "buffer_min_ms",
			       obs_module_text("Min Buffer (ms)"), 10, 200, 5);
	obs_properties_add_int(props, "buffer_max_ms",
			       obs_module_text("Max Buffer (ms)"), 50, 1000,
			       10);
	obs_properties_add_bool(props, "adaptive_speed",
				obs_module_text("Adaptive Speed"));
	obs_properties_add_float_slider(props, "speed_min",
					obs_module_text("Speed Min"), 0.90,
					1.0, 0.01);
	obs_properties_add_float_slider(props, "speed_max",
					obs_module_text("Speed Max"), 1.0,
					1.10, 0.01);

	/* ── PTS Repair ────────────────────────────────────── */

	obs_properties_add_int(props, "small_gap_ms",
			       obs_module_text("Small Gap Threshold (ms)"), 10,
			       500, 5);
	obs_properties_add_int(props, "large_gap_ms",
			       obs_module_text("Large Gap Threshold (ms)"), 500,
			       10000, 100);

	/* ── Advanced ──────────────────────────────────────── */

	obs_properties_add_text(props, "ffmpeg_options",
				obs_module_text("FFmpeg Options"),
				OBS_TEXT_DEFAULT);

	obs_property_t *hw = obs_properties_add_list(
		props, "hw_decode", obs_module_text("Hardware Decode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(hw, obs_module_text("Auto"), 0);
	obs_property_list_add_int(hw, obs_module_text("Off"), 1);

	obs_properties_add_bool(props, "wait_for_keyframe",
				obs_module_text("Wait for Keyframe"));

	/* ── About ─────────────────────────────────────────── */

	obs_properties_add_text(
		props, "about_info",
		obs_module_text(
			"IRL Source v" OBS_IRL_SOURCE_VERSION
			" by Thomas Lekanger\n"
			"https://irlserver.com\n\n"
			"Codec/protocol-agnostic live source with audio jitter "
			"buffering, PTS discontinuity repair, adaptive playback "
			"speed, and first-keyframe gating.\n\n"
			"Licensed under AGPL-3.0-or-later"),
		OBS_TEXT_INFO);

	return props;
}
