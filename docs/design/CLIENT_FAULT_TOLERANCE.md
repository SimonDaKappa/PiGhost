# Wisp Display Protocol — Client Fault Isolation & Ring Redesign

**Status:** Draft for review
**Scope:** server + client_sdk shm ring subsystem
**Author context:** design synthesis from working session, not yet implemented except where noted

## Background

The protocol has one actively-rendering client at a time (others queue) over a shm
ring for pixel/dmabuf payload transfer and a unix domain socket for control. A
generation counter already exists and is bumped on eviction/activation-switch, and the
server already rejects frame slots tagged with a stale generation.

The gap: generation tagging is a **detection** mechanism, not a **prevention**
mechanism. It tells the server not to trust what it reads, but it does not stop an
evicted (or merely stuck/crashed-thread) client process from continuing to write into
memory the newly-promoted client now depends on. Since shm resources can't be revoked
from a process the way an fd-based channel can be closed, the actual ownership
guarantee has to come from **spatial isolation** — evicted and active clients must
never share a live memory region — not from policing writes after they happen.

This document breaks that overall goal into independent, sequenceable features.

---

## Feature 1: Per-Connection memfd Rings (replace named shm)

### Problem
The current implementation uses a single globally-named `shm_open(WISP_SHM_NAME)`
segment. This has two issues:

1. **No real access control.** A well-known name at `0666` is `shm_open`-able by any
   process on the system, not just the intended client.
2. **Does not compose with ring-of-rings isolation.** If multiple rings live inside
   one named/offset-addressed segment, a client that `mmap()`s the whole segment (or
   simply doesn't respect the offset it was "told") has write access to every ring in
   the pool, including ones owned by other clients. The isolation guarantee has to be
   structural, not a convention the client is trusted to follow — trusting the client
   to follow it is exactly the failure mode we're defending against.

### Design
- Replace the single named shm segment with **one `memfd_create()` region per ring**.
- Rings are created up front by the server (small fixed pool — see Feature 2).
- At grant time, the server passes the fd for the specific ring being handed to the
  client over `SCM_RIGHTS` on the already-connected, already-authenticated control
  socket.
- The client's `wispc_shm_attach()` no longer does a retrying `shm_open()` by name; it
  receives an fd directly from the grant message and `mmap()`s only that fd.

### Why this gives the actual guarantee
A client that never receives an fd for a given ring has no addressable path to it —
not "isn't told the offset," but cannot open or map it, full stop. This is the
difference between policy-based and structural isolation.

### Interface sketch
```c
// server: on grant
int ring_fd = memfd_for_ring(assigned_ring);
send_fd_over_scm_rights(client_conn_fd, ring_fd, grant_msg);

// client_sdk
wisp_shm_ring_t *wispc_shm_attach(int received_fd, wisp_render_kind_t kind);
```

### Dependencies / ordering
Independent of Feature 2 and 3 in principle, but only pays off once combined with
Feature 2 — otherwise you still only have one region to hand out.

---

## Feature 2: Ring-of-Rings (Eviction-Safe Ring Pool)

### Problem
Given single-active-client + queued-waiters, a forcibly evicted client (SDK thread
crashed mid-callback, failed graceful winddown, etc.) may still hold live write access
to shm. The newly promoted client must not share that memory.

### Design
- Server maintains a small fixed pool of pre-created rings — **two per payload kind**
  is sufficient given the single-active-client constraint (see Feature 4 for why kind
  matters at creation time): `{pixels_a, pixels_b, dmabuf_a, dmabuf_b}`.
- Exactly one ring is "active" (handed to the current client) per kind at a time.
- On eviction, the server does **not** reuse the client's current ring. It promotes
  the next queued client (or holds until one exists) onto the *other* ring of the same
  kind.
- The vacated ring is **quarantined**, not immediately reclaimed.

### Reclamation requires confirmed death, not just disconnect
A closed control socket does not prove the client process (or a rogue thread within
it) has stopped executing. Quarantine → reclaim should be gated on:
1. `SIGKILL` sent to the evicted client's pid (not just socket close).
2. `waitpid()`/pidfd confirmation of actual process death.
3. Only then is the ring eligible to be handed to a future eviction/promotion as the
   "clean" spare.

Without step 2–3, a stuck-but-alive process could hold a ring quarantined
indefinitely; if that's a concern in practice, size the pool as a small spare set
rather than hard-coding exactly two per kind.

### Unification with normal promotion
Eviction-and-swap and ordinary "next queued client becomes active" should be the same
code path: *hand the next client a known-clean ring via the grant message.* Eviction
is just: kill old owner, confirm death, then run the ordinary promotion path against
the spare ring. This avoids a second state machine for what is otherwise the same
handshake.

### Interaction with generation counter
The generation counter remains as a defense-in-depth / assertion layer on top of
spatial isolation, not a replacement for it:
- Catches stragglers from a race during the handoff window itself.
- A generation mismatch on what should be a freshly-issued clean ring is now a
  **bug signal** (swap logic error) rather than a security boundary actually being
  tested — spatial isolation is the real enforcement mechanism now.

### Existing implementation status
`wisps_ring_evict_client()` and the generation bump already exist and are correct as
a detection layer; this feature adds the pool/promotion logic around it. The eviction
function itself doesn't need to change, it just becomes one step inside the
promotion-to-spare flow instead of an in-place reset of the same ring.

---

## Feature 3: Robust Cross-Process Mutex Recovery

### Status: implemented, documented here for completeness
```c
static void wisp_ring_lock(wisp_shm_ring_t *ring) {
  int rc = pthread_mutex_lock(&ring->bookkeeping_lock);
  if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(&ring->bookkeeping_lock);
  }
}
```
`PTHREAD_MUTEX_ROBUST` + `EOWNERDEAD`/`pthread_mutex_consistent()` handling is the
correct mechanism for surviving a client killed while holding
`bookkeeping_lock` (e.g. via the `SIGKILL` path in Feature 2). No changes proposed;
flagging only because Feature 2's kill-based eviction path is exactly the scenario
this exists to survive, and it should be exercised in eviction testing specifically
(kill client while it holds the lock, confirm server recovers rather than deadlocking).

---

## Feature 4: Variable-Sized Ring Layout via Flexible Array Member

### Problem
Today the ring struct's `frame_bufs` is sized for the full pixel payload
(`WISP_FRAME_MAX_SIZE`) regardless of payload mode, so dmabuf-mode rings waste most of
that space carrying only fd/metadata slots.

### Rejected alternative: union of pointers into a shared pool region
A layout like:
```c
typedef union {
  unsigned char (*pbuffers)[WISP_NUM_BUFFERS][WISP_PIXEL_SIZE];
  unsigned char (*dbuffers)[WISP_NUM_BUFFERS][WISP_DMABUF_SIZE];
} wisp_shm_buffers_t;
```
is unsafe: a pointer written into shared memory by one process is only valid to
dereference from another process if both processes map the segment at the identical
virtual address, which `mmap()` does not guarantee (ASLR, independent address space
layout). This would produce intermittent, environment-dependent corruption rather
than a clean failure — worse than the problem it solves. If a pool-of-offsets design
is wanted in the future, fields must be `size_t` offsets from each process's own
mapping base, never stored pointers.

### Design (adopted)
Keep one `wisp_shm_ring_t` type — not two payload-kind-specific struct types — using a
flexible array member sized at creation time:

```c
typedef struct {
  WISP_ATOMIC WISP_CACHELINE_FIELD(int32_t, latest_ready);
  WISP_ATOMIC WISP_CACHELINE_FIELD(int32_t, server_locked);
  WISP_ATOMIC WISP_CACHELINE_FIELD(int32_t, client_locked);
  WISP_ATOMIC WISP_CACHELINE_FIELD(uint64_t, frame_counter);
  WISP_ATOMIC WISP_CACHELINE_FIELD(uint32_t, generation);
  wisp_render_kind_t render_kind;  // fixed at creation, read-only after
  uint32_t slot_stride;              // bytes/slot, derived from render_kind
  uint64_t frame_id[WISP_NUM_BUFFERS];
  uint64_t write_ts_ns[WISP_NUM_BUFFERS];
  pthread_mutex_t bookkeeping_lock;  // must precede the FAM
  unsigned char frame_bufs[];        // flexible array member — must be last
} wisp_shm_ring_t;

size_t wisp_ring_size(wisp_render_kind_t kind) {
  uint32_t stride = (kind == WISP_PAYLOAD_PIXELS) ? WISP_FRAME_MAX_SIZE
                                                     : sizeof(wisp_dmabuf_slot_t);
  return sizeof(wisp_shm_ring_t) + (size_t)WISP_NUM_BUFFERS * stride;
}

static inline void *wisp_ring_slot_ptr(wisp_shm_ring_t *ring, int idx) {
  return ring->frame_bufs + (size_t)idx * ring->slot_stride;
}
```

`ftruncate()`/`mmap()` (both server creation and client attach) use `wisp_ring_size()`
instead of `sizeof(wisp_shm_ring_t)`. `slot_stride` is stamped in at creation so any
attaching process can compute slot addresses without separately re-deriving
mode-specific logic.

### Preserving render-loop agnosticism
This keeps the existing architectural boundary intact: the universal render callback
core (lock/unlock, checkout/release, evict, generation bookkeeping, `latest_ready` /
`server_locked` / `client_locked` state machine) stays entirely payload-kind agnostic
and unchanged. The only mode-dependent computation — turning a slot index into a byte
offset — moves into the same pixel/dmabuf wrapper implementations that already own
"resolve the correct resource for this payload kind" for the render callback and
`*_publish()` path. No new seam is introduced; the existing one absorbs it.

### Pool sizing implication (ties to Feature 2)
Because `render_kind` and `slot_stride` must be immutable for a ring's lifetime (both
the render core and wrapper read them without expecting them to change), each ring in
the Feature 2 pool is **mode-fixed**, not dynamically retypeable. Concretely: two
pixel-sized rings + two dmabuf-sized rings pre-created, not two generic rings resized
on demand. Re-typing a ring would require re-`ftruncate`/re-`mmap` at a new size,
which is more complex than just keeping the pool partitioned by kind from the start.

---

## Feature 5: Explicit dmabuf Import Lifecycle on Eviction

### Problem
Spatial isolation via Features 1–2 protects the shm control-plane region (ring
metadata, and in pixels-mode the pixel bytes themselves). It does **not** by itself
protect dmabuf-mode GPU resources: the fds a client's SDK announced were *imported* by
the server (its own GEM handle / kernel reference), which is a lifecycle independent
of the client's fd table or the shm ring holding the metadata. Closing the client's
socket or swapping the ring does not revoke that import.

Without explicit teardown, it's possible to have correct spatial isolation on the
control-plane metadata while still sharing the actual underlying GPU allocation across
a trust boundary — silently defeating the point of the swap for dmabuf mode.

### Design
- On eviction (before a ring is considered eligible for reclamation per Feature 2),
  the server explicitly drops its own imports of every GPU resource associated with
  that client's connection.
- The ring handed to the newly-promoted client is populated only with **newly
  imported** fds from that client's own announce handshake — never a carried-over
  import from a previous occupant's set, even if the underlying allocation happens to
  be identical (e.g. reconnect/resize edge cases).
- This is a distinct invariant from "ring is clean" in the shm sense and should be
  tracked/asserted separately in the eviction path (e.g. an explicit
  `dmabuf_imports_released` flag gating reclamation, alongside the `waitpid`-confirmed
  death check from Feature 2).

### Dependencies
Only relevant to dmabuf-mode rings; independent of Feature 4's layout change, but
should land alongside Feature 2 since it's part of the same "what does clean actually
mean" question the ring pool depends on.

---

## Suggested sequencing

1. **Feature 4** (FAM layout) — self-contained struct/sizing change, no protocol
   behavior change, easiest to land and test in isolation.
2. **Feature 1** (memfd + SCM_RIGHTS) — replaces the attach path; can be tested against
   the existing single-ring behavior before the pool exists.
3. **Feature 2** (ring pool + kill/waitpid reclamation) — depends on 1 and 4 (needs
   per-ring fds and correctly-sized rings to pool).
4. **Feature 5** (dmabuf import teardown) — lands alongside 2, gates dmabuf-ring
   reclamation specifically.
5. **Feature 3** (robust mutex) — already implemented; add eviction-under-lock test
   coverage once Feature 2's kill path exists to exercise it against.