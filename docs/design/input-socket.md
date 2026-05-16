# Input Socket (interactive)

A Unix-domain-socket control surface for video-compare. Unlike the one-shot
scripted harness (see [input-testing.md](input-testing.md)), this is a
bidirectional JSON-Lines protocol: a test driver can **inject keyboard and
mouse events** and **query live application state** (playback position, play
mode, window geometry, etc.), then decide what to do next. That makes it
possible to branch on observable state rather than over-estimating sleeps.

Implementation: [app/debug_input_socket.h](../../app/debug_input_socket.h) /
[debug_input_socket.cpp](../../app/debug_input_socket.cpp). Spawned from
[app/video_compare.cpp](../../app/video_compare.cpp) in `VideoCompare::operator()()`
immediately after the scripted harness. The two coexist — a session can load
a setup script via `VIDEO_COMPARE_INPUT_SCRIPT` and then switch to
interactive socket control.

Shared primitives (key-name vocabulary, `ModState`, `push_*` helpers) live in
[app/debug_input_common.h](../../app/debug_input_common.h) so the script and
socket can't drift.

---

## Enabling

Set `VIDEO_COMPARE_INPUT_SOCK=/path/to/sock` before launching:

```bash
VIDEO_COMPARE_INPUT_SOCK=tmp/vc.sock ./video-compare left.mp4 right.mov
```

The env var is read once at startup. Absent or empty → no-op (normal
interactive use). If the path is already in use, the server `unlink()`s it
before `bind()`ing, so a prior crashed run doesn't block the next launch.
On `bind()` or `listen()` failure, a diagnostic is printed on stderr and the
rest of the session runs normally.

Set `VIDEO_COMPARE_INPUT_SOCK_LOG=1` to echo every request and response to
stderr with an `[input-socket] …` prefix.

The server accepts **one client at a time**. The second `accept()` runs
only after the first client disconnects. This matches the expected
single-test-driver use case and keeps the implementation trivial.

---

## Protocol

JSON Lines: one request object per line (terminated with `\n`), one
response object per line back. Requests look like:

```json
{"cmd": "<name>", ...}
```

Responses always have the shape:

```json
{"ok": true,  "value": <...>}    // success
{"ok": true}                     // success, no payload
{"ok": false, "error": "..."}    // failure
```

`value` can be a string, number, boolean, object, or array depending on the
command. See **Commands** below.

---

## Commands

### `key` — inject a keyboard event

```json
{"cmd": "key", "action": "press", "key": "space"}
{"cmd": "key", "action": "press", "key": "a", "mods": ["shift"]}
{"cmd": "key", "action": "down",  "key": "a"}
{"cmd": "key", "action": "up",    "key": "a"}
```

- `action`: `"press"` (down + up), `"down"`, or `"up"`.
- `key`: any name accepted by the script harness — single chars (`a`–`z`,
  `0`–`9`, `-`, `=`, `,`, `.`, `/`, `\`, `;`, `[`, `]`, `` ` ``, space,
  `?`, `(`, `)`), named keys (`space`, `escape`, `return`, `tab`, `backspace`,
  `up`, `down`, `left`, `right`, `pageup`, `pagedown`, `home`, `end`, `delete`,
  `insert`, `grave`, `backslash`, `question`, `leftparen`, `rightparen`),
  and `f1`..`f12`. See [app/debug_input_common.cpp](../../app/debug_input_common.cpp).
- `mods` (optional): array of any subset of `"shift"`, `"ctrl"`, `"alt"`,
  `"gui"` (a.k.a. `cmd`/`super`/`meta`). Also published to SDL's global mod
  state via `SDL_SetModState`, so handlers that read `SDL_GetModState()`
  (wheel-zoom sensitivity, shift-click seek) see the same state.

### `mouse` — inject a mouse event

```json
{"cmd": "mouse", "action": "move",   "x": 400, "y": 300}
{"cmd": "mouse", "action": "button", "button": "left",  "down": true,  "x": 400, "y": 300}
{"cmd": "mouse", "action": "button", "button": "left",  "down": false, "x": 400, "y": 300}
{"cmd": "mouse", "action": "wheel",  "dy": 1, "x": 400, "y": 300}
```

- `action`: `"move"`, `"button"`, or `"wheel"`.
- Coordinates (`x`, `y`) are in **window-space logical pixels** — i.e. the
  same coordinates SDL delivers to real mouse events. Display converts them
  to drawable / video-space internally.
- `button`: `"left"`, `"middle"`, `"right"`, `"x1"`, `"x2"` (or their
  numeric equivalents `1`..`5` as strings).
- For `button` and `wheel`, `x` and `y` are optional — omit them to let the
  current mouse position stand.
- `mods` (optional): same modifier array as `key` above. Published to
  SDL's global mod state via `SDL_SetModState` before the event is
  delivered, so handlers that read `SDL_GetModState()` — e.g. the
  shift-click seek-scoping in `display_input` — see the requested state.
  Omit to leave mod state alone.

**Behind the scenes**: mouse actions call `SDL_WarpMouseInWindow` to update
SDL's internal cursor tracking before pushing the SDL event. This is
necessary because Display reads the current mouse position via
`SDL_GetMouseState` rather than from the event payload, so a raw
`SDL_PushEvent` without a warp would land the click at the wrong place.
**The OS cursor will visibly move** during tests — this matches how
general-purpose UI automation tools (xdotool, etc.) operate.

### `sleep` — delay the socket thread

```json
{"cmd": "sleep", "seconds": 0.25}
```

Pauses *the socket server thread*, not the main loop. Useful to let queued
SDL events reach the main loop before the next query.

### `quit`

```json
{"cmd": "quit"}
```

Push an `SDL_EVENT_QUIT` into the event queue. The main loop exits cleanly.

### `get` — query a single field

```json
{"cmd": "get", "field": "play_state"}
→ {"ok": true, "value": "PLAY"}
```

Fields:

| Field                   | Type    | Notes
| ----------------------- | ------- | ---------------------------------------------------------
| `play_state`            | string  | `"SEEK"`, `"LOOP >"`, `"LOOP <>"`, `"PLAY"`, `"PAUSE"`. Priority: SEEK > loop modes > play/pause.
| `left_pts`              | float   | Left-side playback position in seconds (`AV_TIME_BASE µs / 1e6`).
| `right_pts`             | float   | Active right-side playback position in seconds.
| `effective_time_shift`  | float   | Right-to-left time shift in seconds (user frame-shift + TimeShifter).
| `left_path`             | string  | Path of the left video (as supplied on the command line).
| `right_path`            | string  | Path of the currently-active right video.
| `window_size`           | object  | `{"w": <int>, "h": <int>}` in logical window coordinates.
| `drawable_size`         | object  | `{"w": <int>, "h": <int>}` in physical pixels.
| `swap`                  | bool    | Whether left and right are logically swapped.
| `uptime`                | float   | Seconds since the `VideoCompare` instance was constructed.
| `script_running`        | bool    | Whether a `VIDEO_COMPARE_INPUT_SCRIPT` worker thread is still executing.

### `status` — query every field at once

```json
{"cmd": "status"}
→ {"ok": true, "value": {"play_state": "PLAY", "left_pts": 12.345, ...}}
```

Returns an object keyed by every field name above. A query that fails for
one field does not fail the whole response — the value for that field is
`null` and an `"_error_<field>"` key carries the message.

### `ping`

```json
{"cmd": "ping"}
→ {"ok": true, "value": "pong"}
```

Health check. Round-trips without touching any application state.

---

## Example session (Python)

```python
import json, socket

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("tmp/vc.sock")
f = s.makefile("rw")

def call(msg):
    f.write(json.dumps(msg) + "\n")
    f.flush()
    return json.loads(f.readline())

# Let the buffer settle, pause, verify the state changed.
call({"cmd": "sleep", "seconds": 2.0})
call({"cmd": "key", "action": "press", "key": "space"})
call({"cmd": "sleep", "seconds": 0.3})
assert call({"cmd": "get", "field": "play_state"})["value"] == "PAUSE"

# Click 25% from the left of the timeline to seek.
w = call({"cmd": "get", "field": "window_size"})["value"]["w"]
call({"cmd": "mouse", "action": "button", "button": "left",
      "down": True,  "x": w * 0.25, "y": 30})
call({"cmd": "mouse", "action": "button", "button": "left",
      "down": False, "x": w * 0.25, "y": 30})

# Wait for the SEEK state to clear before sampling left_pts.
call({"cmd": "sleep", "seconds": 1.0})
left = call({"cmd": "get", "field": "left_pts"})["value"]
print(f"left PTS after seek: {left:.3f}s")

call({"cmd": "quit"})
```

## Example session (shell)

For one-off queries, `socat` and `jq` are convenient:

```bash
echo '{"cmd":"status"}' \
  | socat - UNIX-CONNECT:tmp/vc.sock \
  | jq '.value | {play_state, left_pts, right_pts}'
```

---

## Limitations

- **No main-loop synchronization primitive.** After pushing an event, poll
  the relevant field to observe the effect. A future `{"cmd":"wait_idle"}`
  that blocks until the SDL queue drains is feasible but not implemented.
- **No push notifications.** Queries are pull-only; nothing is sent to the
  client spontaneously. A future "notify on play_state change" channel
  could be bolted on without breaking the protocol.
- **Single client at a time.** The server `listen()`s with backlog 1 and
  serves clients sequentially.
- **Cursor-moving side effect.** Mouse actions warp the real OS cursor (via
  `SDL_WarpMouseInWindow`) to keep injected-event coordinates consistent
  with what Display reads from `SDL_GetMouseState`. Run tests on a
  dedicated machine / VNC session if the visible jumping is a problem.
- **Not thread-perfect for all reads.** Display-level getters
  (`get_play_state`, `get_swap_left_right`, etc.) read non-atomic fields
  owned by the main thread. In practice the values are scalars aligned on
  their natural boundary and reads are benign, but strictly this is a data
  race. The `PlaybackStateSnapshot` path (for `*_pts` fields) is mutex-
  protected and safe.
- **Fire-and-forget server shutdown.** The detached accept-thread does not
  participate in the main loop's exit; the process simply terminates. The
  socket file is cleaned up by `atexit`.

---

## Adding new commands / fields

1. **New command**: add a branch in `dispatch()` in
   [app/debug_input_socket.cpp](../../app/debug_input_socket.cpp) and
   implement its handler. Keep the JSON shape consistent — a success
   response should always carry `"ok": true` and (when applicable) a
   `"value"` payload.
2. **New `get` field**: add a case in `query_field()` and a name in
   `kAllFields` so `status` reports it.
3. **New SDL event type** (mouse/keyboard variants, gamepad, etc.): add a
   `push_*` helper in [app/debug_input_common.cpp](../../app/debug_input_common.cpp)
   so both the script and socket can use it.
4. **New key name**: extend `name_to_keycode()` in the common module.
