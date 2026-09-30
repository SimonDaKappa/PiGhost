# RT state machine implementation: tracked tech debt

Running log of code smells and improvement ideas noticed while implementing the
server/client RT lifecycle state machine, deferred until all base events (steps
1-5) are implemented so the refactor has a full view of structural commonalities.
Not a design doc for new behavior -- just a punch list to revisit afterward.

## Code smells

- `wisps_control_plane_run()`'s poll loop and `handle_readable()`'s dispatch switch
  are both turning into god functions: state-transition logic, timer/deadline
  checks, and message parsing are all interleaved in the same few functions instead
  of being separable units.
- Message handling is one large `switch (type)` in `handle_readable()` with inline
  logic instead of per-message-type handler dispatch (e.g. a table/array of
  `{msg_kind, handler_fn}` pairs).
- State-specific timer/deadline handling (heartbeat timeout, eviction grace period)
  lives directly in the poll loop as ad hoc `if` blocks per state, rather than being
  owned by whatever "component" represents that state.
- `send_lock` (`pthread_mutex_t` on `wispc_ctx_t`) is likely no longer needed:
  sends are synchronous and scoped to their immediate call, and no external caller
  should be writing to the ctrl fd directly anymore -- only the control thread
  itself should touch the socket, with callbacks as the sole way for outside code to
  ask the control loop to do something. Worth confirming no code path still needs
  cross-thread send serialization before removing it.

## Design / readability / maintainability / extensibility improvements

- Split "current state handling" from "incoming message handling" into separate
  abstractions, so a state's entry/exit/timer behavior isn't scattered across the
  poll loop and the message switch.
- Formalize message handling: give each `wisp_msg_kind_t` a real handler
  (consistent signature), dispatched via table/lookup instead of a hand-written
  switch.
- Formalize states into components with their own timer hooks (e.g. cooperative
  eviction's grace-period deadline) that the poll loop calls into generically,
  instead of special-casing each state's deadline check inline.

## Redundant / no-longer-needed code

- `wispc_ctx_t::send_lock` (client_sdk) -- candidate for removal once confirmed no
  caller besides the control thread itself ever sends on the ctrl fd.
