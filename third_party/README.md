# third_party

Source files copied verbatim from other projects. They keep their own
copyright headers and licenses; nothing in here is covered by the plugin's
AGPL-3.0-or-later notice.

## obs-websocket-api.h

- Upstream: <https://github.com/obsproject/obs-websocket/blob/master/lib/obs-websocket-api.h>
- Commit: `ee283c7141158e6a3c1a6996a11caea7821bd580` (2024-04-23)
- API version: 3 (obs-websocket 5.x, which ships inside OBS Studio)
- License: GPL-2.0-or-later

The whole obs-websocket plugin API is this one header. Every function in it
is `static inline` and reaches obs-websocket through libobs' global
`proc_handler`, so there is nothing to link against and no build-time
dependency on obs-websocket being installed — `obs_websocket_register_vendor()`
simply returns `NULL` on a machine without it.

Upstream intends the header to be vendored (it lives in a `lib/` directory
for exactly that), so it is copied rather than submoduled. To update it,
replace the file and refresh the commit hash above. Check
`OBS_WEBSOCKET_API_VERSION` when you do: a bump means the proc names or
signatures changed, so `src/websocket-vendor.c` needs a look.
