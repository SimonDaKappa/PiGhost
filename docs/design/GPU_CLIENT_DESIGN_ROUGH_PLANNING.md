# Client-Side GPU Guide: Mesa OpenGL/Vulkan under a Fence-Publish Display Protocol

This assumes the architecture you described: single active publishing client, SPSC ring
buffer of dmabuf-backed framebuffers, client renders + fences but never flips, client
calls `*_FRAME_PUBLISH` and the SDK/server does the actual KMS flip.

The core invariant to design around: **the client is a headless GPU renderer that
produces (dmabuf fd, fence fd) pairs. It never touches the display, never blocks on the
GPU from the CPU if it can avoid it, and never owns anything KMS-related.**

---

## 1. Ownership model recap

| Resource | Owner | Notes |
|---|---|---|
| DRM device / render node | Client opens it, but server tells it *which* node | must match server's scanout GPU |
| GL context / VK device+queue | **Client** (explicit exception in your design) | long-lived across ticks |
| dmabuf-backed images/buffers | **Client** (explicit exception) | allocated once, reused per ring slot |
| GPU fence (sync_file / syncobj) | Client creates, SDK/server consumes | client never waits on it itself |
| CRTC / plane / connector / DRM master | **Server only** | client must never touch these |
| Modeset, atomic commit, page flip | **Server only** | this is what `FRAME_PUBLISH` triggers |
| Format/modifier negotiation | Server dictates, client queries via SDK | can't assume tiling support |

Everything below is organized around not letting the client accidentally reach across
that line — it's very easy to do by accident because the "obvious" GL/VK APIs for
presenting a frame (`eglSwapBuffers`, `vkQueuePresentKHR`) are exactly the APIs you must
**not** use here, since they assume the app owns the window/swapchain.

---

## 2. Shared DRM/KMS layer (both APIs sit on top of this)

### Use
- **Render nodes** (`/dev/dri/renderD1XX`), opened via `drmGetRenderDeviceNameFromFd()`
  or a path the SDK hands you — not `/dev/dri/cardN`.
- `drmPrimeHandleToFD` / `drmPrimeFDToHandle` indirectly via GBM/EGL or Vulkan's
  external-memory-fd path — you generally won't call these raw ioctls yourself, Mesa
  does it under the hood.
- **GEM handles / dmabuf fds** as the actual transport unit you hand to the SDK.
- **DRM syncobjs or raw `sync_file` fds** for fences — see §5.
- Querying **DRM_FORMAT + modifier** support through the SDK callback (the SDK should
  proxy this from the server's KMS plane capabilities, not let you query KMS directly).

### Do NOT use
- `drmSetMaster` / `drmDropMaster` — the server holds DRM master, always. A client
  calling this will fight the server for control and is a correctness bug, not just a
  layering violation.
- `drmModeSetCrtc`, `drmModeAtomicCommit`, `drmModePageFlip`, `drmModeAddFB*` — any KMS
  object manipulation. If the client can call these, your "client minimally trusted"
  boundary is broken; these shouldn't even be linked into the client's allowed surface.
- Opening `/dev/dri/card0` (primary node) directly — no reason to; render nodes give you
  everything needed for rendering + buffer export without master contention.
- Assuming a GPU — on multi-GPU systems (common with hybrid laptops, or a discrete GPU
  doing render while an iGPU drives the display), the client **must** render on the same
  node the server scans out from, or you're forced into an extra copy/import step you
  didn't design for. This should be a value the SDK hands the client at init, not
  something the client enumerates itself.

---

## 3. OpenGL path (Mesa, via EGL + GBM)

### What the client sets up (once, at init)
1. `eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, gbm_device, ...)` — **not**
   `EGL_PLATFORM_X11`/`WAYLAND`. You have no window system; GBM is your "platform" purely
   as a buffer-allocation abstraction.
2. `eglInitialize`, choose an `EGLConfig` — since you're not doing
   `eglCreateWindowSurface`, you actually want a **configless/surfaceless** context:
   `EGL_KHR_surfaceless_context` (near-universal on Mesa). This avoids creating any
   EGL surface at all, which is correct since there's no window.
3. `eglCreateContext` with `EGL_KHR_no_config_context` or a dummy pbuffer-compatible
   config, then `eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)`.
4. Allocate render targets as **GBM buffer objects directly** (`gbm_bo_create` /
   `gbm_bo_create_with_modifiers`), *not* a `gbm_surface`. `gbm_surface` implies a
   swapchain-like "lock front buffer" model that assumes you're going to hand buffers to
   a window system's presentation loop — that's not this design. You want N independent
   BOs matching your ring buffer slot count, allocated once and reused forever.
5. For each BO: `eglCreateImageKHR(..., EGL_LINUX_DMA_BUF_EXT, ...)` or the
   `EGL_MESA_image_dma_buf_export` inverse if you go GL-texture-first — either way you end
   up with an `EGLImage` wrapping a dmabuf fd (`gbm_bo_get_fd` /
   `gbm_bo_get_fd_for_plane` if multi-plane/modified).
6. Bind that `EGLImage` as your render target: create a GL renderbuffer or texture, then
   `glEGLImageTargetRenderbufferStorageOES` (renderbuffer) or
   `glEGLImageTargetTexture2DOES` (texture), attach to an FBO. **This FBO is what your
   per-tick paint callback renders into** — never the default framebuffer (there isn't
   one; you have no `EGLSurface`).

### Per-tick render + publish
1. App's paint callback issues GL draw calls into the bound FBO.
2. `glFlush()` (not `glFinish()`).
3. Create a fence: `eglCreateSyncKHR(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL)` (the
   `EGL_ANDROID_native_fence_sync` extension — despite the name, it's the standard Linux
   mechanism, not Android-specific in practice, and Mesa supports it on desktop GL/GLES).
4. `eglDupNativeFenceFDANDROID(dpy, sync)` to get a `sync_file` fd representing "GPU
   finished rendering this frame."
5. Hand `(buffer_idx, fence_fd)` to the SDK via `*_FRAME_PUBLISH` and increment your ring
   index. Do **not** wait on the fence yourself — that's the server/kernel's job at
   scanout time (KMS atomic commit takes an `IN_FENCE_FD` per-plane and the kernel waits
   for you).

### Do NOT use
- `eglSwapBuffers` — there is no `EGLSurface` to swap; this call doesn't even apply to
  this design and if it compiles/runs you've accidentally created a window-system
  surface somewhere.
- `eglCreateWindowSurface` / any `EGL_PLATFORM_X11|WAYLAND|...` — no window system client
  side, ever.
- `glFinish()` or `eglClientWaitSyncKHR` with a real timeout in the hot path — this
  turns your async pipeline into a synchronous stall and defeats the entire point of
  fence-based publish. (`glFinish` is fine in one-off debug builds, never in the tick
  loop.)
- `gbm_surface_lock_front_buffer` / `gbm_surface_release_buffer` — this is the
  swapchain-style API for window-system-integrated GBM use; irrelevant and actively
  wrong here since you're managing the ring buffer yourself.
- Reading pixels back with `glReadPixels` for anything other than debugging — the whole
  point of dmabuf mode is avoiding a CPU round-trip.

### Extensions to check for at init (fail loud if missing)
`EGL_KHR_surfaceless_context`, `EGL_KHR_no_config_context`,
`EGL_EXT_image_dma_buf_import`, `EGL_MESA_image_dma_buf_export` (if needed),
`EGL_ANDROID_native_fence_sync`, `GL_OES_EGL_image`. On modifier-aware setups also
`EGL_EXT_image_dma_buf_import_modifiers`.

---

## 4. Vulkan path (Mesa ICD — RADV, ANV, NVK, etc.)

### What the client sets up (once, at init)
1. `vkCreateInstance` with **no** `VK_KHR_surface` or any WSI extension
   (`VK_KHR_xcb_surface`, `VK_KHR_wayland_surface`, `VK_KHR_display`, etc.) enabled —
   you're headless by design.
2. Pick the `VkPhysicalDevice` matching the DRM node the SDK told you about — via
   `VK_EXT_physical_device_drm`, which exposes `primaryMajor/Minor` and
   `renderMajor/Minor` on the `VkPhysicalDeviceDrmPropertiesEXT` struct. Match against
   the render node the server gave you; don't just pick device index 0.
3. `vkCreateDevice` enabling: `VK_KHR_external_memory_fd`,
   `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
   `VK_KHR_external_fence_fd` and/or `VK_KHR_external_semaphore_fd`, and
   `VK_KHR_timeline_semaphore` (core in 1.2+, still worth explicitly relying on).
4. Allocate one `VkImage` per ring slot with
   `VkExternalMemoryImageCreateInfo{handleTypes = ..._DMA_BUF_BIT_EXT}` and
   `VkImageDrmFormatModifierListCreateInfoEXT` constrained to the modifier list the SDK
   gave you (again — negotiated, not assumed). Bind memory with
   `VkExportMemoryAllocateInfo`, then `vkGetMemoryFdKHR` to get the dmabuf fd(s) — note
   multi-planar modifiers can mean multiple fds/offsets per image, handle that.

### Per-tick render + publish
1. Paint callback records/submits a command buffer targeting that ring slot's image
   (correct layout transitions — `UNDEFINED`/whatever-you-left-it-in →
   `TRANSFER_SRC`/`COLOR_ATTACHMENT_OPTIMAL` as needed; there's no implicit "present"
   layout transition since there's no swapchain doing it for you).
2. Signal a **timeline semaphore** (preferred over a plain fence — gives you a value you
   can reason about across ring slots without juggling N separate fence objects) on
   `vkQueueSubmit`.
3. Export it: `vkGetSemaphoreFdKHR` with
   `VkSemaphoreGetFdInfoKHR{handleType = ..._SYNC_FD_BIT}` → gives a `sync_file`-style fd
   suitable for the same KMS `IN_FENCE_FD` consumption path as the GL side. (If the
   driver only supports `SYNC_FD` on binary semaphores/fences rather than timeline
   semaphores directly, fall back to a binary `VkFence` + `vkGetFenceFdKHR` — check
   `VkExternalSemaphoreProperties`/`VkExternalFenceProperties` for what's actually
   exportable on your target Mesa driver rather than assuming.)
4. Hand `(buffer_idx, fence_fd)` to `*_FRAME_PUBLISH`, increment ring index.

### Do NOT use
- `VK_KHR_swapchain`, `vkCreateSwapchainKHR`, `vkAcquireNextImageKHR`,
  `vkQueuePresentKHR` — none of this applies; you have no `VkSurfaceKHR` and shouldn't
  create one. This is the Vulkan equivalent of `eglSwapBuffers` in the GL section above.
- `vkDeviceWaitIdle` / `vkQueueWaitIdle` in the tick loop — blocking, kills concurrency
  between CPU-side recording of frame N+1 and GPU execution of frame N. Use the timeline
  semaphore's value to do CPU-side throttling (e.g. "don't record more than 2 frames
  ahead") instead of a hard wait-idle.
- Any `VK_KHR_display*` extensions (`VK_KHR_display`, `VK_EXT_direct_mode_display`,
  `VK_EXT_acquire_drm_display`) — these are for apps that *are* the compositor and want
  to drive KMS directly from Vulkan. Your client is explicitly not that; the server
  owns display.
- Picking a physical device by "first one" / by name heuristics — must match the actual
  scanout GPU via `VK_EXT_physical_device_drm`, otherwise you silently end up rendering
  on the wrong GPU and either fail dmabuf import at the server or force an implicit copy.

---

## 5. Fence mechanics — the part most likely to bite you

Both paths ultimately need to hand the SDK a **`sync_file` file descriptor** (or
something convertible to one), because that's what the kernel's KMS atomic ioctl expects
per-plane (`IN_FENCE_FD` property) for the server to attach as an implicit wait before
flipping. Two adjacent-but-different kernel sync primitives exist — know which one
you're holding:

| Primitive | What it is | How you get it |
|---|---|---|
| `sync_file` fd | one-shot fd wrapping a `dma_fence`, pollable, consumed by KMS `IN_FENCE_FD` | `eglDupNativeFenceFDANDROID` (GL), `vkGetSemaphoreFdKHR`/`vkGetFenceFdKHR` with `SYNC_FD` handle type (VK) |
| DRM syncobj | a kernel object holding a `dma_fence` slot, can be reused/reset, timeline-capable | `vkGetSemaphoreFdKHR` with `OPAQUE_FD` handle type on drivers that prefer syncobj internally; convertible to sync_file via `DRM_IOCTL_SYNCOBJ_EXPORT_SYNC_FILE` |

If your SDK's `*_FRAME_PUBLISH` API expects a raw `sync_file` fd (simplest, most
portable choice for a server that just wants to `poll()` it or feed it to
`drmModeAtomicCommit`), make that the contract explicitly, and have the client-side SDK
shim do the syncobj→sync_file conversion if a given Mesa driver only hands you a syncobj
natively. Don't leave this implicit — it's the single most common source of "works on
RADV, hangs on ANV" bugs in this kind of pipeline.

**Client never calls a blocking wait on this fence.** That's the entire reason it's
being handed off instead of consumed locally — the design point of your architecture is
that fence-wait happens either in the kernel (KMS commit) or async in the server, never
stalling the client's tick loop.

---

## 6. Anti-pattern summary (things that should not compile/link for the client app)

| Anti-pattern | Why it's wrong here |
|---|---|
| `eglSwapBuffers` / `vkQueuePresentKHR` | Assumes client owns presentation; server does |
| `drmModeSetCrtc` / `drmModeAtomicCommit` / `drmModePageFlip` | KMS is server-exclusive |
| `drmSetMaster` | Server holds DRM master permanently |
| `glFinish` / `vkDeviceWaitIdle` in the tick loop | Turns async pipeline synchronous |
| `gbm_surface_*` | Swapchain-shaped API, wrong for ring-buffer-owned-by-client design |
| Any `VK_KHR_*surface*`, `VK_KHR_display*` | WSI/display ownership belongs to server |
| Assuming format/modifier support | Must be negotiated via SDK, server owns the plane |
| Picking GPU device index 0 | Must match server's scanout node explicitly |
| CPU readback (`glReadPixels`, mapped VkImage) in hot path | Defeats purpose of dmabuf zero-copy |

---

## 7. Implied SDK callback/API surface (informative)

Given the above, your SDK's dmabuf-mode surface probably wants to expose at minimum:

- **Init-time, read-only:** target DRM render node path, supported `(DRM_FORMAT,
  modifier[])` list for the plane, ring buffer slot count, target resolution.
- **Client → SDK, per frame:** `*_FRAME_PUBLISH(slot_idx, sync_file_fd)` — the one
  mutating call the client app makes into the "sacred" GPU-context/dmabuf-owned side.
- **SDK → client, gated callback:** something like `on_slot_available(slot_idx)` so the
  client's lock-free SPSC index-increment has a real backpressure signal instead of
  blindly wrapping into a slot the server hasn't finished scanning out of yet — worth
  confirming this exists in your ring buffer design, since GPU-side fencing protects
  *render-after-scanout* correctness but not *client racing ahead of available slots*.

This last point is worth double-checking in your protocol: the GPU fence guarantees the
server won't *display* a partially-rendered frame, but it doesn't by itself guarantee
the client won't try to re-render into a buffer slot the server hasn't flipped away from
yet, unless the SPSC index math and slot count already account for that (classic N-slot
ring buffer producer/consumer bound, independent of GPU fencing).

```
Realistically, how much of this can we asbtract into the sdk. The ultimate zero-trust solution is to do everything SDK-callables, but that is pragmatically impossible. Is it instead something that should just be very heavily documented and paired with extensive example client apps doing so for the multiple variant permutations?
```

Short answer: yes, and I'd go further — trying to make it fully SDK-callable isn't just impractical, it's solving the wrong problem. Let me separate two things you're currently treating as one:

"Trust boundary" (must be enforced) vs. "API ergonomics" (can be helped, doesn't need to be enforced)

Real zero-trust enforcement only has to happen at the points where the client actually crosses into the server's territory — i.e. the wire protocol calls. It does not need to happen by restricting which EGL/GBM/Vulkan calls the client is able to make inside its own process, because:

You can't practically sandbox arbitrary GL/VK driver ioctls without basically writing a GPU virtualization layer (this is what Chrome's GPU process sandbox or gVisor do, at real cost, and even they only allowlist specific ioctls rather than mediate the API).
DRM render nodes already give you most of this for free at the kernel level. That's literally why they exist — a render node fd cannot become DRM master, cannot touch KMS objects, cannot affect any other process's buffers it doesn't hold fds to. A malicious client with a render-node-only GL/VK context is already sandboxed from doing anything display-affecting except through the fds it explicitly hands you.
So the actual attack surface you need to defend isn't "what GL/VK calls did the client make" — it's "what did the client hand me at the wire boundary, and can it hurt me." That's a much smaller, much more tractable problem: validate format/modifier/size against negotiated capabilities before import, never trust the fd blindly, and — importantly — never let a client-supplied fence block you indefinitely (a fence that never signals is a real DoS vector in a single-active-publisher design, since a wedged fence wedges the whole display pipeline). That watchdog/timeout logic belongs entirely in the server/SDK, and it's arguably more valuable security work than anything you'd do trying to restrict the client's API surface.

Given that, here's how I'd bucket the actual work:

Fully SDK-owned (no reason not to abstract these — pure protocol, no GPU context needed):

Render node discovery/selection
Format + modifier negotiation
Ring buffer slot count, geometry, backpressure signaling
The publish/notify calls themselves, with validation on receipt
SDK-provided helper library, linked into the client (this is the sweet spot):

Fence export helpers — sdk_gl_export_fence(dpy, ctx) -> fd / sdk_vk_export_fence(device, semaphore) -> fd. This is the single most bug-prone, driver-inconsistent part of the whole pipeline (sync_file vs. syncobj, RADV vs. ANV differences), and it's exactly the kind of thing where "SDK ships correct code" beats "SDK documents the correct incantation." Ship it as a small header-only or static lib the client links, not an IPC call.
dmabuf export/description normalization (turn a GBM BO or exported VkImage memory into the (fd, format, modifier, offset, stride, plane_count) struct your wire protocol expects) — same validation logic can be shared between "client self-checks before publish" and "server double-checks on receipt," so write it once.
This is a meaningfully different sense of "SDK-managed" than what you started with — not the SDK executes it via IPC, but the SDK authors and ships the code, and it happens to run in the client's address space with the client's context. That distinction is worth being explicit about internally, because it gets you most of the correctness benefit of centralization without needing a trust boundary at all.

Documentation + reference examples only (should not be abstracted):

EGL/GBM context and surfaceless setup
Vulkan instance/device creation, extension enabling, physical device selection
The reason this category shouldn't be pulled into the SDK isn't just complexity — it's that a lot of real client apps will already have a GL or Vulkan context for other reasons (they're a game engine, a media player with its own renderer, whatever), and they'll want to import an existing context/device into your pipeline rather than have your SDK create one for them. If the SDK insists on owning device/context creation, you've made integration with any non-trivial existing renderer actively hostile. This is actually the strongest argument for why "graphics library context" was the right exception to carve out in your original design — it's not just that it's hard to abstract, it's that abstracting it is wrong for a chunk of your real integrators.

So concretely, I'd build the example matrix you're describing along these axes, since each cell has genuinely different footguns:

GL/GBM, single GPU
GL/GBM, multi-GPU (render-on-A, scanout-on-B handoff)
Vulkan headless, single GPU
Vulkan headless, multi-GPU
Vulkan/GL importing an existing app-owned context rather than creating one
And keep the SDK itself scoped to: negotiation API, helper lib for export/fence/validation, and boundary-time enforcement (format/size checks + fence watchdog) — that's where your engineering effort actually buys you safety, versus the examples/docs which buy you correctness and adoption.