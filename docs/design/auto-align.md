# Auto-align

Auto-align temporally synchronizes the right video to the left video's current position by scoring candidate right-side frames for structural similarity against a small temporal window around the left frame, then folding the best offset into `shift_right_frames` so the standard seek dispatch handles the motion. It's designed for mixed-framerate and SDR/HDR pairs with a border or colour-grade difference between them — the matching primitive is robust to all three.

Key files:

- `app/video_compare.cpp` — the inline auto-align block in `compare()` (right before the seek-dispatch branch) and the post-seek verification block (right after `log_seek()` in the `after_seek_block:` epilogue).
- `analysis/metrics/metrics_calculator.{h,cpp}` — `compute_structural_fingerprint`, `structural_correlation`, and `kFingerprintSize`.
- `app/video_compare.h` — `SwsContextDeleter`, `SwsContextUniquePtr`, `AutoAlignSwsKey`, `PendingAutoAlignVerification`, `auto_align_sws_cache_`, `pending_auto_align_verification_`.
- `display/subsystems/playback_controller.h` — request flag + `AutoAlignMode` carried from the key handler.
- `display/display_input.cpp` — `SDLK_GRAVE`, `SDLK_LEFTBRACKET`, `SDLK_RIGHTBRACKET` bindings.
- `display/display_types.h` — `enum class AutoAlignMode { Symmetric, Backward, Forward }`.
- `media/buffering/packet_ring.h` — the encoded-packet buffer the walk reads from (see [buffer.md §10](buffer.md)).

All line numbers referenced in this doc are anchors into the current tree, not stable commit references.

---

## 1. Keybindings and modes

| Key     | `AutoAlignMode` | Window (relative to left's current PTS)  | Typical cost
| ------- | --------------- | ---------------------------------------- | -------------
| `` ` `` | `Symmetric`     | `[-0.5 s, +0.5 s]`                       | Ring-only, no decode (microseconds)
| `[`     | `Backward`      | `[-1.0 s,  0.0 s]`                       | Ring + ~15 decoded frames at 30 fps
| `]`     | `Forward`       | `[ 0.0 s, +1.0 s]`                       | Ring + ~15 decoded frames at 30 fps

The Symmetric window sits entirely inside the default 15/15 FrameRing on typical framerates — a `` ` `` press usually runs with zero decode cost. The directional `[` / `]` windows extend past the ring on one side; the uncovered half is decoded inline via a barriered PacketRing walk (§4 below).

Window extents are constants (`kAutoAlignSymmetricHalfSec`, `kAutoAlignDirectionalSec`) in `video_compare.cpp`, not CLI flags. The user explicitly called out a future interest in making them configurable and in handling pairs with "different sustained black intro cards" — that's on the backlog.

---

## 2. Algorithm overview

Each press runs the same five-phase pipeline:

1. **Resolve window**: map the requested `AutoAlignMode` to signed `[window_start_rel_sec, window_end_rel_sec]` offsets on the left time axis.
2. **Seed candidate pool from the right FrameRing**: fingerprint every ring-resident right frame. These are free — the frames are already RGB-packed after the format converter.
3. **Extend via PacketRing walk** if the ring doesn't span the window: enter a right-side-only `ReadyToSeek` barrier, drive the decoder inline from `keyframe_at_or_before(window_start - 0.5 s)`, fingerprint each decoded YUV frame whose PTS lands in the window, restore the pipeline. The FrameRing is never touched during the walk — the decoded frames are discarded after fingerprinting.
4. **Score every candidate** with a 5-probe windowed structural correlation against left-ring probe frames. Pick the best offset, tie-breaking toward the smallest seek distance.
5. **Decide and dispatch**: refuse to seek under low confidence, "already aligned" early-out, or fold the offset into `shift_right_frames` so the standard L0/L1/L2 seek path carries out the motion. On the next main-loop iteration after the seek lands, rescore against the new right-ring current to verify.

The whole thing is synchronous with the press — no background thread, no follow-up work across frames other than the one verification pass after the seek.

---

## 3. Structural fingerprint

The matching primitive is a normalized downscale; see [`analysis/metrics/metrics_calculator.cpp`](../../analysis/metrics/metrics_calculator.cpp) `compute_structural_fingerprint`.

- **Input**: an AVFrame (either ring-resident RGB or raw-decoded YUV) plus a cached `SwsContext` keyed by `(src_format, src_width, src_height)`.
- **Pass 1 — swscale**: downscale to `kFingerprintSize × kFingerprintSize` (64 × 64) with `AV_PIX_FMT_GRAY8` output and `SWS_BILINEAR`. swscale pulls luma correctly from both packed-RGB and planar-YUV sources, so one code path handles every input format.
- **Pass 2 — normalize**: subtract the mean, divide by the standard deviation. Output is a 4096-element `std::vector<float>` with zero mean and unit variance. Flat frames (stddev below 1e-6) collapse to all-zero output — correlation against them is 0, which correctly signals "uninformative".

The match score is `structural_correlation(a, b) = dot(a, b) / 4096`, i.e., Pearson correlation on the normalized fingerprints. This is algebraically the SSIM `structure` term. Dropping the `luminance` and `contrast` terms is the point — absolute intensity mapping (BT.709 vs PQ vs HLG) cancels out, leaving only spatial structure. That's why the SDR/HDR lg1 test pair correlates at > 0.97 across the border, colour-grade, and codec differences.

Cost per fingerprint: roughly 30 µs on a modern CPU, dominated by the swscale pass.

SwsContext instances are cached on the VideoCompare object (`auto_align_sws_cache_`, a `std::map<AutoAlignSwsKey, SwsContextUniquePtr>`) and freed by the destructor via `SwsContextDeleter`. First press builds them; later presses reuse.

---

## 4. Candidate pool construction

The pool is a `std::vector<Candidate>` where each entry is `{int64_t pts, std::vector<float> fp}`.

### 4.1 Ring-resident candidates

Iterate `right.ring.at(off)` for `off ∈ [-history_size(), +prefetch_size()]`, fingerprint each frame via the RGB path (source format matches the format-converter output — e.g. `AV_PIX_FMT_RGB24`, `AV_PIX_FMT_RGB48LE`). A `std::unordered_set<int64_t>` of ring PTS values is built alongside for the dedup check used during the walk.

Ring-resident candidates are included **regardless of whether their PTS lies inside the requested window**. They're free, they improve scoring density, and — particularly under Symmetric mode — they give the tie-break path natural candidates to prefer.

### 4.2 PacketRing walk (conditional)

If the ring already covers the window (`!candidates.empty() && ring_min_pts ≤ window_start_pts && ring_max_pts ≥ window_end_pts`), the walk is skipped. This is the common case for `` ` ``.

Otherwise the walk runs. The sequence mirrors the L1 re-decode pattern ([buffer.md §11](buffer.md)):

1. **Barrier the right side** via `enter_seek_barrier(right_only_pred)`. Left keeps running.
2. **Reinit the right filterer** — the worker called `close_src()` on it while `is_seeking` was true, so the graph is torn down; calling `reinit()` here leaves it ready for the post-walk pipeline restart.
3. **Flush the decoder** and reset its PTS state. The main thread now owns the codec.
4. **Locate the starting keyframe** via `packet_ring.keyframe_at_or_before(window_start - 0.5 s slack)`. The 0.5 s warmup gives the decoder a GOP to stabilize before its output reaches the window. If no keyframe hit, log `[auto-align-walk-skip]` and fall through to scoring without the walk's contribution.
5. **Iterate packets** from the keyframe forward using `range->iterate_from(index, visitor)`:
   - Clone each packet, `VideoDecoder::send` it, then pump `receive()` in a loop.
   - For each decoded raw frame: if it's in the hardware pixel format, `av_hwframe_transfer_data` it to a CPU frame; otherwise use it directly.
   - Convert `raw.pts` from stream time_base to AV_TIME_BASE µs (the same unit ring frames use) via `av_rescale_q(raw.pts, stream_tb, AV_TIME_BASE_Q) - demuxer_start_us`.
   - If `frame_pts_us > window_end_pts`, stop. If `< window_start_pts`, skip (before the window; keep walking forward). If `ring_candidate_pts.count(frame_pts_us) > 0`, skip (ring already has it). Otherwise fingerprint via the YUV path (native source format) and push onto the candidate pool.
6. **Drain the decoder's reorder buffer** with a `send(nullptr)` + `receive` loop.
7. **Post-walk restore**: flush decoder + reset PTS state; **forward**-seek the demuxer to `right.current.pts + 0.001 s` (with a backward fallback if the forward seek fails on sparse-keyframe inputs); restart all right-side queues. The forward-seek direction prevents the pipeline from re-emitting any frames we just fingerprinted.

Exceptions during the walk are caught and logged; the post-walk restore runs either way.

Cost: on 720 p HEVC the walk completes in ~120 ms for 30 decoded frames; on 4 K 60 fps HDR it's closer to 400–600 ms for the same span. The press is user-triggered and blocking, so the cost is paid synchronously but noticed only for the directional keys.

### 4.3 Why the walk uses the YUV path rather than RGB

Ring-resident frames are already filtered (tone-mapped) and upscaled to `max_width × max_height`, so they're packed RGB. Walk-decoded frames come straight from the decoder in native YUV/NV12 — no tone map, no upscale, no filter-graph round-trip. Fingerprinting the raw YUV is cheaper (swscale pulls luma and downscales in one pass) and, more importantly, the result is still directly comparable to RGB-sourced fingerprints because normalization zeroes out the brightness-distribution differences that tone-mapping would have corrected for.

---

## 5. Windowed multi-probe scoring

For each candidate entry `(cand_pts, cand_fp)`:

1. **Hypothesize the time shift**: `delta_t = cand_pts - left_current_pts`.
2. **Build 5 probe targets** on the left axis at `t_k = left_current_pts + k · probe_step_pts` for `k ∈ {-2, -1, 0, +1, +2}`. `probe_step_pts = max(left.delta_pts, right.delta_pts)` — spacing at the coarser frame rate guarantees each probe maps to a distinct frame on both sides.
3. **Score per probe**: find the nearest left frame to `t_k` within `probe_step_pts / 2` tolerance; fingerprint it (cached by left PTS); find the nearest candidate to `t_k + delta_t` within the same tolerance; correlate.
4. **Aggregate**: if fewer than 3 probes landed, disregard the hypothesis — under-supported. Otherwise `score[cand_pts] = sum / n_valid`.
5. **Track best**: prefer higher scores; on near-ties (`|Δscore| ≤ 0.002`), prefer the smaller `|cand_pts - right_current.pts|` (minimise seek distance when the current position is already near-optimal).

This multi-probe structure is the defence against motion aliasing. A single-pose visual match gets a good score at only the center probe; neighbouring probes fail. A genuine alignment scores well across all five. The framerate asymmetry that motivated the redesign falls out naturally from the same mechanism — each probe lands on whatever frame happens to be nearest on each side, so differing frame densities don't bias the score.

Left probe fingerprints are stored in an `std::unordered_map<int64_t, std::vector<float>>` keyed by left frame PTS, populated on first use inside the scoring loop.

---

## 6. Decision gating

After scoring, the loop has `best_score`, `best_pts`, `current_score` (the score at `cand_pts == right_current.pts` if that candidate was present), and `valid_scored`. Four decisions, in order:

| Condition                                                   | Decision          | User message
| ----------------------------------------------------------- | ----------------- | -------------
| `valid_scored == 0`                                         | `no_frames`       | "Auto-align: insufficient probes — no change"
| `best_score < 0.60` (`kAutoAlignConfidenceFloor`)           | `low_confidence`  | "Auto-align: low confidence (score …) — no change"
| `best_pts == right_current.pts` or `best - current < 0.005` | `already`         | "Auto-align: already aligned (score …)"
| *else*                                                      | `seek`            | "Auto-align: shift +N frame(s) (score …)"

On `seek`, the code computes `shift_right_frames += round((best_pts - right_current.pts) / right.delta_pts)` and populates `pending_auto_align_verification_` with everything needed to rescore post-seek:

```
struct PendingAutoAlignVerification {
  bool active;
  float expected_score;
  int expected_shift_frames;
  int64_t left_current_pts;
  int64_t probe_step_pts;
  int64_t delta_t_pts;
  std::unordered_map<int64_t, std::vector<float>> left_probe_fingerprints;
};
```

The left probe fingerprints survive the seek because left isn't touched during a pure right-frame shift.

The confidence floor refuses to act on a guess. If the user pressed `` ` `` with the pair genuinely misaligned by > 0.5 s, no candidate will structurally match — `best_score` stays near 0 or negative, and the seek is declined. The user sees "low confidence — no change" and knows to use `[` / `]` or manually shift with `+` / `-` before trying again.

---

## 7. Post-seek verification

After the seek branch (`after_seek_block:` → `log_seek()` in `compare()`), if `pending_auto_align_verification_.active`, the verification block runs:

1. For each stored left probe fingerprint (keyed by left PTS), compute the expected right target at `left_pts + delta_t_pts`.
2. Find the nearest right frame in the (now-updated) right ring at that target, within `probe_step_pts / 2` tolerance. Fingerprint it fresh.
3. Correlate with the stored left fingerprint. Sum, count valid probes, divide.
4. Compare the result to `expected_score`. If `landed_score >= expected_score - 0.01` (`kAutoAlignVerificationSlack`), the seek landed cleanly — no action. If worse, emit "Auto-align: landed score X.XXX (expected Y.YYY) — seek imprecision" so the user knows the alignment didn't land quite where it was supposed to.
5. Clear `pending_auto_align_verification_`.

No retry, no rollback — that was an explicit user-chosen policy. A failing verification is a diagnostic, not a correction.

Typical landings via L0 pivot (the happy path, when the best candidate was in-ring) score identically to the pre-seek expectation. L1 or L2 landings can drift by a frame or two due to drain rounding; the verification surfaces those cases.

---

## 8. Diagnostic logging

Set `VIDEO_COMPARE_LOG_AUTO_ALIGN=1` to stream per-press diagnostics to stderr, formatted to match the existing `[seek-timing]` / `[loop]` conventions:

```
[auto-align] pts_delta_ms=<per-candidate>  probes=<n>/5  score=<per-candidate>
  ... one line per scored candidate ...
[auto-align] mode=<sym|back|fwd> ring=<n> decoded=<n> decode_ms=<int>
             walk=<none|done|skipped> candidates=<total> scored=<n>
             best_pts_delta_ms=<float>  best_score=<float|n/a>
             current_score=<float|n/a>  shift_frames=<int>
             decision=<seek|already|low_confidence|no_frames>
             window=[start_rel, end_rel]s
[auto-align] landed_pts_delta_ms=<float>  landed_score=<float>
             expected_score=<float>  ok=<true|false>  probes=<n>
```

The per-candidate lines come first, then the decision summary, then — for a `seek` decision — the post-seek verification line after the seek dispatch completes. `[auto-align-walk-skip] reason=...` fires if the PacketRing walk is skipped (e.g., target not covered by the PacketRing, single-decoder mode, single-frame media).

---

## 9. Headless testing via the input-script harness

The full pipeline can be driven headlessly by the scripted-keystroke harness (see [`input-testing.md`](input-testing.md)). The ` ` `, `[`, and `]` keys are recognized by name (`grave` / `backtick` / `backquote`) or as single characters.

Primary ground-truth test — the lg1 SDR/HDR pair with a known +2.066 s content offset:

```
VIDEO_COMPARE_INPUT_SCRIPT=tmp/auto_align_lg1.txt \
VIDEO_COMPARE_LOG_AUTO_ALIGN=1 \
./video-compare -t 2.0 \
  testdata/lg1/lg1-extrashifted-border10px-sdr-yuv420p-2160p-59.9fps-x264.mp4 \
  testdata/lg1/lg1-full-hdr-yuv420p10le-720p-30fps-hevc.mp4 \
  2> tmp/auto_align_lg1.log
```

Where the script presses space to pause, then `]`, then quits. The expected `[auto-align]` summary line has `best_pts_delta_ms ≈ 2085.4`, `best_score ≥ 0.90`, `decision=seek`, and the subsequent `landed_…` line has `ok=true`.

See [testdata/alignment/README.md](../../testdata/alignment/README.md) for the frame-level ground truth.

---

## 10. Relationship to other pipeline operations

Auto-align doesn't introduce a new seek tier. It reuses existing infrastructure:

- The **`enter_seek_barrier` helper** ([buffer.md §5.3](buffer.md)) parks the follower-side workers while the PacketRing walk runs.
- The **VideoDecoder send/receive/flush API** (same one L1 and loop-materialize use) drives the inline decode.
- The **PacketRing** ([buffer.md §10](buffer.md)) supplies the packets that would otherwise require a round-trip to disk.
- The **`shift_right_frames` input to the seek branch** carries the chosen offset into the standard L0/L1/L2 dispatch when the follower is RIGHT; under swap (follower=LEFT) the seek dispatches via the absolute-seek path (`seek_relative` + `seek_from_start` + `display_->set_right_only_seek(true, LEFT)`), which ends up as an L2 seek scoped to LEFT.

What's distinct to auto-align is the fingerprint primitive, the windowed multi-probe scoring, and the post-seek verification; everything else is infrastructure shared with the rest of the pipeline.

---

## 11. Swap awareness

Auto-align — like `+`/`-` and shift-click — honours the visual swap. The algorithm is written in terms of a **follower** side (the one the user means when they press a "right video" key) and a **master** side (the stationary reference), resolved from [`Display::follower_side_for_input()`](../../display/display.h):

- No swap: follower = RIGHT, master = LEFT. Candidates drawn from the right FrameRing; probes from left.
- Swap active: follower = LEFT, master = RIGHT. Candidates drawn from the left FrameRing; probes from right.

Everything downstream — candidate-pool build, PacketRing walk barrier, scoring, post-seek verification — goes through `follower_state` / `master_state` aliases, so one algorithm body handles both orientations.

**Seek dispatch** splits on follower side because `shift_right_frames` and the L0 pivot fast path are specifically RIGHT-side mechanisms:

- Follower = RIGHT (common case): fold `best_pts − follower.current.pts` into `shift_right_frames`. Standard L0/L1/L2 dispatch follows; L0 pivot handles the common case with no re-decode.
- Follower = LEFT (under swap): dispatch via the absolute-seek path shared with shift-click (Phase 2 of [docs/planning/Swap-seek.md](../planning/Swap-seek.md)). Compute the target as a fractional timeline position, set `seek_relative` + `seek_from_start`, call `display_->set_right_only_seek(true, LEFT)`. The main loop's `should_seek` predicate scopes the seek to LEFT only; the TimeShifter update recovers the stationary side's static_shift with a flipped sign.

The L0 pivot is not symmetric. Under swap, even small LEFT shifts fall through to L2 (~half a second on typical hardware). Making the pivot symmetric is a potential future optimisation but outside the scope of the swap-aware rollout.

**Verification** (`PendingAutoAlignVerification`) carries `follower_side` so the post-seek rescore looks up the landed frame in the correct ring. Probe fingerprints are stored against master PTS keys (master doesn't move during the seek). See [`app/video_compare.h`](../../app/video_compare.h) for the struct.

**Diagnostic logging** gained a `follower=LEFT|RIGHT` field in the `[auto-align] mode=…` summary line. Example:

```
[auto-align] mode=fwd follower=LEFT ring=25 decoded=0 decode_ms=725 walk=done \
             candidates=25 scored=25 best_pts_delta_ms=-2052.056 \
             best_score=0.9657 current_score=0.9657 shift_frames=0 \
             decision=already window=[0.000,1.000]s
```

**Verification harness**: [tmp/swap_autoalign_test.py](../../tmp/swap_autoalign_test.py) drives the socket API to run an auto-align key press both with and without swap and asserts that the `follower=` field in the log follows the swap state. Reproducible in CI without a display.
