# Embedding video-compare as an In-Process API for a Swift App

> **Status (2026-05-29): design proposal, not yet implemented.** This document captures the architecture and rationale for exposing the comparison engine as a library that a native macOS (Swift) app can call in-process — without shelling out to the `video-compare` executable. It records a decision (Variant 3: headless engine + macOS-native rendering, leaning on Core Image) and the reasoning behind it, so the trade-offs are not re-litigated from scratch when the work starts. The engine is **copied into a fresh, single-purpose tree** rather than refactored in place.

## Goal

Make the decode / align / compare engine callable in-process from a Swift app, so a native macOS front-end can own the window, UI chrome, and rendering while reusing this project's FFmpeg pipeline, auto-alignment, time-shift synchronization, and metrics. Shelling out to the CLI is explicitly rejected: it cannot share decoded frames or GPU surfaces, and the per-frame control/inspection a real UI needs is impractical over a process boundary.

The non-negotiable constraint is **interactive performance on a pair of 4K HDR videos** — snappy random seek, fast *iterative* auto-align, and fluid zoomed inspection of paused frames (see *Intended usage*). Sustained playback is secondary; it happens only in short snippets. No design should regress meaningfully against the current libplacebo path.

## Platform target & constraints

**Deployment target: macOS 14 (Sonoma).** A modern target, which keeps the design straightforward:

- **Apple Silicon first.** macOS 14 still runs on some Intel Macs, but Apple Silicon is the norm. On Apple Silicon, decoded VideoToolbox frames live in unified memory as IOSurface-backed `CVPixelBuffer`s, so wrapping them as Metal textures / `CIImage`s is genuinely zero-copy.
- **Mature EDR.** System HDR is fully supported: an EDR-enabled `CAMetalLayer` (`wantsExtendedDynamicRangeContent`, `CAEDRMetadata`) lets the OS tone-map HDR to the display's headroom. This is the **primary HDR path** (see below) — we generally do not tone-map ourselves.
- **Modern Core Image.** `CIContext(mtlCommandQueue:)`, HDR-aware ingestion of tagged BT.2020 PQ/HLG buffers, and the filters we rely on (`CILanczosScaleTransform`, blend modes, `CIAreaHistogram`/`CIHistogramDisplayFilter`, `CIColorMatrix`) are all available. A few HDR specifics (e.g. `CIToneMapHeadroom`, used only for baking SDR) are newer — re-verify exact availability against current docs.

**Use case.** The tool is for **subjective A/B comparison between encoded products**, not reference codec development. We avoid introducing visible degradation, but absolute/bit-exact fidelity is not required — both panes share one pipeline, so *consistency between them* is what protects a comparison. This relaxes where we can lean on Core Image (see below).

## Intended usage & scope

The embedding app drives the **tournament**: it finds candidates, constructs comparison sets, and applies actions from the verdicts. Our component handles the **comparison session** — two videos on screen at a time — over a pool of loaded candidates. The dominant loop is:

1. Load a candidate set; put two on screen and seek to a roughly random point.
2. **Auto-align**, repeatedly — the user re-triggers until the two sides match.
3. **Pixel-peep** a paused, aligned frame (zoom / pan to inspect detail).
4. Play a short snippet.
5. Seek elsewhere, re-align, pixel-peep again.
6. Decide — which may mean **re-pairing** (swap one side to a different candidate and compare again, often bouncing back and forth when two are close), and may **keep more than one** candidate.

This shapes the priorities:

- **Seek + iterative align are the hot path, not sustained playback.** Most wall-clock time is spent paused, zoomed, or aligning. Optimize seek latency and per-attempt align latency first.
- **Two streams are on screen at once,** but the user **revisits pairings non-linearly**, so several candidates should stay **warm** (open, paused at their last position) for instant re-pairing. Warmth is a bounded LRU pool — each warm source costs its ring + packet-buffer memory (the packet ring defaults to 256 MiB), so pool size is a real latency-vs-memory knob.
- **Arbitrary pairing, not a fixed bracket.** Either side can be re-assigned to any candidate in the pool — "running winner vs. next" is just the common case. The verdict is **multi-keep**: the user may keep several.
- **Auto-align is a first-class, stateful, iterative operation** — "hit it again" must search wider each time (the existing incremental retry-cache). Converged offsets should be **cached per source-pair** so revisiting a pairing restores alignment instantly instead of re-searching.
- **Pixel-peeping is where the decision is made**, so zoomed still-frame rendering — including a true-pixel (nearest-neighbor) magnify — matters more than playback smoothness.

Out of scope for our component (owned by the host app): candidate discovery, set construction, the (non-linear, possibly multi-keep) tournament logic, and acting on verdicts. The verdict itself is a human choice captured in the host UI — the engine has no notion of it. We expose the comparison primitives only.

## Background: how the app is built today

The repository currently produces a **single executable**. There is no library target. The entry point parses CLI args into a `VideoCompareConfig` ([config.h](../../src/app/config.h)) and runs `VideoCompare::operator()()` ([video_compare.cpp](../../src/app/video_compare.cpp)) — a blocking, run-to-completion main loop that:

1. **owns a window** — `Display` creates an SDL3 `NSWindow` and a GPU context;
2. **owns the event loop** — it polls `SDL_PollEvent` and drives all keyboard/mouse input;
3. **spawns its own worker threads** — a four-stage producer/consumer pipeline per side (demux → decode → filter → convert).

Rendering today goes through **libplacebo** (Vulkan via MoltenVK on macOS): the decoded **native YUV** frame is handed straight to `pl_map_avframe_ex` ([gpu_renderer.cpp:180-185](../../src/display/gpu_renderer.cpp#L180-L185)), and **all** of YUV→RGB, HDR tone-mapping, scaling, and split/overlay compositing happen in one GPU render graph ([gpu_renderer.cpp:232-294](../../src/display/gpu_renderer.cpp#L232-L294)). swscale (`FormatConverter`) is **only** instantiated on demand for CPU features — subtraction, pixel inspector, metrics — via `RgbFrameCache` ([display_render_gpu.cpp:700-717](../../src/display/display_render_gpu.cpp#L700-L717)).

The good news for embedding: the _engine_ below the renderer is already well-decoupled. Demuxer, decoder, filterer, converter, `FrameRing`, `TimeShifter`, the seek barrier, metrics, and auto-alignment fingerprinting are all pure `AVFrame` logic with **no Display / SDL / GPU references**. The one real entanglement is `VideoCompare::operator()()` itself, which interleaves pipeline-driving with `display_->refresh()`, input flags, and the auto-align trigger.

## The decision

We will pursue a **headless engine** (no window, no SDL) exposed through a flat C ABI, with the **Swift app owning all rendering using native Apple frameworks** — Core Image for the convenience layer, hand-written Metal for the fidelity-critical and non-built-in parts. This is "Variant 3" from the design discussion below. libplacebo, Vulkan, and MoltenVK do not appear in the new tree at all; the legacy repo retains them for its CLI (see *Packaging*).

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

### Packaging: a fresh tree (copy-then-subtract)

Rather than refactor this repo in place and keep the executable building, the engine will be **copied into a fresh, single-purpose tree** built around the Swift app. The reasoning: we are the sole maintainers, there are no external consumers, and the Swift tool will be the only way we interact with this code — so the usual reason to preserve the CLI (and suffer a behavior-preserving, dual-build refactor) doesn't apply, and the project already grants "free rein, including breaking changes."

The discipline is **copy-then-subtract, not greenfield**: the leaf engine modules are *copied verbatim*, and the orchestrator's behavior *slices* (seek barrier, seek-target math, follower-shift, time-shift, align retry-cache wiring) are copied too — but its timer-driven `operator()()` loop shell is *rebuilt* as a paused-first command kernel, because that shell is itself the display/timer coupling we're subtracting. We do not rewrite the seek / sync / align logic from scratch — that is exactly where undocumented, hard-won fixes live. The payoff over an in-place refactor is a codebase with zero SDL/libplacebo/CLI cruft, shaped around the pull-based Swift use case from the start; the cost is re-establishing the build and tests in the new tree, mitigated by keeping the old repo as a runnable **behavioral oracle** (see the phased plan).

A note on scope: only the *orchestrator* is treated differently from an in-place refactor — the front-end is reimplemented in Swift either way, and the leaf modules are reused either way. So this is a packaging/maintenance choice, not a different amount of engine work.

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

### The biggest lever: defer HDR tone-mapping to the OS

On macOS 14 the cleanest path is to **not tone-map ourselves**: keep the `CIImage` in an extended-range (HDR) working space, render into an **EDR-enabled `CAMetalLayer`** (`wantsExtendedDynamicRangeContent = true`, with `CAEDRMetadata`), and let the system map HDR to the display's headroom. This is the Apple-native equivalent of what libplacebo does today, runs on the GPU, and preserves the zero-copy decode path. On a display with no headroom the OS simply maps to SDR — still correct, still no work from us.

Explicit tone-mapping is only needed in two cases:

- **Baking SDR output** — e.g. the PNG/JXL export path (`ImageSaver`). Use `CIToneMapHeadroom` (newer macOS — verify availability) or a small fixed curve.
- **A deliberate SDR-preview mode**, if we want the comparison to match what an SDR viewer would see regardless of the reviewer's display.

If we ever do tone-map ourselves, the bar is low: this tool makes **subjective A/B calls between two encodes**, and both panes get the **same** tone-map, so *consistency between the panes* matters far more than absolute accuracy. A simple shared curve (Hable / Reinhard / BT.2390-lite) — in a Metal shader or via the existing FFmpeg `zscale`/`tonemap` path ([video_filterer.cpp](../../src/media/video_filterer.cpp)) — is entirely acceptable. SDR sources skip all of this; Core Image ingests tagged BT.709 buffers directly.

### Where to be careful with Core Image (but defer freely when it pays)

Given the subjective-A/B use case, none of the following are dealbreakers. Defer to Core Image whenever it buys a meaningful performance or simplicity win, and only reach for custom code if a problem actually shows up in practice:

- **The difference / subtraction mode.** `CIDifferenceBlendMode` (+ `CIColorMatrix` / `CIGammaAdjust` to amplify) is the easy path and is probably fine. If its clamping / precision looks visibly wrong once amplified, swap in a custom `CIKernel`; otherwise don't bother. Either way both panes get identical treatment, so it doesn't bias the call.
- **Chroma upsampling.** Letting Core Image reconstruct 4:2:0 chroma is acceptable — again, applied equally to both sides.
- **Precision and banding.** Use a `.RGBAh` working format and a linear working space to keep headroom. Core Image won't do error-diffusion **dithering**, which can band on 8-bit *export* gradients — add a dither kernel only if exports actually show it.
- **Determinism.** Core Image schedules kernels opaquely; fine for display. Would only matter if we published numbers, which this tool doesn't.

## Performance analysis

The concern is a **pair** of 4K HDR streams — and, per *Intended usage*, mostly seeked and paused rather than continuously played, so seek and align latency dominate. The key realization, from [libplacebo-integration.md:29-41](libplacebo-integration.md#L29-L41), is that **the current pipeline is not zero-copy even with `--hwaccel videotoolbox`**:

```
demux → GPU decode (hardware) → av_hwframe_transfer_data → CPU frame
      → VideoFilterer (CPU, structural filters only)
      → pl_map_avframe_ex (CPU → GPU upload)
      → pl_render_image (GPU)
```

The GPU→CPU readback after decode and the subsequent CPU→GPU upload are both present today; playback is CPU-transfer-limited, not GPU-limited. **This is an opportunity, not just a constraint.**

### The frame handoff (where regressions live or die)

The right native handoff keeps frames GPU-resident and lets Apple frameworks do the color work:

- **HW-decode path (default — `hw_accel_spec` is `"auto"`, i.e. VideoToolbox on macOS):** decode straight to a `CVPixelBuffer` (`AV_PIX_FMT_VIDEOTOOLBOX`), tag it with the source color attachments, wrap its IOSurface as a Metal texture / `CIImage` **zero-copy**, and let Core Image / Metal do YUV→RGB + scale + composite, with HDR handed to the EDR layer. **No swscale, and no `av_hwframe_transfer_data` readback.** On Apple Silicon (unified memory) this can _beat_ the current path, because we skip the very readback + upload that limits it today.
- **SW-decode path (software-decoded codecs, or the rare CPU-filter case):** fall back to swscale → `AV_PIX_FMT_P010LE` written directly into a locked, IOSurface-backed `CVPixelBuffer` plane set. `FormatConverter::operator()` already writes into caller-supplied `dst->data` / `dst->linesize` ([format_converter.cpp:160-164](../../src/media/format_converter.cpp#L160-L164)), so this is zero _extra_ copy — one swscale into the IOSurface. This branch pays a per-frame CPU conversion the HW path avoids, but it is the same class of cost the SDL fallback already pays, and rare in practice (see filters note).

P010 (FFmpeg `AV_PIX_FMT_P010LE`) is layout-compatible with `kCVPixelFormatType_420YpCbCr10BiPlanar{Video,Full}Range` (available since 10.13): 4:2:0, biplanar, 10 bits in the high bits of 16-bit samples. Match the source **range** (most HDR10 is video/limited range → `'x420'`); do not let swscale do range expansion. 4:2:0 chroma subsampling for display is acceptable given the use case, so this format is the sensible default.

**Filters and rotation.** Filters are used infrequently — almost always just to rotate an input. Rotation should be applied as a **render-time transform** (`CIAffineTransform` / a Metal sampler), or honored from the container's display-matrix metadata — *not* via an `avfilter` — so the zero-copy HW-decode path survives the common case. The engine should expose any rotation (container metadata or user request) as a frame tag for the renderer to apply. Only genuinely exotic user filters need the `avfilter` graph (and thus an `hwdownload`), and those are rare enough to accept the SW-fallback cost.

### Regression summary

| #   | Concern                                                                 | Severity (4K HDR ×N)         | Status under Variant 3
| --- | ----------------------------------------------------------------------- | ---------------------------- | ----------------------
| 1   | Per-frame CPU swscale that today's GPU path avoids                      | High                         | **Avoided** on the HW path; confined to the SW fallback
| 2   | CPU↔GPU round trip                                                      | Med–High                     | **Improved** — native path removes the decode readback the current pipeline pays
| 3   | Rebuilding libplacebo's color science                                   | Med perf, low fidelity risk  | **Mostly absorbed** by Core Image + OS EDR; residue is custom Metal (scopes, optional diff kernel)
| 4   | Frame stored in two representations (engine YUV ring + display buffers) | Med (memory)                 | Mitigate: convert/wrap on demand, don't keep a parallel display ring
| 5   | Multi-pass Core Image vs libplacebo's fused graph                       | Low–Med                      | Implementation quality; Core Image fuses kernels internally

Not regressions: decode (same FFmpeg threads), the filter graph in passthrough, and the absence of any IPC/process boundary (the library is linked in-process). Scopes and subtraction are CPU today and can become GPU wins under Core Image / Metal.

### Verification gates

Ordered by how much they shape the experience (hot path first):

- **Seek-to-aligned-frame latency** on a pair of 4K HDR sources — the dominant interaction. Measure random seek → first decoded aligned frame on screen.
- **Auto-align iteration latency and convergence** — how fast one "align again" attempt returns, and that repeated attempts widen the search and converge.
- **Zoomed still-frame render** — fluid zoom / pan on a paused aligned pair, including a true-pixel (nearest-neighbor) magnify.
- **Re-pairing latency** — assigning either side to a *warm* pool source is near-instant; a *cold* source pays one open + seek; revisiting a prior pairing restores the cached alignment without re-searching.
- **Snippet playback** of a 2-stream 4K HDR pair (secondary to the above).
- **Validate the EDR HDR path** end-to-end (tagged HDR `CVPixelBuffer` → extended-range Core Image → EDR `CAMetalLayer`) on real HDR content, on both a headroom-capable display and a plain SDR display.
- **Spot-check render quality** against the current libplacebo output for obvious degradation. Bit-exact parity is *not* required — both panes share one pipeline, so consistency between them is what protects an A/B call.
- **Profile a CPU swscale 4K `yuv420p10le`→P010** to bound the SW-fallback cost (#1).

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
typedef uint32_t VCSourceId;

VCEngine*  vc_open(const VCConfig* cfg, VCError* err);          // no window
void       vc_close(VCEngine*);

// Candidate pool. Sources stay warm (open, paused at their last position) up to an
// LRU cap, so re-pairing is instant.
VCSourceId vc_add_source(VCEngine*, const VCSource* src);
void       vc_drop_source(VCEngine*, VCSourceId);

// Put any pool source on either side — arbitrary pairing, warm if already open.
void       vc_set_side(VCEngine*, VCSide side, VCSourceId src);

double     vc_duration(const VCEngine*);                        // current pair (shorter side)
void       vc_seek(VCEngine*, double seconds);                  // drives the seek barrier

// Aligned frame for the current cursor. Either reports a VideoToolbox-backed
// CVPixelBuffer to wrap, or fills caller-supplied P010 planes. Returns color tags.
VCFrame    vc_frame(VCEngine*, VCSide side);

// Iterative auto-align around the cursor; repeats search wider (incremental retry-cache).
// Offsets are cached per source-pair, so revisiting a pairing restores alignment for free.
VCAlign    vc_auto_align(VCEngine*, VCAlignMode mode);          // {converged, offset_us, score}
void       vc_set_time_shift(VCEngine*, int64_t offset_us, int num, int den);  // manual nudge

VCMetrics  vc_metrics(VCEngine*, VCRect roi);                   // optional PSNR/SSIM/VMAF over a region
```

There is deliberately **no verdict call** — the decision is a human choice (and may keep several candidates), captured in the host UI, not the engine. Zoom / pan for pixel-peeping is a pure render-side concern (Swift), so it is not in the ABI either; the engine just holds the two aligned frames steady while paused. More broadly, the ABI is **commands + snapshots, not polled state**: the SDL app's transient input flags (e.g. `get_auto_align_requested()`) become explicit one-shot calls here.

`VideoCompareConfig` / `InputVideo` ([config.h](../../src/app/config.h)) are mostly POD and map directly to `VCConfig`; the `AVDictionary*` and `std::vector` members need flattening. `get_playback_state_snapshot()` and `format_results_json()` already exist as control/inspection seams to model the API on.

## Phased plan

A **fresh single-purpose tree** that copies the engine out of this repo (see *Packaging*), not an in-place refactor. Two disciplines run throughout: the legacy app is the **behavioral oracle**, and **behavior parity is a per-phase gate, not a final step** — every high-risk behavior maps to an existing test or a captured trace before we port it.

**Phase 0 — Oracle & contracts.** Freeze a fixture matrix (SDR, HDR, mixed frame rates) and capture baseline traces from the legacy app (via its script / socket harness, [input-socket.md](input-socket.md) / [input-testing.md](input-testing.md)) for the dominant loop: seek → auto-align (retry) → pixel-peep → snippet → reseek. Promote existing suites to migration gates — [test_swap_click.py](../../tests/integration/test_swap_click.py) (swap-aware follower, see [Swap-seek.md](../archive/Swap-seek.md)), [test_retry_cache_reset.py](../../tests/integration/test_retry_cache_reset.py) (align retry-cache invalidation), [test_auto_align_iteration.py](../../tests/integration/test_auto_align_iteration.py) + [test_lg_daylight_alignment.py](../../tests/integration/test_lg_daylight_alignment.py) (align convergence). Lock v1 acceptance + explicit deferrals (see *Scope*).
   *Gate:* each high-risk behavior has an executable test or scripted check.
**Phase 1 — Fresh tree & engine closure.** Stand up the new project with a C++ engine library target. Copy the leaf modules **verbatim** — [src/media/](../../src/media/) (demuxer, decoder, filterer, converter, frame_ring, packet_ring, time_shifter), [src/analysis/](../../src/analysis/) (metrics, structural_fingerprint), and their [src/core/](../../src/core/) closure (core_types, side_aware, FFmpeg wrappers, logging, exception holder, config). Leave behind `src/display/`, SDL, libplacebo, `argagg`, and the CLI.
   *Gate:* library compiles linking FFmpeg only; no `Display` symbols reachable from the engine target.
**Phase 2 — Paused-first deterministic kernel.** Build a command-driven engine that steps **paused-first**, with no playback clock yet. Copy the behavior *slices* verbatim from [video_compare.cpp](../../src/app/video_compare.cpp) — seek barrier + seek-target math, follower-shift / swap semantics, time-shift, auto-align retry-cache wiring — but **rebuild the timer-driven `operator()()` loop** as the command kernel (that loop shell *is* the display/timer coupling we're subtracting). Generalize the one-fixed-side + N-switchable mechanism into a **warm LRU pool with arbitrary pairing** (`set_side` on either side). Expose a state-snapshot API for parity checks. Drop the choices-file / RESULTS-verdict plumbing.
   *Gate:* migrated suites green (swap-click, auto-align iteration, retry-cache invalidation); seek-and-land is deterministic under repeated command sequences.
**Phase 3 — Frame contract.** VideoToolbox `CVPixelBuffer` passthrough as default; P010-into-IOSurface as the SW fallback. Surface per-frame color tags. Document the frame-lifetime contract (borrowed-until-next-call vs. pooled) explicitly.
**Phase 4 — C ABI.** `extern "C"` header over the kernel: pool + `set_side`, seek, iterative align, frame access, snapshot. **One-shot commands replace transient flags.** Links FFmpeg only — no SDL3 / libplacebo / Vulkan / MoltenVK in the tree.
   *Gate:* Swift can drive sessions and read snapshots without UI hacks; frame-ownership rules enforced.
**Phase 5 — Obj-C++ bridge & Swift package.** CVPixelBufferPool, attachment tagging, C-struct marshalling, module map / xcframework.
**Phase 6 — Swift rendering & core UX.** Core Image for ingest / scale / layout / composite / histogram, HDR into an EDR `CAMetalLayer` (the OS tone-maps); custom Metal for waveform / vectorscope and, only if needed, the difference kernel and a dither pass. Rotation as a render-time transform; a true-pixel (nearest-neighbor) magnify for pixel-peeping. Tune rapid reseek / re-align ergonomics.
   *Gate:* v1 workflows pass on the fixture matrix; HDR/EDR validated on real hardware (a headroom display and a plain SDR display).
**Phase 7 — Real-time & advanced parity.** Add continuous playback cadence / loop-mode parity beyond snippets, then v1.5 / v2 features by priority, then multi-4K HDR perf tuning. Retire the legacy app as the oracle once v1 parity holds.

## Scope: v1 / v1.5 / v2

A deliberately small v1 centered on the dominant loop, with explicit deferrals.

- **v1 (must-have):** pair session over a candidate pool with **arbitrary pairing + warm reuse**; split / hstack / vstack; fast seek + reseek; follower-shift and swap-aware semantics; auto-align baseline incl. iterative retry, cache invalidation, and per-pairing offset cache; pixel-peep (paused zoom / pan with stable frame identity, true-pixel magnify); play-short-snippet from the cursor with quick return to paused; basic screenshot / export. Verdict stays host-side.
- **v1.5 (high-value follow-on):** directional auto-align modes + richer retry controls; ROI selection and crop / save-selected-area; basic paused-first quality-metrics overlay.
- **v2 (advanced parity):** full scopes (histogram / waveform / vectorscope); advanced dock interactions if still needed post-integration; continuous loop-mode parity; deeper multi-4K HDR perf tuning.

> It's tempting to defer multi-source / arbitrary pairing to keep v1 small. We **reject** that: non-linear re-pairing across a candidate pool *is* the primary workflow (the user bounces between candidates and keeps several), so it is core v1. The warm pool's instant-revisit *optimization* may degrade to cold reload under memory pressure, but the pairing capability itself is not deferrable.

## What is reimplemented in Swift (not in the engine)

The library provides decode, filters, CPU tone-map-or-passthrough, PSNR/SSIM/VMAF, auto-alignment, and time-shift sync — all already display-independent. The Swift side **reimplements** the front-end that lives in `src/display/` today: split/vstack/hstack compositing, difference/subtraction mode, the scopes (histogram/vectorscope/waveform), overlays, and the dock scrubber.

## Future work / open questions

- **Rotation handling.** Confirm container display-matrix rotation and user rotate requests can both be applied as render-time transforms, keeping the zero-copy path for the common case. Exotic filters remain on the SW-fallback branch.
- **Codec coverage.** Confirm which sources VideoToolbox decodes to `CVPixelBuffer` directly vs. which hit software decode + the swscale fallback (mainly non-HEVC/H.264 codecs, and older Intel Macs still on 14).
- **Warm-pool policy.** How many candidate pipelines to keep open (LRU) given packet-ring memory (~256 MiB each by default), and whether to prefetch likely-next candidates to hide open latency.
- **Alignment caching.** Confirm converged offsets are stable enough to cache per source-pair (and reuse across seek positions), so re-pairing and seek-back can skip re-aligning — the workflow re-aligns after nearly every seek, so this is a meaningful win if offsets hold.
- **Frame-lifetime / pool sizing** for deep scrub history without doubling memory (relates to `frame_buffer_size` and the per-source packet ring).
- **Mixed-headroom / multi-display EDR.** Behavior when the window spans displays with different headroom, and whether to offer a fixed SDR-preview mode for consistent A/B regardless of the reviewer's display.

## References

- [libplacebo integration](libplacebo-integration.md) — the current GPU pipeline, including the non-zero-copy readback note that motivates the native handoff.
- [Architecture](../ARCHITECTURE.md) — overall subsystem layering.
- [Playback buffer](buffer.md) — `FrameRing` / `PacketRing` semantics the pull-based API builds on.
- [Auto-alignment](auto-align.md) — the fingerprint/correlation engine exposed via `vc_auto_align`.
