# Tonemap Optimization Opportunities

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

## Optimization #6: GPU-side tone mapping (Metal shader)

**CPU impact: moves entire tonemap cost off CPU (~2.4 cores freed)**
**Visual impact: depends on shader precision**

Upload the raw yuv420p10le frame to a Metal texture and perform the BT.2020 HLG/PQ -> BT.709 sRGB conversion in a compute or fragment shader. Metal on Apple Silicon has native 16-bit float support and the GPU is otherwise underutilized during playback (profile shows minimal Metal/GPU time). This would eliminate the entire CPU-side tonemap pipeline (zscale, swscale conversions, FormatConverter).

**Risk:** high implementation effort. Shader must match zscale's precision for the tool to remain trustworthy — any per-pixel difference between CPU and GPU tonemap would show up as a false diff in the comparison view. Also requires a fallback path for non-Metal platforms (Linux, older macOS). Most invasive option and only justified if the other optimizations are insufficient.

## Recommended priority

| # | Optimization              | Est. CPU savings | Visual risk          | Effort  | Status
|---|---------------------------|------------------|----------------------|---------|-------
| 1 | Drop `format=rgb48`       | ~0.78 core (measured) | none            | trivial | **done**
| 2 | zscale output format      |        ~0.3 core | low (verify dither)  | small   |
| 3 | Fix colorspace mismatch   |       negligible | correctness fix      | small   |
| 4 | Tonemap curve choice      |        ~0.2 core | intentional change   | small (expose CLI flag) |
| 5 | Cache tonemapped frames   |         variable | none                 | moderate |
| 6 | GPU tonemap               |       ~2.4 cores | precision-dependent  | large   |
