## Source Tree Reorganization

Reorganize the repo around stable subsystem boundaries that already exist in the code: app entry/orchestration, media pipeline, display/rendering, analysis/scopes, core/shared infrastructure, and docs/assets. The recommended approach is to finish the partially-started `display/` extraction, introduce top-level subsystem folders, and keep only build/config/metadata files at the root. This yields a more intuitive layout without optimizing for churn. **Priority: decouple headers** (especially `config.h` from display types) to unblock future subsystem changes.

## Phased Execution Plan

Organize moves into logically-independent phases that can be verified independently. Each phase compiles and tests cleanly before the next begins.

**Phase 1 — Create structure and move non-interdependent groups**
1. Create target directory structure: `app/`, `media/`, `display/`, `analysis/`, `core/`, `assets/` with subdirectories per below.
2. Move `core/` utilities (logging, FFmpeg, data structures, timing, strings, concurrency). These have minimal upward dependencies.
3. Move `assets/` (embedded fonts/icons). Independent, zero dependencies on other moves.
4. Move app entry point: `main.cpp` → `app/main.cpp`, `version.{h,cpp}` → `app/version.{h,cpp}`, `argagg.h` → `app/argagg.h`.

**Phase 2 — Decouple headers (critical linchpin)**
5. Create `app/public_types.h` with shared enums: `Side`, `DisplayMode`, `SelectionState`, `CropTargetSide`, etc. (not display-internal enums; use `display/display_types.h` for those).
6. Update `config.h` to import from `app/public_types.h` instead of `display/display.h` (line 5 of current config.h).
7. Audit and fix all includes so that `config.h` does not trigger compilation of the full display module.
8. Update Makefile source discovery (see below) so new subsystems are automatically buildable.

**Phase 3 — Move media pipeline**
9. Move demux/decode/filter/convert/playback-timing files into `media/`, organized as:
   - Root: `demuxer.{h,cpp}`, `decoder.{h,cpp}` (rename from video_decoder), `filterer.{h,cpp}` (rename from video_filterer), `filter_context.{h,cpp}`, `format_converter.{h,cpp}`
   - `media/playback/`: `time_shifter.h`
   - `media/buffering/`: `frame_ring.h`
10. Move app orchestration: `main.cpp` (already done in Phase 1), `video_compare.{h,cpp}` → `app/video_compare.{h,cpp}`, `config.h` → `app/config.h`.

**Phase 4 — Organize display (smallest, cleanest group)**
11. Move GPU rendering into display: `gpu_renderer.{h,cpp}` → `display/gpu_renderer.{h,cpp}` (primary renderer, not a pluggable backend abstraction).
12. Move display-coupled export: `jxl_saver.{h,cpp}` → `display/jxl_saver.{h,cpp}` (only exporter, tightly tied to display rendering pipeline).
13. Move UI/input: `controls.{h,cpp}` → `display/controls.{h,cpp}` or `app/ui_controls.{h,cpp}` (keyboard maps + help text; used by both main.cpp and display).
14. Move analysis windows: `scope_manager.{h,cpp}`, `scope_window.{h,cpp}` → `analysis/scopes/` (not display-internal; separate subsystem).

**Phase 5 — Move analysis**
15. Move analysis metrics: `vmaf_calculator.{h,cpp}`, `metrics_calculator.{h,cpp}` → `analysis/metrics/`.
16. Move analysis support: `sdl_event_info.{h,cpp}` → `analysis/` (if event analysis; otherwise `core/`).

**Phase 6 — Reorganize docs and finalize**
17. Move docs into consistent structure: `docs/design/`, `docs/usage/`, `docs/archive/`, etc.
18. Update Makefile to reflect new source layout (already done in Phase 2).
19. Create `docs/ARCHITECTURE.md` describing subsystem roles and where new files belong (so tree doesn't drift back).

## Target Structure

```
video-compare/
├── app/                           # Application entry, orchestration, configuration
│   ├── main.cpp                   # Entry point
│   ├── video_compare.h/cpp        # Session state; coordinates video/display/analysis
│   ├── config.h                   # Global config (decoupled from display via public_types.h)
│   ├── public_types.h             # Shared enums: Side, DisplayMode, SelectionState, CropTargetSide
│   ├── version.h/cpp              # Build metadata
│   ├── argagg.h                   # CLI parser (vendor library)
│   └── runtime_notes.h/cpp        # Runtime instrumentation/logging utilities
│
├── media/                         # FFmpeg decoding, filtering, conversion pipeline
│   ├── demuxer.h/cpp              # File reading, stream detection
│   ├── decoder.h/cpp              # Video decoding, hardware acceleration (rename: video_decoder)
│   ├── filterer.h/cpp             # Filter graph construction (rename: video_filterer)
│   ├── filter_context.h/cpp       # Filter state management
│   ├── format_converter.h/cpp     # Pixel format/bit-depth conversions
│   ├── playback/
│   │   └── time_shifter.h         # Playback speed/timing primitives
│   └── buffering/
│       └── frame_ring.h           # Circular buffer for frame storage
│
├── display/                       # Rendering, UI, display orchestration (already well-organized)
│   ├── display.h/cpp              # Main display class; owns SDL window, coordinates subsystems
│   ├── display_types.h            # Display-specific enums (DisplayMode, etc.)
│   ├── display_utils.h/cpp        # Shared display helpers (layout, colors, conversions)
│   ├── pixel_format_utils.h       # Bit-depth traits, pixel math templates
│   ├── gpu_renderer.h/cpp         # Primary GPU renderer (libplacebo/Vulkan/Metal)
│   ├── jxl_saver.h/cpp            # Frame export to JPEG-XL format
│   ├── controls.h/cpp             # Keyboard input maps, help text overlays
│   └── subsystems/                # 8 collaborator subsystems
│       ├── view_transform.h/cpp
│       ├── window_layout.h/cpp
│       ├── rgb_frame_cache.h/cpp
│       ├── difference_processor.h/cpp
│       ├── selection_manager.h/cpp
│       ├── image_saver.h/cpp
│       ├── metadata_panel.h/cpp
│       └── overlay_manager.h/cpp
│
├── analysis/                      # Quality metrics, monitoring, scopes
│   ├── scope_manager.h/cpp        # Scope window lifecycle
│   ├── scopes/
│   │   └── scope_window.h/cpp     # Histogram, vectorscope, waveform
│   ├── metrics/
│   │   ├── vmaf_calculator.h/cpp
│   │   └── metrics_calculator.h/cpp
│   └── sdl_event_info.h/cpp       # Event analysis utilities
│
├── core/                          # Shared infrastructure and primitives
│   ├── logging/
│   │   ├── filtered_logger.h/cpp  # Severity-filtered logging
│   │   └── side_aware_logger.h/cpp # Side-aware logging (left/right video context)
│   ├── ffmpeg/
│   │   └── ffmpeg.h/cpp           # FFmpeg initialization, resource management
│   ├── time/
│   │   └── timer.h/cpp            # Timing utilities
│   ├── concurrency/
│   │   └── row_workers.h          # Thread pool for parallel row processing
│   ├── data/
│   │   ├── queue.h                # Generic FIFO queue
│   │   ├── circular_buffer.h      # Ring buffer template
│   │   ├── sorted_flat_deque.h    # Sorted flat deque
│   │   └── core_types.h/cpp       # Fundamental types (VideoMetadata, AVFramePtr, etc.)
│   ├── strings/
│   │   └── string_utils.h/cpp     # String formatting, path utilities
│   ├── side_aware.h               # Enums for left/right video sides
│   └── single_decoder_mode.h      # Single-video playback mode flag
│
├── assets/                        # Embedded static resources
│   ├── fonts/
│   │   └── source_code_pro_regular_ttf.h
│   └── icons/
│       └── video_compare_icon.h
│
├── docs/
│   ├── design/                    # Architecture and design notes
│   │   └── Libplacebo-design.md
│   ├── usage/                     # User-facing documentation
│   │   └── README.md
│   ├── archive/                   # Historical notes
│   │   └── Project-structure-alt.md (delete after merge)
│   ├── ARCHITECTURE.md            # Subsystem roles and new-file placement guide
│   └── Project-structure.md       # This file
│
└── [root-level files: Makefile, .clang-format, .clang-tidy, etc.]
```

## Build System Changes

Update the Makefile to discover sources recursively instead of hard-coding directory paths.

**Current (line 72):**
```makefile
cpp_src = $(wildcard *.cpp) $(wildcard display/*.cpp) $(wildcard display/subsystems/*.cpp)
```

**New:**
```makefile
cpp_src = $(wildcard *.cpp) $(wildcard */*.cpp) $(wildcard */*/*.cpp)
```

This automatically discovers sources in any subdirectory at depth 1–2 (e.g., `app/*.cpp`, `media/playback/*.cpp`), making new subsystem folders immediately buildable without Makefile edits.

## Header Decoupling Strategy

The highest-impact change: **`config.h` must not include the full display module.**

**Current problem:** [config.h](../config.h) line 5 includes `display/display.h` just to access enums like `Side` (which is actually not from display at all). This forces every compilation unit that includes config to pull in the entire display.h dependency graph, blocking refactoring.

**Solution:**

1. Create `app/public_types.h` with app-layer enums needed across subsystems:
   - `enum Side { LEFT, RIGHT }` (video sides)
   - `enum class DisplayMode` (comparison modes shared by config)
   - `enum class SelectionState` (crop state)
   - `enum class CropTargetSide`
   - Shared structs like `VideoMetadata` (if app-level)

2. Keep display-internal enums in `display/display_types.h` (already exists):
   - Display-specific `DisplayMode` variants for rendering
   - Pixel layout details
   - Window state enums

3. Update `app/config.h` to depend only on `app/public_types.h` and `core/` headers, never on `display/display.h`.

4. Update all includes throughout codebase:
   - `video_compare.cpp` includes `app/public_types.h` for enum types
   - `display.cpp` includes `display/display_types.h` for internal enums
   - Config and app don't know about display implementation

This unblocks future reorganization without cascading breakage.

## Key Design Decisions

- **No `src/` wrapper:** The repo root *is* the source directory; subsystems are direct children.
- **gpu_renderer stays in `display/`:** It's the primary renderer, not a pluggable backend abstraction. Logically adjacent to `display.cpp`.
- **jxl_saver in `display/`:** Only exporter, tightly coupled to the display rendering pipeline. No separate `export/` folder.
- **controls.h in `display/`:** Keyboard maps and help overlays are display concerns, even though `main.cpp` uses them. Avoids circular dependency (display can't depend on app, so controls stay with display).
- **time_shifter in `media/playback/`:** Playback timing is media pipeline state, not core infrastructure.
- **frame_ring in `core/data/`:** Reused by both media buffering and display; lives in core.
- **Naming cleanup deferred:** File renames like `decoder.cpp` (from `video_decoder.cpp`) are incidental to this reorganization; treat them as optional per-phase touch-ups.

## Verification Checklist

After completing all phases:

1. **Subsystem ownership:** Every `.cpp/.h` pair is clearly owned by one subsystem (e.g., `media/decoder.cpp` is owned by media, not ambiguous).
2. **Header decoupling:** Run `grep -r “include.*display/display.h” app/ core/` and verify only expected includes remain (should be zero in `app/` and `core/`).
3. **Build discovery:** Verify `make clean && make` rebuilds all sources by running `grep cpp_src Makefile` and checking that all subsystem directories are covered by the recursive wildcard pattern.
4. **Contributor clarity:** New contributor should be able to answer “where does a new render feature go?” (→ `display/`) and “where does a new FFmpeg stage go?” (→ `media/`) from folder names alone.
5. **Docs consistency:** `docs/` taxonomy mirrors code: `docs/design/` for architecture, `docs/usage/` for user docs, etc. No design docs mixed with user docs.
6. **ARCHITECTURE guide:** `docs/ARCHITECTURE.md` exists and clearly states where new subsystems go, preventing future drift back to root-level chaos.

## Scope and Out-of-Scope

**Included:**
- Reorganizing around subsystem boundaries (not low-churn moves)
- Header decoupling (especially config.h)
- Creating proper public interfaces
- Updating Makefile and build system
- Docs restructuring
- **Changing public APIs** (no external consumers; free to refactor interfaces for clarity)
- Renaming files and symbols for consistency (e.g., `video_decoder` → `decoder`, `video_filterer` → `filterer`)

**Excluded:**
- Preserving git-blame history (moves are acceptable)
- Minimizing churn for churn's sake (reorganization takes priority)
