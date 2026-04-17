# libplacebo GPU Rendering Pipeline: Design Plan

## Status (2026-04-17)

**Phase 1 — video pipeline migration: COMPLETE.** With `--hwaccel videotoolbox`, 4K HDR 60fps plays smoothly. GPU renderer is no longer the bottleneck — software decode is, as expected.

**Phase 2 — HUD overlays: COMPLETE.** All SDL_Renderer-based HUD elements re-implemented via `pl_overlay`:
- Split line, progress dots, selection/crop rects (monochrome primitives)
- File labels + position times (with left-edge fade-out for long paths, swap-aware)
- Zoom factor, playback speed, frame counter (with loop-mode pulsing background), target seek position
- Video/UI FPS counters (default on)
- Message toast (2s hold + 1s fade)
- Help screen and metadata panel (scrollable, multi-line)

**Phase 3 — features needing RGB pixel access: NOT STARTED.** Subtraction mode, per-pixel color inspector, live PSNR/SSIM/VMAF overlay currently no-op or misbehave in GPU mode because the FrameRing holds native YUV frames. Planned fix: on-demand sws_scale only when these features are active.

**Phase 4 — features needing `pl_tex_download`: NOT STARTED.** Zoom/magnifier window, save-selected-area, full-screen screenshot. Need GPU→CPU readback after `pl_render_image`.

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

| Component | Current | libplacebo |
|-----------|---------|------------|
| YUV→RGB conversion | FormatConverter / sws_scale | automatic in `pl_render_image` |
| Color space (BT.2020→709) | zscale filter in VideoFilterer | automatic from `pl_frame.color` |
| Tone mapping (PQ/HLG→SDR) | zscale + tonemap filter | built-in: hable, mobius, reinhard, bt.2390, clip, spline |
| HDR passthrough | SDL_COLORSPACE_HDR10 texture | set target `pl_color_space` to match source |
| Scaling | not currently done in filter | `pl_render_params.upscaler` / `downscaler` |
| Dithering | not currently done | automatic at output bit depth |
| Peak detection | MaxCLL from metadata | `pl_peak_detect_params` (GPU histogram) |
| Video texture upload | SDL_LockTexture + memcpy (17ms) | `pl_map_avframe_ex` (<1ms, DMA) |
| Video rendering | SDL_RenderTexture (3μs) | `pl_render_image` (<1ms, GPU shader) |
| Display present | SDL_RenderPresent (40μs) | `pl_swapchain_submit_frame` + `swap_buffers` |

### What stays on CPU / needs alternative

| Component | Current | Replacement |
|-----------|---------|-------------|
| **Text rendering** | SDL_ttf → SDL_CreateTextureFromSurface | SDL_ttf → CPU surface → `pl_tex_upload` → `pl_overlay` |
| **Help overlay** | ~30 SDL text textures + semi-transparent rect | Composite as `pl_overlay` array on the target frame |
| **Metadata overlay** | ~20 dynamic SDL text textures | Same: `pl_overlay` |
| **Message/notification** | SDL text texture with alpha fade | `pl_overlay` with alpha |
| **Progress dots** | SDL_RenderLine / SDL_RenderRect | Render to small CPU surface → `pl_overlay`, or Vulkan draw commands |
| **Split line** | SDL_RenderLine | Render to 1-pixel-wide `pl_overlay`, or Vulkan draw |
| **Selection/crop rect** | SDL_RenderFillRect / SDL_RenderRect | `pl_overlay` or Vulkan draw |
| **Zoom window** | SDL_RenderReadPixels → texture | `pl_tex_download` from rendered frame → re-upload as `pl_overlay` |
| **Screenshot** | SDL_RenderReadPixels → PNG/JXL | `pl_tex_download` from rendered frame |
| **Scope windows** | Separate SDL_Renderer per window | Keep as-is (independent windows, not performance-critical) |

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

| File | Changes |
|------|---------|
| **display.h** | Replace SDL_Renderer/SDL_Texture members with pl_vulkan/pl_swapchain/pl_renderer. Add pl_tex arrays for overlays. Keep SDL_Window. |
| **display.cpp** | Major rewrite: init (Vulkan surface + libplacebo), texture upload → pl_map_avframe_ex, rendering → pl_render_image, HUD → pl_overlay, zoom/screenshot → pl_tex_download. |
| **video_compare.cpp** | Remove FormatConverter stage entirely. Pipeline becomes: decode → filter (minimal) → frame ring → display. Pass raw AVFrames to display instead of RGB-converted frames. |
| **video_compare.h** | Remove format_converters_ map. |
| **video_filterer.cpp** | Remove all tonemap/colorspace filters (they move to libplacebo). Filter chain becomes: fps + deinterlace + rotation + crop only. Remove output_pixel_format parameter. |
| **format_converter.cpp/h** | Delete entirely (libplacebo handles all conversion). |
| **config.h** | Add tone mapping algorithm selection (maps to pl_tone_map_function). |
| **main.cpp** | Expose tone mapping algorithm CLI flag. |
| **makefile** | Add `-lplacebo` to LDLIBS. |
| **scope_window.cpp** | No changes (keeps its own SDL_Renderer). |
| **png_saver.cpp, jxl_saver.cpp** | Screenshot source changes from RGB frame to pl_tex_download output. |

### Dependencies

Already installed on the build system:
- `libplacebo` 7.360.1 (Homebrew, linked by ffmpeg-full)
- `vulkan-loader` 1.4.x (Homebrew)
- `MoltenVK` (Homebrew, provides Vulkan→Metal translation)
- `shaderc` (Homebrew, runtime shader compilation)

Build: add `pkg-config --cflags --libs libplacebo` to makefile, or `-I/opt/homebrew/include -lplacebo`.

### Risk assessment

| Risk | Mitigation |
|------|------------|
| MoltenVK compatibility | mpv uses this exact stack on macOS; well-tested |
| First-frame shader stutter | `pl_cache` for persistent shader cache |
| Color accuracy vs current CPU path | libplacebo uses higher-precision math; result should be equal or better |
| HUD rendering complexity | Phase 2 can be deferred; initially skip HUD or use a minimal overlay approach |
| Split-view rendering | Two `pl_render_image` calls with `target.crop` — straightforward |
| Linux/Windows portability | Vulkan is native on both; no MoltenVK needed |
| Subtraction mode | Compute diff on CPU, upload as single frame — or use custom pl_shader |

### Performance expectations

Based on mpv benchmarks on Apple Silicon with `vo=gpu-next`:
- **4K HDR 60fps** with default tone mapping: achievable
- **4K SDR 60fps**: easily achievable
- **GPU overhead per frame**: <1ms for render, ~1ms for upload
- **Pipeline bottleneck shifts to: decoder** (software decode ~25fps per side at 4K; hardware decode via VideoToolbox removes this)

Combining libplacebo rendering with `--hwaccel videotoolbox` would enable full 4K 60fps end-to-end.

---

## Implementation notes (learned during Phase 1 + 2)

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

### Intentionally deferred / still broken in GPU mode

- **Subtraction mode, per-pixel inspector, live PSNR/SSIM/VMAF** — all read `frame->data[0]` assuming RGB, but frames are native YUV in GPU mode. Phase 3 will add on-demand sws_scale for these features only.
- **Zoom/magnifier window, save-selected-area, full-screen screenshot** — all need `SDL_RenderReadPixels` (SDL-renderer-only). Phase 4 will reimplement via `pl_tex_download` after `pl_render_image`.
- **`format_converter.cpp/h`** is still compiled and instantiated even though it's bypassed in GPU mode — delete after Phase 3.
