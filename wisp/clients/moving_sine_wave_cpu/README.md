# moving_sine_wave_cpu

A reference CPU-pixel producer app for libpgdp: draws a single animated
sine wave curve scrolling across the frame, entirely in software, and
publishes it into the server's shm ring at the negotiated fps. This is
the canonical "hello world" producer -- the whole app is one file
(`main.c`), and everything besides the actual pixel-drawing loop
(session establishment, mode negotiation, activation/eviction handling,
reconnect-safety) is handled by `libpgdp.h` itself.

## Building

```sh
cd pg_display/clients/moving_sine_wave_cpu
cmake -S . -B build
cmake --build build
```

Produces `build/sine_wave_cpu`. No dependencies beyond a C11 compiler,
pthreads, `libm`, and `librt` (all pulled in by the `CMakeLists.txt` here).

## Running against a live server

You need a running `pgdp_server` (the server service) first --
see `pg_display/server/README.md` / `SPEC.md` for how to build it. Both
processes talk over well-known paths in `/dev/shm` (`PGDP_SHM_NAME`,
`PGDPS_CONTROL_SOCK_PATH`, `PGDP_ADMIN_SOCK_PATH` in `libpgdp.h`), so
they must be run as the same user on the same machine (or same container
namespace, once dockerized).

### 1. Start the server

```sh
cd pg_display/server
./build/pgdp_server
```

Leave this running in its own terminal. It logs the socket/shm paths it's
using and writes rendered frames to `/tmp/pgdp_frames/latest.ppm` (via
`fb_sink_file.c` -- the file-based stand-in sink used until real
`/dev/fb0`/HDMI hardware is available).

### 2. Start the client

In a second terminal:

```sh
cd pg_display/clients/moving_sine_wave_cpu
./build/sine_wave_cpu
```

Since it's the only producer connected, it activates immediately. You'll
see:

```
[sine_wave_cpu] connecting to server...
[pgipc:sine_wave_cpu] negotiated mode: 320x240@60fps
[pgipc:sine_wave_cpu] ACTIVATED, generation=1
```

followed by a status line every 5 seconds while it keeps running (it is
designed to run indefinitely -- Ctrl+C to stop it cleanly).

### 3. Watch it live with an auto-reloading PPM viewer

`/tmp/pgdp_frames/latest.ppm` is overwritten on every single blit (60
times/sec at the default negotiated mode), so any viewer that polls the
file and reloads on change works as a live preview. A few options that
work well on a dev machine:

- **feh** (lightweight, X11):
  ```sh
  feh --reload 0.1 /tmp/pgdp_frames/latest.ppm
  ```
- **GNOME/Eye of GNOME (`eog`)**: does not auto-reload; not recommended
  for this use case.
- **ImageMagick's `server`** with a polling loop:
  ```sh
  watch -n 0.1 'convert /tmp/pgdp_frames/latest.ppm /tmp/pgdp_frames/latest.png'
  # then open latest.png in any auto-reloading image viewer/browser tab
  ```
- **VS Code**: open `/tmp/pgdp_frames/latest.ppm` in the built-in image
  preview; VS Code auto-reloads a previewed file when it changes on disk,
  so this works with zero extra tooling if you're already in an editor
  session.

You should see a bright cyan/gold sine curve scrolling smoothly across a
dark gradient background.

### 4. Verify via the admin socket (optional)

While the client is connected, you can confirm the server's session
table sees it as `ACTIVE` from a separate admin client (see
`pg_display/server/tests/integration/control_plane_test.cpp` for a
working example of building one, or use any future `pgipc-admin` CLI once
it exists) -- the client_id will be `sine_wave_cpu` and `negotiated_mode`
should match what was logged above.

## What to expect / troubleshooting

| Symptom | Likely cause |
|---|---|
| `failed to connect -- is pgdp_server running?` | The server isn't running, or its control socket path doesn't match (stale `/dev/shm` files from a previous crashed run -- `pgdp_server` unlinks stale paths on its own startup, so just restart it). |
| Client logs `ACTIVATED` but `latest.ppm` never updates | The server's data-plane thread isn't running/blocked -- check `pgdp_server`'s own stdout for errors from `fb_sink_file_create()` (e.g. can't create `/tmp/pgdp_frames`). |
| Client logs `DEACTIVATED -- paused, holding session` | Another producer (or an admin `switch`) took over activation. The sine wave app keeps its session alive and will resume automatically if it's switched back in -- no need to restart it. |
| Curve looks torn/flickery | Expected only under extreme load; the shm ring's checkout/release/generation mechanism guarantees the server never reads a half-written frame, so this should not happen under normal conditions -- report it as a bug if seen. |

## Design notes

- **Pixel format**: XRGB8888 (DRM `XR24`), tightly packed rows, in-memory
  byte order `[B, G, R, X]` per pixel -- see `libpgdp.h`'s format section
  and `pg_display/server/dataplane/fb_sink_file.c`'s PPM conversion for
  the authoritative reference.
- **Only writes frames while active**: `pgdpc_is_active()` is
  checked every tick; while inactive the loop just sleeps and polls,
  spending no CPU on rendering it can't publish anyway.
- **Frame pacing**: the loop measures its own render time each tick and
  sleeps only the remainder of the negotiated frame interval, so it stays
  close to the negotiated fps regardless of render cost (trivial here,
  but the same pattern scales to more expensive producers).
- **Mode re-read every tick**: the negotiated mode is re-read from
  `pgdpc_negotiated_mode()` on every active tick rather than cached
  once at connect time, since an admin `switch` can re-activate this
  producer at a different negotiated size than its first activation.
