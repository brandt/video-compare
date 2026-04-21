# Swap seek

## Problem

Pressing `S` flips a display-only bit (`swap_left_right_` in [display/display.h:147](../../display/display.h#L147)) that swaps which underlying pipeline is rendered on which half of the window. The pipelines themselves — `LEFT` and `RIGHT` in the code — never actually swap roles. `LEFT` stays the master; `RIGHT` stays the follower with its `TimeShifter`-tracked offset, regardless of visual position.

Shift-click's right-only seek scopes by `s.is_right()` at [app/video_compare.cpp:2158](../../app/video_compare.cpp#L2158) and [:2255](../../app/video_compare.cpp#L2255), which targets the *underlying* `RIGHT` pipeline. With `swap_left_right_` active, the underlying `RIGHT` is showing on the visual left half of the window — so the side that appears to seek is the visual-left one, which is the opposite of what a user tracking "shift-click moves the right video" expects.

The same inconsistency already exists for `+` / `-`: after `S`, pressing `+` still shifts the underlying `RIGHT` pipeline, which is now visually on the left. It just hasn't been reported.

## Root cause — the asymmetry

The whole pipeline is built around a master/follower asymmetry:

- `LEFT` owns the timeline. `RIGHT` is expressed as `LEFT.pts + TimeShifter::static_shift + dynamic_shift(pts)`.
- `TimeShifter` tracks **right-relative-to-left** only. There's no mirror representation for left-relative-to-right.
- `shift_right_frames`, `adjust_shift_right_frames`, `pop_and_reset`'s `effective_time_shift`, and every seek-target computation in the L2 path is written in terms of "right side." Auto-align's candidate pool is specifically the right side. `L1` and `L0` pivot paths are scoped by `side.is_right()`.
- Sync (`sync_frame_queue` at [app/video_compare.cpp:3140](../../app/video_compare.cpp#L3140)) is symmetric in wording ("catch the lagging side up"), but in practice left is never behind because every seek moves left first and right catches up to it.

So "seek the visually-right side" under swap means "do all the right-only machinery, but to the `LEFT` pipeline instead." That's not a rewiring; it's a symmetry requirement the rest of the code doesn't currently have.

## Three redesign directions

### Option 1: Surgical — refuse shift-click under swap

Leave the pipeline asymmetry alone. In the shift-click handler, check `swap_left_right_`. If set, skip the right-only seek entirely and surface a message like "Shift-click disabled while swap is active; press S to un-swap first."

**Change size:** ~5 lines in [display/display_input.cpp:172](../../display/display_input.cpp#L172).

**Pros:** Trivially correct, zero risk of touching TimeShifter semantics, documents the architectural limitation honestly.

**Cons:** Visibly regresses the UX from where it is today (the wrong side seeks, but *something* seeks). A surprising dead key is arguably worse than a wrong-direction seek.

### Option 2: Medium — `follower_side` at the input boundary, shift-click path only

Replace the `right_only_seek` boolean with a `Side follower_side` carried through the request. `follower_side` is:

- `RIGHT` when `swap_left_right_ == false` (status quo).
- `LEFT` when `swap_left_right_ == true` (new behavior).

The main loop's `should_seek` predicate becomes `s == follower_side`. The seek-target computation uses `side_states.at(follower_side)` instead of hard-coding `right_ptr`. The TimeShifter update that follows the shift-click seek flips its reference: under swap, `new_static_shift = left_raw_pts_after_seek - right.pts_` rather than `right_raw_after - left.pts_`.

Scope-guard: **this only applies to the shift-click path**. `+` / `-`, auto-align, timeline click without shift, and every other seek-adjacent operation continue to hard-code `RIGHT` and work as today.

**Files affected:**
- [display/display_input.cpp](../../display/display_input.cpp) — mouse handler computes `follower_side` from `swap_left_right_`, passes via a new PlaybackController accessor (replaces the `right_only_seek_` bool, or extends it).
- [display/subsystems/playback_controller.h](../../display/subsystems/playback_controller.h) — replace `bool right_only_seek_` with `Side follower_side_` + a sentinel for "no right-only scope."
- [display/display.{h,cpp}](../../display/display.h) — accessor forwarder.
- [app/video_compare.cpp](../../app/video_compare.cpp) around [:2152](../../app/video_compare.cpp#L2152), [:2255](../../app/video_compare.cpp#L2255), [:2781](../../app/video_compare.cpp#L2781), and [:2970](../../app/video_compare.cpp#L2970) — the seek-scoping predicate, target computation loop, and TimeShifter update pick the configured follower side instead of hard-coding `RIGHT`.

**Change size:** ~60-100 lines, concentrated in four locations.

**Pros:** Directly fixes the reported UX bug. Leaves TimeShifter's semantics stable (still "right-relative-to-left" globally; sign flips just inside the shift-click path). Bounds the blast radius to one workflow.

**Cons:** Leaves `+` / `-` inconsistent under swap — that's a separate next-increment request. TimeShifter's sign-flip reasoning becomes a little subtler in code review because the update is now conditional on which side moved.

### Option 3: Full redesign — make every input follow the visual swap

Introduce a `follower_side_for_input` concept at the Display layer such that every user input that conceptually targets "the right video" translates into a pipeline-side decision that respects `swap_left_right_`:

- `+` / `-` shifts whichever pipeline is visually on the right.
- Shift-click seeks whichever pipeline is visually on the right.
- Auto-align's `` ` `` / `[` / `]` keys align the visual-right to the visual-left.
- HUD labels, metrics, and seek-tier logs follow the same renaming.

This would make the app fully symmetric under swap but requires TimeShifter to become bi-directional (a `master_side` enum and shifts that can represent `LEFT - RIGHT` as well as `RIGHT - LEFT`), `shift_right_frames` → `shift_follower_frames`, `right_ptr` → `follower_ptr`, and essentially every "right" in the code to be revisited.

**Files affected:** the main loop, `TimeShifter`, `SideState` usage sites, auto-align's entire inline block + its helpers, the L1 re-decode path, every seek-timing log, and the HUD label code.

**Change size:** several hundred lines across a dozen files; days of careful work including regression re-testing of auto-align, L1, loop-mode materialize, timeline click, and arrow-key seeks under both swap states.

**Pros:** Finally resolves the visual/pipeline mismatch at every touch point; the swap button truly swaps sides rather than just labeling them.

**Cons:** Large refactor for a UI affordance many users never toggle. Risk of regressing the hot paths that are currently known-stable. `TimeShifter`'s symmetry-preserving invariants would need explicit tests.

## Recommendation

**Option 2**, with the explicit scope-guard that only the shift-click path is swap-aware. It fixes the reported complaint with contained, reviewable change, and keeps Option 3 open as a future increment if the `+` / `-` inconsistency under swap also becomes a pain point. Option 1 is too regressive; Option 3 is out of proportion for the reported problem.

## Open question if we go with Option 2

Under swap, the TimeShifter update following a shift-click needs a reference-point decision:

- **A.** Keep `static_shift`'s meaning stable as "right-relative-to-left" globally. Under swap, the shift-click moves LEFT, so the update becomes `new_static_shift = right.pts_ - left_raw_after_seek` (sign flipped, math applied to the opposite side). Preserves the invariant "`static_shift == 0` ↔ aligned."
- **B.** Redefine `static_shift` as "follower-relative-to-master". Becomes meaningful only together with `follower_side`; loses the "0 means aligned" invariant because the interpretation depends on which side is the master this iteration.

Option A keeps TimeShifter easier to reason about and is what I'd pick — but we should decide this before implementation because it shapes the TimeShifter API and any future Option 3 follow-on.

## Out-of-scope for this doc

- HUD labels and the metrics overlay under swap — already handled by the Display layer's `displayed_left_side_` / `displayed_right_side_` resolution and don't require pipeline-side changes.

---

# Implementation plan

Three-phase rollout following Path A (TimeShifter stays "right-relative-to-left"; sign flips happen at input translation). Each phase is independently reviewable, testable, and shippable. Total ~200-250 lines across the three phases. The order is deliberately smallest → largest so the shared abstraction is validated early.

All three phases are fully scriptable end-to-end: Phase 1 via the legacy input-script harness or the socket API, Phase 2 via the socket API's mouse-button injection + modifier support, Phase 3 via the legacy harness plus socket queries for state assertion. See [input-testing.md](../design/input-testing.md) and [input-socket.md](../design/input-socket.md).

## Shared plumbing (added in Phase 1, reused by 2 and 3)

One accessor on Display:

```cpp
// display/display.h (public section, near get_swap_left_right)
Side Display::follower_side_for_input() const;

// display/display.cpp
Side Display::follower_side_for_input() const {
  return swap_left_right_ ? LEFT : RIGHT;
}
```

Meaning: "when the user presses a `+`/`-`/shift-click/auto-align key and mentally refers to 'the right video', which underlying pipeline side do they actually mean?" It's computed entirely from `swap_left_right_` — no per-frame state, no transient-request semantics. The accessor is the only shared code across the three phases.

## Phase 1 — `+`/`-` swap-aware (~15 lines)

Scope: pressing `+` / `-` (or Ctrl/Alt variants) under swap shifts the offset in the direction that makes the visually-right video appear to advance.

**Why the sign just flips.** `shift_right_frames` targets the underlying `RIGHT` pipeline's offset relative to `LEFT`. Under swap, underlying `RIGHT` is visually on the left. For visual-right (= underlying `LEFT`) to appear to advance, the stored offset must *decrease*, which makes underlying `RIGHT` show earlier content, which from the visual perspective looks like visual-LEFT going backward relative to visual-RIGHT — equivalent to visual-RIGHT advancing. TimeShifter's semantics stay unchanged; the interpretation at the user-input boundary flips.

**Changes:**

- [display/display.h](../../display/display.h), [display/display.cpp](../../display/display.cpp) — add `Side follower_side_for_input() const` (accessor used here and by Phases 2 and 3).
- [display/display_input.cpp:479-487](../../display/display_input.cpp#L479-L487) — `SDLK_PLUS` / `SDLK_KP_PLUS` / `SDLK_EQUALS` / `SDLK_MINUS` / `SDLK_KP_MINUS`:

  ```cpp
  const int sign = (swap_left_right_ ? -1 : +1);
  const int magnitude = is_alt_down ? 100 : (is_ctrl_down ? 10 : 1);
  playback_.adjust_shift_right_frames(sign * magnitude);  // + case
  playback_.adjust_shift_right_frames(-sign * magnitude); // - case
  ```

  Or factor both cases to a local lambda.

**Verification:**

Two paths, both scriptable:

*Legacy-script flavor* — uses [input-testing.md](../design/input-testing.md)'s one-shot harness and greps logs:

```
# tmp/swap_plus.txt
sleep 3.0
keypress space
sleep 0.5
keypress = 3 0.1        # baseline: 3× +1 under no swap
sleep 0.5
keypress s              # swap
sleep 0.5
keypress = 3 0.1        # under swap: 3× should translate to -1 each
sleep 0.5
keypress s              # un-swap
sleep 0.5
quit
```

Run with `VIDEO_COMPARE_LOG_SEEK_TIMING=1`. Expected: the first three `[seek-timing]` lines show `shift_right_frames=+1`; the next three show `shift_right_frames=-1`.

*Socket flavor* — uses [input-socket.md](../design/input-socket.md) for precise state assertions:

```python
call({"cmd": "key", "action": "press", "key": "space"})
call({"cmd": "sleep", "seconds": 0.3})
baseline_shift = call({"cmd": "get", "field": "effective_time_shift"})["value"]

# Three +'s, no swap. Expect effective_time_shift to increase by 3 × right_delta.
for _ in range(3):
    call({"cmd": "key", "action": "press", "key": "="})
call({"cmd": "sleep", "seconds": 0.3})
shift_after_plus = call({"cmd": "get", "field": "effective_time_shift"})["value"]
assert shift_after_plus > baseline_shift

# Swap, three +'s again. Under swap, sign flips at input; expect shift to DECREASE.
call({"cmd": "key", "action": "press", "key": "s"})
call({"cmd": "sleep", "seconds": 0.3})
for _ in range(3):
    call({"cmd": "key", "action": "press", "key": "="})
call({"cmd": "sleep", "seconds": 0.3})
shift_after_swap_plus = call({"cmd": "get", "field": "effective_time_shift"})["value"]
assert shift_after_swap_plus < shift_after_plus
# Net: six conceptually-forward presses cancel out if the sign flip is correct.
assert abs(shift_after_swap_plus - baseline_shift) < 0.01
```

No pipeline regression: the L0 pivot path is unaffected; `shift_right_frames` feeds into the same dispatch it always did.

## Phase 2 — shift-click swap-aware (~80 lines)

Scope: shift-click's right-only seek scopes to the visual-right side, regardless of swap. The TimeShifter update at the end of the seek flips sign when follower=LEFT.

**Changes:**

- [display/subsystems/playback_controller.h](../../display/subsystems/playback_controller.h) — replace `bool right_only_seek_` with `std::optional<Side> right_only_seek_follower_`:

  ```cpp
  std::optional<Side> right_only_seek_follower() const { return right_only_seek_follower_; }
  void set_right_only_seek(std::optional<Side> follower) { right_only_seek_follower_ = follower; }
  ```

  `clear_transient_state` resets to `std::nullopt`.

- [display/display.{h,cpp}](../../display/display.h) — update `get_right_only_seek()` signature to return `std::optional<Side>`. Call sites in main loop update accordingly.

- [display/display_input.cpp:172-181](../../display/display_input.cpp#L172-L181) — shift-click handler:

  ```cpp
  } else if (event.button.button != SDL_BUTTON_RIGHT) {
    const SDL_Keymod mod = SDL_GetModState();
    const bool shift_down = (mod & SDL_KMOD_SHIFT) != 0;
    playback_.set_seek_relative(static_cast<float>(mouse_x_) / static_cast<float>(window_width_));
    playback_.set_seek_from_start(true);
    playback_.set_right_only_seek(shift_down ? std::optional<Side>(follower_side_for_input()) : std::nullopt);
  }
  ```

- [app/video_compare.cpp:2158](../../app/video_compare.cpp#L2158) — resolve follower side up front in the seek branch:

  ```cpp
  const std::optional<Side> follower_side_opt = display_->get_right_only_seek();
  const bool right_only_seek =
      pure_right_frame_shift
      || (follower_side_opt.has_value() && !force_seek_current_position);
  // Which side is the follower for this seek's scoping? pure_right_frame_shift
  // always targets RIGHT; shift-click carries its follower explicitly.
  const Side follower_side =
      follower_side_opt.value_or(RIGHT);  // default safe for the pure-right-shift case
  const Side master_side = (follower_side == LEFT) ? RIGHT : LEFT;
  ```

- [app/video_compare.cpp:2255](../../app/video_compare.cpp#L2255) — `should_seek` predicate switches from "is right?" to "is follower?":

  ```cpp
  const auto should_seek = [right_only_seek, follower_side](const Side& s) -> bool {
    return !right_only_seek || s == follower_side;
  };
  ```

- [app/video_compare.cpp:2781-2829](../../app/video_compare.cpp#L2781-L2829) — the right-side seek loop generalizes. Currently it iterates `side_states` and guards with `if (side.is_right())`. Under swap with `follower_side == LEFT`, we want to seek LEFT via the same code. The scope-guard becomes `if (should_seek(side) && side != master_side)` — meaning "any side we're supposed to seek that isn't the master." For non-swap cases this is identical to the existing `side.is_right()` behavior because master is always LEFT and follower sides are all RIGHT.

  The `next_right_position` computation continues to use `right_state.start_time_`; the variable just happens to be LEFT's SideState under swap. Rename the locals from `next_right_position` / `right_state` to `next_follower_position` / `follower_state` for readability.

  One subtlety: the `compute_right_position` lambda at line 2771 hard-codes `left.pts_`. Generalize to `compute_follower_position(const SideState& follower_state) { return master_side_state.pts_ * AV_TIME_TO_SEC + follower_state.start_time_; }`.

- [app/video_compare.cpp:2808-2810](../../app/video_compare.cpp#L2808-L2810) — the `skip_shift_for_shift_click` guard I added earlier stays; the static_shift addition is unchanged (still reads `time_shifter_.static_shift()`, still only applies when not in shift-click).

- [app/video_compare.cpp:2970-2977](../../app/video_compare.cpp#L2970-L2977) — the TimeShifter update flips based on follower side:

  ```cpp
  if (right_only_seek && seek_from_start && right_delta > 0) {
    const float nrp = right_seek_positions[follower_side];
    const SideState& master_state = (follower_side == RIGHT) ? left : side_states.at(master_side);
    const SideState& follower_state = side_states.at(follower_side);
    const int64_t expected_follower_raw_us =
        static_cast<int64_t>(static_cast<double>(nrp - follower_state.start_time_) * AV_TIME_BASE);
    // TimeShifter means right-relative-to-left. If follower is RIGHT, the
    // new shift is expected_follower_raw - master.pts_ (current formula).
    // If follower is LEFT (swap), we flipped sides: the master now moves...
    // wait no — under swap, the user moves the *visual*-right which is
    // underlying LEFT. So LEFT jumps, RIGHT stays. TimeShifter still means
    // right-relative-to-left, so new_static_shift = right.pts_ - expected_left_raw.
    const int64_t new_static_shift_us =
        (follower_side == RIGHT)
            ? (expected_follower_raw_us - master_state.pts_)
            : (master_state.pts_ - expected_follower_raw_us);
    total_right_time_shifted = static_cast<int>((new_static_shift_us - time_shifter_.offset_av_time()) / right_delta);
    time_shifter_.set_frame_shift_accumulator(total_right_time_shifted, right_delta);
  }
  ```

  The `right_delta` variable still refers to underlying `RIGHT`'s delta_pts — that's correct because `TimeShifter`'s `frame_shift_accumulator` is always measured in RIGHT-frame units regardless of which side actually moved.

**Verification:**

Fully scriptable via the socket control surface ([input-socket.md](../design/input-socket.md)), which supports mouse button events with modifiers plus `get`/`status` queries for `left_pts`, `right_pts`, `effective_time_shift`, and `swap`.

Test driver outline (Python pseudocode):

```python
# Start video-compare with -t 2.0 for a known initial offset.
# VIDEO_COMPARE_INPUT_SOCK=tmp/vc.sock ./video-compare -t 2.0 ...

def call(msg): ...  # JSON-Lines round trip

call({"cmd": "sleep", "seconds": 2.5})
call({"cmd": "key", "action": "press", "key": "space"})  # pause
call({"cmd": "sleep", "seconds": 0.3})

baseline_left  = call({"cmd": "get", "field": "left_pts"})["value"]
baseline_right = call({"cmd": "get", "field": "right_pts"})["value"]
baseline_shift = call({"cmd": "get", "field": "effective_time_shift"})["value"]
w = call({"cmd": "get", "field": "window_size"})["value"]["w"]

# Case A — no swap, shift-click at 50%. Expect right seeks, left unchanged.
call({"cmd": "mouse", "action": "button", "button": "left",
      "down": True, "x": w * 0.5, "y": 30, "mods": ["shift"]})
call({"cmd": "mouse", "action": "button", "button": "left",
      "down": False, "x": w * 0.5, "y": 30, "mods": ["shift"]})
call({"cmd": "sleep", "seconds": 1.5})  # seek + verification settle

# Poll until SEEK badge clears, then sample.
while call({"cmd": "get", "field": "play_state"})["value"] == "SEEK":
    call({"cmd": "sleep", "seconds": 0.1})

after_left  = call({"cmd": "get", "field": "left_pts"})["value"]
after_right = call({"cmd": "get", "field": "right_pts"})["value"]
assert abs(after_left - baseline_left) < 0.05, "left must not move on shift-click"
assert abs(after_right - baseline_right) > 0.5, "right must seek meaningfully"

# Case B — swap, then shift-click at 50%. Expect LEFT seeks, RIGHT unchanged.
call({"cmd": "key", "action": "press", "key": "s"})
call({"cmd": "sleep", "seconds": 0.3})
assert call({"cmd": "get", "field": "swap"})["value"] is True

pre_swap_left  = call({"cmd": "get", "field": "left_pts"})["value"]
pre_swap_right = call({"cmd": "get", "field": "right_pts"})["value"]

call({"cmd": "mouse", "action": "button", "button": "left",
      "down": True, "x": w * 0.25, "y": 30, "mods": ["shift"]})
call({"cmd": "mouse", "action": "button", "button": "left",
      "down": False, "x": w * 0.25, "y": 30, "mods": ["shift"]})
call({"cmd": "sleep", "seconds": 1.5})
while call({"cmd": "get", "field": "play_state"})["value"] == "SEEK":
    call({"cmd": "sleep", "seconds": 0.1})

post_swap_left  = call({"cmd": "get", "field": "left_pts"})["value"]
post_swap_right = call({"cmd": "get", "field": "right_pts"})["value"]
assert abs(post_swap_right - pre_swap_right) < 0.05, "right must not move under swap shift-click"
assert abs(post_swap_left - pre_swap_left) > 0.5, "left must seek under swap"

# Sign check on effective_time_shift: the TimeShifter update should reflect
# the new content offset. Magnitude should be consistent with the click
# landing point.
shift_after = call({"cmd": "get", "field": "effective_time_shift"})["value"]
# Exact value depends on click position and video duration, so just sanity-check sign.
```

Manual spot-check afterwards: visually confirm the HUD labels swap correctly and that `+`/`-` behavior (Phase 1) is consistent — press `+` a few times, confirm the visually-right video advances regardless of swap state.

## Phase 3 — auto-align swap-aware (~100-150 lines)

Scope: ` , [ , ] under swap align the visual-right side to the visual-left side, by scoring candidates on the follower side's pipeline against probes from the master side.

**The biggest chunk of this is renaming inside the inline auto-align block** ([app/video_compare.cpp:~1580](../../app/video_compare.cpp#L1580) through [:~2120](../../app/video_compare.cpp#L2120)). The algorithm is unchanged; only the side references are parameterized.

**Changes:**

- At the top of the auto-align block:

  ```cpp
  const Side follower_side = display_->follower_side_for_input();
  const Side master_side = (follower_side == LEFT) ? RIGHT : LEFT;
  SideState& follower = side_states.at(follower_side);
  SideState& master = side_states.at(master_side);
  // Delta-PTS names follow the side identities, not left/right positions.
  const int64_t master_delta_pts = master.delta_pts_;
  const int64_t follower_delta_pts = follower.delta_pts_;
  ```

- Replace uses of `left` → `master`, `right_ptr->ring` → `follower.ring`, `right_ptr->side_` → `follower_side`, etc.

- The PacketRing walk accesses `packet_rings_.at(follower_side)` and `demuxers_.at(follower_side)`. The post-walk demuxer reseek targets `follower_raw_pts + start_time_sec + 0.001`. All already parameterized once the follower rename is done.

- The scoping predicate `right_only_pred = [follower_side](Side s) { return s == follower_side; }` for `enter_seek_barrier`.

- **Seek dispatch (the one place with branching, not just renaming):**

  Current code folds the frame delta into `shift_right_frames`, relying on the L0/L1/L2 dispatch to pick up from there. Under swap (`follower_side == LEFT`), `shift_right_frames` doesn't apply — the L0 pivot path is hard-coded to right-only, and the `shift_right_frames` number is in RIGHT-frame units, not LEFT-frame units.

  So the commit site splits:

  ```cpp
  if (follower_side == RIGHT) {
    // Preserve the L0 pivot fast path for the common (no-swap) case.
    shift_applied = static_cast<int>(std::llround(
        static_cast<double>(shift_pts) / static_cast<double>(follower_delta_pts)));
    shift_right_frames += shift_applied;
  } else {
    // Swap case: dispatch via the absolute-seek path. Uses Phase-2's
    // right_only_seek mechanism to scope the seek to LEFT.
    const double target_sec =
        static_cast<double>(best_pts + follower.start_time_us()) / AV_TIME_BASE;
    const float normalized_position_sec = static_cast<float>(target_sec);
    // The seek_relative + seek_from_start path expects a normalized [0,1]
    // position; translate target_sec back through shortest_duration_.
    const float fractional = static_cast<float>(target_sec - follower.start_time_)
                            / shortest_duration_;
    seek_relative = fractional;
    seek_from_start = true;
    // Set the right_only_seek follower from Phase 2's plumbing.
    // This is a direct shortcut from the auto-align block into the seek
    // dispatch's scoping mechanism; document that the follower is LEFT here.
    display_->playback_controller().set_right_only_seek(LEFT);
    shift_applied = 0;  // not used in this branch
  }
  ```

  The second branch means auto-align under swap falls through to L2 (full seek) rather than the L0 pivot. The cost is a single L2 seek per auto-align press under swap — the user is pressing a deliberate alignment key, not dragging, so this is acceptable. (Future optimization: extend the L0 pivot to work on LEFT too, under the swap-aware refactor. Not for this phase.)

- **Post-seek verification** (`pending_auto_align_verification_`): the probe-fingerprint lookups and the `right_ring_after` reference need to be follower-relative. Rename and parameterize: `follower_ring_after = side_states.at(pending_auto_align_verification_.follower_side).ring`. Store `follower_side` in the pending-verification struct.

**Verification:**

Reuse the `tmp/autoalign_ts.txt` fixture from step 10 of the earlier auto-align work, layered with an initial `keypress s` to enter swap mode before the backtick press:

```
# tmp/autoalign_swap.txt
sleep 3.0
keypress space
sleep 0.5
keypress s
sleep 0.5
keypress ]
sleep 3.0
quit
```

Run with the lg1 pair (file order unchanged; swap is user-induced via `s`):

```bash
VIDEO_COMPARE_INPUT_SCRIPT=tmp/autoalign_swap.txt \
VIDEO_COMPARE_LOG_AUTO_ALIGN=1 \
VIDEO_COMPARE_LOG_SEEK_TIMING=1 \
./video-compare -t 2.0 \
  testdata/lg1/lg1-extrashifted-border10px-sdr-yuv420p-2160p-59.9fps-x264.mp4 \
  testdata/lg1/lg1-full-hdr-yuv420p10le-720p-30fps-hevc.mp4 \
  2> tmp/autoalign_swap.log
```

Expected log evidence:
- `[auto-align] mode=fwd ... follower=LEFT ...` (new field in the summary line).
- `decision=seek`, `best_score ≥ 0.90`.
- `[auto-align] landed_... ok=true`.
- `[seek-timing] tier=L2 ...` (not L0forward — because under swap, the L0 pivot isn't available to the follower=LEFT case yet).

Repeat without swap (no `keypress s`) to confirm the existing result still reproduces exactly (should still land tier=L0forward with shift_right_frames=5).

For richer state assertions (landed PTS precision, TimeShifter sign correctness), pair the log grep with socket-based queries ([input-socket.md](../design/input-socket.md)) on `left_pts`, `right_pts`, and `effective_time_shift` before and after the backtick press. The socket path is preferred when asserting "follower is LEFT" — `swap == true` + `right_pts` didn't move + `left_pts` did move = the scoping reached the correct side.

## Integration points across phases

- `follower_side_for_input()` ships in Phase 1 but is used by all three. Don't create it until Phase 1.
- `std::optional<Side> right_only_seek_follower_` from Phase 2 gets reused by Phase 3's swap-dispatch branch. Phase 3 depends on Phase 2 being landed.
- Phase 1 is independent of both — land first, verify in isolation, then build on top.

## Rollback plan

Each phase lands as an independent commit. If a phase regresses something not caught in its test matrix, it reverts cleanly:

- Phase 1 revert: trivially restore the three-line `SDLK_PLUS` / `SDLK_MINUS` switch body. No state or API changes outside the case body.
- Phase 2 revert: re-widen `right_only_seek_follower_` back to `bool right_only_seek_`. Drop the `follower_side` local. Predicate returns to checking `s.is_right()`.
- Phase 3 revert: restore the inline auto-align block's `right_ptr` / `left` references. Delete the swap-dispatch branch.

None of the three touches `TimeShifter`'s API, `SideState` layout, or the FrameRing/PacketRing interfaces — so revert is always local.

## Open decision to confirm before implementation

Confirm **Option A** from the analysis above (TimeShifter stays "right-relative-to-left"; sign flips at input translation). Phase 2's TimeShifter update assumes this — if we go Option B ("follower-relative-to-master"), Phases 2 and 3 both change their update math, and the name `total_right_time_shifted` becomes misleading.

Recommended: Option A. The invariant "`static_shift == 0` ↔ content-aligned" is worth preserving, and the sign-flip lives in one place (Phase 2's update, and the mirror in Phase 3).

## Not in scope

- Symmetric L0 pivot for LEFT. The L0 pivot is a substantial asymmetric optimization; making it symmetric is a separate project.
- Auto-align's probe cadence or confidence thresholds under swap. The algorithm is identical; only the side assignment flips.
- Any rename of `shift_right_frames` to `shift_follower_frames` or similar. Under Path A, the integer continues to mean "frames of RIGHT-side shift relative to LEFT," and Phase 1 flips the sign at input when the user's intent is visually-right (= underlying LEFT). Renaming would cascade into TimeShifter and dozens of log lines for no user-visible benefit.
