# Resource Investigation

## Status (2026-04-18)

**Resolved.** Root cause of the 15s → 25s CPU regression identified: commit `628eab8` linked `ffmpeg-full`, which enabled HDR tonemapping (zscale) that was previously silently skipped. Not a code bug — a feature activating.

The CPU pipeline has since been fully migrated to libplacebo GPU rendering (see `Docs/Libplacebo-design.md`). With `--hwaccel videotoolbox`, 4K HDR 60fps now plays smoothly. The display.cpp mega-function decomposition (phases 10–11) has consolidated the GPU and SDL rendering paths via a RenderContext pattern, further clarifying the architecture. See `Docs/Libplacebo-design.md` post-refactor note for the module map. The CPU-side optimizations below were the stepping stones before the full GPU migration.

---

## Note on the baseline data

The data does show a regression. Between tag `20260308` and `cff1cc7` (a short window), **user CPU jumped 15s → 25s (+67%)** and **RSS grew 3.34GB → 3.95GB (+18%)**, while everything from `20250824` through `20260308` is flat. So "compounded gradually" is partly true for RSS long-term, but there is a concrete recent CPU regression worth bisecting first.

Cheapest-first plan:

## 1. Bisect the recent CPU regression (cheap, high signal)

`git log --oneline 20260308..cff1cc7` is probably <30 commits. Re-run the `gtime` harness across that range (or `git bisect run` with a script that fails when user time > 20s). This likely explains the 15→25s jump for free.

## 2. macOS-native profiling (no rebuild needed)

- **Instruments → Time Profiler**: attach to running `./video-compare`, get flame graph + per-symbol CPU. Best tool available on Darwin.
- **Instruments → Allocations** (with "Record reference counts" off for speed): shows live allocations by call stack, persistent vs transient, and growth over time. This is the right tool for "where are my 3.95GB going."
- **`sample ./video-compare 5`** while running: one-line poor-man's profiler, gives stack samples fast.
- **`vmmap $(pgrep video-compare)`**: breaks RSS down by region (MALLOC_LARGE, MALLOC_TINY, mapped files, VM_ALLOCATE from SDL/FFmpeg/GL). Tells you whether the bulk is frame buffers, GPU textures, or general heap — that alone redirects the investigation.
- **`leaks --atExit -- ./video-compare ...`**: catches true leaks (distinct from "uses a lot").
- **`MallocStackLogging=1 ./video-compare ...`** then `malloc_history <pid> -allBySize`: ranks every live allocation by stack. Heavy but definitive.

## 3. Linux-side tools if available

`heaptrack` is dramatically better than anything on macOS for heap analysis (shows total allocations, temporary allocation count, peak, leaks, flame graph). Often reveals "you allocated 40GB total over 5s churning small buffers" — which would explain rising CPU without rising RSS. `perf record -g` + `hotspot` for CPU.

## 4. Lightweight in-process instrumentation

Add a debug thread that samples `mach_task_basic_info` (RSS) + `getrusage(RUSAGE_SELF)` (user/sys time, major/minor faults, voluntary ctx switches) every 250ms and logs to CSV. Plot vs. wall-clock to see whether memory grows monotonically (leak-like) or plateaus (working-set), and whether CPU spikes correlate with frame-ring fills or filter graph rebuilds. The 371k minor faults at `cff1cc7` vs 349k at `20260308` suggests more allocation churn, not just bigger steady-state buffers — that is a meaningful signal.

## 5. Targeted counters

Given the domain (frame ring, filter graphs, VMAF, scope windows), add cheap counters around suspects: frames allocated/freed, `av_frame_alloc` / `sws_getContext` / `av_buffersink` calls, format_converter reconfigures, VMAF invocations. Log deltas every second. Cheap to add, often pinpoints the culprit faster than a profiler.

## Recommended order

1. Bisect `20260308..cff1cc7` first — likely one or two commits.
2. `vmmap` + Instruments Allocations on `cff1cc7` to characterize the 600MB long-term growth.
3. `heaptrack` (on Linux) if you want to quantify allocation churn.

## Findings (2026-04-15)

### Recent CPU regression: root-caused to `ffmpeg-full` linkage

`git bisect` over `20260308..cff1cc7` (script at [bisect-cpu.sh](bisect-cpu.sh)) identified `628eab8` as the first bad commit. That commit is a **makefile-only** change: it adds `-I/opt/homebrew/opt/ffmpeg-full/include/` and `-L/opt/homebrew/opt/ffmpeg-full/lib/` when `ffmpeg-full` is installed. No C++ source changed. So the regression is entirely in the linked FFmpeg dependency, not our code.

A/B (same source tree, same compiler, `-Ofast`, 5s-baseline plus 15s run to separate startup vs steady-state):

| build            | 5s user | 15s user | steady-state (Δ/10s) | RSS@15s | CPU%  |
|------------------|--------:|---------:|---------------------:|--------:|------:|
| mainline ffmpeg  |   14.45 |    45.61 |          3.12 cores  | 3.41 GB |  315% |
| ffmpeg-full      |   21.16 |    80.18 |          5.90 cores  | 4.20 GB |  541% |

So `ffmpeg-full` linkage costs us **~6.7s startup user-CPU**, **~2× steady-state CPU**, and **~800 MB extra RSS** versus mainline `ffmpeg`.

The decoder path is *not* the culprit. Decoding `testdata/large-1.mp4` to `-f null` with each build's own `ffmpeg` CLI gives **identical user CPU** (19.95s both) — libavcodec per-frame cost is unchanged. `ffmpeg-full` reports the same avcodec/avfilter/swscale version numbers as mainline and was built with the same clang, same `--enable-neon`, same `--enable-videotoolbox`.

The observed CPU% jump (315% → 541%) corresponds to ~2–3 additional cores busy in parallel during the *same wall-clock time*, which is the signature of background threads from transitively-loaded dylibs, not extra work on the decode thread. `ffmpeg-full`'s libavcodec/libavfilter pulls in ~60 extra dylibs vs mainline, including: `libggml`, `libwhisper`, `vulkan-loader`, `libplacebo`, `shaderc`, `libtesseract`, `libass`, `libharfbuzz`, `fontconfig`, `libjxl`, `libvmaf`, `libaom`, `librav1e`, `libsvt-av1`, and more. Several of these perform non-trivial init work or start worker threads at load time — `ggml` and `libplacebo`/Vulkan in particular.

### Reproducing the A/B locally

Temporarily guard the `ffmpeg-full` block in [makefile](../makefile) with `NO_FFMPEG_FULL`:

```make
ifeq "$(NO_FFMPEG_FULL)" ""
ifneq "$(wildcard /opt/homebrew/opt/ffmpeg-full)" ""
  ...
endif
endif
```

Then:

```bash
make clean && NO_FFMPEG_FULL=1 make -j   # mainline ffmpeg
gtime -f 'user=%U sys=%S wall=%e cpu=%P rss=%M' \
  timeout -k 10s 15s ./video-compare testdata/large-1.mp4 testdata/large-2.mp4

make clean && make -j                    # ffmpeg-full
gtime -f 'user=%U sys=%S wall=%e cpu=%P rss=%M' \
  timeout -k 10s 15s ./video-compare testdata/large-1.mp4 testdata/large-2.mp4
```

### Next steps / options

1. **Confirm the thread source.** Run `sample $(pgrep video-compare) 5` against the `ffmpeg-full` build during steady-state and look for thread stacks in `ggml`, `libplacebo`, Vulkan, `fontconfig`, or `libass`. That will name the culprit dylib(s).
2. **If the extra threads are avoidable**, we may be able to suppress them via env var (e.g. `GGML_N_THREADS=1`) or by not actually using the filters that cause them to spin up.
3. **Structural fix** — we link against `ffmpeg-full` only because Homebrew moved `libvmaf` into it. Options:
   - Link only `libvmaf` directly (from `/opt/homebrew/opt/libvmaf`) and continue linking mainline FFmpeg for everything else. VMAF usage in `vmaf_calculator.cpp` is via `libvmaf` API, not via FFmpeg's `libavfilter` vmaf filter — if that's true, we don't need `ffmpeg-full` at all.
   - If we *do* use the FFmpeg `vmaf` filter, consider invoking it as a subprocess (a separate `ffmpeg-full` binary) only for VMAF mode, and keep the interactive player linked against mainline FFmpeg.
4. **Document the trade-off** in the README for users who install `ffmpeg-full`: they will pay ~2× the CPU and ~800 MB extra RSS.

### Still to investigate (longer-term growth independent of this regression)

The older-tag RSS baseline (~3.34 GB) is still large. A pass with Instruments Allocations or `vmmap` on a mainline-linked build can characterize where that sits — expected frame-ring vs other. Defer until after the `ffmpeg-full` decision above, since that alone changes RSS by ~800 MB.

## Correction (2026-04-15, after `sample` A/B)

The "regression" is not library overhead. It is **HDR tonemapping that was previously being silently skipped**.

### What the sample showed

Sampled both builds for 3 s in steady-state playback:

| build            | thread count | libswscale samples | libzimg samples | libavcodec samples |
|------------------|-------------:|-------------------:|----------------:|-------------------:|
| mainline ffmpeg  |           52 |                274 |               0 |               1125 |
| ffmpeg-full      |           89 |               1707 |            1090 |               1152 |

Decoder cost is identical. Extra CPU is entirely in `libswscale` + `libzimg`. The `libzimg` stacks are called from `libavfilter` via `zimg_filter_graph_process` → `zimg::colorspace::...::ToLinearLutOperationNeon` / `ToGammaLutOperationNeon` / `MatrixOperationNeon` / `ResizeImplV_U16_Neon`. No `VMAFCalculator`, `libvmaf`, `run_libvmaf_filter`, `ggml`, `vulkan`, `libplacebo`, or `fontconfig` frames appear in hot stacks — ruling out both VMAF-dependent behavior and background-thread-init hypotheses.

### Why the delta is real work, not overhead

The test files (`testdata/large-1.mp4`, `testdata/large-2.mp4`) are **3840×2160, 10-bit, HLG, BT.2020** — HDR content:

```
codec_name=h264  pix_fmt=yuv420p10le  color_space=bt2020nc  color_transfer=arib-std-b67  color_primaries=bt2020
codec_name=hevc  pix_fmt=yuv420p10le  color_space=bt2020nc  color_transfer=arib-std-b67  color_primaries=bt2020
```

In `video_filterer.cpp:145`, default `ToneMapping::Auto` + `is_hdr_trc` → `must_tonemap = true`, inserting `zscale=t=linear:npl=...,tonemap=clip:param=...,zscale=p=...:t=...` into the graph. `video_filterer.cpp:208` explicitly checks `avfilter_get_by_name("zscale")` and, if it returns null, pushes a warning (`"zscale filter missing in libavfilter build"`) and proceeds without tonemapping.

Mainline Homebrew `ffmpeg` is built without `--enable-libzimg`, so `zscale` is not registered. Before `628eab8`, the tonemap branch was being silently degraded to "pass HDR through as SDR" on this platform. After `628eab8`, the build links against `ffmpeg-full` which has `libzimg` → `zscale` works → full HDR-to-display tonemapping runs per frame on 4K 10-bit pairs. That is genuinely expensive colorspace work (per-pixel linear-light LUT + matrix + gamma LUT, on two streams).

### Implication

- **Do not revert `628eab8`.** Prior behavior was silently wrong for HDR content (with a warning that was easy to miss). The post-commit behavior is correct but costly.
- The 15 s "baseline" from older tags / the mainline A/B is not a valid performance target — it measures broken HDR handling, not a faster version of the same work.

### Suggested next A/B

To isolate how much of the CPU/RSS budget is genuinely tonemap vs everything else, re-run the harness on an **SDR** input pair of similar resolution/bitdepth (e.g. 4K 10-bit BT.709, or 4K 8-bit BT.709). On SDR content `must_tonemap` is false, the zscale/tonemap branch never inserts, and the profile should collapse back to the libavcodec + libswscale cost we see in the mainline sample. That gives three meaningful numbers:

1. ffmpeg-full + SDR content — the real "no-HDR" baseline.
2. ffmpeg-full + HDR content — the current observed 5.90-core steady state.
3. Their delta — the actual cost of tonemapping, which is what we'd want to optimize.

### Legitimate optimization angles (after the SDR A/B)

If tonemapping dominates and we want it cheaper:

- Tweak `zscale` precision / dither — current chain uses default float intermediates; `d=none` and lower-precision intermediates may help.
- Switch `tonemap=clip` to `tonemap=hable` / `reinhard` / `mobius` and measure — curve choice affects cost.
- Skip tonemapping when the display's reported color space matches the content's (HDR-capable displays showing HDR content natively).
- Cache the tonemapped frame keyed on frame pts + tonemap params, so scrubbing/pause redraws don't re-run the graph.
- Move tonemap to GPU (Metal shader on the already-uploaded texture) instead of CPU via avfilter — substantial work, but this is where the budget goes.

The ~800 MB RSS delta is almost certainly zscale's internal working buffers for 4K 10-bit two-stream float-precision processing; it should disappear on SDR input.

## SDR A/B results (2026-04-15)

SDR test clips generated by stripping HDR metadata (same codec, resolution, 10-bit depth; only `color_{primaries,transfer,space}` changed to `bt709`):

```bash
ffmpeg -i testdata/large-1.mp4 -map 0 -c:v copy -c:a copy -c:s copy \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -tag:v avc1 \
  testdata/sdr-1.mp4
ffmpeg -i testdata/large-2.mov -map 0 -c:v copy -c:a copy -c:s copy \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -tag:v hvc1 \
  testdata/sdr-2.mov
```

Same `ffmpeg-full`-linked binary, same machine, 5 s and 15 s runs:

| run           | user@5s | user@15s | Δ/10s wall | steady-state | RSS@15s | CPU% |
|---------------|--------:|---------:|-----------:|-------------:|--------:|-----:|
| HDR           |   25.54 |    81.72 |      56.18 |  5.62 cores  | 4.47 GB | 554% |
| SDR           |   20.34 |    65.31 |      44.97 |  4.50 cores  | 4.42 GB | 444% |
| Δ (tonemap)   |   +5.20 |   +16.41 |     +11.21 | +1.12 cores  |  +50 MB | +110%|

**Isolated cost of HDR tonemapping:** ~1.12 extra cores during steady state, ~5 s extra startup user-CPU, **~50 MB extra RSS (negligible)**. So the HDR tonemap path, while real, explains only about 40 % of the ffmpeg-full vs mainline CPU gap and essentially none of the ~800 MB RSS gap.

### What accounts for the rest

A 3 s stack sample against the SDR run still shows substantial libzimg activity (**558 samples, vs 1090 on HDR, vs 0 on mainline**). Caller chain: `libavfilter → zimg_filter_graph_process`. But the zimg operations are different on SDR:

```
SDR hot zimg ops:   depth_convert_w (10→float), ordered_dither_f (float→8-bit)
HDR hot zimg ops:   ToLinearLutOperationNeon, ToGammaLutOperationNeon,
                    MatrixOperationNeon, ResizeImplV_U16_Neon (i.e. tonemap)
```

Both test sets are `yuv420p10le`. libavfilter in the `ffmpeg-full` build appears to prefer `zscale` over `swscale` for bit-depth / format conversion paths whenever `libzimg` is registered, even when no tonemap is needed. That is where the remaining steady-state CPU and most of the 800 MB RSS delta live — zscale's float intermediates on 4K 10-bit streams on two sides are the memory cost, and its per-frame conversion cost is the steady CPU.

### Updated conclusion

The `628eab8` commit did not introduce a performance regression in our code. It swapped our runtime dependency to a FFmpeg build that (a) actually registers `zscale`, enabling correct HDR tonemapping that had been silently degraded before, and (b) causes libavfilter to route bit-depth/format conversion through zscale instead of swscale for 10-bit content, which is heavier but higher-quality.

Two cleanly separable costs, both real:

1. **HDR tonemapping** (~1.12 cores steady, ~5 s startup, ~50 MB RSS) — quality feature, was broken before.
2. **zscale-over-swscale for 10-bit format conversion** (~1.4 cores steady, ~800 MB RSS) — also a quality / precision improvement, applies to *all* 10-bit content not just HDR.

### Candidate optimizations, re-prioritized

From biggest budget first:

- **zscale RSS** — zscale's internal working buffers dominate the ~800 MB delta. Investigate zscale options we could set (via the `scale` / `zscale` filter args) for lower-precision intermediates or narrower strips. If our filter-graph construction doesn't currently pass `zscale` parameters, we may be getting its default precision.
- **Force swscale for 8-bit-equivalent content** — if the output we render is 8-bit anyway, we may be able to convert 10→8 in swscale and skip the zscale depth path entirely, for SDR 10-bit content. Non-trivial because libavfilter picks zscale when available; would need to structure the filter chain to keep format conversion out of avfilter.
- **Cache tonemapped output** keyed on frame pts + tonemap params so paused / scrubbed-back redraws don't re-tonemap.
- **Switch `tonemap=clip` to `hable` / `mobius` / `reinhard`** and benchmark — curve choice can move ~0.2–0.5 cores.
- **GPU tonemap** via Metal shader on the uploaded texture instead of CPU via avfilter — largest win potential but most invasive.

## Correction #2 (2026-04-15): the "zscale-over-swscale for 10-bit" claim was wrong

The first SDR A/B was contaminated. `ffmpeg -c:v copy -color_primaries bt709 ...` only rewrites **container-level** color tags; for HEVC (and H.264) the decoder reads the color info from the **bitstream VUI** inside the SPS, which `-c:v copy` leaves untouched. `ffprobe` happened to show the new container tag and made the file look SDR, but the decoder still saw HDR. Video-compare's own log line was the giveaway:

```
[RIGHT]  Input: ... yuv420p10le (tv, bt2020nc/bt2020/arib-std-b67), ... testdata/sdr-2.mov ...
```

### Correct SDR re-encode

Rewrite the bitstream VUI with a bitstream filter (`h264_metadata` / `hevc_metadata`) alongside the copy:

```bash
ffmpeg -i testdata/large-1.mp4 -map 0 -c:v copy -c:a copy -c:s copy \
  -bsf:v h264_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1 \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -tag:v avc1 \
  testdata/sdr-1.mp4

ffmpeg -i testdata/large-2.mov -map 0 -c:v copy -c:a copy -c:s copy \
  -bsf:v hevc_metadata=colour_primaries=1:transfer_characteristics=1:matrix_coefficients=1 \
  -color_primaries bt709 -color_trc bt709 -colorspace bt709 -tag:v hvc1 \
  testdata/sdr-2.mov
```

Verify with `ffmpeg -i <file>` (not `ffprobe`), which reports the decoded stream params; both should show `yuv420p10le(tv, bt709, ...)`.

### Filter-graph evidence (from `VC_DUMP_FILTER_GRAPH=1`)

Instrumented the three filter-graph sites (`video_filterer.cpp`, `vmaf_calculator.cpp`, `scope_window.cpp`) with `avfilter_graph_dump` gated on env var `VC_DUMP_FILTER_GRAPH`. Left in place for future diagnosis.

**HDR graph** (both sides): `buffer → fps → auto_scale(scale) → format(rgb48) → auto_scale(scale) → zscale → buffersink` — two auto-inserted conversion nodes, both of type `scale` (i.e. **libswscale**, not zscale). The explicit `format=rgb48` is effectively redundant because the preceding auto_scale already produces `rgb48le`.

**True-SDR graph** (LEFT): `buffer → fps → buffersink`. **RIGHT**: `buffer → copy → buffersink`. No conversion, no zscale, no format change — pure pass-through at `yuv420p10le`.

This debunks the earlier hypothesis that libavfilter was routing 10-bit format conversion through zscale. The auto-inserted conversion uses `(scale)` = libswscale. The `(zscale)` nodes are only the ones our filter string explicitly requests, and those only fire when `must_tonemap` is true.

### Clean numbers (same `ffmpeg-full`-linked binary)

| run                      | user@5s | user@15s | Δ/10s | steady-state | RSS@15s |  CPU% |
|--------------------------|--------:|---------:|------:|-------------:|--------:|------:|
| HDR                      |   25.86 |    82.27 | 56.41 |  5.64 cores  | 4.28 GB | 559%  |
| True SDR                 |   15.27 |    47.40 | 32.13 |  3.21 cores  | 4.07 GB | 326%  |
| Δ (tonemap, isolated)    |  +10.59 |   +34.87 |+24.28 | +2.43 cores  | +210 MB | +233% |

### Profile confirms (3 s sample, true-SDR, steady-state)

| module      | HDR samples | True-SDR samples | mainline (HDR passthrough) |
|-------------|------------:|-----------------:|----------------------------:|
| libavcodec  |        1152 |             1182 |                        1125 |
| libswscale  |        1707 |              265 |                         274 |
| libzimg     |        1090 |            **0** |                           0 |

### Final conclusion

The full cost of the `628eab8` "regression" — **all of it** — is HDR tonemapping actually running. There is no hidden 10-bit format-conversion tax from `ffmpeg-full`. On truly SDR 10-bit content, `ffmpeg-full` and mainline `ffmpeg` produce near-identical profiles (3.21 vs 3.12 cores, 4.07 vs 3.41 GB RSS; the remaining ~600 MB RSS gap is attributable to the larger resident image from `ffmpeg-full`'s ~60 extra dylibs).

### Revised optimization priorities

- **Tonemap path is the only real target.** Budget it at ~2.4 cores + ~210 MB RSS for a 4K 10-bit HDR pair.
- Cache the tonemapped frame keyed on pts + tonemap params, so re-draws don't re-tonemap.
- Try cheaper tonemap curves (`hable`, `mobius`, `reinhard`) in place of default `clip`.
- Investigate moving tonemap to GPU (Metal shader) — avoids avfilter/zimg/libswscale entirely.
- Skip the tonemap chain when the display is HDR-capable and the content's primaries/transfer already match the display.
- No action needed on the ~600 MB RSS image gap — it's fixed-cost dylib mapping, not working set.

Detailed tonemap optimization analysis and implementation progress: see [Tonemap-optimization.md](Tonemap-optimization.md).
