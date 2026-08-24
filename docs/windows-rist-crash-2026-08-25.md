# Windows direct-RIST startup crash — 2026-08-25

## Incident

- OBS 32.2.1 x64 terminated with access violation `c0000005` on the first
  direct-RIST source open.
- The installed artifact came from GitHub Actions run `32764501489`, commit
  `5bbb42a5de85658ea5d22e4e16ab0ee07f893f63`.
- The installer archive SHA-256 was
  `cdb16625d81949951cb23eb6546138d6a827b4969c7f6a0ba114e2f7416c3eda`.
  Its matching raw artifact supplied the exact DLL and PDB used below.

## Symbolized path

The crash report loaded `obs-irl-source.dll` at `0x7ff84ac10000`. Resolving
the plugin RVAs with the matching PDB produced:

```text
pthread_mutex_lock
rist_log_priv3
init_common_ctx
rist_receiver_create
irl_rist_transport_open          src/rist-transport.c
irl_open_direct_rist_input       src/ffmpeg-rist-avio.c
open_stream_attempt              src/receiver-stream.c
irl_open_stream
irl_receiver_thread              src/receiver.c
irl_thread_trampoline
```

## Root cause

The bundled libRIST 0.2.20 native-Windows build uses its internal pthread shim
(`HAVE_PTHREADS=0`), where `pthread_mutex_t` is a Win32 `CRITICAL_SECTION`.
The global logging lock is zero-initialized in that configuration.

`rist_log_priv3()` locks the global logging mutex directly, while the public
logging APIs are what call libRIST's `init_once_global()` and initialize that
mutex. `rist_receiver_create()` calls `init_common_ctx()`, which logs before it
stores the caller's logging settings. The former `NULL` logging argument
therefore allowed `EnterCriticalSection()` to receive an uninitialized lock.

## Decision

`irl_rist_transport_open()` now calls `rist_logging_set()` with a disabled
per-transport logging settings object before any other libRIST API. That public
call initializes libRIST's global mutex. The same settings pointer is passed to
`rist_receiver_create()` and remains alive until after `rist_destroy()`.

This is a narrow compatibility workaround using public libRIST APIs. Patching
the vendored libRIST source would be broader and would require dependency-patch
and cache plumbing. A future libRIST version that initializes the lock inside
`rist_log_priv3()` makes this workaround redundant but still safe.

## Verification boundary

- Confirmed: installer provenance, exact DLL/PDB match, full symbolized stack,
  libRIST 0.2.20 source and configuration, source-level initialization and
  cleanup order, patcher idempotence, and updated knowledge-graph artifact.
- Regression harness: the transport stub refuses receiver creation unless
  logging was initialized first, injects a logging-bootstrap failure, and
  checks that logging is freed after the receiver context. The modified C
  harness was not recompiled locally because this workstation has no C
  compiler.
- Pending: compile the patched tree in Windows GitHub Actions, install that new
  artifact, and open/close a real RIST source in OBS. Static and symbol evidence
  do not substitute for that runtime acceptance test.
