# libplacebo GPU Rendering Pipeline: Design Plan

> **Post-refactor note (2026-04-18):** this document refers to the code as it was at the end of the libplacebo migration, when everything lived in `display.cpp` / `display.h`. The code has since been split across 11 collaborator classes and decomposed into focused sub-phases (phases 0–8,10-11 complete of the display-refactor plan). Key moves for reading this doc:
> - `rgb_frames_[...]`, `rgb_converter_[...]`, `ensure_rgb_frames` → `RgbFrameCache` in [rgb_frame_cache.h](../rgb_frame_cache.h).
> - `diff_buffer_`, `diff_planes/pitches_`, `diff_upload_frame_`, `update_difference`, `convert_to_packed_10_bpc` → `DifferenceProcessor` in [difference_processor.h](../difference_processor.h).
> - `save_image_frames_core`, `save_frame_image`, `save_selected_area` → `ImageSaver` in [image_saver.h](../image_saver.h). The SDL OSD capture stays as `Display::save_image_frames_sdl`.
> - `selection_state_`, `selection_start/end_`, `selection_wrap_`, `save_selected_area_`, `crop_mode_`, `crop_target_side_`, `pending_crop_request_`, `get_left_selection_rect`, `wrap_to_left_frame` → `SelectionManager` in [selection_manager.h](../selection_manager.h).
> - `global_zoom_level/factor_`, `move_offset_`, `global_center_`, `zoom_left/right_`, `compute_zoom_rect`, `window_to_video_position`, `video_to_zoom_space`, `compute_relative_move_offset`, `update_zoom_factor*`, `update_move_offset` → `ViewTransform` in [view_transform.h](../view_transform.h). Takes a `ViewTransformLayout` interface (Display::LayoutAdapter satisfies it; will be replaced by WindowLayout if phase 9 ever gets done).
> - `metadata_textures/surfaces_`, `left/right_metadata_`, `metadata_dirty_`, `build_metadata_textures`, `render_metadata_overlay`, `ensure_metadata_textures_current` → `MetadataPanel` in [metadata_panel.h](../metadata_panel.h).
> - `show_help_`, `show_metadata_`, `help_textures/surfaces_`, `pending_message_`/`message_*_`/`gpu_active_message_`, `rebuild_help_textures`, `render_help`, `set_pending_message`, `clamp_overlay_offsets` (partial) → `OverlayManager` in [overlay_manager.h](../overlay_manager.h).
> - `play_`, `buffer_play_loop_mode_`, `playback_speed_*_`, seek / navigation / shift_right_frames / auto_align fields → `PlaybackController` in [playback_controller.h](../playback_controller.h).
> - PSNR/SSIM/rgb_to_grayscale/format_pixel/get_and_format_rgb_yuv_pixel/get_rgb_pixel/convert_rgb_to_yuv/compute_frame_ssim → `MetricsCalculator` namespace in [metrics_calculator.h](../metrics_calculator.h).
> - `BitDepthTraits`, `clamp_u32`, `clamp_int_to_byte*` → [pixel_format_utils.h](../pixel_format_utils.h).
> - Free utilities + constants (colors, zoom/speed steps, `check_sdl`, `clamp_range`, `to_frect`, `AVFramePtr`, etc.) → [display_utils.h](../display_utils.h) / [display_utils.cpp](../display_utils.cpp).
>
> **Phase 10 (possibly_refresh decomposition):** Introduced `RenderContext` struct to capture per-frame invariants (frame keys, change flags, zoom rectangles, mouse position, drawable dimensions, HDR flags), eliminating redundant computation between the GPU and SDL rendering paths. Decomposed `possibly_refresh` from 1570 lines to a 55-line dispatcher + two sibling paths (`render_frame_gpu` / `render_frame_sdl`) + 6 GPU sub-phases + 6 SDL sub-phases organized by rendering responsibility. Result: ~25% reduction in display.cpp (5172 → 3858 lines).
>
> **Phase 11 (handle_event decomposition):** Decomposed `handle_event` from 560 lines to a 40-line dispatcher, 6 event-type handlers, and 9 first-match-wins key-group cascade helpers organized by semantic domain (playback, view modes, zoom/pan, diff, crop/save, scope, window, misc). All keyboard cases (~200) now clustered by feature area. Promoted lambdas `update_cursor_mode()` and `is_clipboard_mod_pressed()` to methods.
>
> The invariants and behaviors documented below are unchanged; only the addresses have moved, and the mega-functions have been decomposed into focused sub-phases.


## Status (2026-04-17) — all four phases complete

### Scope of "complete"

This migration replaces **GPU rendering and compositing**: YUV→RGB, colorspace / tonemap, scale, overlay composition, and swapchain present all run on the GPU via libplacebo. That work is done.

It does **not** make the pipeline GPU-resident end-to-end. When `--hwaccel videotoolbox` (or any other hw accelerator) is used:

```
demux → GPU decode (hardware) → av_hwframe_transfer_data → CPU frame
      → VideoFilterer (CPU, structural filters only)
      → pl_map_avframe_ex (CPU → GPU upload)
      → pl_render_image (GPU)
      → pl_swapchain_submit_frame + swap_buffers
```

The GPU→CPU readback after decode and the subsequent CPU→GPU upload are **still present**. A real zero-copy path would hand the decoder's output texture (e.g. a `CVPixelBuffer`/IOSurface on macOS) directly to libplacebo via `pl_map_avframe_ex` on an `AV_PIX_FMT_VIDEOTOOLBOX` frame. That's listed under *Future work* and has not been attempted.

In practice the Apple Silicon CPU absorbs the readback + upload at 4K HDR 60fps with plenty of headroom, so playback is smooth — but the performance is CPU-limited on the transfer path, not GPU-limited.

**Phase 1 — video pipeline migration: COMPLETE.** GPU rendering/compositing works. With `--hwaccel videotoolbox`, 4K HDR 60fps plays smoothly (see scope note above). Without `--hwaccel`, software decode is the bottleneck, as expected.

**Phase 2 — HUD overlays: COMPLETE.** All SDL_Renderer-based HUD elements re-implemented via `pl_overlay`:
- Split line, progress dots, selection/crop rects (monochrome primitives)
- File labels + position times (with left-edge fade-out for long paths, swap-aware)
- Zoom factor, playback speed, frame counter (with loop-mode pulsing background), target seek position
- Video/UI FPS counters (default on)
- Message toast (2s hold + 1s fade)
- Help screen and metadata panel (scrollable, multi-line)

**Phase 3 — features needing RGB pixel access: COMPLETE.** Subtraction mode, per-pixel color inspector, and live PSNR/SSIM/VMAF overlay work in GPU mode via an on-demand sws_scale cache (`rgb_frames_[kSideCount]`) populated only when at least one of those features is active. Known tradeoff: activating any of them drops framerate to ~20fps (CPU YUV→RGB per frame + CPU metrics). Future work: move to GPU via compute shaders.

**Phase 4 — features needing GPU→CPU readback: COMPLETE.** Scope of original plan revised: only the full-screen screenshot's OSD capture truly needs `pl_tex_download`. Zoom magnifier is just another set of `SideRenderOp`s; save-selected-area reuses the Phase 3 `rgb_frames_` cache. All three work in GPU mode now.


## Goal

Replace the CPU-side video processing pipeline (decode → filter → sws_scale → memcpy → SDL texture upload) with a GPU-accelerated pipeline using libplacebo's Vulkan renderer. Target: **4K HDR 60fps** on Apple Silicon, matching mpv's `vo=gpu-next` performance.


## Current pipeline (CPU-bound)

```
decoder(yuv420p10le)
  → VideoFilterer(zscale tone mapping, ~20ms)
  → FormatConverter(sws_scale yuv→rgb, ~17ms)
  → SDL_LockTexture + memcpy (~17ms)
  → SDL_RenderTexture (~3μs)
  → SDL_RenderPresent (~40μs)
  ≈ 60ms total per frame → ~16.5 fps at 4K
```


## Proposed pipeline (GPU-accelerated)

```
decoder(yuv420p10le)
  → pl_map_avframe_ex() [CPU→GPU upload, ~1ms]
  → pl_render_image() [GPU: YUV decode + color convert + tone map + scale, <1ms]
  → pl_swapchain_submit_frame() + swap_buffers() [present]
  ≈ 2-3ms total per frame → 60fps+ at 4K
```

Everything between decode and present moves to a single GPU call. No sws_scale, no FormatConverter, no texture memcpy.


## Architecture: Option A (libplacebo owns the swapchain)

### Overview

libplacebo creates a Vulkan swapchain via SDL's Vulkan surface, renders video frames directly to the display, and composites HUD overlays on top. SDL_Renderer is not used for the main window.

```
SDL_Window (SDL_WINDOW_VULKAN)
  └─ VkSurfaceKHR (from SDL_Vulkan_CreateSurface)
       └─ pl_vulkan (libplacebo Vulkan context)
            ├─ pl_swapchain (display output)
            ├─ pl_renderer (video processing + compositing)
            └─ pl_frame overlays (HUD text, UI elements)
```

### What moves to libplacebo

| Component                 | Current                                    | libplacebo
|---------------------------|--------------------------------------------|-------------
| YUV→RGB conversion        | FormatConverter / sws_scale                | automatic in `pl_render_image`
| Color space (BT.2020→709) | zscale filter in VideoFilterer             | automatic from `pl_frame.color`
| Tone mapping (PQ/HLG→SDR) | zscale + tonemap filter                    | built-in: hable, mobius, reinhard, bt.2390, clip, spline
| HDR passthrough           | SDL_COLORSPACE_HDR10 texture               | set target `pl_color_space` to match source
| Scaling                   | not currently done in filter               | `pl_render_params.upscaler` / `downscaler`
| Dithering                 | not currently done                         | automatic at output bit depth
| Peak detection            | MaxCLL from metadata                       | `pl_peak_detect_params` (GPU histogram)
| Video texture upload      | SDL_LockTexture + memcpy (17ms)            | `pl_map_avframe_ex` (<1ms, DMA)
| Video rendering           | SDL_RenderTexture (3μs)                    | `pl_render_image` (<1ms, GPU shader)
| Display present           | SDL_RenderPresent (40μs)                   | `pl_swapchain_submit_frame` + `swap_buffers`

### What stays on CPU / needs alternative

| Component                 | Current                                   | Replacement
|---------------------------|-------------------------------------------|--------------
| **Text rendering**        | SDL_ttf → SDL_CreateTextureFromSurface    | SDL_ttf → CPU surface → `pl_tex_upload` → `pl_overlay`
| **Help overlay**          | ~30 SDL text textures + semi-transparent rect | Composite as `pl_overlay` array on the target frame
| **Metadata overlay**      | ~20 dynamic SDL text textures             | Same: `pl_overlay`
| **Message/notification**  | SDL text texture with alpha fade          | `pl_overlay` with alpha
| **Progress dots**         | SDL_RenderLine / SDL_RenderRect           | Render to small CPU surface → `pl_overlay`, or Vulkan draw commands
| **Split line**            | SDL_RenderLine                            | Render to 1-pixel-wide `pl_overlay`, or Vulkan draw
| **Selection/crop rect**   | SDL_RenderFillRect / SDL_RenderRect       | `pl_overlay` or Vulkan draw
| **Zoom window**           | SDL_RenderReadPixels → texture            | `pl_tex_download` from rendered frame → re-upload as `pl_overlay`
| **Screenshot**            | SDL_RenderReadPixels → PNG/JXL            | `pl_tex_download` from rendered frame
| **Scope windows**         | Separate SDL_Renderer per window          | Keep as-is (independent windows, not performance-critical)

### Implementation phases

**Phase 1: Vulkan init + video rendering (biggest win)**

Replace video texture path only. Keep SDL_Renderer for HUD by rendering to a separate hidden window or surface.

1. Create SDL window with `SDL_WINDOW_VULKAN` flag
2. `SDL_Vulkan_CreateSurface()` → `VkSurfaceKHR`
3. `pl_vulkan_create()` with the surface, get `pl_gpu`
4. `pl_vulkan_create_swapchain()` → `pl_swapchain`
5. Create `pl_renderer` for video processing
6. Create `pl_cache` for shader caching (avoids first-frame stutter)
7. In pipeline: replace FormatConverter with `pl_map_avframe_ex()` — upload decoded AVFrame directly to GPU
8. In display refresh: replace SDL_UpdateTexture + SDL_RenderTexture with:
   ```c
   pl_swapchain_start_frame(swapchain, &sw_frame);
   pl_frame_from_swapchain(&target, &sw_frame);
   // Set target color space based on display HDR capability
   target.color = display_is_hdr ? pl_color_space_hdr10 : pl_color_space_srgb;
   pl_render_image(renderer, &left_frame, &target, &render_params);
   // Right side: render to same target with appropriate sub-rect
   pl_render_image(renderer, &right_frame, &target, &render_params);
   pl_swapchain_submit_frame(swapchain);
   pl_swapchain_swap_buffers(swapchain);
   ```
9. Remove: VideoFilterer tonemap chain, FormatConverter, X2RGB10LE conversion, SDL_LockTexture path
10. Keep: decoder, demuxer, frame ring, queue system, scope windows

**Render params configuration:**
```c
struct pl_render_params params = pl_render_fast_params;  // or pl_render_default_params
params.color_map_params = &(struct pl_color_map_params){
    .tone_mapping_function = pl_tone_map_hable,  // or mobius, reinhard, bt2390, clip
    // ... peak detection, gamut mapping
};
```

**HDR passthrough:** when display is HDR, set `target.color = source.color` — libplacebo passes through with no conversion. When display is SDR, libplacebo automatically tone-maps. No separate code paths needed.

**Phase 2: HUD overlays via pl_overlay**

Migrate text/UI rendering to libplacebo's overlay system.

1. Keep SDL_ttf for text surface creation (CPU)
2. Upload text surfaces to `pl_tex` via `pl_tex_upload()`
3. Attach as `pl_overlay` entries on the target frame before rendering:
   ```c
   struct pl_overlay overlays[MAX_OVERLAYS];
   int num_overlays = 0;
   // For each text/UI element:
   overlays[num_overlays++] = (struct pl_overlay){
       .tex = text_gpu_tex,
       .rect = { .x0 = x, .y0 = y, .x1 = x+w, .y1 = y+h },
       .mode = PL_OVERLAY_NORMAL,
       .repr = pl_color_repr_rgb,
       .color = pl_color_space_srgb,
   };
   target.overlays = overlays;
   target.num_overlays = num_overlays;
   ```
4. For semi-transparent backgrounds (help, metadata): render a solid-color `pl_tex` with alpha
5. For geometric primitives (split line, progress dots, selection rect): render to a small CPU surface, upload as overlay. Or use Vulkan draw commands after `pl_render_image` but before present.

**Phase 3: Split/comparison rendering**

The comparison tool's core feature — split view — needs special handling:

1. **Split mode:** render left frame to left portion of swapchain, right frame to right portion. Two `pl_render_image` calls with different `target.crop` rects.
2. **HStack/VStack:** render each frame to its half of the swapchain.
3. **Subtraction mode:** use libplacebo's shader system or compute the diff on CPU and render as a single frame.
4. The split position follows the mouse — `target.crop` changes per frame.

**Phase 4: Zoom, screenshot, scope integration**

1. **Zoom window:** after rendering, `pl_tex_download()` a small region from the rendered output, scale up on CPU or via a second `pl_render_image` to a sub-rect.
2. **Screenshot:** `pl_tex_download()` the full rendered frame to CPU, pass to PngSaver/JxlSaver.
3. **Scope windows:** keep as separate SDL_Renderer windows (they process decoded frames, not rendered output). No migration needed.

### Files affected

| File                              | Changes
|-----------------------------------|-----------
| **display.h**                     | Replace SDL_Renderer/SDL_Texture members with pl_vulkan/pl_swapchain/pl_renderer. Add pl_tex arrays for overlays. Keep SDL_Window.
| **display.cpp**                   | Major rewrite: init (Vulkan surface + libplacebo), texture upload → pl_map_avframe_ex, rendering → pl_render_image, HUD → pl_overlay, zoom/screenshot → pl_tex_download.
| **video_compare.cpp**             | Remove FormatConverter stage entirely. Pipeline becomes: decode → filter (minimal) → frame ring → display. Pass raw AVFrames to display instead of RGB-converted frames.
| **video_compare.h**               | Remove format_converters_ map.
| **video_filterer.cpp**            | Remove all tonemap/colorspace filters (they move to libplacebo). Filter chain becomes: fps + deinterlace + rotation + crop only. Remove output_pixel_format parameter.
| **format_converter.cpp/h**        | Delete entirely (libplacebo handles all conversion).
| **config.h**                      | Add tone mapping algorithm selection (maps to pl_tone_map_function).
| **main.cpp**                      | Expose tone mapping algorithm CLI flag.
| **makefile**                      | Add `-lplacebo` to LDLIBS.
| **scope_window.cpp**              | No changes (keeps its own SDL_Renderer).
| **png_saver.cpp, jxl_saver.cpp**  | Screenshot source changes from RGB frame to pl_tex_download output.

### Dependencies

Already installed on the build system:
- `libplacebo` 7.360.1 (Homebrew, linked by ffmpeg-full)
- `vulkan-loader` 1.4.x (Homebrew)
- `MoltenVK` (Homebrew, provides Vulkan→Metal translation)
- `shaderc` (Homebrew, runtime shader compilation)

Build: add `pkg-config --cflags --libs libplacebo` to makefile, or `-I/opt/homebrew/include -lplacebo`.

### Risk assessment

| Risk                                | Mitigation
|-------------------------------------|------------
| MoltenVK compatibility              | mpv uses this exact stack on macOS; well-tested
| First-frame shader stutter          | `pl_cache` for persistent shader cache
| Color accuracy vs current CPU path  | libplacebo uses higher-precision math; result should be equal or better
| HUD rendering complexity            | Phase 2 can be deferred; initially skip HUD or use a minimal overlay approach
| Split-view rendering                | Two `pl_render_image` calls with `target.crop` — straightforward
| Linux/Windows portability           | Vulkan is native on both; no MoltenVK needed
| Subtraction mode                    | Compute diff on CPU, upload as single frame — or use custom pl_shader

### Performance expectations

Based on mpv benchmarks on Apple Silicon with `vo=gpu-next`:
- **4K HDR 60fps** with default tone mapping: achievable
- **4K SDR 60fps**: easily achievable
- **GPU overhead per frame**: <1ms for render, ~1ms for upload

Bottleneck depends on the decode path:
- **Software decode**: decoder itself is the bottleneck (~25fps per side at 4K).
- **`--hwaccel videotoolbox`**: decoder is fast, but `av_hwframe_transfer_data` + `pl_map_avframe_ex` together constitute a GPU→CPU→GPU round-trip on the hot path. CPU absorbs it at 4K60 on Apple Silicon with headroom; if that ever becomes the bottleneck, the fix is to hand the hardware frame directly to libplacebo (see *Future work*).

---

## Implementation notes

### GpuRenderer wrapper API

New file `gpu_renderer.h/cpp` owns all libplacebo state: `pl_vulkan`, `pl_swapchain`, `pl_renderer`, per-side frame texture arrays (`frame_tex_[2][4]`), shared 1×1 white texture for monochrome primitives, and a slot vector of RGBA textures for text overlays (reused across frames — `pl_tex_recreate` is a no-op when dims match).

Three op types passed to `render()`:
- `SideRenderOp` — src rect in video coordinates + dst rect in FBO pixel coordinates. Caller computes dst via the existing `video_to_zoom_space` + `video_rect_to_drawable_transform` chain so zoom/split behave identically to the SDL path.
- `OverlayOp` — solid-colored filled rect (monochrome). All batched into one `pl_overlay` with `PL_OVERLAY_MONOCHROME` referencing `white_tex_`.
- `TextOverlayOp` — arbitrary RGBA source (e.g. SDL_ttf output) + dst position. Each becomes its own `pl_overlay` with `PL_OVERLAY_NORMAL`.

### Render pipeline inside `render()`

1. `pl_swapchain_start_frame`
2. `pl_frame_from_swapchain` to fill target; optionally override `target.color` for HDR passthrough
3. `pl_frame_clear` once with background color
4. For each `SideRenderOp`: `pl_render_image(renderer, &image, &target, &params)` with `params.background = PL_CLEAR_SKIP; params.border = PL_CLEAR_SKIP` so sides don't clobber each other
5. Final `pl_render_image(renderer, NULL, &target, &params)` pass composites all `target.overlays[]` (primitives first, then text)
6. `pl_swapchain_submit_frame` + `pl_swapchain_swap_buffers`

### Gotchas encountered

- **Libplacebo clears target by default.** Second `pl_render_image` would erase the first. Fix: `params.border = PL_CLEAR_SKIP; params.background = PL_CLEAR_SKIP`.
- **Zero-dim overlays trip an assertion.** libplacebo asserts `dst` extent is non-zero. Filter out degenerate rects (e.g. a just-started selection with 0×0 area) at the GpuRenderer level.
- **`pl_map_avframe_ex` racing with FrameRing clear on seek** caused a kernel panic (MoltenVK). Fix: only call `upload_frame` inside `possibly_refresh` gated by `has_updated_*_frame`, not once-per-iteration from `compare()`.
- **Split-view sub-pixel jitter.** When the split moves, the right-video's dst rect changes too, causing visible 1-pixel resampling jitter. Fix (Split mode only): render the right side to its FULL dst rect first, then paint the left on top clipped at `split_x`. Right side is stationary; only the left's clip changes.
- **`PL_OVERLAY_NORMAL` ignores per-part `color[]` / alpha multiplier.** To fade a text overlay, bake the alpha multiplier into the surface's alpha channel on CPU before upload (`row[x*4 + 3] *= keep_alpha`).
- **Libplacebo's convenience macros use C99 compound literals**, which don't compile in C++. Use named temporaries: `struct pl_vulkan_params vk_p = pl_vulkan_default_params; vk_p.surface = ...; pl_vulkan_create(log, &vk_p);`.
- **`PL_LIBAV_IMPLEMENTATION`** in `libplacebo/utils/libav.h` needs a single C translation unit. `pl_libav_impl.c` does that with `#define PL_LIBAV_IMPLEMENTATION 1` + `#include`.

### HUD layering compromise

libplacebo renders overlays in array order: primitives first (one batched `pl_overlay`), then text overlays (N `pl_overlay` entries). Full-screen help/metadata panels need a dim background below text, which conflicts with HUD text needing to be above HUD-background primitives. Rather than restructure the overlay ordering (would need interleaved primitive/text groups), the GPU path **suppresses HUD rendering while help or metadata is visible**. The panel's own dim bg fully covers what HUD would have drawn. Minor behavioral delta vs SDL (which draws HUD under the dim) but visually equivalent.

### Text clipping / fade (for long file paths)

When a file path is wider than `max_text_width_`:
- `clip_amount` pixels from the LEFT are hidden; a `gradient_amount` (≤24 px) ramp fades between invisible and fully-visible.
- GPU implementation: bake the alpha gradient into the rendered SDL_Surface's alpha channel on CPU (zero for clipped-out region, ramped up across the gradient region, unchanged beyond). Upload the modified surface. Position the overlay so the visible portion lands at the requested (x, y) for left-align, or ends at `x + width` for right-align.
- Background rect gets a hard edge; only text fades. Simpler than SDL's per-strip gradient and looks fine in practice.

### Help / metadata scroll math

`clamp_overlay_offsets` and `handle_scroll` both account for inter-line spacing via `count * HELP_TEXT_LINE_SPACING`. In GPU mode, `count` must be `help_surfaces_.size()` / `metadata_surfaces_.size()` (the SDL texture vectors are empty), otherwise the last section of the help screen stays stuck below the fold.

### Filterer changes

`VideoFilterer` takes a `gpu_color_processing` ctor flag. When set, `must_tonemap` is forced false AND the HLG→PQ passthrough filter is skipped — libplacebo handles all color conversion on the GPU. The filterer only applies structural transforms (fps, deinterlace, rotation, crop). `format_convert_video` in `video_compare.cpp` passes filtered frames through unchanged (no sws_scale) but still sets `frame_key` + `original_*` metadata since `FormatConverter` is bypassed.

---

## Phase 3 implementation notes

### On-demand RGB cache (`ensure_rgb_frames`)

GPU-mode FrameRing holds native YUV frames. Features that still need CPU pixel access — subtraction mode, per-pixel inspector, live PSNR/SSIM/VMAF — are wired to a lazy per-side `FormatConverter` + packed-RGB destination frame keyed by `frame_key`:

- Cache is only populated when at least one RGB-dependent feature is active (`need_rgb` gate at the top of `possibly_refresh`'s GPU path). Otherwise the pipeline stays YUV-only and GPU-fast.
- Dest format mirrors `requires_10_bpc() ? RGB48LE : RGB24` so existing helpers (`update_difference`, `get_rgb_pixel`, `rgb_to_grayscale`, `crop_rgb_frame`) work unchanged.
- `color_primaries` and `color_trc` are copied from the src frame to the dst so downstream savers (JxlSaver) preserve HDR metadata on saved output.

### Subtraction in GPU mode

`diff_buffer_` (already allocated by `reinitialize_video_dimensions`) is filled by `update_difference` from the two RGB frames, then handed to the GPU renderer via a reusable `diff_upload_frame_` AVFrame shell whose `data[0]` aliases `diff_buffer_`. Uploaded in place of the right frame; `right_needs_update` gates both the diff recompute and the upload (matches SDL path cadence).

### Quality metrics overlay

Live PSNR/SSIM is recomputed every frame the feature is on; VMAF is only computed on paused frames (it's slow) and cached until the frame changes. The three lines are rendered as a background `OverlayOp` + three `TextOverlayOp`s at the top-right; suppressed while help/metadata panels are visible, same as other HUD.

### Known perf hit

Activating subtraction or quality metrics drops framerate to ~20fps even with `--hwaccel videotoolbox` — the CPU pays for both sws_scale conversion and the metric computations. Acceptable for now; real fix is compute-shader-based subtraction/metrics (see *Future work* below).

---

## Phase 4 implementation notes

### Zoom magnifier (no readback needed)

Each active zoom pushes additional `SideRenderOp`s that sample directly from the per-side frame textures into bottom-corner dst boxes. Both corners show the same composited view (Split: right full + left clipped at `split_x`; HStack/VStack: split at the stack boundary when the src straddles it) so the user can switch corners when one obscures the area being inspected.

Gotchas worth remembering:

- **`ops` storage.** Fixed `std::array<SideRenderOp, 2>` became `std::vector` with `reserve(8)` — zoom can push up to 4 extra (2 sides × up to 2 slices each).
- **Sub-pixel jitter.** Initial implementation computed the src rect via `window_to_video_position` (floor/ceil to int video coords); as the mouse moved, the src width could fluctuate by 1 video pixel, amplified by dst scale → visible right-frame jitter. Fix: compute a *fixed* video-coord src extent (64 drawable pixels × current zoom factor), centered on the raw mouse — float only, no integer snaps.
- **Slider-line alignment.** The slider was drawn at the dst center; but the src is centered on the raw mouse while `split_x` is rounded to an integer video texel, so the true left/right boundary in the zoom was up to ~half-a-video-pixel × dst-scale drawable pixels off. Fix: draw the slider at `mx(split_x)` (`split_x` mapped through the zoom's src→dst transform) rather than at dst center.

### Save selected area (reuses Phase 3 RGB cache)

`save_selected_area_` was added to the `need_rgb` trigger; the GPU deferred block calls `possibly_save_selected_area(rgb_frames_[0], rgb_frames_[1])`. Bugs fixed along the way:

- **`file_stem` never populated in GPU mode.** It was assigned inside `rebuild_side_ui_textures`, which early-returned for GPU → save files came out named `__cutout_…`. Moved the stem assignment ahead of the GPU return (used by both paths for save filenames).
- **`update_right_video` latent crash in GPU mode** (SDL texture creation with `renderer_=nullptr`) — now guarded.
- **`save_selected_area` pixel_size** was inferred from display flags (`hdr_passthrough_`, `requires_10_bpc()`); wrong for GPU-mode RGB cache frames under HDR passthrough. Switched to `switch(frame->format)` — RGB24→3, RGB48LE→6, X2RGB10LE→4.
- **HDR extension detection generalised:** `.jxl` when `format == X2RGB10LE` **or** `color_trc ∈ {PQ, HLG}`. Previously only X2RGB10LE triggered it, and GPU mode's RGB cache is never X2RGB10LE → HDR content would have saved as 16-bit PNG (big and loses HDR metadata). Fix applied to both `save_image_frames` and `save_selected_area`.

### Full-screen screenshot (the one feature that truly needs `pl_tex_download`)

- **`GpuRenderer::render` was refactored** to extract a private `compose_frame(target, fbo_w, fbo_h, ops, overlays, text_overlays)` helper that does the overlay build + per-side renders + final overlay pass. Both `render()` (swapchain target) and the new `capture_osd()` call it. No behavioral change to `render()`.
- **`GpuRenderer::capture_osd(out_rgb24, pitch, w, h, ops, overlays, text_overlays)`** lazy-creates a host-readable RGBA8 `osd_capture_tex_` matching the current swapchain size, re-composes the same scene onto it with sRGB color (so HDR content tonemaps to SDR for the screenshot), then `pl_tex_download`s and packs RGB24 into the caller's buffer.
- **Target tex params needed `sampleable=true`, `renderable=true`, `host_readable=true`, `blit_dst=true`** — and the matching `pl_find_fmt` caps. Initial attempt missed `blit_dst` and failed `pl_frame_clear` validation (the clear uses a blit under the hood).
- **`pl_find_fmt` caps** must be cast to `pl_fmt_caps` in C++ (enum vs int signature — C99 compound literal workaround again).
- **OSD capture runs inside `possibly_refresh` BEFORE text `SDL_Surface`s are destroyed.** `TextOverlayOp.rgba_data` points at surface pixels; capture must run while they're still alive. Surfaces are destroyed immediately after.
- **`Display::save_image_frames`** was split into a thin public SDL wrapper (builds OSD via `SDL_RenderReadPixels`) and a shared `save_image_frames_core(left, right, osd)` that both paths call. GPU deferred block builds the OSD AVFrame via `capture_osd` then invokes the core.
- **OSD file extension is always `.png`** — matches SDL behavior; the OSD has already been tonemapped to SDR.
- **User-visible cost:** brief stall on the capture frame (extra full-res render + GPU→CPU download of `drawable_w × drawable_h × 4` bytes). One-shot, so acceptable.

---

## Future work (orthogonal to the migration)

- **GPU-resident hardware decode.** Today `av_hwframe_transfer_data` pulls decoded frames back to system memory before libplacebo re-uploads them. Skipping the readback by passing the hw-frame (e.g. `AV_PIX_FMT_VIDEOTOOLBOX` / `CVPixelBuffer`) straight to `pl_map_avframe_ex` would be a true zero-copy path. Requires libplacebo/MoltenVK support for importing the platform-specific surface — viable on macOS via IOSurface, and on Linux via VAAPI/DRM-PRIME. This is the biggest remaining optimization for hwaccel users.
- **GPU-side subtraction / metrics** — the Phase 3 ~20fps regression is pure CPU load. A compute-shader diff (or pl_shader custom hook) would bring the performance cost close to zero while the feature is active.
- **CLI flag for tone mapping algorithm** — expose `pl_tone_map_function` (hable, mobius, reinhard, bt2390, clip, spline).
- **`pl_cache`** — persistent shader cache to avoid first-frame shader-compile stutter on startup.
- **`pl_peak_detect_params`** — GPU histogram-based peak detection for HDR tonemapping when `MaxCLL` metadata is missing.
- **Delete `format_converter.cpp/h` passthrough path.** It's bypassed in GPU mode but still compiled/instantiated. The SDL renderer path needs it, but a refactor could limit instantiation to SDL-path only.
