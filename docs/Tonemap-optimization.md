# Tonemap Optimization Opportunities

## Status (2026-04-18)

**Superseded by libplacebo migration (`Docs/Libplacebo-design.md`) and display.cpp refactor (`phases 0–8,10-11 complete`).** In GPU mode (default on Apple Silicon + Vulkan/MoltenVK), the entire CPU tonemap chain is bypassed:
- `VideoFilterer` is constructed with `gpu_color_processing = true` — tonemap filters and HLG→PQ passthrough are both skipped.
- `FormatConverter` / `sws_scale` is bypassed in `format_convert_video` — filtered frames pass through unchanged.
- Native YUV frames go directly to `pl_map_avframe_ex` → `pl_render_image`, which handles YUV→RGB, color primaries / transfer conversion, and tone mapping on the GPU in <1ms per frame.

The optimizations below are still relevant for the SDL_Renderer fallback path (when Vulkan is unavailable).

---

Context: video-compare is used to compare the visual effects of different encoding settings on the same frame. Visual fidelity matters — subtle color shifts, banding, or clipping artifacts during tone mapping directly undermine the comparison. Users rely on the tool to show real differences between encodes, not differences introduced by the viewer's own processing.

## Current Filter Chain (measured)

For HDR content with default `ToneMapping::Auto`, `tone_adjustment == 1.0` (the common case), the filter string we construct is:

```
format=rgb48,zscale=p=bt709:t=iec61966-2-1:npl=<peak_nits>
```

What libavfilter actually builds (from `avfilter_graph_dump`):

```
buffer(yuv420p10le)
  -> fps
  -> auto_scale_0 (scale/swscale): yuv420p10le -> rgb48le      [A]
  -> format: rgb48le -> rgb48le                                 [B] no-op
  -> auto_scale_1 (scale/swscale): rgb48le -> gbrp16le          [C]
  -> zscale: gbrp16le -> gbrp16le                                [D] colorspace work
  -> buffersink
```

Then outside the graph:

```
FormatConverter (sws_scale): gbrp16le -> rgb24 (or rgb48le)     [E]
```

For `tone_adjustment != 1.0` (Relative mode with differing peak nits, or boost_tone != 1.0), the filter string is:

```
format=gbrpf32,zscale=t=linear:npl=<peak_nits>,tonemap=clip:param=<adj>,zscale=p=bt709:t=iec61966-2-1
```

## Costs (from profiling)

On 4K 10-bit HLG BT.2020 content (the measured HDR case), the tonemap path adds **~2.4 cores** of steady-state CPU and **~210 MB RSS** compared to SDR pass-through. The profile shows:

- `libswscale`: 1707 samples (conversions A, C, E)
- `libzimg`: 1090 samples (conversion D)
- `libavcodec`: ~1150 samples (unchanged vs SDR)

## Optimization #1: Remove `format=rgb48` before zscale [DONE]

**CPU impact: ~40-50% of tonemap cost eliminated (est. ~1 core saved)**
**Visual impact: none**

The explicit `format=rgb48` was actively harmful. zscale (libzimg) works natively with YUV and planar formats. Our `format=rgb48` forced packed RGB, which zscale cannot consume directly, so libavfilter auto-inserted two `scale` (swscale) nodes to bridge:

- `auto_scale_0`: `yuv420p10le -> rgb48le` (to satisfy `format=rgb48`)
- `auto_scale_1`: `rgb48le -> gbrp16le` (to satisfy zscale's planar input)

Dropping `format=rgb48` lets libavfilter negotiate directly. zscale accepts `yuv420p10le` natively. The resolved graph is now:

```
buffer(yuv420p10le) -> fps -> zscale(yuv420p10le -> yuv420p10le) -> buffersink
```

Zero auto-inserted conversion nodes. zscale operates entirely in native YUV space.

### Measured results (4K 10-bit HLG BT.2020, ffmpeg-full)

| metric                  |     before |      after |                   delta |
|-------------------------|-----------:|-----------:|------------------------:|
| user@5s                 |      25.86 |      22.22 |                **-14%** |
| user@15s                |      82.27 |      70.78 |                **-14%** |
| steady-state (Δ/10s)    | 5.64 cores | 4.86 cores |         **-0.78 cores** |
| RSS@5s                  |    3.97 GB |    2.76 GB |            **-1.21 GB** |
| RSS@15s                 |    4.28 GB |    4.17 GB |             **-110 MB** |
| CPU%                    |       559% |       482% |                    -77% |
| libswscale samples (3s) |       1707 |        270 |                **-84%** |
| libzimg samples (3s)    |       1090 |       2780 | +155% (native YUV path) |

libswscale collapsed to SDR-baseline levels (270 ≈ 274 on mainline). libzimg grew because zscale now handles the full colorspace conversion natively in YUV instead of receiving pre-converted RGB — the net is faster because two swscale hops are eliminated.

The large RSS@5s improvement (-1.21 GB) is from eliminating the 4K 16-bit RGB intermediates (rgb48le and gbrp16le buffers) that the auto_scale nodes allocated. At 15s the frame ring dominates so the gap narrows.

**Change:** removed `post_filters.push_back("format=rgb48")` in `video_filterer.cpp` (the `tone_adjustment == 1.0` branch of `must_tonemap`).

## Optimization #2: Specify zscale output format to skip FormatConverter

**CPU impact: eliminates conversion [E] (~10-15% of tonemap cost)**
**Visual impact: none to minor**

After zscale outputs `gbrp16le`, FormatConverter runs swscale to convert to `rgb24` (8-bpc) or `rgb48le` (10-bpc) for SDL texture upload. We can specify the output pixel format directly in the zscale filter args:

```
zscale=p=bt709:t=iec61966-2-1:npl=<nits>:format=rgb24
```

or add a trailing `format=rgb24` after zscale. This makes FormatConverter a no-op (src == dest format) or lets us skip it entirely. If the output format is already what SDL expects, the final sws_scale call just does a memcpy or becomes unnecessary.

**Risk:** minor. zscale's dither behavior when converting to 8-bit rgb24 may differ from swscale's. For a tool focused on faithful visual comparison, we should verify that the dither doesn't introduce per-pixel noise that masks real encode differences. A/B screenshot comparison of the same frame with and without this change would catch it.

## Optimization #3: Fix FormatConverter colorspace mismatch

**CPU impact: negligible**
**Visual impact: potentially significant for correctness**

`FormatConverter` is initialized with the **source decoder's** color space and range (`video_compare.cpp:350-351`):

```cpp
format_converters_[side] = std::make_unique<FormatConverter>(
    ..., video_decoders_[side]->color_space(),   // e.g. BT.2020
         video_decoders_[side]->color_range(), ...);
```

After zscale has already converted the frame to BT.709/sRGB, FormatConverter calls `sws_setColorspaceDetails()` with BT.2020 YUV coefficients on data that is now BT.709 planar RGB (`gbrp16le`). For a purely RGB-to-RGB bit-depth conversion (gbrp16le -> rgb24), swscale may ignore the YUV matrix entirely, making this harmless in practice. But if any code path triggers a YUV intermediate (e.g., if the output format were YUV, or if swscale uses a YUV fast path internally), the wrong matrix would shift colors.

**Fix:** when the filter chain includes tonemapping, pass `AVCOL_SPC_BT709` and `AVCOL_RANGE_JPEG` (full range) as the source colorspace to FormatConverter, since zscale's output is BT.709 full-range RGB. Or propagate the output colorspace from the filtered frame's metadata rather than the decoder's.

**Risk:** behavioral change. Currently, for non-tonemapped SDR content, the decoder's color space is correct. The fix needs to be conditional on whether tonemapping ran. Visual verification required: compare a known HDR frame's colors before and after the fix.

## Optimization #4: Tonemap curve choice

**CPU impact: ~5-15% of tonemap cost (uncertain, needs benchmarking)**
**Visual impact: significant — changes highlight rendering**

Currently hardcoded as `tonemap=clip`. Available alternatives: `hable`, `mobius`, `reinhard`, `bt2390`. These differ in how they compress highlights:

- `clip`: hard-clips values above the target range. Cheapest computation, but destroys highlight detail. For encode comparison, clipped highlights in the viewer could mask real differences between encodes that preserved or lost those highlights.
- `hable`: S-curve (Uncharted 2 filmic). Preserves more highlight detail with a smooth rolloff. Moderate cost.
- `mobius`: smooth transition near the knee, linear below. Good balance of speed and quality. Preserves shadow/midtone accuracy while compressing highlights.
- `reinhard`: global operator, simple division. Compresses entire range slightly, which can reduce contrast in shadows/midtones — potentially undesirable when comparing encode quality in those regions.
- `bt2390`: ITU-R BT.2390 EETF. Most standards-compliant, most computationally expensive.

For a comparison tool, `mobius` or `hable` are likely better defaults than `clip` — they don't introduce hard discontinuities that could mask or fake differences between encodes. The `clip` method can turn a subtle highlight difference between two encodes into "both clipped to white, looks identical," which is misleading.

**Risk:** any change to the tonemap curve changes the visual appearance of all HDR content. Users who have calibrated their perception to the current `clip` behavior will notice. Should be user-selectable via a CLI flag, with the current `clip` as a named option for backward compatibility.

## Optimization #5: Cache tonemapped frames

**CPU impact: eliminates re-tonemapping on pause, scrub-back, redraw**
**Visual impact: none (identical output)**

During paused playback, window resize, or scrubbing backward into already-buffered frames, the same frame is re-filtered and re-tonemapped. Since tone mapping is deterministic (same frame + same npl = same output), caching the tonemapped result keyed on `(pts, npl, filter_generation)` avoids redundant work.

The frame ring already stores decoded frames. An additional "tonemapped frame" cache alongside it (or as a parallel ring of filtered frames) would cover the common re-display case. Memory cost is one extra frame per cached entry (4K rgb24 = ~25 MB, rgb48 = ~50 MB), bounded by the ring size.

**Risk:** low. Cache invalidation is straightforward — invalidate on filter-graph reinit, npl change, or boost_tone change. No visual impact since the cached result is identical to a fresh computation.

## Optimization #6: SDL3 HDR passthrough (replaces custom GPU tonemap)

**CPU impact: eliminates entire tonemap cost (~2.4 cores freed)**
**Visual impact: more faithful HDR rendering — display sees native HDR signal**

Instead of writing a custom Metal tonemap shader, leverage SDL3's built-in HDR rendering pipeline. SDL3 3.4.4 (already in use) has full HDR support: HDR-aware renderer, texture colorspace metadata, display HDR detection, and a built-in GPU-side tonemap (Chrome-derived algorithm) for when content headroom exceeds display capability.

### Current state (what needs to change)

| component | current | HDR passthrough |
|---|---|---|
| renderer | `SDL_CreateRenderer(window_, NULL)` — defaults to `SDL_COLORSPACE_SRGB` | `SDL_CreateRendererWithProperties` with `SDL_COLORSPACE_SRGB_LINEAR` |
| textures | `SDL_CreateTexture(..., SDL_PIXELFORMAT_ARGB2101010, ...)` — no colorspace | `SDL_CreateTextureWithProperties` with `SDL_COLORSPACE_HDR10`, headroom from MaxCLL |
| filter chain | always tonemaps HDR → SDR via zscale | skip tonemap when display is HDR; pass PQ/BT.2020 through |
| format converter | outputs rgb24/rgb48le (SDR) | outputs 10-bit packed (ARGB2101010) retaining HDR metadata |
| display detection | none | `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN` + `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` |

### Architecture

```
HDR display detected?
  ├─ YES (HDR passthrough):
  │    decoder(yuv420p10le, PQ/HLG, BT.2020)
  │      → [HLG only: lightweight zscale=t=smpte2084 to convert HLG→PQ]
  │      → FormatConverter: yuv420p10le → packed ARGB2101010
  │      → SDL HDR10 texture (PQ, BT.2020, headroom from MaxCLL)
  │      → Metal layer: wantsExtendedDynamicRangeContent=YES
  │      → Display renders natively (GPU handles excess headroom)
  │
  └─ NO (existing SDR tonemap, unchanged):
       decoder(yuv420p10le, PQ/HLG, BT.2020)
         → VideoFilterer: zscale(p=bt709:t=iec61966-2-1:m=bt709)
         → FormatConverter: → rgb24/rgb48le
         → SDL SDR texture
         → Display renders as SDR
```

### Implementation plan

**Phase 1: HDR renderer + display detection**
- Replace `SDL_CreateRenderer` with `SDL_CreateRendererWithProperties` using `SDL_COLORSPACE_SRGB_LINEAR` output colorspace.
- Query `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN` after window creation.
- Handle `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` to track HDR state dynamically.
- Store `hdr_display_available_` flag in Display class.
- This phase alone changes nothing visually — it just detects capability.

**Phase 2: HDR texture creation**
- When HDR display is available AND content is HDR, create textures via `SDL_CreateTextureWithProperties` with:
  - `SDL_PIXELFORMAT_ARGB2101010` (already used for 10-bit mode)
  - `SDL_COLORSPACE_HDR10`
  - `SDL_PROP_TEXTURE_CREATE_HDR_HEADROOM_FLOAT` from MaxCLL/peak luminance
  - `SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT` = 100.0 (standard HDR10)
- SDR textures remain unchanged.
- Texture recreation needed when HDR state changes (window moves between displays).

**Phase 3: Skip CPU tonemap**
- In VideoFilterer: when HDR passthrough is active for this side, skip `must_tonemap` entirely — no zscale, no format conversion, no tonemap filter. Just fps + copy.
- For HLG content: insert a lightweight `zscale=t=smpte2084` to convert HLG transfer to PQ (required because SDL's HDR10 path expects PQ). This is much cheaper than the full tonemap — no primaries conversion, no matrix change, just a per-pixel transfer curve remap.
- FormatConverter outputs ARGB2101010 (packed 10-bit) instead of rgb24.

**Phase 4: Dynamic fallback**
- Handle `SDL_EVENT_WINDOW_HDR_STATE_CHANGED`: when the window moves from an HDR display to SDR (or vice versa), reinitialize the filter chain and textures.
- This triggers VideoFilterer reinit (adds/removes tonemap), texture recreation, and FormatConverter reinit.

### Design decisions

**Mixed HDR+SDR comparison (one side HDR, other SDR):**
When comparing an HDR encode vs SDR encode of the same content on an HDR display, the HDR side renders at full headroom while the SDR side renders at SDR white point (1.0 EDR). This is visually accurate — it shows the real dynamic range difference between the two encodes, which is exactly what the tool exists to reveal. No special handling needed; SDL's renderer correctly composites SDR textures at SDR brightness within an HDR compositor.

**HLG content:**
SDL's HDR10 path expects PQ (SMPTE 2084) transfer. HLG content must be converted to PQ first. A single `zscale=t=smpte2084` does this as a per-pixel LUT operation — fast compared to full tonemap because:
- No primaries conversion (stays BT.2020)
- No matrix conversion (stays same YUV encoding)
- Just transfer curve remap (HLG OOTF → PQ EOTF)
Profile shows zimg's ToLinearLut + ToGammaLut are the hot functions; this would be roughly half the current zscale cost since we skip primaries/matrix.

**Headroom metadata:**
`SDL_PROP_TEXTURE_CREATE_HDR_HEADROOM_FLOAT` should be `MaxCLL / SDR_white_point`. If MaxCLL = 1000 nits, headroom = 10.0. SDL's Metal backend compares texture headroom vs display headroom and applies GPU tonemap only when content exceeds display capability. If MaxCLL metadata is absent, use a conservative default (e.g., 10.0 for PQ, 3.0 for HLG).

**Risk:** moderate implementation effort (touches display, filterer, format_converter, and event handling). But no custom shaders — SDL handles the GPU rendering. Visual output is more faithful than CPU tonemap because the display sees the actual HDR signal. Fallback to CPU tonemap on SDR displays preserves existing behavior exactly.

### Implementation progress and measured results (2026-04-16)

Phases 1–3 implemented and functional. HDR content renders on HDR displays via native passthrough. Key findings:

**What was implemented:**

- **Phase 1 (display.cpp):** Renderer created with `SDL_CreateRendererWithProperties` + `SDL_COLORSPACE_SRGB_LINEAR`. HDR display detection via `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN` and `SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT`. Dynamic tracking via `SDL_EVENT_WINDOW_HDR_STATE_CHANGED`. Public API: `get_hdr_display_available()`, `get_hdr_display_headroom()`, `set_hdr_passthrough()`.
- **Phase 2 (display.cpp):** HDR textures created with `SDL_CreateTextureWithProperties` using `SDL_COLORSPACE_HDR10`, `SDL_PIXELFORMAT_ARGB2101010`, headroom from display. Buffer reallocation on passthrough state change. `requires_10_bpc()` refactored to mean "needs RGB48LE→ARGB2101010 packing" (true for `--10-bpc` without HDR passthrough); HDR passthrough uses `AV_PIX_FMT_X2RGB10LE` (already packed, direct upload).
- **Phase 3 (video_compare.cpp, video_filterer.cpp):** `probe_hdr_display()` called before filterer construction. Content-aware activation: HDR passthrough only when display supports HDR AND all video sides have HDR content. `hdr_passthrough` flag passed to `VideoFilterer` — disables `must_tonemap`. For HLG content, `zscale=t=smpte2084` inserted for HLG→PQ transfer conversion. PQ content passes through with no conversion. FormatConverter outputs `AV_PIX_FMT_X2RGB10LE` (4 bytes/pixel packed 10-bit RGB) when HDR passthrough active. Mixed HDR+SDR comparisons fall back to CPU tonemap.
- **Bug fixes:**
  - Video textures set to `SDL_BLENDMODE_NONE` so the 2-bit alpha in ARGB2101010/X2RGB10LE is ignored by the renderer. This replaced an earlier per-pixel alpha fixup loop (`|= 0xC0000000`) that added ~16ms per 4K frame — a significant throughput hit.
  - Right-side byte offsets in split-view upload use correct bytes-per-pixel (4 for X2RGB10LE, not hardcoded 3 for RGB24).
  - `pixel_size` for screenshot/selection memcpy accounts for X2RGB10LE format.
  - HDR frames saved as lossless JPEG-XL (via `JxlSaver`, separate from `PngSaver`) with color metadata preserved. SDR frames remain PNG.
- **Frame-skip fix (video_compare.cpp):** The adaptive refresh skip formula `next_refresh_at += refresh_time / target_time` was dropping 2/3 of frames when the display refresh (~57ms at 4K) exceeded the content's target frame period (~16.7ms at 60fps). For a comparison tool, smooth slow-motion is preferable to choppy frame-skipping. Fixed by capping the effective target to `max(target_time, refresh_time)`, which produces ratio=1.0 (display every frame) when the display can't hit the content's target FPS. Normal-speed playback is preserved when the display can keep up.

**Measured performance (4K 10-bit HLG BT.2020):**

| run                             | user@5s | user@15s | Δ/10s (steady) | RSS@15s |
|---------------------------------|--------:|---------:|---------------:|--------:|
| Original (full tonemap)         |   25.86 |    82.27 |     5.64 cores | 4.28 GB |
| Opt #1 (drop format=rgb48)      |   22.22 |    70.78 |     4.86 cores | 4.17 GB |
| HDR passthrough (RGB48LE, old)  |   19.68 |    73.87 |     5.42 cores | 7.37 GB |
| **HDR passthrough (X2RGB10LE)** |   20.07 |    73.61 |     5.35 cores | 4.65 GB |
| SDR baseline                    |   15.27 |    47.40 |     3.21 cores | 4.07 GB |

**Analysis:** Switching from RGB48LE to X2RGB10LE saved **2.7 GB RSS** (-37%) with no CPU change — frame rings now store 4 bytes/pixel instead of 6, and `convert_to_packed_10_bpc` is skipped entirely. vs original full tonemap: **-5% CPU** (5.64→5.35 cores), **+9% RSS** (4.28→4.65 GB), with visually faithful HDR output. SDR is unaffected (47.43s = baseline match).

The remaining ~2.1-core steady-state gap vs SDR is the HLG→PQ `zscale=t=smpte2084`. For PQ content (no HLG→PQ step), steady-state would approach the SDR baseline (~3.2 cores).

### Additional implementations (2026-04-17)

- **Phase 4: Dynamic fallback (video_compare.cpp, display.cpp):** `Display::update_hdr_display_state()` sets a dirty flag when `SDL_EVENT_WINDOW_HDR_STATE_CHANGED` fires. `VideoCompare::handle_hdr_state_change()` checks the flag each main-loop iteration, recalculates `hdr_passthrough_active_` based on new display state + content type, reconstructs all VideoFilterers and FormatConverters, updates Display textures, and triggers a pipeline flush via seek. Stored `VideoFilterContext` as a member (`video_filter_context_`) so filterers can be reconstructed at runtime.

- **Headroom from content metadata (video_compare.cpp, display.cpp):** `Display::set_hdr_content_headroom(float)` sets the HDR10 texture's headroom property. `VideoCompare` calculates `max(safe_peak_luminance_nits) / 100.0` across all decoder sides and passes it to Display before `set_hdr_passthrough`. SDL's GPU tonemapper now activates when content headroom exceeds display headroom (e.g., 1000-nit content on a 500-nit display). Default is 10.0 (1000 nits) when MaxCLL metadata is absent.

- **SDR texture colorspace tagging (display.cpp):** SDR textures now created with `SDL_CreateTextureWithProperties` + `SDL_COLORSPACE_SRGB` instead of plain `SDL_CreateTexture`. The `SDL_COLORSPACE_SRGB_LINEAR` renderer now has explicit gamma information for SDR content, ensuring correct sRGB rendering.

### Display refresh optimization (2026-04-17)

`possibly_refresh` was taking ~57ms at 4K, dominated entirely by `SDL_UpdateTexture` (~53ms for two sub-rect uploads to a shared texture). `SDL_RenderPresent` and `SDL_RenderTexture` were negligible (~35μs each).

**Root cause:** `SDL_UpdateTexture` on Metal streaming textures blocks waiting for the GPU command buffer to complete before allowing writes to the staging buffer.

**Approaches tested:**

| approach | upload time | notes |
|---|---:|---|
| `SDL_UpdateTexture`, shared texture, sub-rects | 53ms | original — Metal fence wait per texture |
| `SDL_LockTexture`, per-side double-buffered | 105ms | **worse** — Metal locks on command buffer, not individual textures; 2 textures = 2 waits |
| `SDL_LockTexture`, per-side single-buffered | **17ms** | **3× faster** — one lock per side, full-frame upload |

**What was implemented:**

- **Per-side textures (display.h, display.cpp):** replaced the single shared texture (`video_width*2 × video_height` for HStack, etc.) with two independent textures (`video_width × video_height` each), one per side. Each side uploads its full frame independently — no sub-rect calculations needed. This eliminates the HStack/VStack texture size doubling and simplifies the upload path.
- **`SDL_LockTexture` upload (display.cpp):** `update_side_texture()` uses `SDL_LockTexture` to get a direct pointer to the staging buffer, copies the frame data via `memcpy`, and calls `SDL_UnlockTexture`. Falls back to `SDL_UpdateTexture` if lock fails. Full-frame lock (no sub-rect) appears to be more efficient on Metal.
- **Independent render (display.cpp):** each side is rendered from its own texture to the appropriate screen region. The split/HStack/VStack logic now only affects the `SDL_RenderTexture` source and destination rects, not the texture layout. Subtraction mode refreshes when either side's frame changes (both inputs affect the diff).
- **Frame-skip fix (video_compare.cpp):** capped the adaptive refresh skip to `max(target_time, refresh_time)` so frames aren't dropped when the display can't hit the content's target FPS. Smooth slow-motion is preferable to choppy skipping for a comparison tool.

**Result:** display refresh dropped from 57ms to 17ms. At 4K 60fps content, UI FPS improved from ~9fps (choppy frame-skipping) to ~16.5fps (smooth, every produced frame displayed). The remaining ~43ms per cycle is pipeline production time (decode + filter + format convert), not display. Further improvement requires pipeline optimizations (hardware decode, parallel format conversion, etc.).

## Recommended priority

| # | Optimization              | Est. CPU savings       | Visual risk          | Effort   | Status
|---|---------------------------|------------------------|----------------------|----------|-------
| 1 | Drop `format=rgb48`       | ~0.78 core (measured)  | none                 | trivial  | **done**
| 2 | zscale output format + convert to display format in filter | ~0.9 core (measured) | low (verify dither) | small | **done** (in HDR path)
| 3 | Fix colorspace mismatch   | negligible             | correctness fix      | small    | **done** (m=bt709 in zscale)
| 4 | Tonemap curve choice      | ~0.2 core              | intentional change   | small (expose CLI flag) |
| 5 | Cache tonemapped frames   | variable               | none                 | moderate |
| 6 | SDL3 HDR passthrough      | ~0.3 core HLG (measured), ~2.4 PQ (est.) | more faithful HDR | moderate | **done** (phases 1-4)
| 7 | Display refresh (per-side `SDL_LockTexture`) | 53→17ms upload (3×) | none | moderate | **done**

## Display refactor impact (phases 10–11, 2026-04-18)

The 1570-line `possibly_refresh` mega-function was decomposed into a RenderContext-based dispatcher pattern (phase 10) that eliminated duplicate GPU/SDL render path computations (frame keys, zoom rectangles, mouse transforms, drawable dimensions). This consolidation reduced line count by ~25% (5172 → 3858 in display.cpp) while improving clarity through six focused GPU sub-phases and six SDL sub-phases. Tonemap optimization #1 (drop `format=rgb48`) and #6 (SDL3 HDR passthrough) are now cleanly separated and co-located in the GPU path.

The 560-line `handle_event` mega-function was similarly decomposed into a 40-line dispatcher + 9 first-match-wins key-group cascade handlers (phase 11), making the input grammar navigable and organized by semantic domain (playback, view modes, zoom, diff, crop/save, scope, window, misc).
