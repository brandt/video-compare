# Embedding video-compare as an In-Process API for a Swift App

> **Status (2026-05-29): design proposal, not yet implemented.** This document captures the architecture and rationale for exposing the comparison engine as a library that a native macOS (Swift) app can call in-process — without shelling out to the `video-compare` executable. It records a decision (Variant 3: headless engine + macOS-native rendering, leaning on Core Image) and the reasoning behind it, so the trade-offs are not re-litigated from scratch when the work starts.

## Goal

Make the decode / align / compare engine callable in-process from a Swift app, so a native macOS front-end can own the window, UI chrome, and rendering while reusing this project's FFmpeg pipeline, auto-alignment, time-shift synchronization, and metrics. Shelling out to the CLI is explicitly rejected: it cannot share decoded frames or GPU surfaces, and the per-frame control/inspection a real UI needs is impractical over a process boundary.

The non-negotiable constraint is **performance for multiple 4K HDR videos**. Any design that regresses meaningfully against the current libplacebo path is unacceptable.

## Background: how the app is built today

The repository currently produces a **single executable**. There is no library target. The entry point parses CLI args into a `VideoCompareConfig` ([config.h](../../src/app/config.h)) and runs `VideoCompare::operator()()` ([video_compare.cpp](../../src/app/video_compare.cpp)) — a blocking, run-to-completion main loop that:

1. **owns a window** — `Display` creates an SDL3 `NSWindow` and a GPU context;
2. **owns the event loop** — it polls `SDL_PollEvent` and drives all keyboard/mouse input;
3. **spawns its own worker threads** — a four-stage producer/consumer pipeline per side (demux → decode → filter → convert).

Rendering today goes through **libplacebo** (Vulkan via MoltenVK on macOS): the decoded **native YUV** frame is handed straight to `pl_map_avframe_ex` ([gpu_renderer.cpp:180-185](../../src/display/gpu_renderer.cpp#L180-L185)), and **all** of YUV→RGB, HDR tone-mapping, scaling, and split/overlay compositing happen in one GPU render graph ([gpu_renderer.cpp:232-294](../../src/display/gpu_renderer.cpp#L232-L294)). swscale (`FormatConverter`) is **only** instantiated on demand for CPU features — subtraction, pixel inspector, metrics — via `RgbFrameCache` ([display_render_gpu.cpp:700-717](../../src/display/display_render_gpu.cpp#L700-L717)).

The good news for embedding: the _engine_ below the renderer is already well-decoupled. Demuxer, decoder, filterer, converter, `FrameRing`, `TimeShifter`, the seek barrier, metrics, and auto-alignment fingerprinting are all pure `AVFrame` logic with **no Display / SDL / GPU references**. The one real entanglement is `VideoCompare::operator()()` itself, which interleaves pipeline-driving with `display_->refresh()`, input flags, and the auto-align trigger.

## The decision

We will pursue a **headless engine** (no window, no SDL) exposed through a flat C ABI, with the **Swift app owning all rendering using native Apple frameworks** — Core Image for the convenience layer, hand-written Metal for the fidelity-critical and non-built-in parts. This is "Variant 3" from the design discussion below. libplacebo, Vulkan, and MoltenVK are **dropped from the embedded library**; the standalone executable keeps them.

### Why this over the alternatives

The integration question has two axes: _who renders_ (Swift vs. reuse libplacebo) and _if libplacebo is reused, against what dependency cost_. libplacebo is **not** tied to the window — only the SDL swapchain is, and `pl_render_image` can target an offscreen `pl_tex`. So keeping it headless is technically possible. The variants considered:

| Variant | Who renders                                           | Keeps libplacebo?                    | Verdict
| ------- | ----------------------------------------------------- | ------------------------------------ | -------
| 1       | libplacebo composites the final image; Swift blits it | Yes (offscreen `pl_tex` → IOSurface) | Rejected — Swift loses control of video compositing; this is really the "embed the existing UI" mode, not native
| 2       | libplacebo tonemaps per source; Swift composites      | Yes                                  | Rejected — keeps fidelity but still drags MoltenVK + Vulkan↔Metal interop
| **3**   | **Swift / Core Image / Metal does everything**        | **No**                               | **Chosen** — fully native, lightest dependency footprint

The deciding factor is **dependency footprint and platform-nativeness**. On macOS, libplacebo has no Metal backend; it _is_ Vulkan via MoltenVK. Keeping it (Variants 1–2) means the Swift app bundle ships and links MoltenVK + the Vulkan loader + libplacebo, and pays for two awkward seams: sharing a `VkImage` with a Metal `MTLTexture`/IOSurface (`VK_EXT_metal_objects`), and cross-API synchronization (Vulkan timeline semaphore ↔ `MTLSharedEvent`). For a macOS-native app, eliminating the Vulkan translation layer entirely is worth the cost of rebuilding the color path — _especially_ because that cost turns out to be smaller than it first appears (see Core Image, below).

### The cost we are accepting

Variant 3's price is that we no longer get libplacebo's GPU color pipeline for free. We must reproduce, on Apple-native frameworks: YUV→RGB, HDR handling, scaling, split/stack layout, the difference mode, and the analysis scopes. The mitigating facts:

- The current app runs libplacebo with `pl_render_fast_params` + `pl_color_map_default_params` and `map_dovi = false` ([gpu_renderer.cpp:182-234](../../src/display/gpu_renderer.cpp#L182-L234)). We are matching libplacebo's **defaults**, not a heavily tuned HDR renderer.
- Core Image can absorb most of it (next section).
- The HDR display tone-map can be deferred to the **OS** via an EDR-enabled layer, so for display we may implement no tone-mapping at all.

## How much defers to Core Image

Core Image is a color-managed, Metal-backed pipeline that maps cleanly onto most of libplacebo's _display_ responsibilities.

| Current libplacebo job           | Core Image coverage                                                                                          | Residual custom work
| -------------------------------- | ------------------------------------------------------------------------------------------------------------ | --------------------
| YCbCr→RGB + interpret colorspace | `CIImage(cvPixelBuffer:)` reads the tagged buffer's matrix / primaries / transfer and converts automatically | None — _if_ the CVPixelBuffer is correctly tagged
| High-quality scaling for panes   | `CILanczosScaleTransform` (also bicubic)                                                                     | None
| Split / hstack / vstack layout   | `cropped(to:)` + transform + `CISourceOverCompositing`                                                       | Split-line / wipe overlay (trivial generator or solid composite)
| Difference / subtraction mode    | `CIDifferenceBlendMode` then `CIColorMatrix` / `CIGammaAdjust` to amplify                                    | **Exact diff semantics** — see fidelity caveats
| Histogram scope                  | `CIAreaHistogram` + `CIHistogramDisplayFilter` (built-in)                                                    | None — likely a GPU win over today's FFmpeg CPU filter
| Waveform / vectorscope scopes    | No built-in filters                                                                                          | Custom `CIKernel` / Metal compute either way
| Overlays / HUD / text            | Compose `CIImage`s, or draw native AppKit / SwiftUI layers on top                                            | Layout glue
| Output to display                | `CIContext.render(_:to:…)` into a Metal texture / `CAMetalLayer`                                             | Sync / present plumbing

### The biggest lever: don't tone-map for display at all

For the display case, defer HDR tone-mapping to the **OS**, not even to Core Image: keep the `CIImage` in an extended-range working space, render into an **EDR-enabled `CAMetalLayer`** (`wantsExtendedDynamicRangeContent = true`, with `CAEDRMetadata`), and let the system map to the screen's headroom. This is the Apple-native equivalent of libplacebo's tone-map and arguably the more correct macOS behavior. An explicit tone-map (`CIToneMapHeadroom`, recent macOS) is only needed when **baking SDR** — e.g. the PNG/JXL export path (`ImageSaver`).

### Where we shouldn't defer to Core Image

As a tool for inspecting small differences in encoding, users pixel-peep subtle artifacts. Core Image trades control for convenience, so the boundary matters:

- **The difference / measurement path.** `CIDifferenceBlendMode`'s precision and clamping are not under our control. The A−B that a reviewer reads as "the artifact" should be a **custom `CIKernel` or Metal compute** with explicit float format and known semantics, validated against the existing CPU `DifferenceProcessor`.
- **Chroma upsampling.** When `CIImage` ingests a 4:2:0 YCbCr buffer, Core Image picks the chroma reconstruction; we have less control over siting / filter quality than libplacebo. Chroma reconstruction differences are exactly what codec work cares about — verify it is acceptable, or upsample explicitly before handing Core Image RGB.
- **Precision and banding.** Set the `CIContext` `workingFormat` to `.RGBAh` (half-float) or `.RGBAf` (full float) with a wide-gamut linear working space to avoid clipping / banding in HDR. Core Image will not do libplacebo's error-diffusion **dithering**, so 8-bit SDR output can band on gradients — add a dither kernel when exporting 8-bit.
- **Determinism.** Core Image fuses and schedules kernels opaquely. For anything published as a measurement, pin precision and verify rather than trust defaults.

However, none of the above are dealbreakers if there's a big performance win or reduction in complexity to be had by deferring to Core Image.

## Performance analysis

The headline concern is multiple 4K HDR streams. The key realization, from [libplacebo-integration.md:29-41](libplacebo-integration.md#L29-L41), is that **the current pipeline is not zero-copy even with `--hwaccel videotoolbox`**:

```
demux → GPU decode (hardware) → av_hwframe_transfer_data → CPU frame
      → VideoFilterer (CPU, structural filters only)
      → pl_map_avframe_ex (CPU → GPU upload)
      → pl_render_image (GPU)
```

The GPU→CPU readback after decode and the subsequent CPU→GPU upload are both present today; playback is CPU-transfer-limited, not GPU-limited. **This is an opportunity, not just a constraint.**

### The frame handoff (where regressions live or die)

The right native handoff keeps frames GPU-resident and lets Apple frameworks do the color work:

- **HW-decode path (default — `hw_accel_spec` is `"auto"`, i.e. VideoToolbox on macOS):** decode straight to a `CVPixelBuffer` (`AV_PIX_FMT_VIDEOTOOLBOX`), tag it with the source color attachments, wrap its IOSurface as a Metal texture / `CIImage` **zero-copy**, and let Core Image / Metal do YUV→RGB + scale + composite, with HDR handed to the EDR layer. **No swscale, and no `av_hwframe_transfer_data` readback.** On Apple Silicon this can _beat_ the current path, because we skip the very readback + upload that limits it today.
- **SW-decode path (or when CPU filters force a download):** fall back to swscale → `AV_PIX_FMT_P010LE` written directly into a locked, IOSurface-backed `CVPixelBuffer` plane set. `FormatConverter::operator()` already writes into caller-supplied `dst->data` / `dst->linesize` ([format_converter.cpp:160-164](../../src/media/format_converter.cpp#L160-L164)), so this is zero _extra_ copy — one swscale into the IOSurface. This branch pays a per-frame CPU conversion the current GPU path avoids, but it is the same class of cost the SDL fallback already pays, and only on this branch.

P010 (FFmpeg `AV_PIX_FMT_P010LE`) is layout-compatible with `kCVPixelFormatType_420YpCbCr10BiPlanar{Video,Full}Range`: 4:2:0, biplanar, 10 bits in the high bits of 16-bit samples. Match the source **range** (most HDR10 is video/limited range → `'x420'`); do not let swscale do range expansion. If 4:2:2/4:4:4 sources must stay unsubsampled, target a non-4:2:0 CV format instead, trading display fast-paths for fidelity.

### Regression summary

| #   | Concern                                                                 | Severity (4K HDR ×N)         | Status under Variant 3
| --- | ----------------------------------------------------------------------- | ---------------------------- | ----------------------
| 1   | Per-frame CPU swscale that today's GPU path avoids                      | High                         | **Avoided** on the HW path; confined to the SW fallback
| 2   | CPU↔GPU round trip                                                      | Med–High                     | **Improved** — native path removes the readback the current pipeline pays
| 3   | Rebuilding libplacebo's color science                                   | Med perf, High fidelity risk | **Mostly absorbed** by Core Image + OS EDR; residue is custom Metal (diff, scopes, dither)
| 4   | Frame stored in two representations (engine YUV ring + display buffers) | Med (memory)                 | Mitigate: convert/wrap on demand, don't keep a parallel display ring
| 5   | Multi-pass Core Image vs libplacebo's fused graph                       | Low–Med                      | Implementation quality; Core Image fuses kernels internally

Not regressions: decode (same FFmpeg threads), the filter graph in passthrough, and the absence of any IPC/process boundary (the library is linked in-process). Scopes and subtraction are CPU today and can become GPU wins under Core Image / Metal.

### Mandatory verification gates

- **Profile a CPU swscale 4K `yuv420p10le`→P010 on target hardware** at the intended stream count, to bound the SW-fallback cost (#1).
- **Pixel-parity check**: Core Image / Metal output vs the current libplacebo render, and the custom diff kernel vs the existing CPU `DifferenceProcessor`. Treat divergence as a release blocker, given the audience.

## Proposed architecture

```
┌────────────────────────── Swift app (native) ──────────────────────────┐
│  SwiftUI / AppKit UI · CAMetalLayer (EDR) · Core Image · Metal kernels  │
│         ▲ tagged CVPixelBuffers (zero-copy IOSurface)   ▲ metrics       │
│         │                                               │               │
│  ┌──────┴───────────────── Obj-C++ bridge ──────────────┴────────────┐  │
│  │  creates/tags CVPixelBufferPool, maps C structs ↔ CoreVideo        │  │
│  └──────▲─────────────────────────────────────────────────────────────┘ │
└─────────┼────────────────────────────────────────────────────────────────┘
          │ extern "C" ABI (opaque handle, POD structs)
┌─────────┴──────────────── libvideocompare (C++) ─────────────────────────┐
│  vc::Engine  — pipeline, FrameRing, TimeShifter, seek barrier,           │
│                metrics, auto-align   (NO Display, NO SDL, NO libplacebo)  │
└──────────────────────────────────────────────────────────────────────────┘
```

The C++ engine stays **platform-neutral**: it produces frames (VideoToolbox `CVPixelBuffer` passthrough, or P010 written into caller-supplied planes) and reports per-frame color tags; it never touches CoreVideo or Metal. A thin **Objective-C++ bridge** in the Swift package owns the `CVPixelBufferPool`, attachment tagging, and C-struct marshalling.

### Target C ABI (sketch)

```c
typedef struct VCEngine VCEngine;

VCEngine*  vc_open(const VCConfig* cfg, VCError* err);   // build pipeline, no window
void       vc_close(VCEngine*);

double     vc_duration(const VCEngine*);
void       vc_seek(VCEngine*, double seconds);           // drives the seek barrier
void       vc_set_time_shift(VCEngine*, int64_t offset_ms, int num, int den);

// Aligned frame for the current cursor. Either reports a VideoToolbox-backed
// CVPixelBuffer to wrap, or fills caller-supplied P010 planes. Returns color tags.
VCFrame    vc_frame(VCEngine*, VCSide side);

VCMetrics  vc_metrics(VCEngine*, VCRect roi);            // PSNR/SSIM/VMAF over a region
int64_t    vc_auto_align(VCEngine*, VCSide master);      // returns discovered offset (µs)
```

`VideoCompareConfig` / `InputVideo` ([config.h](../../src/app/config.h)) are mostly POD and map directly to `VCConfig`; the `AVDictionary*` and `std::vector` members need flattening. `get_playback_state_snapshot()` and `format_results_json()` already exist as control/inspection seams to model the API on.

## Phased plan

1. **Carve `vc::Engine` out of `VideoCompare`.** Split `operator()()` into `init() / seek() / pull_frame() / step()` with no `Display` member; turn the loop's display calls into explicit API calls or remove them. Largest file in the repo — budget the most time here. Keep the executable building against the same engine so CLI and library never diverge.
2. **Pull-based playback.** Replace the timer-paced `advance()` with `seek(t)` / `frame()` over the existing `FrameRing` (`current_frame()`, `at(offset)`, `pivot_*`). The seek barrier stays.
3. **Frame contract.** VideoToolbox CVPixelBuffer passthrough as the default; P010-into-IOSurface as the SW fallback. Surface per-frame color tags through the ABI. Document the frame-lifetime contract (borrowed-until-next-call vs. pooled) explicitly.
4. **C ABI + library target.** Add `libvideocompare` (static `.a` + `extern "C"` header) linking FFmpeg but **not** SDL3 / libplacebo. Confirm `src/media/`, `src/analysis/metrics/`, and the carved engine do not transitively pull in `src/display/`.
5. **Obj-C++ bridge + Swift package.** CVPixelBufferPool, attachment tagging, module map / xcframework.
6. **Swift rendering.** Core Image for ingest/scale/layout/composite/histogram + EDR display; custom Metal for waveform/vectorscope, the difference kernel, and dithering. Run the verification gates.

## What is reimplemented in Swift (not in the engine)

The library provides decode, filters, CPU tone-map-or-passthrough, PSNR/SSIM/VMAF, auto-alignment, and time-shift sync — all already display-independent. The Swift side **reimplements** the front-end that lives in `src/display/` today: split/vstack/hstack compositing, difference/subtraction mode, the scopes (histogram/vectorscope/waveform), overlays, and the dock scrubber.

## Future work / open questions

- **VideoToolbox decode + filters.** CPU filters on HW frames force an `hwdownload`, collapsing the zero-copy advantage. Quantify which auto-filters / user filters actually run, and whether they can be skipped or expressed in Metal.
- **4:2:2 / 4:4:4 fidelity** vs. display fast-paths — pick CV formats per source.
- **Multi-monitor / mixed-headroom EDR** behavior across displays.
- **Frame-lifetime / pool sizing** for deep scrub history without doubling memory (relates to `frame_buffer_size` and the packet ring).
- **OS version floor.** HDR-correct Core Image (extended-range working spaces, `CIToneMapHeadroom`) is version-gated; confirm the minimum macOS target.

## References

- [libplacebo integration](libplacebo-integration.md) — the current GPU pipeline, including the non-zero-copy readback note that motivates the native handoff.
- [Architecture](../ARCHITECTURE.md) — overall subsystem layering.
- [Playback buffer](buffer.md) — `FrameRing` / `PacketRing` semantics the pull-based API builds on.
- [Auto-alignment](auto-align.md) — the fingerprint/correlation engine exposed via `vc_auto_align`.
