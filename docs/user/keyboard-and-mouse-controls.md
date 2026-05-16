# Keyboard and Mouse Controls

This table is a quick reference for keyboard controls.

The authoritative source is [controls.cpp](../../src/display/controls.cpp) (user-facing labels) and `display_input.cpp` handlers (actual behavior).

The "primary" modifier is `Cmd` on macOS and `Ctrl` on Linux/Windows. Where a shortcut uses it, both forms are noted as `Cmd/Ctrl+…`.

### Playback

| Key                | Action
|--------------------|-------
| Space              | Play / pause
| `(` (Shift+9)      | Toggle bidirectional in-buffer loop
| `)` (Shift+0)      | Toggle forward-only in-buffer loop
| J / L              | Speed –1 / +1 (with Shift: ÷5)
| A / `,`            | Move to previous frame in buffer
| D / `.`            | Move to next frame in buffer
| Shift+A / Shift+D  | Decode-and-step previous / next frame

### Seek

| Key                | Action
|--------------------|-------
| ←  /  →            | Seek ±1 s (with Shift: ÷4)
| ↑  /  ↓            | Seek ±10 s (with Shift: ÷4)
| PgUp / PgDn        | Seek ±600 s (with Shift: ÷4)

### Sync

| Key                | Action
|--------------------|-------
| `\`                | Auto-align symmetric (±0.5 s)
| `[` / `]`          | Auto-align backward / forward (1 s window)
| `+` / `-`          | Time-shift right ±1 frame
| Shift + `+`/`-`    | Time-shift right ±10 frames
| Alt + `+`/`-`      | Time-shift right ±100 frames

### Display / view

| Key                   | Action
|-----------------------|-------
| `<` / `>`             | Toggle hide-show left / right video
| H                     | Toggle hide-show HUD
| I                     | Toggle video info overlay
| O                     | Toggle subtraction mode
| Alt+1                 | Zoom 1:1
| Alt+2                 | Zoom 100% (x1)
| Alt+3                 | Zoom 200% (x2)
| Alt+4                 | Zoom 400% (x4)
| Alt+5                 | Zoom 800% (x8)
| Alt+6                 | Zoom 50% (x0.5)
| E                     | Re-center view around mouse
| R                     | Global re-center + reset zoom
| Shift+Z (held)        | Magnify around cursor (lower-left corner) — transient
| Shift+C (held)        | Magnify around cursor (lower-right corner) — transient
| Q                     | Toggle live quality-metrics overlay
| Y / Shift+Y           | Cycle subtraction modes forward / backward
| U                     | Toggle luminance-only subtraction
| Shift+M               | Cycle display mode (split / vstack / hstack)
| Shift+S               | Cycle aspect view mode
| Alt+T                 | Toggle bilinear / nearest-neighbor texture filter
| Alt+I                 | Toggle fast / high-quality input-alignment filter

### Scopes

| Key                | Action
|--------------------|-------
| F1 / Shift+1       | Toggle Histogram window
| F2 / Shift+2       | Toggle Vectorscope window
| F3 / Shift+3       | Toggle Waveform window

### Crop

| Key                | Action
|--------------------|-------
| Shift+L            | Interactively crop left video
| Shift+R            | Interactively crop right video
| Shift+B            | Interactively crop both
| K                  | Auto-crop black borders
| Backspace          | Clear all crops

### Sources

| Key                  | Action
|----------------------|-------
| S                    | Swap the two visual slots
| Tab / Shift+Tab      | Cycle visual-right slot through right pipelines
| Ctrl+Shift+1..9 / 0  | Direct-select right pipeline into visual-right slot
| Z                    | Toggle bottom video-picker dock

### Window

| Key                | Action
|--------------------|--------
| Cmd/Ctrl+Enter     | Toggle fullscreen
| Ctrl+W             | Restore startup window size
| Shift+W            | Restore saved window size
| Ctrl+Shift+W       | Save current window size

### Misc / clipboard

| Key                          | Action
|------------------------------|-------
| `?` (Shift+/)                | Toggle help overlay
| G                            | Toggle FPS overlay
| Shift+G                      | Print display state to console
| P                            | Print mouse position + pixel value to console
| Shift+Q                      | Print image similarity metrics to console
| Cmd/Ctrl+S                   | Save both frames + on-screen content as images
| Shift+F                      | Select a region and save cutouts
| Cmd/Ctrl+C                   | Copy current left-video timestamp to clipboard
| Cmd/Ctrl+V                   | Paste timestamp from clipboard and seek
| Escape                       | Quit

### Mouse Controls

Move the mouse horizontally to adjust the movable slider position.

Use the mouse wheel to zoom in/out on the pixel under the cursor. Pan the view by moving the mouse while holding down the right button.

Left-click the mouse to perform a time seek based on the horizontal position of the mouse cursor relative to the window width (the target position is shown in the lower right corner).

Hold Shift while left-clicking to move only the right video to the clicked position, leaving the left video's playhead in place. Useful for nudging a misaligned pair into sync without disturbing the reference side.

### Other

Hold `Shift` for smaller relative seek, playback-speed, and zoom adjustments where available.
Availability may depend on conflicts with application shortcuts or operating system bindings.
