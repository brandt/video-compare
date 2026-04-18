# Project structure

This doc describes the overall structure of the codebase.

## Current state (2026-04-18)

The project is split into two organizational zones: the **display module** (recently reorganized into a logical hierarchy) and **remaining modules** (still at root level pending wider reorganization).

### Display module — `display/` and `display/subsystems/`

**Location:** `display/` directory

**Core files:**
- `display.h` / `display.cpp` — Main orchestrator class. Owns SDL window management, input handling, frame timing, texture management. Composes 8 collaborator subsystems and coordinates their interactions.
- `display_types.h` — Shared enums and type definitions (DisplayMode, SelectionState, CropTargetSide, etc.)
- `display_utils.h` / `display_utils.cpp` — Shared utility functions and constants (layout calculations, text rendering helpers, pixel format conversions)
- `pixel_format_utils.h` — Bit depth traits, clamping helpers, packed format conversion (`BitDepthTraits<8>`, `BitDepthTraits<10>`)

**Subsystems — `display/subsystems/`**

Eight collaborator classes owned by `Display`, extracted during mega-function decomposition:

1. **`view_transform.h/.cpp`** — Zoom level, pan offset, and viewport calculations. Owns all zoom/pan state and math.
2. **`window_layout.h/.cpp`** — Window geometry, DPI scaling, fullscreen mode, font management. Provides readable factors (video-to-window width/height ratios) used throughout rendering.
3. **`rgb_frame_cache.h/.cpp`** — Caches RGB-converted frames for the left and right video sides. Owns `FormatConverter` instances.
4. **`difference_processor.h/.cpp`** — Computes frame differences for subtraction mode. Owns diff buffers, planes, and bit-depth-specific processing templates.
5. **`selection_manager.h/.cpp`** — Manages user selection and crop state. Stores selection geometry and crop requests.
6. **`image_saver.h/.cpp`** — Handles frame image export (JPEG-XL). Owns file naming and save counters.
7. **`metadata_panel.h/.cpp`** — Renders file metadata overlay (left/right side names, dimensions, current ROI). Owns texture/surface caches.
8. **`overlay_manager.h/.cpp`** — Manages transient overlays: help text (H key), quality metrics (Q key), message toasts. Owns help/message textures and scroll state.

**Architecture notes:**
- Each subsystem encapsulates a cohesive cluster of state and operations.
- `Display` coordinates subsystems via their public APIs; subsystems do not communicate directly.
- Includes use relative paths: subsystem files reference parent display utilities via `../` (e.g., `#include "../display_utils.h"`).
- Display module can be understood as a self-contained rendering + interaction subsystem of the larger app.

### Remaining modules — root level (pending reorganization)

**Video processing pipeline:**
- `video_compare.h` / `video_compare.cpp` — Main application class. Owns frame buffers, playback state, and coordinates video decoding and display updates.
- `video_filterer.h` / `video_filterer.cpp` — FFmpeg filter graph construction and management.
- `video_decoder.h` / `video_decoder.cpp` — Manages FFmpeg decoding, hardware acceleration, and frame retrieval.
- `demuxer.h` / `demuxer.cpp` — Opens media files, reads streams, extracts metadata.
- `format_converter.h` / `format_converter.cpp` — Converts between video formats (YUV → RGB, HDR → SDR, bit depth conversion).

**GPU rendering:**
- `gpu_renderer.h` / `gpu_renderer.cpp` — libplacebo GPU renderer. Provides tone mapping, color space conversion, and accelerated compositing via Vulkan/Metal.

**Analysis:**
- `scope_window.h` / `scope_window.cpp` — Histogram, vectorscope, and waveform monitoring windows.
- `vmaf_calculator.h` / `vmaf_calculator.cpp` — VMAF quality metric computation via libvmaf.

**Export:**
- `jxl_saver.h` / `jxl_saver.cpp` — Image export to JPEG-XL format.

**Utilities:**
- `core_types.h` / `core_types.cpp` — Fundamental types (AVFrame, frame key, metadata).
- `string_utils.h` / `string_utils.cpp` — String formatting, case conversion, paths.
- `config.h` — Global configuration state (window dimensions, paths, CLI args).
- `ffmpeg.h` / `ffmpeg.cpp` — FFmpeg initialization and resource management.
- `row_workers.h` — Parallel CPU work scheduling (thread pool).
- `controls.h` — Input key mapping and help text definitions.
- `metrics_calculator.h` / `metrics_calculator.cpp` — Free functions for PSNR, SSIM, pixel formatting (no state).

**Entry points:**
- `main.cpp` — Entry point; parses CLI args, creates `VideoCompare` app, runs main loop.
- `Makefile` — Build rules (searches root, `display/`, and `display/subsystems/` for source files).

## Proposed wider reorganization

The remaining root-level modules are candidates for grouping into functional subdirectories.

Proposed structure:

```
src/
  display/            (already organized)
  pipeline/           (video decoding and processing)
    video_compare.h/cpp
    video_filterer.h/cpp
    video_decoder.h/cpp
    format_converter.h/cpp
    demuxer.h/cpp
  
  gpu/                (GPU rendering)
    gpu_renderer.h/cpp
  
  analysis/           (analysis tools)
    scope_window.h/cpp
    vmaf_calculator.h/cpp
  
  export/             (export tools)
    savers/
      jxl_saver.h/cpp
  
  utils/              (utilities and types)
    string_utils.h/cpp
    core_types.h/cpp
    config.h
    ffmpeg.h/cpp
    controls.h
    row_workers.h
    metrics_calculator.h/cpp
  
  main.cpp, Makefile
```

**Pros:** Clearer logical grouping; easy to understand the role of each module.

**Cons:** More directory levels to navigate.
