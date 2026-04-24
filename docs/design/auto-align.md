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

Each key's *first* press seeds a searched interval on the follower PTS axis, centered on the follower's current frame. Subsequent presses grow the interval outward in the pressed key's allowed direction(s), carrying the candidate pool and master probes forward — the searched region monotonically grows across presses until something invalidates the cache (see §12).

| Key     | `AutoAlignMode` | Grows on each press                          | Typical cost
| ------- | --------------- | -------------------------------------------- | -------------
| `` ` `` | `Symmetric`     | ±0.5 s on both ends                          | Ring-only on first press, then +decode per extension
| `[`     | `Backward`      | -1.0 s on the low end only                   | Ring + ~15 decoded frames at 30 fps per press
| `]`     | `Forward`       | +1.0 s on the high end only                  | Ring + ~15 decoded frames at 30 fps per press

Mode changes carry the cache forward: pressing `` ` `` then `[` keeps the 0.5 s forward strip the symmetric press already fingerprinted, and extends the low end by another 1.0 s (net interval becomes `[-1.5 s, +0.5 s]`). The mental model is "progress bar on the follower axis that fills in whatever direction the pressed key allows." See §12 for the full cache rules.

The searched interval is clamped to the follower clip's `[start_time, start_time + duration)` — pressing `[` near the clip start stops growing at 0, pressing `]` near the end stops at `duration`. When a directional press lands saturated against its boundary, the decision becomes `boundary_start` / `boundary_end` (not `low_confidence`) so the user knows expansion in that direction has no more frames to offer.

Base widths are constants (`kAutoAlignSymmetricBaseWidthSec`, `kAutoAlignDirectionalBaseWidthSec`) in `video_compare.cpp`, not CLI flags. The user called out a future interest in making them configurable and in handling pairs with "different sustained black intro cards" — that's on the backlog.

---

## 2. Algorithm overview

Each press runs the same five-phase pipeline:

1. **Resolve searched interval**: look up the retry cache; if the prior press's cache is still valid, grow its `[searched_low_pts, searched_high_pts]` by the mode's base width in the allowed direction(s), clamped to the follower clip bounds. If the cache is invalid (or this is a fresh press after a manual intervention), seed the interval from follower.current ± base width. Saturation flags are set whenever growth hits a clip boundary.
2. **Seed candidate pool** from the cache (if reusing) and the follower FrameRing: fingerprint every ring-resident follower frame not already in the cache. Ring frames are free — they're already RGB-packed after the format converter.
3. **Extend via PacketRing walk** if the pool doesn't span the searched interval (plus probe/ring slack): enter a follower-only `ReadyToSeek` barrier, drive the decoder inline from `keyframe_at_or_before(searched_low - 0.5 s warmup)`, fingerprint each decoded YUV frame whose PTS lands in-window and isn't already in the pool, restore the pipeline. Dedup is PTS-keyed, so ring frames are never re-fingerprinted and cache-carried frames from earlier presses are preserved.
4. **Score every candidate** with a 5-probe windowed structural correlation against master-ring probe frames. Scores are stored per-PTS; the decision phase picks based on the mode's rule.
5. **Decide and dispatch per mode**: strictly-stronger-than-current gate for `` ` ``, at-or-above-current gate (iteration-friendly) for `[` / `]`. Boundary saturation overrides "nothing found" outcomes with a dedicated boundary message. On seek, fold into `shift_right_frames` (or the absolute-seek path under swap) and populate `pending_auto_align_verification_`; the next main-loop iteration rescores the landed frame.

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

### 4.1 Cache and ring-resident candidates

On presses with a valid cache, the pool is seeded from `retry.candidates` (move, not copy — reclaimed from the cache). The dedup set `already_fingerprinted_pts` is populated from the cached PTS values so the ring and walk phases skip them.

Then iterate `follower.ring.at(off)` for `off ∈ [-history_size(), +prefetch_size()]`, fingerprint each ring frame whose PTS isn't already in the dedup set. Ring frames use the RGB path (source format matches the format-converter output — e.g. `AV_PIX_FMT_RGB24`, `AV_PIX_FMT_RGB48LE`).

Ring-resident candidates are included **regardless of whether their PTS lies inside the searched interval**. They're free, they improve scoring density, and — particularly under Symmetric mode — they give the tie-break path natural candidates to prefer.

### 4.2 PacketRing walk (conditional)

If the pool already spans `[searched_low_pts − slack, searched_high_pts + slack]` (cache + ring combined), the walk is skipped. This is the common case for `` ` `` and for repeat presses where the cache already covers the expanded interval.

Otherwise the walk runs. The sequence mirrors the L1 re-decode pattern ([buffer.md §11](buffer.md)):

1. **Barrier the follower side** via `enter_seek_barrier(follower_only_pred)`. Master keeps running.
2. **Reinit the follower filterer** — the worker called `close_src()` on it while `is_seeking` was true, so the graph is torn down; calling `reinit()` here leaves it ready for the post-walk pipeline restart.
3. **Flush the decoder** and reset its PTS state. The main thread now owns the codec.
4. **Locate the starting keyframe** via `packet_ring.keyframe_at_or_before(searched_low - 0.5 s warmup)`. The 0.5 s warmup gives the decoder a GOP to stabilize before its output reaches the searched interval. If no keyframe hit, log `[auto-align-walk-skip]` and fall through to scoring without the walk's contribution; this press's decision tags `packet_buffer_miss` so the user sees a distinct message from a clip-boundary saturation.
5. **Iterate packets** from the keyframe forward using `range->iterate_from(index, visitor)`:
   - Clone each packet, `VideoDecoder::send` it, then pump `receive()` in a loop.
   - For each decoded raw frame: if it's in the hardware pixel format, `av_hwframe_transfer_data` it to a CPU frame; otherwise use it directly.
   - Convert `raw.pts` from stream time_base to AV_TIME_BASE µs (the same unit ring frames use) via `av_rescale_q(raw.pts, stream_tb, AV_TIME_BASE_Q) - demuxer_start_us`.
   - If `frame_pts_us > window_end_pts` (searched_high + slack), stop. If `< window_start_pts`, skip (before the window; keep walking forward). If `already_fingerprinted_pts.count(frame_pts_us) > 0`, skip (ring or cache already has it). Otherwise fingerprint via the YUV path (native source format) and push onto the candidate pool.
6. **Drain the decoder's reorder buffer** with a `send(nullptr)` + `receive` loop.
7. **Post-walk restore**: flush decoder + reset PTS state; **backward**-seek the demuxer to `follower.current.pts + 0.001 s`; restart all follower-side queues. Backward-seek direction prevents landing on a far-forward keyframe (sparse-key encodes) that would pollute the ring prefetch.

Exceptions during the walk are caught and logged; the post-walk restore runs either way.

Cost: on 720 p HEVC the walk completes in ~120 ms for 30 decoded frames; on 4 K 60 fps HDR it's closer to 400–600 ms for the same span. The press is user-triggered and blocking, so the cost is paid synchronously but noticed only for the directional keys.

### 4.3 Why the walk uses the YUV path rather than RGB

Ring-resident frames are already filtered (tone-mapped) and upscaled to `max_width × max_height`, so they're packed RGB. Walk-decoded frames come straight from the decoder in native YUV/NV12 — no tone map, no upscale, no filter-graph round-trip. Fingerprinting the raw YUV is cheaper (swscale pulls luma and downscales in one pass) and, more importantly, the result is still directly comparable to RGB-sourced fingerprints because normalization zeroes out the brightness-distribution differences that tone-mapping would have corrected for.

---

## 5. Windowed multi-probe scoring

For each candidate entry `(cand_pts, cand_fp)`:

1. **Hypothesize the time shift**: `delta_t = cand_pts - master_current_pts`.
2. **Build 5 probe targets** on the master axis at `t_k = master_current_pts + k · probe_step_pts` for `k ∈ {-2, -1, 0, +1, +2}`. `probe_step_pts = max(master.delta_pts, follower.delta_pts)` — spacing at the coarser frame rate guarantees each probe maps to a distinct frame on both sides.
3. **Score per probe**: find the nearest master frame to `t_k` within `probe_step_pts / 2` tolerance; fingerprint it (cached by master PTS); find the nearest candidate to `t_k + delta_t` within the same tolerance; correlate.
4. **Aggregate**: if fewer than 3 probes landed, disregard the hypothesis — under-supported. Otherwise `score[cand_pts] = sum / n_valid`.

Scores are stored per-PTS; the decision phase (§6) picks based on the pressed mode's rule. The tie-break band used inside those rules is 0.0005 (`kAutoAlignTieBreakBand`) — much tighter than the improvement epsilon (0.005) so that a sharp 1.000 peak beats a near-peer a few frames closer to current. A 0.002 band historically let a 0.9982 candidate beat a 1.0000 peak on this content; 0.0005 preserves the peak-wins ordering while still absorbing reporting-noise-level jitter.

This multi-probe structure is the defence against motion aliasing. A single-pose visual match gets a good score at only the center probe; neighbouring probes fail. A genuine alignment scores well across all five. The framerate asymmetry that motivated the redesign falls out naturally from the same mechanism — each probe lands on whatever frame happens to be nearest on each side, so differing frame densities don't bias the score.

Master probe fingerprints are stored in an `std::unordered_map<int64_t, std::vector<float>>` keyed by master frame PTS, populated on first use and carried forward across subsequent presses via the retry cache — the master never moves during auto-align, so the same probes are always valid.

---

## 6. Decision gating

After scoring, each candidate has a score keyed by its PTS. The decision phase picks based on the pressed mode, with clip-boundary saturation overriding "nothing found" outcomes. The improvement epsilon (`kAutoAlignImprovementEps = 0.005`) governs WHEN a candidate is eligible for selection; the tie-break band (`kAutoAlignTieBreakBand = 0.0005`) governs ordering between eligible candidates.

**`` ` `` Symmetric — strictly stronger wins:**

- Eligibility: `candidate.score > current_score + 0.005`.
- Among eligible, pick strongest. Score ties (within 0.0005) break by smaller `|pts − follower_current|`; distance ties break toward ahead of current.
- If no eligible candidate: decision = `already` (convergent — re-pressing from a peak doesn't move).
- If both clip boundaries reached and no eligible candidate: decision = `boundary_both`.

**`[` Backward / `]` Forward — at-or-above steps through near-equals:**

- Eligibility: in the pressed direction, `pts ≠ follower_current`, and `candidate.score ≥ current_score - 0.005`.
- Among eligible, pick strongest. A 1.0 short-circuit (score ≥ `1.0 - 0.002`) wins immediately regardless of distance. Score ties break by nearest-in-direction to current.
- If no eligible candidate in the direction and the direction is saturated: decision = `boundary_start` (for `[`) / `boundary_end` (for `]`).
- If no eligible candidate and the direction isn't saturated: decision = `no_stronger_in_direction` — the HUD message suggests pressing again to expand further.

**Confidence floor**: regardless of mode, if the best *unconstrained* score in the pool is below `0.60` (`kAutoAlignConfidenceFloor`), nothing in the pool meets the "this looks like a real alignment" bar. Decision = `low_confidence`. If the pressed direction is saturated this becomes a boundary decision instead, since expansion can't help.

Full decision table:

| Condition                                                      | Decision                        | User message
| -------------------------------------------------------------- | ------------------------------- | -------------
| `valid_scored == 0` and the pressed direction is saturated     | `boundary_start`/`_end`/`_both` | "Auto-align: reached clip start/end" (or exhausted)
| `valid_scored == 0` otherwise                                  | `no_frames`                     | "Auto-align: insufficient probes — no change"
| `pool_best < 0.60` and pressed direction saturated             | `boundary_start`/`_end`/`_both` | (same as above)
| `pool_best < 0.60` and no new work this press                  | `low_confidence`                | "Auto-align: still low confidence (…, window …)"
| `pool_best < 0.60` otherwise                                   | `low_confidence`                | "Auto-align: low confidence (…) — press again to extend"
| Mode rule picked nothing, direction saturated (`[`/`]`)        | `boundary_start`/`_end`         | "Auto-align: reached clip start/end"
| Mode rule picked nothing, `` ` `` with both ends saturated     | `boundary_both`                 | "Auto-align: search exhausted (both clip boundaries reached…)"
| Mode rule picked nothing, `` ` `` otherwise                    | `already`                       | "Auto-align: already aligned (score …)"
| Mode rule picked nothing, `[` / `]` otherwise                  | `no_stronger_in_direction`      | "Auto-align: no stronger match ahead/behind (…)"
| Mode rule picked a candidate                                   | `seek`                          | "Auto-align: shift +N frame(s) (score …)"

On `seek`, the code computes `shift_right_frames += round((best_pts - follower_current.pts) / follower.delta_pts)` when follower == RIGHT, or dispatches via the absolute-seek path (see §11 for swap). It populates `pending_auto_align_verification_` with everything needed to rescore post-seek:

```
struct PendingAutoAlignVerification {
  bool active;
  float expected_score;
  int expected_shift_frames;
  Side follower_side;
  int64_t master_current_pts;
  int64_t probe_step_pts;
  int64_t delta_t_pts;
  std::unordered_map<int64_t, std::vector<float>> master_probe_fingerprints;
};
```

The master probe fingerprints survive the seek because the master side isn't touched during a follower-scoped shift.

**Iteration semantics** for `[` / `]` fall out of the at-or-above gate: after a seek, follower.current is the previous target, and its score becomes the new `current_score`. The next press filters in-direction candidates with score ≥ new-current-score − eps, excluding current. Near-peer candidates in the direction still qualify; the picker's nearest-in-direction tie-break advances one frame at a time through them. Convergence isn't automatic for `[` / `]` — the user decides when to stop.

---

## 7. Post-seek verification

After the seek branch (`after_seek_block:` → `log_seek()` in `compare()`), if `pending_auto_align_verification_.active`, the verification block runs:

1. For each stored master probe fingerprint (keyed by master PTS), compute the expected follower target at `master_pts + delta_t_pts`.
2. Find the nearest follower frame in the (now-updated) follower ring at that target, within `probe_step_pts / 2` tolerance. Fingerprint it fresh.
3. Correlate with the stored master fingerprint. Sum, count valid probes, divide.
4. Compare the result to `expected_score`. If `landed_score >= expected_score - 0.01` (`kAutoAlignVerificationSlack`), the seek landed cleanly — no action. If worse, emit "Auto-align: landed score X.XXX (expected Y.YYY) — seek imprecision" so the user knows the alignment didn't land quite where it was supposed to.
5. Sync `auto_align_retry_cache_.follower_pts_at_cache` to the actual landed PTS — this keeps the cache valid for the next press (without this update, the invalidation guard would reject the cache the moment the seek lands on a slightly-different PTS than planned).
6. Clear `pending_auto_align_verification_`.

No retry, no rollback — that was an explicit user-chosen policy. A failing verification is a diagnostic, not a correction.

Typical landings via L0 pivot (the happy path, when the best candidate was in-ring) score identically to the pre-seek expectation. L1 or L2 landings can drift by a frame or two due to drain rounding; the verification surfaces those cases.

---

## 8. Diagnostic logging

Set `VIDEO_COMPARE_LOG_AUTO_ALIGN=1` to stream per-press diagnostics to stderr, formatted to match the existing `[seek-timing]` / `[loop]` conventions:

```
[auto-align] pts_delta_ms=<per-candidate>  probes=<n>/5  score=<per-candidate>
  ... one line per scored candidate ...
[auto-align] mode=<sym|back|fwd> follower=<LEFT|RIGHT> ring=<n> decoded=<n>
             decode_ms=<int> walk=<none|done|skipped>
             candidates=<total> scored=<n>
             best_pts_delta_ms=<float>  best_score=<float|n/a>
             current_score=<float|n/a>  shift_frames=<int>
             decision=<seek|already|no_stronger_in_direction|low_confidence
                       |no_frames|boundary_start|boundary_end|boundary_both>
             searched_window=[low_rel, high_rel]s
             low_sat=<0|1>  high_sat=<0|1>
[auto-align] landed_pts_delta_ms=<float>  landed_score=<float>
             expected_score=<float>  ok=<true|false>  probes=<n>
```

The per-candidate lines come first, then the decision summary, then — for a `seek` decision — the post-seek verification line after the seek dispatch completes. `searched_window` is reported relative to `follower_current` at press-time, so you can see at a glance how far the searched region has walked in each direction across consecutive presses. `low_sat` / `high_sat` report clip-boundary saturation on the low/high ends of the searched interval. `[auto-align-walk-skip] reason=...` fires if the PacketRing walk is skipped (e.g., target not covered by the PacketRing, single-decoder mode, single-frame media).

---

## 9. Headless testing

The pytest integration harness ([tests/README.md](../../tests/README.md)) drives the binary through the Unix-socket control surface and parses `[auto-align]` log lines for verification. Tests covering the current auto-align behavior:

- [`test_lg_daylight_alignment.py`](../../tests/integration/test_lg_daylight_alignment.py) — 2-press forward-mode convergence on the lg-daylight pair, frame-exact to 2 ms. Verifies `searched_window` growth ([0, +1]s then [0, +2]s) and cache carry-forward via `ring=0` on press 2.
- [`test_directional_backward.py`](../../tests/integration/test_directional_backward.py) — backward-mode convergence on a testsrc+concat pair with a known -2.002 s shift.
- [`test_auto_align_boundary.py`](../../tests/integration/test_auto_align_boundary.py) — clip-boundary saturation on repeated `[` presses at clip start.
- [`test_auto_align_cross_mode.py`](../../tests/integration/test_auto_align_cross_mode.py) — cache carries across mode changes (`` ` `` → `[`).
- [`test_auto_align_iteration.py`](../../tests/integration/test_auto_align_iteration.py) — directional step-through past a converged peak.
- [`test_retry_cache_reset.py`](../../tests/integration/test_retry_cache_reset.py) — cache invalidation by intervening `+` keypress.
- [`test_2160p_alignment_performance.py`](../../tests/integration/test_2160p_alignment_performance.py) — UHD performance canary with frame-exact endgame.

Primary ground-truth fixture is the lg-daylight pair; see [tests/fixtures/pairs/](../../tests/fixtures/pairs/) for pair manifests and [testdata/alignment/README.md](../../testdata/alignment/README.md) for the frame-level ground truth.

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

**Diagnostic logging** includes a `follower=LEFT|RIGHT` field in the `[auto-align] mode=…` summary line. Example:

```
[auto-align] mode=fwd follower=LEFT ring=25 decoded=0 decode_ms=725 walk=done \
             candidates=25 scored=25 best_pts_delta_ms=-2052.056 \
             best_score=0.9657 current_score=0.9657 shift_frames=0 \
             decision=already searched_window=[0.000,1.000]s low_sat=0 high_sat=0
```

**Verification**: [tests/integration/test_swap.py::test_autoalign_follower_follows_swap](../../tests/integration/test_swap.py) drives the socket API to run an auto-align key press with and without swap and asserts that the `follower=` field follows the swap state.

---

## 12. Searched-interval cache

The retry cache tracks a *searched interval* on the follower PTS axis — a single absolute range `[searched_low_pts, searched_high_pts]` that grows monotonically across consecutive presses. Each press extends the interval in its mode's allowed direction(s) by one base width, clamped to the follower clip's `[start_time, start_time + duration)`. The candidate pool and master probe fingerprints are carried forward; PTS-keyed dedup ensures each new press's decode work is limited to the net-new strip.

The cache stays valid across **mode changes and successful seeks**. The user's mental model is "progress bar that fills in as I search" — whatever key they press, the searched region grows rather than resets. Examples:

- `` ` `` then `[`: symmetric press seeds `[-0.5 s, +0.5 s]` around follower.current; backward press extends the low end by 1.0 s, keeping the symmetric press's forward half. Net searched interval: `[-1.5 s, +0.5 s]`.
- `[` then `[` then `]`: two backward presses walk the low end to `-2.0 s`; the forward press extends the high end to `+1.0 s`. Net: `[-2.0 s, +1.0 s]`.
- Successful `` ` `` seek then `` ` `` again: cache preserved; `follower_pts_at_cache` is updated to the landed PTS by the post-seek verification block; the next press grows the interval around the new follower position.

The cache **is** invalidated when:

- Follower side was swapped (different side = different FrameRing, master axes flipped).
- Master-side or follower-side `current.pts` mismatches the cached values without auto-align having produced the move: any `+`/`-` shift, manual scrub, shift-click, playback advance.

State lives on `VideoCompare::auto_align_retry_cache_` (`AutoAlignRetryCache` in [`app/video_compare.h`](../../app/video_compare.h)):

```
struct AutoAlignRetryCache {
  bool valid;
  int64_t master_pts_at_cache;
  int64_t follower_pts_at_cache;  // post-seek: landed PTS
  Side follower_side;
  int64_t searched_low_pts;
  int64_t searched_high_pts;
  bool low_saturated;              // low end at clip start
  bool high_saturated;             // high end at clip end
  bool packet_buffer_miss;         // last walk failed to find a keyframe
  std::vector<AutoAlignCandidate> candidates;
  std::unordered_map<int64_t, std::vector<float>> master_probe_fingerprints;
};
```

`packet_buffer_miss` is distinct from clip-boundary saturation: the former means the PacketRing's keyframe lookup failed (buffer doesn't currently cover the target — the clip might extend further), the latter means the clip itself ends here (no further expansion possible in that direction). Each surfaces a different user message.

The `searched_window=[low_rel, high_rel]s` field in the `[auto-align]` summary line reports the interval relative to follower.current at press-time, so you can watch it grow across consecutive presses. The `low_sat` / `high_sat` flags show saturation. When the cache is reused, `ring=0` confirms no new ring contribution and `decoded=N` is only the net-new strip from the extended portion.
