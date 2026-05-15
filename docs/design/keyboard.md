# Keyboard Input

See also: [Key bindings](../user/key-bindings.md) for a current snapshot of all bindings in table form.

This doc describes how SDL key events become user actions in `video-compare`: the dispatch path, the first-match-wins cascade that maps keycodes to handlers, the rules for transient keys, and how the help overlay derives its labels.

Key files:

- [src/display/display_input.cpp](../../src/display/display_input.cpp) — the entire event dispatch + key cascade. All keyboard handlers are defined here.
- [src/display/display.h](../../src/display/display.h) — declarations for the per-group handler helpers.
- [src/display/controls.cpp](../../src/display/controls.cpp) / [controls.h](../../src/display/controls.h) — the single source of truth for the user-facing key labels rendered in the `H` help overlay and printed by `--show-controls`.
- [src/display/subsystems/overlay_manager.cpp](../../src/display/subsystems/overlay_manager.cpp) — `rebuild_help()` consumes `get_control_sections()` to build the on-screen overlay.
- [src/display/display_utils.h](../../src/display/display_utils.h) — the slowdown ratios (`ZOOM_SLOWDOWN_RATIO`, `PLAYBACK_SPEED_SLOWDOWN_RATIO`, `RELATIVE_SEEK_SLOWDOWN_RATIO`) used when Shift / Ctrl is held.
- [src/app/debug_input_script.cpp](../../src/app/debug_input_script.cpp) / [debug_input_socket.cpp](../../src/app/debug_input_socket.cpp) — synthetic key-event injection paths that bypass SDL polling but reuse the same `handle_event` entry point.

All line numbers are anchors into the current tree, not stable references.

---

## 1. Event flow

```
SDL_PollEvent ──►  VideoCompare::compare()        (app/video_compare.cpp main loop)
                       │
                       ▼
                  scope_manager_->handle_event()  (consumed-by-scope-window?)
                       │ false
                       ▼
                  Display::handle_event(event)    (display_input.cpp:22)
                       │
                       ├─ SDL_EVENT_KEY_DOWN  ─► handle_key_down
                       ├─ SDL_EVENT_KEY_UP    ─► handle_key_up
                       ├─ SDL_EVENT_MOUSE_*   ─► handle_mouse_* / handle_wheel_*
                       ├─ SDL_EVENT_WINDOW_*  ─► handle_window_event
                       └─ SDL_EVENT_QUIT      ─► quit_ = true
```

A single polling loop in `VideoCompare::compare()` is the only place that calls `SDL_PollEvent`. The scope manager gets first refusal (so clicks inside a scope window stay inside it); whatever it doesn't consume reaches `Display::handle_event`. The debug input script and debug input socket are alternative producers — they synthesize `SDL_Event` values and feed them through the same dispatcher, so any scripted run exercises the production code path.

`begin_input_frame()` is called once per main-loop iteration before pumping events. It clears `playback_.transient_state` (the per-frame seek / play / nav deltas accumulated by handlers) and resets `toggle_scope_window_requested_`. Handlers themselves only *set* these flags; the main loop consumes them after dispatch finishes. This split keeps the dispatcher fully synchronous yet decoupled from the heavyweight seek/decode work.

`mark_input_received()` is the side-effect every input-producing event sets to flip `input_received_ = true`. The render path uses it to bypass an "idle skip" guard, ensuring the next refresh redraws even if no decoded frame changed (a key press alone can flip a UI toggle that needs to be reflected).

---

## 2. The key-down cascade

`handle_key_down` (display_input.cpp:228) is structured as a **first-match-wins cascade**. The keycode and modifier flags (`is_shift_down` / `is_ctrl_down` / `is_alt_down`) are computed once at the top, then handed to a sequence of `handle_*_keys` helpers. Each helper returns `bool` — `true` means "I consumed the key, stop the cascade":

```
if (handle_right_video_index_shortcut(...)) return;   // Ctrl+Shift+digit ─ highest priority
if (handle_crop_save_keys(...))             return;   // F / Shift+F / Shift+R/L/B/K / Backspace
if (handle_scope_window_keys(...))          return;   // F1-F3 / Shift+1-3
if (handle_window_size_keys(...))           return;   // Ctrl+W variants / Alt+Enter
if (handle_view_mode_keys(...))             return;   // 1-3, 0, M, S, T, I
if (handle_zoom_pan_keys(...))              return;   // 4-9, E, R, Shift+Z
if (handle_playback_keys(...))              return;   // SPACE, A, D, J, L, arrows, +/-, GRAVE, [ ]
if (handle_diff_keys(...))                  return;   // Y, U
handle_misc_keys(...);                                // catch-all: Z, H, ESC, P, Q, X, TAB, C, V
```

### Why first-match-wins

The cascade isn't an arbitrary ordering — it's structured so that **modifier-heavier groups are checked first**, with the unmodified catch-all (`handle_misc_keys`) running last. That makes adding new shortcuts safe: the most narrowly-scoped binding (e.g. `Ctrl+Shift+5`) doesn't have to defend itself against being shadowed by a broader binding (`5`) further down the chain.

The cascade also lets the same keycode mean different things based on modifiers without a giant `switch` in one place. Example: `Z` cycles through three handlers —

- `handle_zoom_pan_keys` claims **Shift+Z** (zoom-left magnifier; transient — see §4).
- `handle_misc_keys` claims **plain Z** (toggle the bottom dock).
- Both fall through when their modifier predicate doesn't match, so the right behavior fires for each modifier combination without any helper having to know about the others' bindings.

Similarly for `C`: Shift+C → zoom-right magnifier (in misc), Cmd/Ctrl+C → copy timestamp to clipboard (in misc), plain C → unused (falls through).

### Adding a new key

1. Pick the group whose semantics it fits (modifier-heavy → earlier; cross-cutting global toggle → `handle_misc_keys`).
2. Match the existing pattern in that helper: switch on the keycode, check modifiers, set the relevant state on a subsystem (`playback_`, `selection_`, `view_transform_`, `dock_`, …), return `true`.
3. If the action is gated by modifiers (e.g. only fires on Shift), make sure the helper **returns `false`** on the unmodified case so a later helper can claim it. This is how `Z` and `C` cooperate across groups.
4. Add a row to the corresponding section in [controls.cpp](../../src/display/controls.cpp) so the help overlay reflects it.

### Handler responsibilities

| Helper | Lines | What it owns |
|---|---|---|
| `handle_right_video_index_shortcut` | 260 | `Ctrl+Shift+1..9/0` → `set_slot_side(1, Side::Right(N))`. Highest priority because of the heavy modifier combo. |
| `handle_crop_save_keys` | 281 | `F` (save frames), `Shift+F` (save selection), `Shift+R/L/B` (per-side crop), `Shift+K` (auto-crop black borders), `Backspace` (clear crop). |
| `handle_scope_window_keys` | 327 | `F1/F2/F3` and `Shift+1/2/3` — toggle scope windows. The latter is checked before `handle_view_mode_keys` so Shift+digit doesn't toggle hide/show. |
| `handle_window_size_keys` | 350 | `Ctrl+W` (startup size), `Shift+W` (saved size), `Ctrl+Shift+W` (save current), `Alt+Enter` (fullscreen). Plain `W` is swallowed (no-op) so the keycap isn't accidentally bound to anything by a fallthrough. |
| `handle_view_mode_keys` | 397 | `1/2/3` (hide/show panels), `0` (subtraction), `M` (print metrics) / `Shift+M` (mode cycle), `S` (slot swap) / `Shift+S` (aspect view), `T` (texture filter), `I` (input alignment filter). |
| `handle_zoom_pan_keys` | 461 | `4-9` preset zooms, `E` (mouse-centered pan), `R` (reset), `Shift+Z` (transient zoom-left). Plain Z falls through. |
| `handle_playback_keys` | 501 | `Space`, `,`/`.` (loop modes), `J/L` (speed), `A/D` (buffer step) / `Shift+A/D` (frame nav), arrows + PageUp/Dn (seek), `+/-` (frame time-shift), `Ctrl/Alt +/-` (×10 / ×100), `\``/`[`/`]` (auto-align). |
| `handle_diff_keys` | 566 | `Y` / `Shift+Y` (cycle subtraction modes), `U` (luma-only). |
| `handle_misc_keys` | 599 | Last-chance handler: `Z` (dock toggle), `H` (help), `Esc` (quit), `P` (pixel print), `Q` (quality overlay), `X` / `Shift+X` (fps / state print), `Tab` / `Shift+Tab` (cycle right slot), `Shift+C` (zoom-right transient), `Cmd/Ctrl+C` (copy timestamp), `Cmd/Ctrl+V` (paste timestamp), plain `V` (metadata overlay). |

### Modifier scaling

Two scalar factors are derived once at the top of `handle_key_down`:

```cpp
relative_seek_scale  = (Shift || Ctrl) ? 1 / RELATIVE_SEEK_SLOWDOWN_RATIO  : 1   // ÷4
playback_speed_scale = (Shift || Ctrl) ? 1 / PLAYBACK_SPEED_SLOWDOWN_RATIO : 1   // ÷5
```

These flow into `handle_playback_keys` so that holding a modifier produces finer-grained seeking and speed adjustments. The mouse-wheel zoom uses `ZOOM_SLOWDOWN_RATIO` for the same effect in `handle_wheel_event`. The actual numeric constants live in [display_utils.h](../../src/display/display_utils.h).

Time-shift (`+/-`) ignores the global scaling and uses its own integer magnitude: `Ctrl` ⇒ ×10 frames, `Alt` ⇒ ×100 frames, otherwise ×1.


## 3. Mouse + wheel routing (for context)

`handle_event` doesn't keep keyboard and pointer paths separate at the subsystem layer — both can mutate the same playback / view state. A few notable interactions:

- **Mouse left-click** seeks to the clicked horizontal position. `Shift+left-click` becomes a "right-only seek" through `follower_side_for_input()`, which translates "the user means the visually-right video" into the underlying pipeline side (RIGHT normally; LEFT under swap).
- **Right-button drag** pans the view (no key involved).
- **Mouse wheel** zooms with the same Shift/Ctrl slowdown.
- **Dock intercept** — the very first check in `handle_mouse_button_event` (line 172) routes left-clicks inside the bottom dock to the picker subsystem before any seek/selection logic runs. Plain click → `set_slot_side(1, …)`; `Shift+click` → `set_slot_side(0, …)`. Returns early so the seek path doesn't also see the click.


## 4. Key-up handling (transient bindings)

`handle_key_up` (display_input.cpp:250) is intentionally minimal — only three keys care about release:

- `Shift+Z` while pressed → zoom-left magnifier visible. On release, `view_transform_.set_zoom_left(false)`.
- `Shift+C` while pressed → zoom-right magnifier visible. On release, `view_transform_.set_zoom_right(false)`.
- `X` while pressed → FPS overlay visible. On release, `show_fps_ = false`.

The key-up handler doesn't inspect modifiers, so releasing only Z / C / X clears the flag regardless of whether Shift is still being held when the key-up event fires. This is intentional — the alternative (only un-setting on Shift+Z key-up) would leave the magnifier stuck if the user releases Shift before Z.

The Z/C flags are set unconditionally by the down handlers in `handle_zoom_pan_keys` / `handle_misc_keys`. Their guards are about *when to turn them on*, not when to turn them off; the off-rule is owned solely by `handle_key_up`.


## 5. Special slot-aware bindings

A handful of keys interact with the multi-input slot mapping introduced for the dock (see also [the dock subsystem](../../src/display/subsystems/dock.h)):

- **`S` (slot swap)** — `handle_view_mode_keys` reads `displayed_left_side_` and `displayed_right_side_`, then calls `set_slot_side(0, old_right); set_slot_side(1, old_left)`. This generalizes the legacy binary LEFT↔RIGHT swap to handle any pair the user has selected via the dock.
- **`Tab` / `Shift+Tab`** — `handle_misc_keys` advances the visual-right slot through the configured RIGHT pipelines, **skipping** whichever pipeline currently occupies the visual-left slot so Tab never produces a degenerate same-side layout.
- **`Ctrl+Shift+1..9/0`** — direct-select a right pipeline into the visual-right slot via `set_slot_side(1, Side::Right(target))`. Index 0 routes to `9` (i.e. the 10th right) so the digit keys cover 1..10 in human-friendly order.
- **`+/-` (frame time-shift)** — adjusts the visually-right video. Under a binary swap (`swap_left_right_` true) the underlying RIGHT pipeline is on the visual left, so the sign flips: pressing `+` still advances *what the user sees on the right*. The compute is `direction × magnitude × swap_sign`. See also `docs/planning/Swap-seek.md` for the broader rationale.

The `Z` dock toggle is described in §2 and §1 of [the dock subsystem](../../src/display/subsystems/dock.h).


## 6. The help overlay and `--show-controls`

User-visible key labels are **not derived from the input handlers** — they live in [src/display/controls.cpp](../../src/display/controls.cpp) as a static `std::vector<ControlSection>`. `get_control_sections()` is the only accessor.

```
controls.cpp ────► get_control_sections()
                       │
                       ├─► main.cpp::print_controls()       ─► stdout for --show-controls
                       └─► OverlayManager::rebuild_help()   ─► on-screen H overlay
```

This is a **manual sync point**: any time a binding changes in `display_input.cpp`, the matching row in `controls.cpp` has to be updated by hand. The advantage of the split is that the help text can describe shortcuts at human granularity (grouping clipboard variants, describing modifier scaling, listing mouse-only behaviors that have no SDL keycode) without forcing a one-to-one map to handler code. The cost is the manual upkeep.

Sections are flat key-value pairs (`{key_label, description}`) grouped under titled headings: **Basic**, **Advanced**, **Mouse Controls**, **Other**. An empty `key` field is treated as a free-form paragraph (used in the Mouse Controls and Other sections).

The help overlay also supports scrolling via mouse motion (see `handle_mouse_motion_event:159`) — `OverlayManager::clamp_help_scroll` ensures the offset stays in range.
