# moving_sine_wave_cpu

A reference CPU-pixel producer app for libpgipc: draws a single animated
sine wave curve scrolling across the frame, entirely in software, and
publishes it into the display's shm ring at the negotiated fps. This is
the canonical "hello world" producer -- the whole app is one file
(`main.c`), and everything besides the actual pixel-drawing loop
(session establishment, mode negotiation, activation/eviction handling,
reconnect-safety) is handled by `libpgipc.h` itself.

## Building

```sh
cd pgipc_dev/writers/moving_sine_wave_cpu
cmake -S . -B build
cmake --build build
```

Produces `build/sine_wave_cpu`. No dependencies beyond a C11 compiler,
pthreads, `libm`, and `librt` (all pulled in by the `CMakeLists.txt` here).

## Running against a live display

You need a running `pgipc_reader` (the display service) first --
see `pgipc_dev/display/README.md` / `SPEC.md` for how to build it. Both
processes talk over well-known paths in `/dev/shm` (`PGIPC_SHM_NAME`,
`PGIPC_CONTROL_SOCK_PATH`, `PGIPC_ADMIN_SOCK_PATH` in `libpgipc.h`), so
they must be run as the same user on the same machine (or same container
namespace, once dockerized).

### 1. Start the display

```sh
cd pgipc_dev/display
./build/pgipc_reader
```

Leave this running in its own terminal. It logs the socket/shm paths it's
using and writes rendered frames to `/tmp/pgipc_frames/latest.ppm` (via
`fb_sink_file.c` -- the file-based stand-in sink used until real
`/dev/fb0`/HDMI hardware is available).

### 2. Start the writer

In a second terminal:

```sh
cd pgipc_dev/writers/moving_sine_wave_cpu
./build/sine_wave_cpu
```

Since it's the only producer connected, it activates immediately. You'll
see:

```
[sine_wave_cpu] connecting to display...
[pgipc:sine_wave_cpu] negotiated mode: 320x240@60fps
[pgipc:sine_wave_cpu] ACTIVATED, generation=1
```

followed by a status line every 5 seconds while it keeps running (it is
designed to run indefinitely -- Ctrl+C to stop it cleanly).

### 3. Watch it live with an auto-reloading PPM viewer

`/tmp/pgipc_frames/latest.ppm` is overwritten on every single blit (60
times/sec at the default negotiated mode), so any viewer that polls the
file and reloads on change works as a live preview. A few options that
work well on a dev machine:

- **feh** (lightweight, X11):
  ```sh
  feh --reload 0.1 /tmp/pgipc_frames/latest.ppm
  ```
- **GNOME/Eye of GNOME (`eog`)**: does not auto-reload; not recommended
  for this use case.
- **ImageMagick's `display`** with a polling loop:
  ```sh
  watch -n 0.1 'convert /tmp/pgipc_frames/latest.ppm /tmp/pgipc_frames/latest.png'
  # then open latest.png in any auto-reloading image viewer/browser tab
  ```
- **VS Code**: open `/tmp/pgipc_frames/latest.ppm` in the built-in image
  preview; VS Code auto-reloads a previewed file when it changes on disk,
  so this works with zero extra tooling if you're already in an editor
  session.

You should see a bright cyan/gold sine curve scrolling smoothly across a
dark gradient background.

### 4. Verify via the admin socket (optional)

While the writer is connected, you can confirm the display's session
table sees it as `ACTIVE` from a separate admin client (see
`pgipc_dev/display/tests/integration/control_plane_test.cpp` for a
working example of building one, or use any future `pgipc-admin` CLI once
it exists) -- the app_id will be `sine_wave_cpu` and `negotiated_mode`
should match what was logged above.

## What to expect / troubleshooting

| Symptom | Likely cause |
|---|---|
| `failed to connect -- is pgipc_reader running?` | The display isn't running, or its control socket path doesn't match (stale `/dev/shm` files from a previous crashed run -- `pgipc_reader` unlinks stale paths on its own startup, so just restart it). |
| Writer logs `ACTIVATED` but `latest.ppm` never updates | The display's data-plane thread isn't running/blocked -- check `pgipc_reader`'s own stdout for errors from `fb_sink_file_create()` (e.g. can't create `/tmp/pgipc_frames`). |
| Writer logs `DEACTIVATED -- paused, holding session` | Another producer (or an admin `switch`) took over activation. The sine wave app keeps its session alive and will resume automatically if it's switched back in -- no need to restart it. |
| Curve looks torn/flickery | Expected only under extreme load; the shm ring's checkout/release/generation mechanism guarantees the display never reads a half-written frame, so this should not happen under normal conditions -- report it as a bug if seen. |

## Design notes

- **Pixel format**: XRGB8888 (DRM `XR24`), tightly packed rows, in-memory
  byte order `[B, G, R, X]` per pixel -- see `libpgipc.h`'s format section
  and `pgipc_dev/display/dataplane/fb_sink_file.c`'s PPM conversion for
  the authoritative reference.
- **Only writes frames while active**: `pgipc_writer_is_active()` is
  checked every tick; while inactive the loop just sleeps and polls,
  spending no CPU on rendering it can't publish anyway.
- **Frame pacing**: the loop measures its own render time each tick and
  sleeps only the remainder of the negotiated frame interval, so it stays
  close to the negotiated fps regardless of render cost (trivial here,
  but the same pattern scales to more expensive producers).
- **Mode re-read every tick**: the negotiated mode is re-read from
  `pgipc_writer_negotiated_mode()` on every active tick rather than cached
  once at connect time, since an admin `switch` can re-activate this
  producer at a different negotiated size than its first activation.
