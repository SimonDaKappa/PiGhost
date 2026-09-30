Protocol vs other (wire compatibility)

Don't just validate a static magic value baked into both binaries — that only tells you both sides compiled against a copy of the header, not that they agree at runtime, and it gives you no room to negotiate. Instead:

Bake a (MAGIC, MAJOR, MINOR) triple into the protocol lib as compile-time constants.
Make the first thing that happens on a connection an explicit hello/ack exchange (not implicit header validation) — think TLS ClientHello/ServerHello or SSH's version banner, not "assert my compiled-in constant equals your compiled-in constant."
MAGIC = sanity check, catches "this isn't even my protocol" / framing corruption.
MAJOR = hard gate. Mismatch → reject connection outright. Bump only on wire-breaking changes (framing, field layout, alignment, message ordering semantics).
MINOR = soft gate, backward-compatible wire additions only (e.g. new message types appended, never changes to existing ones). This numeric minor is still too coarse to drive feature-level decisions though — that's the next problem.

The important shift: version compatibility is something negotiated at connect time between two live instances, not something asserted at compile time between two link units. Static linking means you can't even guarantee two "vN" servers on the same host are byte-identical (hotfix branches, backports), so the wire has to be the arbiter, not the header file.

Client vs server (functional compatibility) — the hard one

You're right to be suspicious of a single "min feature-set" number. A monolithic version gate forces every feature onto one linear timeline, and it forces you to bump that number every time you add one event — which re-couples SDK/server to protocol versioning exactly the thing you're trying to decouple. It also can't express "server has A and C but not B" (patch releases, vendor forks, staged rollouts).

This is precisely the problem Wayland's actual architecture solves — worth stealing the mechanism even without the XML/codegen part:

Per-interface versioning, not per-protocol versioning. Each thing that can grow independently (an event, a request family, an object type) gets its own identifier and its own version counter, separate from the core wire version:

```c
// conceptually, not literally wl's registry format
struct feature_descriptor {
    uint32_t id;        // stable identifier for this capability
    uint32_t version;    // monotonically increasing, additive-only
};
```

At connect time, after the core wire hello succeeds:

Server advertises its full set of {id, version} pairs it supports.
Client (via SDK) advertises the set it was built to understand.
Negotiated capability for each id = min(client_version, server_version) if present on both sides, else absent.
Server must never emit an event under a feature id/version the client didn't ack. If the client didn't advertise id=EVENT_FOO v2, the server simply doesn't send it — same as Wayland never binding a global the client didn't request, or GL never exposing an extension you didn't query for.

This gets you exactly the property you want: old SDK, new server → server silently withholds new events from that connection, no crash, no protocol bump required, and no need for the server to hardcode "if client is built against SDK >= X." The client's advertised capability set is the truth about what it can handle — you don't infer it from a build number.

Make this resilient by construction, not just by discipline. Two things matter a lot here:

TLV / self-describing framing at the message level. Every message on the wire should carry (type_id, length, payload), not just (payload) assumed from a schema both sides share. That way even if negotiation logic has a bug, or a message slips through that wasn't negotiated, the old client can skip an unrecognized type_id by length rather than misparsing the stream or desyncing framing. This is the actual mechanism that makes "ignore what you don't understand" possible — capability negotiation tells you what should arrive, TLV framing protects you when something unexpected does anyway.
Negotiated state is per-connection, not global. A daemon will have old-SDK and new-SDK clients connected simultaneously. The capability set is a property of that specific connection's handshake, cached in the connection's session state, checked on every event emission (if (conn->caps[EVENT_FOO] >= 2) send_event(...)).

Where a single min-version number is still fine: for the "am I even parseable" gate (your MAJOR/MINOR from part 1) — that one should stay a blunt hard cutoff. Reserve fine-grained per-feature negotiation for additive/optional functionality. Two-tier is the pattern: coarse wire-format version as a hard gate, fine-grained capability set as a soft negotiated intersection.

One more axis you'll hit even though you didn't ask about it: since the SDK is a .so dynamically loaded by client apps, you eventually also have SDK-vs-app ABI compatibility (symbol versioning, .so version bumps) as a third, orthogonal concern from client-vs-server protocol compatibility — an app can link an old SDK build that's perfectly protocol-compatible with a new server, but ABI-incompatible with a newer SDK on disk. Worth keeping conceptually separate from day one so you don't accidentally conflate "SDK build number" with "protocol capability version" the way the single-min-version approach would have.

Here's how I'd extend the model — think of it as adding a third axis (hardware/platform baseline) underneath the two you already have, plus a fourth surface (fleet-level reporting to the dashboard) on top:

Adding the platform/BSP axis

You now have three independent version dimensions instead of two, and the new one — Yocto image vs. runtime bundle — is really the same hard-gate vs. soft-negotiate pattern from before, just moved down a layer:

Yocto image version = hard gate, coarse, rarely bumped. It fixes kernel ABI, GPU driver (vc4/v3d for Pi4), mesa version, and — critically for your multi-platform future — the machine layer itself (meta-raspberrypi vs. whatever meta-newplatform shows up later). This should be an A/B-style atomic image swap (RAUC, mender, or swupdate), because a half-applied kernel/driver update is not a state you want a device stuck in.
Runtime bundle version = the proto+server+sdk triple, versioned and updatable independently of the image, exactly like you wanted ("minor sdk bump shouldn't trigger a whole yocto release"). This is a separate delivery channel — could be as simple as an OTA payload that drops new binaries into a writable overlay + restarts the daemon, no reflash required.

The coupling between these two is a minimum-compatibility constraint, checked at install time, not at runtime negotiation:
```yaml
bundle_manifest.yaml:
  min_yocto_image: "4.2.0"     # needs mesa/kernel features from this image or later
  min_kernel_abi: "6.1"
```

The OTA agent refuses to install a bundle update if the running image doesn't satisfy it — same idea as an apt dependency check or Android's minSdkVersion, just applied one layer down. This is what actually lets you decouple the pipelines: the bundle can ship on its own cadence as long as it stays within the compatibility floor the image guarantees.

Reusing your capability mechanism for hardware, not just software

This is the nice part: the per-feature {id, version} negotiation from before generalizes cleanly to hardware gating too. A capability can be withheld for two reasons — client didn't advertise it, or this platform doesn't have the hardware for it — and both collapse to the same mechanism:

At startup, the server probes what it's actually running on (device-tree, /sys GPU nodes, core count) and only advertises capability IDs the hardware backs. A hypothetical future SoC without a hardware overlay plane simply never advertises COMPOSITE_HW_OVERLAY, and old and new clients alike fall back the same way they would for an unsupported SDK version.
The Yocto machine layer still matters at build time (you're not cross-compiling GPU-specific codepaths at runtime), but whether a given capability is exposed on the wire is a runtime decision layered on top of that build-time reality. Keeps "can I build for this platform" and "does this specific unit report the feature as live" as separate questions.
The fleet dashboard is a new negotiation surface, not a client

One thing worth calling out explicitly: the PWA dashboard almost certainly isn't a peer on your wayland-like local IPC — it's managing a fleet remotely, so it needs its own reporting contract, distinct from the connection-time capability handshake between client and server on-device. Each device should periodically publish a manifest (heartbeat over MQTT/HTTPS, whatever your management-plane transport is):

```json
{
  "device_id": "...",
  "machine": "raspberrypi4-64",
  "yocto_image_version": "4.2.1",
  "bundle_version": {"proto": "1.4.0", "server": "2.1.0", "sdk": "2.0.3"},
  "capabilities": {"EVENT_FOO": 2, "COMPOSITE_HW_OVERLAY": 1}
}
```

The dashboard reads this registry rather than holding a live protocol connection — which matters because your OTA desync means the fleet is guaranteed to be heterogeneous (some devices on bundle 2.1, some lagging at 1.9, staged rollouts, the occasional device that failed an update). The dashboard's job is then to gate its own UI/commands per-device off that reported capability set — never assume fleet-wide homogeneity, never issue a command a device's manifest doesn't support.

One gap worth deciding early: does the dashboard talk to devices directly, or through a backend/broker that caches the last-known manifest? For an IoT fleet at any scale you'll want the latter — direct dashboard-to-device polling doesn't survive devices being offline, behind NAT, or on flaky wifi, and a cached manifest lets the dashboard render fleet state even for devices that are currently unreachable.

Bubblewrap doesn't create a filesystem, it constructs a view by composing bind mounts, tmpfs, and overlays into a new mount namespace. Whatever ends up visible in that view, at the paths the loader searches, is what gets linked. The bytes' physical origin (host rootfs, an OCI layer, a network mount) is irrelevant to ld.so.

What the loader actually searches

At exec, ld.so resolves each NEEDED entry in the ELF (the soname, e.g. libdisplaysdk.so.2) by checking, in order:

RPATH/RUNPATH baked into the binary at link time (if any)
LD_LIBRARY_PATH env var
/etc/ld.so.cache (built by ldconfig scanning paths in /etc/ld.so.conf)
Default paths (/lib, /usr/lib, etc.)

All of these are resolved inside the mount namespace the process is running in. So the real question isn't "container or host" — it's "does the SDK .so appear somewhere in the union of bind mounts bwrap constructed for this specific sandbox invocation, at a path the loader will check."

This gives you a genuine architectural choice, not a constraint:

Option A — bake the SDK into every app's OCI image

Each client app's image includes libdisplaysdk.so as a layer, built at image-build time, ldconfig cache generated as part of that layer. Fully self-contained — the app never depends on anything bubblewrap injects at runtime beyond the kernel.

Pro: hermetic, reproducible, immune to a host-side SDK update silently changing behavior underneath an already-tested app.
Con: this directly fights the thing you were trying to solve last message — an SDK patch now requires rebuilding and re-pushing every app image through your marketplace pipeline, not just an OTA to the device. (OCI's content-addressable layers do dedupe on disk if every app derives from a shared base layer with an identical SDK .so, so storage isn't the real cost — the rebuild/redistribute cycle is.)
Option B — bind-mount the SDK from the host into the sandbox at launch

The SDK .so lives in exactly one place on the device (managed by your firmware/bundle OTA channel), and your launcher's bwrap invocation does something like:

bwrap \
  --ro-bind /var/lib/sdk-versions/2.x /usr/lib/displaysdk \
  --ro-bind /path/to/app-image-rootfs / \
  ...
  exec /app/bin/client-app

The app's container image never contains the SDK at all — it just links against a soname it expects to find at a well-known path, and the host supplies it at sandbox construction time.

This is exactly the pattern Flatpak uses — and it's worth calling out because Flatpak's sandboxing is bubblewrap, solving almost precisely your problem. Flatpak apps don't bundle GTK, libc, etc.; they declare a runtime (e.g. org.gnome.Platform//45), and that gets bind-mounted into the sandbox at /usr. The app's own files land at /app. Your client SDK is structurally the "runtime" in this analogy, and your app manifest (the one your PWA marketplace already tracks) is the natural place to declare which SDK major version an app requires.

Given what you told me about goals — SDK patches shouldn't need a new pipeline — Option B is the fit. An SDK OTA update becomes: replace the bytes at /var/lib/sdk-versions/2.x/, and every already-installed app picks it up on next launch, no rebuild, no repush.

The two gotchas Option B introduces

1. ld.so.cache staleness. The cache is built by ldconfig at whatever time it's run — if that happened at image-build time and the SDK wasn't present in that filesystem view yet, the cache won't know about it, and a bind-mount added later won't retroactively appear in it. Two clean fixes:

Skip the cache entirely for this one library — bake a fixed RUNPATH into the app binary pointing at the well-known mount path (e.g. $ORIGIN/../lib:/usr/lib/displaysdk), or set LD_LIBRARY_PATH in the bwrap environment. Either bypasses the cache and hits the directory directly — trivially fine for a small, fixed set of libraries.
Or run ldconfig as part of sandbox setup after the bind mounts are in place, before exec.

2. ABI discipline becomes load-bearing, not optional. Once multiple apps — built at different times against different SDK builds — are all resolving the same host-provided .so at runtime, a single shared copy per major version only works if that major version maintains strict ABI stability (this is exactly the symbol-versioning point from your first message, now with real teeth). The clean way to support this: don't bind-mount one universal SDK path, bind-mount a version-specific one, keyed off what the app's manifest declares:

/var/lib/sdk-versions/1.x/libdisplaysdk.so.1
/var/lib/sdk-versions/2.x/libdisplaysdk.so.2

Your launcher reads the app's manifest-declared SDK major version and bind-mounts the matching directory to the path the app expects. This lets you carry an old-ABI app forward across an SDK major bump without forcing a rebuild — you just keep the old major version's directory around on-device until nothing declares it anymore — while still giving every app the latest minor/patch within its major line for free on every launch.

One more small wrinkle worth flagging since you're on bubblewrap specifically rather than a full runc-based OCI runtime: bubblewrap has no concept of "the container's rootfs" as a privileged notion the way runc does — it's just bind mounts assembled by an unprivileged process. That's actually convenient for you here: it means "inject one extra directory from host into an otherwise-isolated app rootfs" is a first-class, cheap operation (one --ro-bind flag), not a special hybrid runtime feature you'd have to build.