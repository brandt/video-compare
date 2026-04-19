# Architecture

## What this is

`video-compare` plays two (or more) videos side-by-side, frame-synchronized, so you
can inspect encoding differences, compare filter chains, or score quality. FFmpeg
drives the pipeline side (demuxing, decoding, filtering, pixel format conversion);
libplacebo/Vulkan drives the rendering side (with an SDL3/software fallback).
Scopes (histogram, vectorscope, waveform) and metrics (PSNR, SSIM, VMAF) are
optional analysis layered on top.

## Layers

```
          app/
         /  |  \
        /   |   \
  display/  |    analysis/
        \   |   /     \
         \  |  /       \
          media/ ───────┘
            │
          core/
```

Arrows point toward dependencies. The one rule: **lower layers never include
higher-layer headers.** Two sanctioned upward edges exist and are listed under
"Known exceptions" below.

Each layer, one sentence:

- **`app/`** — entry point, orchestration, configuration. `main.cpp` parses
  argv; `video_compare.*` runs the producer/consumer thread graph that glues
  media/display/analysis together.
- **`display/`** — rendering and UI. Owns the SDL window, libplacebo GPU
  renderer, overlay text, controls, and frame export. The subsystems in
  `display/subsystems/` are the per-feature collaborators that `Display` calls
  into.
- **`analysis/`** — passive observers of frames. Scope windows
  (`analysis/scopes/`) host FFmpeg filter-based visualizers; metrics
  (`analysis/metrics/`) compute PSNR/SSIM/VMAF.
- **`media/`** — FFmpeg pipeline stages (demuxer, decoder, filterer, filter
  context, format converter) plus frame buffering
  (`media/buffering/frame_ring.h`) and timeline arithmetic
  (`media/playback/time_shifter.h`).
- **`core/`** — shared infrastructure with no outgoing edges to other
  subsystems: core types, FFmpeg error helpers, logging, string utilities,
  timers, concurrency primitives, data structures, SDL event introspection.
- **`assets/`** — embedded TTF font and window icon.

## Data flow

Per side (Left / Right / RightN), one thread per pipeline stage, connected by
bounded `Queue`s owned by `VideoCompare`:

```
Demuxer  ─▶  VideoDecoder  ─▶  VideoFilterer  ─▶  FormatConverter  ─▶  FrameRing
 (media/)      (media/)          (media/)          (media/)             (media/buffering/)
```

The main thread pulls from each side's `FrameRing`, advances the right-side PTS
through `TimeShifter`, and hands aligned packed-RGB frames (or native YUV in GPU
mode) to `Display::refresh()`. `Display` dispatches to either `GpuRenderer`
(libplacebo) or SDL's software path, overlays metadata and controls, and — if
enabled — forwards the frame to `ScopeWindow` filters and
`MetricsCalculator`.

`VideoCompare` tracks readiness with `ReadyToSeek` (one flag per processor
thread per side); seeks wait for all flags idle before rewinding the
queues. Exceptions propagate back via `ExceptionHolder`.

## Where new code goes

- New codec/filter/conversion behavior → `media/`
- New render mode, overlay, or interactive feature → `display/`
  (or `display/subsystems/` for a self-contained collaborator)
- New frame metric or scope visualization → `analysis/`
- New shared utility or zero-dependency type → `core/`
- New CLI option, option-file handling, or top-level orchestration →
  `app/`
- New embedded resource → `assets/`

Prefer splitting a new collaborator into `display/subsystems/` over growing
`display.cpp`; the existing split is the template.

## Include conventions

The Makefile has `-I.`, so includes are root-relative from the repo:

```cpp
#include "media/video_decoder.h"    // canonical
#include "core/strings/string_utils.h"
```

Same-directory relative includes are tolerated in `display/subsystems/` where
it's pervasive (and in `display_types.h` → `display_modes.h` for one
co-located pair). Everywhere else, use root-relative form.

## Known exceptions

Two intentional upward edges that violate the strict layering:

- **`analysis/scope_manager.h` → `app/config.h`** — pulls in `ScopesConfig`.
  Splitting `ScopesConfig` and `TimeShiftConfig` out of `app/config.h` into
  their owning subsystem headers is a deferred cleanup.
- **`core/strings/string_utils.cpp` → `media/video_decoder.h`** — only in the
  `.cpp`, needed because `stringify_decoder(const VideoDecoder*)` dereferences
  the pointer. The header uses a forward declaration, so this coupling does
  not propagate to callers.

## Build, run, smoke-test

```bash
make                                                     # build
./video-compare --help                                   # CLI reference
./video-compare -w 800x screenshot_1.jpg screenshot_2.jpg   # golden path
./video-compare --verbose -w 400x400 screenshot_1.jpg screenshot_2.jpg
# Verbose run prints FFmpeg/SDL versions and picks libplacebo if a Vulkan
# device is available; falls back to the SDL renderer otherwise.
```

No automated tests; exercise features interactively (help overlay, scope
windows, zoom/pan, frame save, GPU toggle) when touching `display/`.
