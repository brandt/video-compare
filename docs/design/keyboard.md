# Keyboard Input

See also: **[Keyboard and Mouse Controls](user/keyboard-and-mouse-controls.md)** for a user-facing reference.

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

`handle_key_down` is structured as a **first-match-wins cascade**. The keycode and modifier flags (`is_shift_down` / `is_ctrl_down` / `is_alt_down`) are computed once at the top, then handed to a sequence of `handle_*_keys` helpers. Each helper returns `bool` — `true` means "I consumed the key, stop the cascade":

```
if (handle_right_video_select_shortcut(...)) return;  // Cmd/Ctrl+digit, Cmd/Ctrl+Arrow ─ highest priority
if (handle_crop_save_keys(...))             return;   // Cmd/Ctrl+S, Shift+F/R/L/B, K, Backspace
if (handle_scope_window_keys(...))          return;   // F1-F3 / Shift+1-3
if (handle_window_size_keys(...))           return;   // Ctrl+W variants, Cmd/Ctrl+Enter
if (handle_view_mode_keys(...))             return;   // 1, 2, O, Shift+M, S/Shift+S, I, Alt+T, Alt+I
if (handle_zoom_pan_keys(...))              return;   // Alt+1..6, E, R, Shift+Z
if (handle_playback_keys(...))              return;   // SPACE, (, ), A/,, D/., J, L, arrows, +/-, \, [, ]
if (handle_diff_keys(...))                  return;   // Y, U
if (handle_dock_action_keys(...))           return;   // `, M, B, N, X — dock focus + mark
handle_misc_keys(...);                                // catch-all: Z, ?, H, ESC, P, Q/Shift+Q, G/Shift+G, TAB, C, V, Cmd/Ctrl+R
```

### Why first-match-wins

The cascade isn't an arbitrary ordering — it's structured so that **modifier-heavier groups are checked first**, with the unmodified catch-all (`handle_misc_keys`) running last. That makes adding new shortcuts safe: the most narrowly-scoped binding (e.g. `Ctrl+Shift+5`) doesn't have to defend itself against being shadowed by a broader binding (`5`) further down the chain.

The cascade also lets the same keycode mean different things based on modifiers without a giant `switch` in one place. Example: `Z` cycles through two handlers —

- `handle_zoom_pan_keys` claims **Shift+Z** (zoom-left magnifier; transient — see §4).
- `handle_misc_keys` claims **plain Z** (toggle the bottom dock).
- Both fall through when their modifier predicate doesn't match, so the right behavior fires for each modifier combination without any helper having to know about the others' bindings.

Similarly for `C`: Shift+C → zoom-right magnifier (in misc), Cmd/Ctrl+C → copy timestamp to clipboard (in misc), plain C → unused (falls through).

The digit row (`1`..`6`) cooperates the same way across `handle_scope_window_keys` (Shift+1/2/3), `handle_view_mode_keys` (plain 1/2 for hide-show), and `handle_zoom_pan_keys` (Alt+1..6 preset zooms). `handle_view_mode_keys` returns `false` when `Alt` is held so the zoom handler claims the binding.

### Adding a new key

1. Pick the group whose semantics it fits (modifier-heavy → earlier; cross-cutting global toggle → `handle_misc_keys`).
2. Match the existing pattern in that helper: switch on the keycode, check modifiers, set the relevant state on a subsystem (`playback_`, `selection_`, `view_transform_`, `dock_`, …), return `true`.
3. If the action is gated by modifiers (e.g. only fires on Shift), make sure the helper **returns `false`** on the unmodified case so a later helper can claim it. This is how `Z` and `C` cooperate across groups.
4. Add a row to the corresponding section in [controls.cpp](../../src/display/controls.cpp) so the help overlay reflects it.

### Handler responsibilities

| Helper | What it owns |
|---|---|
| `handle_right_video_select_shortcut` | `Cmd/Ctrl+1..9/0` → `set_slot_side(1, Side::Right(N))`; `Cmd/Ctrl+Left/Right` → `cycle_right_slot(±1)`. Highest priority so plain digits / arrows can still seek or hide/show. |
| `handle_crop_save_keys` | `Cmd/Ctrl+S` (save frames), `Shift+F` (save selection), `Shift+R/L/B` (per-side crop), `K` (auto-crop black borders), `Backspace` (clear crop). |
| `handle_scope_window_keys` | `F1/F2/F3` and `Shift+1/2/3` — toggle scope windows. The latter is checked before `handle_view_mode_keys` so Shift+digit doesn't toggle hide/show. |
| `handle_window_size_keys` | `Ctrl+W` (startup size), `Shift+W` (saved size), `Ctrl+Shift+W` (save current), `Cmd/Ctrl+Enter` (fullscreen — `Cmd` on macOS, `Ctrl` elsewhere, via `is_primary_mod_pressed`). Plain `W` is swallowed (no-op) so the keycap isn't accidentally bound to anything by a fallthrough. |
| `handle_view_mode_keys` | `1/2` (hide/show panels), `O` (subtraction), `Shift+M` (mode cycle), `S` (slot swap) / `Shift+S` (aspect view), `Alt+T` (texture filter), `Alt+I` (input alignment filter), plain `I` (info / metadata overlay). Returns `false` when `Alt` is held on a digit key so `handle_zoom_pan_keys` can claim it. |
| `handle_zoom_pan_keys` | `Alt+1..6` preset zooms (1:1, 100%, 200%, 400%, 800%, 50%), `E` (mouse-centered pan), plain `R` (reset pan/zoom — `Cmd/Ctrl+R` falls through to `handle_misc_keys`), `Shift+Z` (transient zoom-left magnifier). Plain Z falls through to `handle_misc_keys`. |
| `handle_playback_keys` | `Space`, `(`/`)` loop modes (Shift+9 / Shift+0), `,`/`.` aliases of `A`/`D`, `J/L` (speed), `A/D` (buffer step) / `Shift+A/D` (decode-step), arrows + PageUp/Dn (seek), `+/-` (frame time-shift), `Shift +/-` (×10) / `Alt +/-` (×100), `\` symmetric auto-align, `[`/`]` directional auto-align. |
| `handle_diff_keys` | `Y` / `Shift+Y` (cycle subtraction modes), `U` (luma-only). |
| `handle_dock_action_keys` | `` ` `` (focus the LEFT entry), `M` (Keep), `B` (Toss), `N` (Skip), `X` (Keep focused, Toss every other entry). All require no modifier so chords like `Shift+M` fall through to `handle_misc_keys`. |
| `handle_misc_keys` | Last-chance handler: `Z` (dock toggle), `?` (Shift+/, help), `H` (HUD toggle), `Esc` (quit), `P` (pixel print), `Q` quality / `Shift+Q` metrics print, `G` FPS toggle / `Shift+G` state print, `Tab` / `Shift+Tab` (cycle right slot via `cycle_right_slot`), `Shift+C` (zoom-right transient), `Cmd/Ctrl+C` (copy timestamp), `Cmd/Ctrl+V` (paste timestamp), `Cmd/Ctrl+R` (reveal-in-Finder, macOS-only). |

The two platform-aware helpers `is_clipboard_mod_pressed` and `is_primary_mod_pressed` (both in `display_input.cpp`) gate Cmd-on-macOS / Ctrl-elsewhere bindings. They have identical bodies today but are kept separate so the divergence (e.g. clipboard rule changes vs. fullscreen rule changes) can land in only one place.

### Modifier scaling

Two scalar factors are derived once at the top of `handle_key_down`:

```cpp
relative_seek_scale  = Shift ? 1 / RELATIVE_SEEK_SLOWDOWN_RATIO  : 1   // ÷4
playback_speed_scale = Shift ? 1 / PLAYBACK_SPEED_SLOWDOWN_RATIO : 1   // ÷5
```

These flow into `handle_playback_keys` so that holding `Shift` produces finer-grained seeking and speed adjustments. The mouse-wheel zoom uses `ZOOM_SLOWDOWN_RATIO` for the same effect in `handle_wheel_event`. The actual numeric constants live in [display_utils.h](../../src/display/display_utils.h).

`Ctrl` is no longer a slowdown modifier — it's reserved for OS-style shortcuts (`Ctrl+S`, `Ctrl+W`, `Ctrl+Shift+digit`) and the platform-primary helper.

Time-shift (`+/-`) ignores the global scaling and uses its own integer magnitude: `Shift` ⇒ ×10 frames, `Alt` ⇒ ×100 frames, otherwise ×1.


## 3. Mouse + wheel routing (for context)

`handle_event` doesn't keep keyboard and pointer paths separate at the subsystem layer — both can mutate the same playback / view state. A few notable interactions:

- **Mouse left-click** seeks to the clicked horizontal position. `Shift+left-click` becomes a "right-only seek" through `follower_side_for_input()`, which translates "the user means the visually-right video" into the underlying pipeline side (RIGHT normally; LEFT under swap).
- **Right-button drag** pans the view (no key involved).
- **Mouse wheel** zooms with the same `Shift` slowdown.
- **Dock intercept** — the very first check in `handle_mouse_button_event` (line 172) routes left-clicks inside the bottom dock to the picker subsystem before any seek/selection logic runs. Plain click → `set_slot_side(1, …)`; `Shift+click` → `set_slot_side(0, …)`. Returns early so the seek path doesn't also see the click.


## 4. Key-up handling (transient bindings)

`handle_key_up` is intentionally minimal — only two keys care about release:

- `Shift+Z` while pressed → zoom-left magnifier visible. On release, `view_transform_.set_zoom_left(false)`.
- `Shift+C` while pressed → zoom-right magnifier visible. On release, `view_transform_.set_zoom_right(false)`.

The key-up handler doesn't inspect modifiers, so releasing Z / C clears the flag regardless of whether Shift is still being held when the key-up event fires. This is intentional — the alternative (only un-setting on Shift+Z key-up) would leave the magnifier stuck if the user releases Shift before Z.

The Z/C flags are set unconditionally by the down handlers in `handle_zoom_pan_keys` / `handle_misc_keys`. Their guards are about *when to turn them on*, not when to turn them off; the off-rule is owned solely by `handle_key_up`.

The FPS overlay (`G`) was formerly a transient binding (`X` held). It is now a regular toggle owned entirely by the down handler.


## 5. Special slot-aware bindings

A handful of keys interact with the multi-input slot mapping introduced for the dock (see also [the dock subsystem](../../src/display/subsystems/dock.h)):

- **`S` (slot swap)** — `handle_view_mode_keys` reads `displayed_left_side_` and `displayed_right_side_`, then calls `set_slot_side(0, old_right); set_slot_side(1, old_left)`. This generalizes the legacy binary LEFT↔RIGHT swap to handle any pair the user has selected via the dock.
- **`Tab` / `Shift+Tab`** — `handle_misc_keys` advances the visual-right slot through the configured RIGHT pipelines, **skipping** whichever pipeline currently occupies the visual-left slot so Tab never produces a degenerate same-side layout.
- **`Ctrl+Shift+1..9/0`** — direct-select a right pipeline into the visual-right slot via `set_slot_side(1, Side::Right(target))`. Index 0 routes to `9` (i.e. the 10th right) so the digit keys cover 1..10 in human-friendly order.
- **`+/-` (frame time-shift)** — adjusts the visually-right video. Under a binary swap (`swap_left_right_` true) the underlying RIGHT pipeline is on the visual left, so the sign flips: pressing `+` still advances *what the user sees on the right*. The compute is `direction × magnitude × swap_sign`, where magnitude is 1, 10 (Shift), or 100 (Alt). See also `docs/planning/Swap-seek.md` for the broader rationale.

The `Z` dock toggle is described in §2 and §1 of [the dock subsystem](../../src/display/subsystems/dock.h).


## 6. The help overlay and `--show-controls`

User-visible key labels are **not derived from the input handlers** — they live in [src/display/controls.cpp](../../src/display/controls.cpp) as a static `std::vector<ControlSection>`. `get_control_sections()` is the only accessor.

```
controls.cpp ────► get_control_sections()
                       │
                       ├─► main.cpp::print_controls()       ─► stdout for --show-controls
                       └─► OverlayManager::rebuild_help()   ─► on-screen `?` overlay
```

This is a **manual sync point**: any time a binding changes in `display_input.cpp`, the matching row in `controls.cpp` has to be updated by hand. The advantage of the split is that the help text can describe shortcuts at human granularity (grouping clipboard variants, describing modifier scaling, listing mouse-only behaviors that have no SDL keycode) without forcing a one-to-one map to handler code. The cost is the manual upkeep.

Sections are flat key-value pairs (`{key_label, description}`) grouped under titled headings. The current top-level layout is **Playback**, **Seek**, **Sync**, **Display**, **Scopes**, **Crop**, **Sources**, **Window**, **Misc**, **Mouse Controls**, **Other**. An empty `key` field is treated as a free-form paragraph (used in the Mouse Controls and Other sections).

The help overlay also supports scrolling via mouse motion (see `handle_mouse_motion_event`) — `OverlayManager::clamp_help_scroll` ensures the offset stays in range. It renders **after** the dock in both the SDL and GPU paths so the help table is always visible (and dims everything else with a semi-transparent black background); see `display_render_sdl.cpp::render_overlay_sdl` and the corresponding GPU block in `display_render_gpu.cpp`.
