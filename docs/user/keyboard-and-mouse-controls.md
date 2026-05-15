# Keyboard and Mouse Controls

This table is a quick reference for keyboard controls.

The authoritative source is [controls.cpp](../../src/display/controls.cpp) (user-facing labels) and `display_input.cpp` handlers (actual behavior).

### Playback

| Key                | Action
|--------------------|-------
| Space              | Play / pause
| `,`                | Toggle bidirectional in-buffer loop
| `.`                | Toggle forward-only in-buffer loop
| J / L              | Speed –1 / +1 (with Shift or Ctrl: ÷5)
| ←  /  →            | Seek ±1 s (with Shift or Ctrl: ÷4)
| ↑  /  ↓            | Seek ±10 s (with Shift or Ctrl: ÷4)
| PgUp / PgDn        | Seek ±600 s (with Shift or Ctrl: ÷4)
| A / D              | Move to previous / next frame in buffer
| Shift+A / Shift+D  | Decode-and-step previous / next frame
| `+` / `-`          | Time-shift right ±1 frame (Ctrl: ±10, Alt: ±100)
| `` ` ``            | Auto-align symmetric (±0.5 s)
| `[` / `]`          | Auto-align backward / forward (1 s window)

### Display / view

| Key                | Action
|--------------------|-------
| 1 / 2 / 3          | Toggle hide-show left / right / HUD
| 0                  | Toggle subtraction mode
| 4 / 5 / 6 / 7 / 8 / 9 | Zoom 1:1 / 50% / 100% / 200% / 400% / 800%
| E                  | Re-center view around mouse
| R                  | Global re-center + reset zoom
| Shift+Z (held)     | Magnify around cursor (lower-left corner) — transient
| Shift+C (held)     | Magnify around cursor (lower-right corner) — transient
| X (held)           | Show FPS overlay — transient
| Shift+X            | Print display state to console
| Q                  | Toggle live quality-metrics overlay
| T                  | Toggle bilinear / nearest-neighbor texture filter
| I                  | Toggle fast / high-quality input-alignment filter
| Y / Shift+Y        | Cycle subtraction modes forward / backward
| U                  | Toggle luminance-only subtraction
| Shift+M            | Cycle display mode (split / vstack / hstack)
| Shift+S            | Cycle aspect view mode
| V                  | Toggle metadata overlay
| H                  | Toggle help overlay
| Z                  | Toggle bottom video-picker dock

### Multi-input slots

| Key                  | Action
|----------------------|-------
| S                    | Swap the two visual slots
| Tab / Shift+Tab      | Cycle visual-right slot through right pipelines
| Ctrl+Shift+1..9 / 0  | Direct-select right pipeline into visual-right slot

### Window

| Key                | Action
|--------------------|--------
| Alt+Enter          | Toggle fullscreen
| Ctrl+W             | Restore startup window size
| Shift+W            | Restore saved window size
| Ctrl+Shift+W       | Save current window size

### Crop, save, scopes

| Key                | Action
|--------------------|-------
| F                  | Save both frames + on-screen content as images
| Shift+F            | Select a region and save cutouts
| Shift+L / Shift+R  | Interactively crop left / right video
| Shift+B            | Interactively crop both
| Shift+K            | Auto-crop black borders
| Backspace          | Clear all crops
| F1 / F2 / F3       | Toggle Histogram / Vectorscope / Waveform window
| Shift+1 / 2 / 3    | Same as F1 / F2 / F3 (alternate bindings)

### Misc / clipboard

| Key                          | Action
|------------------------------|-------
| Escape                       | Quit
| P                            | Print mouse position + pixel value to console
| M                            | Print image similarity metrics to console
| Cmd+C (macOS) / Ctrl+C       | Copy current left-video timestamp to clipboard
| Cmd+V (macOS) / Ctrl+V       | Paste timestamp from clipboard and seek

### Mouse Controls

Move the mouse horizontally to adjust the movable slider position.

Use the mouse wheel to zoom in/out on the pixel under the cursor. Pan the view by moving the mouse while holding down the right button.

Left-click the mouse to perform a time seek based on the horizontal position of the mouse cursor relative to the window width (the target position is shown in the lower right corner).

Hold Shift while left-clicking to move only the right video to the clicked position, leaving the left video's playhead in place. Useful for nudging a misaligned pair into sync without disturbing the reference side.

### Other

Hold `Ctrl` or `Shift` for smaller relative seek, playback-speed, and zoom adjustments where available.
Availability may depend on conflicts with application shortcuts or operating system bindings.
