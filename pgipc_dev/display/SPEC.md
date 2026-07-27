# PiGhost Display/Reader Service - Specification (v1)

This document specifies the behavior of the **display service** (the single
long-lived "reader" side of libpgipc) so that the CPU pixels-mode
implementation can be built against a fixed contract instead of improvised
ad hoc. It complements, and must stay consistent with, the doc-comments in
`libpgipc.h`. Where this spec and the header disagree, the header (the
actual compiled contract) wins - update this doc.

Scope: this version covers the **PGIPC_PAYLOAD_PIXELS** data path in full,
and specifies the dmabuf/GPU control-plane handling needed so the reader
doesn't have to be revisited when the GPU producer arrives (the *data*-plane
KMS/DRM scanout code itself is out of scope here - see the GPU app's own
skeleton doc later).

## 1. Roles

- Exactly one **display/reader process** runs at a time on the device. It
  owns:
  - the shm frame ring (`pgipc_shm_ring_create`)
  - the frame-ready semaphore (`pgipc_shm_sem_create`)
  - the control-protocol Unix socket (`PGIPC_CONTROL_SOCK_PATH`)
  - a new **admin Unix socket** (see §6) for management-plane queries
  - the physical HDMI output (framebuffer console or DRM/KMS in later
    iterations; this spec's v1 reference implementation targets a
    plain Linux framebuffer device, `/dev/fb0`, for the CPU pixels path)
- Zero or more **writer/producer processes** connect as transient clients.
  At most one is ever the *active* writer (holds the activation grant).

## 2. Startup ordering

1. Display creates the shm ring, semaphore, control socket, and admin
   socket, in that order, before accepting any connections.
2. Display begins its accept loop on the control socket.
3. Producers retry-connect (per `pgipc_writer_connect`'s existing
   100-attempt/100ms backoff) until the socket exists.

There is no ordering requirement between admin clients and producers.

## 3. Client session table

- Fixed-size array, `MAX_CLIENTS = 8` (matches `PGIPC_MAX_MODES`-scale
  simplifications elsewhere in the header; revisit if the marketplace ever
  needs more than 8 simultaneously-installed producer apps running at
  once - not the same limit as "how many apps exist", just how many may
  hold an open control-socket connection concurrently).
- A 9th connection attempt while the table is full is accepted at the
  socket level (so the client gets a clean disconnect, not ECONNREFUSED
  noise) then immediately closed with no CONNECT response. This is a
  server resource limit, not a protocol message - a future version could
  add a dedicated deny reason for it if needed.
- Each table slot tracks:

  | field              | type                 | purpose |
  |--------------------|----------------------|---------|
  | `in_use`           | bool                 | slot occupied |
  | `fd`                | int                 | control socket fd |
  | `app_id`           | `char[PGIPC_APP_ID_LEN]` | from CONNECT |
  | `state`            | enum (§4)            | session state machine |
  | `offered_modes`    | `pgipc_render_mode_t[PGIPC_MAX_MODES]` | from CONNECT |
  | `num_offered_modes`| uint32               | from CONNECT |
  | `negotiated_mode`  | `pgipc_render_mode_t`| set once MODE accepted |
  | `granted_generation` | uint32             | copy of ring generation at grant time |
  | `last_heartbeat_monotonic` | `struct timespec` | last HEARTBEAT recv time |
  | `payload_kind`     | `pgipc_payload_kind_t` | PIXELS until ACKed DMABUF |
  | `dmabuf_set`       | `pgipc_dmabuf_set_t` | valid only if payload_kind==DMABUF |

- Slot lookup key for admin commands (§6) is `app_id`, not fd/slot index
  - `switch <app_id>` looks up by `app_id` whether the target is
  NEGOTIATED (grants it) or already ACTIVE (no-op). If `app_id` has no
  connected slot at all, the switch is rejected outright - **no queued/
  pending grants in v1**, see §6.3.

## 4. Per-client state machine

```
   CONNECTED --(CONNECT recv'd, mode accepted)--> NEGOTIATED
   CONNECTED --(CONNECT recv'd, mode rejected)--> REJECTED --(closed)
   NEGOTIATED --(ACTIVATE_REQUEST, no one active)--> ACTIVE
   NEGOTIATED --(ACTIVATE_REQUEST, someone else active)--> NEGOTIATED (DENY sent, stays negotiated)
   ACTIVE --(admin switch to someone else / heartbeat timeout / disconnect)--> NEGOTIATED (DEACTIVATE sent, if still connected)
   any --(socket EOF / error / BYE-now-DISCONNECT)--> CLOSED (slot freed)
```

- **CONNECTED**: socket accepted, no `PGIPC_MSG_CONNECT` yet.
- **NEGOTIATED**: mode accepted; not currently the active writer. May be
  retried into ACTIVE at any time via a later `ACTIVATE_REQUEST` or an
  admin `switch`.
- **REJECTED**: mode negotiation failed (offered modes don't exactly
  match any display-supported mode, see §5). Display sends
  `PGIPC_MSG_MODE{accepted=0}` then closes the connection - matches
  `pgipc_writer_connect`'s behavior of returning NULL on rejection.
- **ACTIVE**: holds the current activation grant; the only client whose
  publishes are considered "real" by data-plane consumers until evicted.
- **CLOSED**: fd closed, dmabuf set (if any) closed via
  `pgipc_dmabuf_set_close`, slot freed for reuse by a new connection.

Only one client may be ACTIVE at a time; this is enforced entirely inside
the display (never inferred from the shm ring alone, matching the header's
"single active writer" arbitration described in `pgipc_msg_type_t`).

## 5. Mode negotiation

- The display is configured (at startup, e.g. via CLI args / a small
  config struct - not over the wire) with a short list of
  **display-supported modes**, `pgipc_render_mode_t supported[N]`,
  `N` small (e.g. up to 4, matching `PGIPC_MAX_MODES`). This is a real
  list, not a single fixed mode - see the confirmed decision to support
  exact-match against a small configurable list (e.g. multiple refresh
  rates at the same fixed panel resolution).
- On `PGIPC_MSG_CONNECT`, the display walks the producer's
  `offered_modes` **in the producer's preference order**, and picks the
  **first** one that exactly matches (width, height, fps all equal) any
  entry in `supported`. First match wins; there is no scoring/distance
  metric.
- If no offered mode exactly matches any supported mode, reply
  `PGIPC_MSG_MODE{accepted=0}` and close (client → REJECTED → CLOSED).
- `PGIPC_FRAME_MAX_WIDTH` / `PGIPC_FRAME_MAX_HEIGHT` in the header bound
  the shm ring's fixed buffer allocation; every entry in `supported` MUST
  fit within those bounds (display-side static assertion / startup check,
  not a wire-protocol check - a producer never sees this constant, it's
  purely the display's own allocation ceiling).

## 6. Admin interface (management plane, v1)

A second, display-owned Unix domain socket, separate from the producer
control socket, so producer apps never need to link against or even know
about admin semantics:

```
#define PGIPC_ADMIN_SOCK_PATH "/dev/shm/frame_ring_admin.sock"
```

### 6.1 Transport & framing

- Same length-prefixed framing style as the control protocol
  (1-byte type + 4-byte BE length + payload), but a **separate, smaller
  message enum** (`pgipc_admin_msg_type_t`) - admin messages must never be
  confusable with producer control messages even though they reuse
  `pgipc_ctrl_send`/`pgipc_ctrl_recv` framing helpers over a different fd.
- Every admin connection is short-lived: connect → send one request →
  receive one response → close. No persistent admin session state, no
  heartbeats. This keeps the orchestrator's HTTP-sidecar translation
  (`GET /status` → `PGIPC_ADMIN_MSG_LIST_REQUEST`, `POST /switch` →
  `PGIPC_ADMIN_MSG_SWITCH_REQUEST`) trivial later.

### 6.2 Messages

| type | direction | payload | purpose |
|------|-----------|---------|---------|
| `PGIPC_ADMIN_MSG_LIST_REQUEST` | client→display | none | ask for session table snapshot |
| `PGIPC_ADMIN_MSG_LIST_RESPONSE` | display→client | array of `{app_id, state, negotiated_mode, payload_kind}`, up to `MAX_CLIENTS` entries | snapshot reply |
| `PGIPC_ADMIN_MSG_SWITCH_REQUEST` | client→display | `{app_id}` | request activation switch to `app_id` |
| `PGIPC_ADMIN_MSG_SWITCH_RESPONSE` | display→client | `{ok, reason}` | switch outcome |

### 6.3 Switch semantics

- If `app_id` matches a connected client in NEGOTIATED state: display
  evicts the current ACTIVE client (if any) exactly per §7, then grants
  the requested client (bump ring generation once, not twice - the same
  generation bump serves both the eviction and the new grant).
- If `app_id` matches a connected client already ACTIVE: no-op, respond
  `{ok=1}` (idempotent).
- If `app_id` does not match any connected client: respond `{ok=0,
  reason="app not connected"}`. **No queued/pending grants in v1** - this
  simplifies the session table at the cost of requiring the orchestrator
  to retry the switch after confirming the target container is up and
  has connected (poll `LIST_REQUEST` until the app_id appears in
  NEGOTIATED state, then `SWITCH_REQUEST`). Revisit if this polling proves
  too slow in practice.

### 6.4 Why a socket instead of stdin (v1 decision)

Chosen over the old toy's stdin-typed commands specifically so the future
PWA/orchestrator has a real, scriptable local API to call today instead of
another rewrite later - this is the piece explicitly called out as
`docker attach` + typing in the old README's "Path to the real management
plane" section, and we're building that real thing now instead of
deferring it again.

## 7. Activation & eviction

- **Granting**: on `PGIPC_MSG_ACTIVATE_REQUEST` from a NEGOTIATED client:
  - Regardless of whether anyone was previously ACTIVE, the display calls
    `pgipc_evict_writer(ring)` immediately before every grant (including
    the very first grant ever, off generation 0). This is a deliberate
    simplification: it costs nothing (bumping 0→1 and resetting an
    already-`-1` `latest_ready` is a no-op in the empty-slate case) and
    means the reader implementation has exactly one code path for
    "grant to X" instead of a special first-grant case - every grant the
    reader ever sends is preceded by exactly one generation bump, full
    stop.
  - if no client was previously ACTIVE: grant immediately after the bump
    above.
  - if a different client was previously ACTIVE: also evict it (§7's
    eviction steps 2/3/4 below) as part of the same generation bump, then
    grant - this is what makes admin `switch` (§6.3) a single atomic
    generation increment rather than two.
  - if a different client is ACTIVE: send `PGIPC_MSG_ACTIVATE_DENY{reason}`
    to the requester; requester stays NEGOTIATED and retries per its own
    `PGIPC_RETRY_ACTIVATE_MS` (2000ms, entirely producer-side, per
    `pgipc_writer_ctx_t` doc-comment - display does not need to schedule
    retries).
  - First-come-first-served: no priority field, no preemption by a later
    request - matches the confirmed decision.
- **Eviction** (display-initiated, via admin switch, heartbeat timeout, or
  disconnect of the active client):
  1. Call `pgipc_evict_writer(ring)` - bumps generation, resets
     `latest_ready = -1`.
  2. If the evicted client's socket is still open (i.e. this is a
     heartbeat-timeout or admin-switch eviction, not a disconnect), send
     it `PGIPC_MSG_DEACTIVATE`. A disconnect obviously has nothing to send
     to.
  3. If the evicted client was in DMABUF payload mode, do **not** call
     `pgipc_dmabuf_set_close()` yet if the display is mid-flip away from
     its buffer - see the header's explicit ordering note ("never scan
     out a buffer you are about to release"). For the CPU pixels path
     this ordering concern doesn't apply (no scanout buffer ownership),
     so pixels-mode eviction may close resources immediately after step 1.
  4. Set the evicted slot's `state = NEGOTIATED` (if still connected) or
     free the slot (if this eviction is *because of* a disconnect).
  5. Grant the new client (if any) per the Granting rules above, using the
     SAME generation bump from step 1 - i.e. eviction+grant during a
     `switch` is one generation increment, not two.
- **Heartbeat timeout**: display tracks `last_heartbeat_monotonic` per
  ACTIVE client only (NEGOTIATED clients don't heartbeat - see
  `pgipc_writer_ctx_t`'s ctrl thread, which only sends HEARTBEAT while
  `active==true`). If `now - last_heartbeat_monotonic > PGIPC_HEARTBEAT_TIMEOUT_MS`
  (2000ms, `2 * PGIPC_HEARTBEAT_INTERVAL_MS`, chosen to tolerate one dropped
  heartbeat; both constants are declared unconditionally in libpgipc.h so a
  LIBPGIPC_READER-only build can reference them), evict per above.
- **Disconnect detection**: display's poll/select loop watches every
  connected fd for `POLLHUP`/read-returns-0/read-error. On detection of
  the ACTIVE client's disconnect, evict immediately (no need to wait for
  the heartbeat timeout - matches the old toy's verified behavior of
  near-instant `SIGKILL` eviction via socket disconnect).

## 8. Where the admin protocol lives (code organization)

`libpgipc.h`'s documented scope is the **producer↔display** contract only
(see its header comment: "writer convenience API" + "control protocol").
The admin protocol in §6 is display-internal management surface that no
producer ever links against, so it does NOT belong in `libpgipc.h` - it
gets its own small header, `display/admin/admin_proto.h`, defining
`pgipc_admin_msg_type_t` and the two payload structs, reusing
`pgipc_ctrl_send`/`pgipc_ctrl_recv` from libpgipc.h for framing (those two
functions are intentionally unconditional/always-declared in libpgipc.h
regardless of `LIBPGIPC_WRITER`/`LIBPGIPC_READER`, so this reuse is free).
A future admin CLI/PWA-sidecar includes `admin_proto.h` + links libpgipc
with neither `LIBPGIPC_WRITER` nor `LIBPGIPC_READER` defined for producer
data-plane symbols it doesn't need (or just uses raw sockets directly,
since the protocol is deliberately tiny).

## 9. Data plane loop (CPU pixels mode reference implementation)

Runs on a dedicated thread (or the main thread, in the simplest first cut)
separate from the control-socket accept/event loop:

1. `sem_wait(fsem)` - blocks until a writer publishes (any writer; the
   semaphore has no notion of *which* writer posted).
2. `idx = pgipc_shm_ring_checkout(ring)`. If `idx < 0`, spurious wake
   (e.g. semaphore posted by a writer that was evicted between publish
   and this checkout) - loop back to step 1.
3. **Read-after-checkout ordering**: read `ring->frame_id[idx]` /
   `ring->write_ts[idx]` only after checkout, never before - checkout is
   what makes the read of that slot's contents race-free against a writer
   reusing it (writers never pick a `reader_locked` slot, per
   `pgipc_shm_ring_pick_write_slot`). The reader loop does not need its
   own generation check: `pgipc_writer_publish`'s own generation
   comparison already guarantees an evicted writer's frames stop landing
   in the ring, so by the time this loop's `sem_wait` wakes, any frame it
   checks out was legitimately published by whoever held the grant at
   publish time.
4. Blit `ring->frame_bufs[idx]` (exactly
   `negotiated_mode.width * negotiated_mode.height * PGIPC_BYTES_PER_PIXEL`
   bytes - the negotiated mode may be smaller than
   `PGIPC_FRAME_MAX_WIDTH`×`PGIPC_FRAME_MAX_HEIGHT`, the ring's
   allocation ceiling, so the blit range comes from the ACTIVE client's
   `negotiated_mode`, not the ring's max size) to `/dev/fb0` (or the
   active KMS front buffer in a later DRM-based version).
5. `pgipc_shm_ring_release(ring)`.
6. Update fps/latency counters (`now - ring->write_ts[idx]`) for
   diagnostics/admin LIST responses.
7. Loop to step 1.

This loop does not know or care which client is ACTIVE - the shm ring
enforces single-writer safety structurally (`latest_ready`/
`reader_locked`), and the *control*-plane's job (§7) is solely to ensure
only one producer process is ever alive-and-publishing at a time. If two
producers somehow publish concurrently (a control-plane bug), this loop
would still render blindly, which is why `pgipc_writer_publish`'s
generation check (drop-if-stale) is the real safety net - a defense the
reader-side loop is not exempt from watching in step 3.

## 10. Threading model (reference implementation)

Three logical loops, three threads (simplest correct option; a future
version could merge the two socket-accept loops into one `poll()` set if
thread count becomes a real cost - not expected on a Pi4's 4 cores when
display is pinned to its own dedicated core per the project's cpuset
plan):

1. **Control thread**: owns the producer control socket + all client fds
   in the session table. Single-threaded `poll()` loop over
   `[listen_fd, client_fd...]`. All session-table reads/writes happen
   only on this thread - no locking needed for the table itself.
2. **Admin thread**: owns the admin socket. Since each admin connection is
   connect→request→response→close (§6.1), this can be a simple
   accept-then-handle-then-loop thread. Reads session-table snapshots by
   posting a request to the control thread (e.g. a small
   internal pipe/eventfd + response future) rather than reading the table
   directly from a second thread - keeps "only the control thread touches
   the table" invariant simple and avoids adding a mutex around the whole
   table for what is a low-frequency, latency-insensitive path.
3. **Data-plane thread** (§9): touches only the shm ring (already
   lock-free/atomics-based by construction) and the admin-visible
   fps/latency counters, which DO need a small lock or atomics since both
   this thread and the admin thread's snapshot path read them.

## 11. Error handling & edge cases

- **Malformed CONNECT** (`num_modes` outside `1..PGIPC_MAX_MODES`, or
  `app_id` not NUL-terminated within `PGIPC_APP_ID_LEN`): treat as
  REJECTED - respond `MODE{accepted=0}` and close. Do not crash the
  display process on any malformed input from a producer; producers are
  third-party marketplace code and must be treated as adversarial input
  at the protocol boundary even though they run in their own container.
- **Duplicate `app_id`** (same id connects twice concurrently): both
  connections are tracked as independent session-table slots - `app_id`
  is not a uniqueness key at the protocol level in v1. (Revisit if the
  marketplace's process-management guarantees "one instance per app_id"
  make this unreachable in practice; until then, don't assume it.)
- **`pgipc_ctrl_recv`/`_fds` returning -2 (oversized message)**: log and
  drop the individual message, keep the connection open - a single
  malformed frame should not tear down an otherwise-healthy session
  (matches the producer ctrl thread's own `rc == -2: continue` handling).
- **DMABUF import failure** (`drmPrimeFDToHandle`/`AddFB2` fails on the
  display's KMS/DRM side - out of scope to implement in v1, but the
  control-plane response is in scope): reply
  `PGIPC_MSG_DMABUF_ACK{accepted=0, reason}`; session stays in PIXELS
  payload mode; do not evict/deactivate the client over this - a GPU
  producer's fallback to `glReadPixels`-into-shm (mentioned in the header)
  is entirely its own decision to make, not the display's to force.
- **Admin `switch` to an app_id currently REJECTED/CLOSED**: same as "not
  connected" (§6.3) - REJECTED/CLOSED slots are not distinguished from
  "never connected" in the admin LIST/SWITCH surface once freed.

## 12. Non-goals (v1)

- Real DRM/KMS/GBM scanout - v1 reference implementation targets
  `/dev/fb0` for the CPU pixels path only; the dmabuf/GPU data-plane
  (actual page-flipping) is specified at the *control-plane* level only
  (§7 step 3, §11) and will get its own implementation doc alongside the
  GPU producer skeleton.
- Persistent app registry/catalog - the display only knows about apps
  once they connect, exactly as the old toy README already called out;
  that registry lives in the orchestrator/mgmt UI, not here.
- Any form of resolution scaling/letterboxing - mode negotiation is
  always exact-match (§5); this is a one-fixed-panel device.
- Dynamic/growable `MAX_CLIENTS` - fixed array per the confirmed decision;
  revisit only if real usage hits the limit.
- Priority-based or preemptive activation - first-come-first-served only
  (§7); the admin `switch` command is the only way to move activation away
  from a client that isn't misbehaving.
- HTTP admin API - out of scope for v1; §6's Unix socket is designed so a
  thin HTTP sidecar can be added later without changing this protocol.

## 13. Open items for a future revision

- Whether `PGIPC_ADMIN_MSG_SWITCH_REQUEST` should support a "queued"
  grant for an app_id that hasn't connected yet (§6.3) once the
  orchestrator's real start/poll/switch sequencing is built and its
  actual latency is measured.
- Whether heartbeat timeout (2000ms) needs to be configurable per
  deployment once real producer apps (not just the sine-wave demo) exist
  and their worst-case per-frame CPU cost is known.

## 14. Build & test

The service is built with CMake (no more hand-written Makefile). From
`display/`:

```sh
cmake -S . -B build -DPGIPC_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

- `PGIPC_BUILD_TESTS` (default `ON`) fetches GoogleTest via CMake
  `FetchContent` on first configure (needs network once; cached under
  `build/_deps` afterwards) and builds two test binaries:
  - `pgipc_unit_tests` - pure logic, no threads/sockets/shm
    (`tests/unit/`). Fast, safe to run on every change.
  - `pgipc_integration_tests` - real threads, real POSIX shm/semaphores/
    Unix sockets (`tests/integration/`). Each test binds fixed,
    well-known resource names (the shm ring, `PGIPC_ADMIN_SOCK_PATH`), so
    all integration tests are registered `RUN_SERIAL TRUE` under ctest -
    they must never run concurrently with each other or with a second
    copy of themselves.
- `PGIPC_SANITIZE` (cache var, default `""`) can be set to `thread` or
  `address` to add `-fsanitize=...` project-wide, e.g.:
  ```sh
  cmake -S . -B build-tsan -DPGIPC_BUILD_TESTS=ON -DPGIPC_SANITIZE=thread
  cmake --build build-tsan -j$(nproc)
  ctest --test-dir build-tsan --output-on-failure
  ```
  Use this to validate any change touching `admin_plane.c`'s poll loop or
  `control_query.c`'s mutex/condvar rendezvous - both were built around a
  self-pipe wakeup pattern specifically to be race-free under TSan (a
  prior direct-`accept()`-plus-cross-thread-`close()` design was
  confirmed racy and replaced; see `admin/admin_plane.c`).
- To hunt for flakiness in the integration tests (real threads/timing),
  repeat them under a sanitizer build:
  ```sh
  ctest --test-dir build-tsan -R integration --repeat-until-fail 20 --output-on-failure
  ```
- Requires **C++23** for the test binaries specifically (`CMakeLists.txt`
  sets `CMAKE_CXX_STANDARD 23`): `libpgipc.h`'s structs use C11
  `<stdatomic.h>` types (`atomic_int`, `atomic_bool`, ...) as field types
  directly, and those typedefs are only made visible to C++ translation
  units via libstdc++'s own `<stdatomic.h>` compatibility shim, which is
  itself gated behind `__cpp_lib_stdatomic_h` (C++23). The core library
  and `pgipc_reader` itself remain plain C11.
