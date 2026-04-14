obs = obslua

function script_description()
    return "Updates a text source with IRL Source stats"
end

function script_update(settings)
end

function script_tick(seconds)
    local source = obs.obs_get_source_by_name("IRL Source (irlserver.com)")
    if not source then return end

    local ph = obs.obs_source_get_proc_handler(source)
    local cd = obs.calldata_create()
    obs.proc_handler_call(ph, "get_stats", cd)

    local buf_ms = obs.calldata_int(cd, "buffer_fill_ms")
    local speed = obs.calldata_float(cd, "current_speed")
    local reconnecting = obs.calldata_bool(cd, "reconnecting")
    local video = obs.calldata_int(cd, "total_video_frames")
    local audio = obs.calldata_int(cd, "total_audio_frames")
    local repairs = obs.calldata_int(cd, "pts_repairs")
    local delay = obs.calldata_int(cd, "stream_delay_ms")

    obs.calldata_destroy(cd)
    obs.obs_source_release(source)

    local status = reconnecting and "RECONNECTING" or "LIVE"
    local text = string.format(
        "Status: %s\nDelay: %dms\nBuffer: %dms\nCorrection: %.3fx\nFrames: %d/%d (v/a)\nPTS Repairs: %d",
        status, delay, buf_ms, speed, video, audio, repairs
    )

    local text_source = obs.obs_get_source_by_name("IRL Stats")
    if text_source then
        local settings = obs.obs_data_create()
        obs.obs_data_set_string(settings, "text", text)
        obs.obs_source_update(text_source, settings)
        obs.obs_data_release(settings)
        obs.obs_source_release(text_source)
    end
end