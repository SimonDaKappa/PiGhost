# Overview

The design is __asymmetrical__, with 3 parts:

## Protocol

The protocol is the shared contract the __client__ and __server__ are expected to adhere to.
- What they IPC layout looks like, etc.

This is a base implementation they both must include/link against.

## Server

The server is a static, non-developer-friendly object. It is meant to exist, and not be changed.
It provides the control, ipc ownership, and communication implementation of the __protocol__.
It does not define the protocol, it __enforces__ it.

## Client

The client is developer-friendly object. The goal is to abstract away all of the nitty gritty
details of the protocol and IPC.
- Hardware interaction
- Application state machine
- Timing requirements

Thus, it is __callback-heavy__. The client app provides its ways to "do thing X", but it does 
not have to define "how and when do I actually execute do thing X". 

The client links against a `.so` that provides the necessary abstraction, and lives within the 
simple world of "render this with this graphics API" and "perform this business logic". It does
not have to worry about synchronization, hardware resources, etc.


mgmt-socket detail you just added, then the tree.

Two adjustments

1. libwisp.h is misleading as a name if it's just the protocol. Nothing in wisp/protocol/ produces a .so called libwisp — it's a headers-only wire contract (shm layout, magic/version, frame structs) consumed by both wisp_server and wisp_client_sdk at compile time. I'd call the file wisp_wire.h (or wisp_proto.h) to avoid implying it's a linkable library in its own right — it's closer to common/ in the last tree than a "lib."

2. The mgmt socket doesn't belong in protocol/ at all, if the client can't access it. It's a private contract between wisp_server, the dashboard PWA, and the watchdog — the client SDK has zero business knowing its wire format. Putting its struct defs in the same shared header the client SDK includes would leak protocol surface across a boundary you specifically don't want crossed. It should live entirely inside wisp/server/, private, not installed, not exposed to wisp/client_sdk/ at all.
```
Tree
wisp/
├── protocol/                          # shared wire contract — client_sdk & server both consume
│   └── include/
│       └── wisp_wire.h                # shm header struct, DISP_MAGIC/VERSION, frame layout
│
├── server/                            # single daemon, own repo-in-repo, not a public SDK
│   ├── include/                       # not installed — server has no external consumers
│   │   └── (nothing public; internal headers live next to .c)
│   ├── src/
│   │   ├── main.c
│   │   ├── session_table.c            # up to 8 connected sessions
│   │   ├── promotion.c                # promote/demote state machine
│   │   ├── display_push.c             # active client's shm frames -> display HW
│   │   ├── shm_owner.c                # creates + owns wisp_wire.h segment(s)
│   │   ├── mgmt_socket.h              # PRIVATE — dashboard/watchdog wire format
│   │   └── mgmt_socket.c              # server<->dashboard PWA / watchdog, client can't touch
│   └── CMakeLists.txt                 # builds `wisp_server` executable, no .so
│
├── client_sdk/                        # THE public SDK — separately installable, versioned .so
│   ├── include/
│   │   └── wisp_client.h              # the ONLY header third-party client devs ever see
│   ├── src/
│   │   ├── client_control.c           # state machine + control-thread IPC (socket to server)
│   │   ├── client_shm.c               # attaches wisp_wire.h segment, validates magic/version
│   │   ├── client_sched.c             # affinity/sched_setscheduler for promotion
│   │   └── internal.h                 # not installed
│   ├── pkg/
│   │   ├── wisp_client.pc.in          # pkg-config
│   │   └── (yocto/buildroot recipe or .deb control)
│   └── CMakeLists.txt                 # installs to /usr/include /usr/lib, SOVERSION
│
└── CMakeLists.txt                     # top-level: add_subdirectory(protocol/server/client_sdk)
```
server/CMakeLists.txt and client_sdk/CMakeLists.txt both do target_include_directories(... PRIVATE ${CMAKE_SOURCE_DIR}/protocol/include) — private, not re-exported, since neither wisp_server nor third-party client apps should ever need to #include <wisp_wire.h> directly; it's an implementation detail both SDKs hide behind their own public API (wisp_client.h on the client side, nothing at all on the server side since there's no server SDK).

The control-thread socket (client_control.c) talks to server session-management, but that's a separate, client-facing socket/protocol from mgmt_socket.c's dashboard/watchdog channel — two different Unix domain sockets, two different wire formats, two different trust boundaries, and only one of them (wisp_wire.h's shm side, plus whatever minimal connect/promote/demote messages the client's own socket uses) is something wisp_client.h consumers are indirectly depending on. Worth naming that client-facing session socket's wire format too, maybe as wisp_wire.h's sibling (wisp_session_proto section within it, or a second small header) so it's clear it's part of the client SDK's private-but-versioned contract with the server, distinct from the truly-external-only mgmt socket.