# TODO

- FEATURE: Shift-click only moves the right video playhead.
- BUG: Fix "Window exceeds display area (use -W flag to resize)" warning firing when the screen actually is big enough.

## WIP

## Done

- FEATURE: Add an option to crop out the black bars around videos in GPU mode.
- FEATURE: Add an option to crop out the black bars around videos in SDL mode.
- BUG: Seeking while paused is only going to the keyframe and not then playing to the exact frame.
- BUG: Shift+A is not going to the previous frame, but instead is going to the previous keyframe.
- BUG: Backtick does not auto-align videos when the lower frame rate video is on the right. Fixed by switching from full-frame SSIM on ring-resident candidates to windowed multi-probe structural correlation over a configurable time window; ±0.5s on `` ` ``, ±1s one-sided on `[`/`]`. Candidate pool is seeded from the right FrameRing and extended via a barriered PacketRing walk when the ring doesn't span the window. Robust to SDR/HDR differences (normalized fingerprints cancel brightness/contrast) and framerate asymmetry (multi-probe scoring disambiguates motion aliasing).
