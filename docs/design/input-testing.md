# Input Testing

> [!TIP]  
> There is also a UNIX domain socket-based input API for external control documented in [design/input-socket.md](input-socket.md).

A small scripted-keystroke harness lets a test (or an AI agent, or anyone automating) drive video-compare without holding focus on the window or relying on OS-level keystroke injection. Commands in a text file are played back by a detached thread that pushes synthesized SDL events into the main loop's event queue, so every code path that a real keypress would trigger (seek dispatch, crop, loop mode, etc.) is exercised end-to-end.

Implementation: [app/debug_input_script.h](../../app/debug_input_script.h) / [debug_input_script.cpp](../../app/debug_input_script.cpp). Spawned from [app/video_compare.cpp](../../app/video_compare.cpp) in `VideoCompare::operator()()` right after the worker threads are launched.

---

## Enabling

Set `VIDEO_COMPARE_INPUT_SCRIPT=/path/to/script.txt` before launching:

```bash
VIDEO_COMPARE_INPUT_SCRIPT=my-script.txt ./video-compare left.mp4 right.mov
```

The env var is read once at startup. Absent or empty → no-op (normal interactive use). If set but the path can't be opened, a diagnostic is printed on stderr and the rest of the session runs interactively.

Set `VIDEO_COMPARE_INPUT_SCRIPT_LOG=1` to echo each command as it fires, with an `[input-script] …` prefix on stderr. Useful when a script isn't doing what you expect.

---

## Script syntax

One command per line. Whitespace-separated tokens. Blank lines and comment lines (`#…`, `//…`, or `;…`) are ignored.

| Command   | Args                         | Behavior
| --------- | ---------------------------- | -------------------------------------------------------------------------
| `sleep`   | `<seconds>`                  | Float seconds. E.g. `sleep 0.25`, `sleep 5`.
| `keypress`| `<key>`                      | Push `KEY_DOWN` then `KEY_UP` with a 5 ms gap.
| `keypress`| `<key> <count> [interval]`   | Press `<count>` times; sleep `<interval>` s between presses (default 0.03 s).
| `hold`    | `<key> <seconds>`            | `KEY_DOWN`, sleep, `KEY_UP`. Use when repeat-key behavior matters.
| `keydown` | `<key>`                      | Push `KEY_DOWN` only. Caller must match with `keyup`.
| `keyup`   | `<key>`                      | Push `KEY_UP` only.
| `modshift`| `on|off`                     | Set/clear the Shift modifier flag for subsequent key events.
| `modctrl` | `on|off`                     | Set/clear the Ctrl modifier.
| `modalt`  | `on|off`                     | Set/clear the Alt modifier.
| `modgui`  | `on|off`                     | Set/clear the GUI / Cmd / Meta / Super modifier (used for macOS-style shortcuts like `Cmd+Enter` fullscreen).
| `quit`    |                              | Push `SDL_EVENT_QUIT` so the main loop exits cleanly.
| `log`     | `<message>`                  | Print the remainder of the line to stderr as `[input-script] <message>`.

The script is parsed once at startup and executed top-to-bottom in a detached thread. There's no branching, looping, or conditionals — if you need logic, generate the script from a higher-level tool.

---

## Key names

Case-insensitive. Either a single character or a named key:

- **Single char**: `a`–`z`, `0`–`9`, `-`, `=`, `+`, `.`, `,`, `/`, `\`, `;`, `[`, `]`, `` ` ``, `?`, `(`, `)`.
- **Whitespace/control**: `space` (`spc`), `escape` (`esc`), `return` (`enter`), `tab`, `backspace` (`bsp`), `delete` (`del`), `insert` (`ins`).
- **Navigation**: `up`, `down`, `left`, `right`, `pageup` (`pgup`), `pagedown` (`pgdn`), `home`, `end`.
- **Function keys**: `f1` through `f12`.
- **Named equivalents**: `minus`, `plus`/`equals`, `period`/`dot`, `comma`, `grave`/`backtick`/`backquote`, `backslash`, `question`/`questionmark`, `leftparen`/`lparen`/`openparen`, `rightparen`/`rparen`/`closeparen`.

To press `+` in video-compare (which maps `+`/`=`/`KP_PLUS` to the same "shift right by 1 frame" action), use any of `+`, `=`, `plus`, `equals`. Don't wrap the key in quotes.

Unknown key names produce an `[input-script] line N: unknown key 'xyz'` diagnostic and skip the command.

---

## Example scripts

### Headline L1 check — pause, step back, step forward

```
# Let the buffer fill, pause, drain FrameRing history with L0 pivots then one
# more to trigger L1, then step forward and quit.
sleep 5.0
keypress space
sleep 3.0
keypress - 20 0.2
sleep 0.5
keypress = 15 0.2
sleep 0.5
quit
```

Run with `VIDEO_COMPARE_LOG_SEEK_TIMING=1` and `grep 'tier=' tmp/vc_l1.log | …` to see tier counts.

### Loop-mode eager materialize

```
# Play enough to fill PacketRing, then enter PingPong loop, let it run, exit.
sleep 7.0
keypress ,
sleep 6.0
keypress ,
sleep 1.0
quit
```

Combine with `VIDEO_COMPARE_LOOP_CAP_SEC=3` to cap the materialized decoded range.

### Shift+A frame step (both-sides seek)

```
sleep 5.0
keypress space
sleep 2.0
modshift on
keypress a 8 0.3
modshift off
sleep 0.5
quit
```

### Crop flow via keyboard

```
sleep 5.0
keypress space
sleep 2.0
# Enter crop selection, draw a rect via arrow keys, confirm, clear.
keypress k
sleep 0.2
keypress right 50 0.02
keypress down 30 0.02
keypress return
sleep 2.0
keypress backspace
sleep 1.0
quit
```

---

## How events reach the main loop

`start_from_env()` launches one detached `std::thread`. That thread reads the file, then for each command calls one of:

- `SDL_PushEvent` with a constructed `SDL_EVENT_KEY_DOWN` / `SDL_EVENT_KEY_UP` — key code, modifier mask, and `down` flag filled in. `windowID` is left at 0, which is fine: `Display::handle_event` only filters `windowID` for `SDL_EVENT_WINDOW_*`, not for keyboard events.
- `SDL_PushEvent` with `SDL_EVENT_QUIT` for `quit`.
- `std::this_thread::sleep_for` for `sleep`.

SDL's `SDL_PushEvent` is documented thread-safe, and the main thread's `SDL_PollEvent` loop in `VideoCompare::compare()` pulls events in the order they were pushed. There's no window focus requirement — events go straight to the queue regardless of which app owns the keyboard.

---

## Limitations

- **No branching or conditions**. If the desired behavior depends on runtime state (e.g., "wait until buffer is full"), over-estimate the sleep and rely on observable log output for verification.
- **No mouse events** yet. Timeline click, scroll-wheel, drag-select, and crop-rect drag are currently out of reach. A `click x y` / `wheel dy` extension is feasible but not implemented.
- **Single script per run**. No chaining, no include/source directives.
- **Timing jitter**. `sleep` is monotonic-clock `sleep_for`, but SDL event delivery is at main-loop cadence (typically 60–120 Hz). Scripts shouldn't assume sub-frame-interval precision.
- **No introspection back to the script**. The script can't read `tier=L1` results and branch. Validation lives in the surrounding shell pipeline (`grep`, `awk`, `diff` against expected tier counts).

---

## Adding new commands

The command dispatch is a flat `if/else if` chain in `run_script` inside [app/debug_input_script.cpp](../../app/debug_input_script.cpp). To add a new command:

1. Add a case to the chain that tokenizes and validates its args.
2. If a new SDL event type is needed, construct it the same way as the keyboard / quit cases (union-aliased `ev.type` plus the specific sub-struct fields).
3. Document the command in this file's command table.

Key-name additions go in `name_to_keycode()` in the same file.
