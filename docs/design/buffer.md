# Buffer, Seeking, and Frame Stepping

This doc describes how frames flow through the pipeline, how they are buffered around the current playback position, and how user-driven seeks and step operations (timeline click, arrow keys, Shift+A/D, `+`/`-`, `,`/`.`) interact with that buffer.

Key files:

- `app/video_compare.cpp` — main loop, seek branch, ring-pivot fast path, timer pacing.
- `app/video_compare.h` — queue declarations, `SideState`, `seeking_per_side_`.
- `media/buffering/frame_ring.h` — the buffered cursor data structure.
- `core/data/queue.h` — the mutex + CV queue used between pipeline threads.
- `display/subsystems/playback_controller.{h,cpp}` — `play_`, loop mode, frame-navigation deltas (set by input handlers, consumed by the main loop).

All line numbers are anchors into the current tree, not stable commit references.

---

## 1. Pipeline stages and queues

Each input video has its own worker threads and its own chain of queues. A frame's journey, per side:

```
demuxer  ──►  packet_queues_      ──►  decoder    ──►  decoded_frame_queues_
                                                                   │
                                                                   ▼
                                                            filterer (VideoFilterer)
                                                                   │
                                                                   ▼
                                                           filtered_frame_queues_
                                                                   │
                                                                   ▼
                                                         format converter (sws_scale)
                                                                   │
                                                                   ▼
                                                          converted_frame_queues_
                                                                   │
                                                                   ▼
                                                               main thread
                                                                   │
                                                                   ▼
                                                          SideState.ring (FrameRing)
                                                                   │
                                                                   ▼
                                                          Display::possibly_refresh
```

All four queues are instances of `Queue<T>` from `core/data/queue.h`. The queue class:

- Is a std::queue protected by `mutex_` with a `full_` / `empty_` condition-variable pair.
- `push()` and `pop()` block when full / empty respectively — used by worker threads so they pace against downstream availability.
- `try_pop()` is non-blocking — used by the main-loop intake drain so it never stalls on empty converter output.
- `stop()` flips a `stopped_` atomic so subsequent `pop()` calls return `false` once the queue drains (used on EOF and at the start of seeks).
- `restart()` clears `stopped_` so workers can resume pushing.
- `empty()` drops all contents under lock (used repeatedly during seek drain).

Queue instances and capacities (`app/video_compare.h:34-36`, `app/video_compare.cpp:287-290`):

- `packet_queues_`, `decoded_frame_queues_`, `filtered_frame_queues_`, `converted_frame_queues_`, keyed by `Side` (LEFT / Right(i)).
- All four created with `QUEUE_SIZE = 3` (`app/video_compare.cpp:25`). Phase 3 shrank this from 5 to 3: the PacketRing (§10) is now the main spill buffer, so these queues only need enough headroom to smooth out burstiness across stages.

These inter-stage queues are deliberately shallow. The user-visible buffering is _after_ the converter, in each side's `FrameRing`, and backing it is the per-side `PacketRing` of encoded packets.

---

## 2. FrameRing — the buffered cursor

`FrameRing` (`media/buffering/frame_ring.h`) is the per-side buffer of post-converter frames surrounding the current play position. Layout:

```
  history[N-1] ... history[1] history[0]  |CURRENT|  prefetch[0] ... prefetch[M-1]
                                       ^                ^
                           most recent past         next to display
```

- **`current_`** — the frame currently displayed (one slot or nullptr).
- **`history_`** — deque of up to `history_capacity_` past frames. `history_[0]` is the most recent past frame.
- **`prefetch_`** — deque of up to `prefetch_capacity_` future frames. `prefetch_[0]` is the next frame that will be displayed.

Capacities are independent but in practice both are set to `frame_buffer_size_` (see §7 below), so history and prefetch are symmetric.

Cursor operations (main thread only — no internal locking):

- `advance()` — normal playback step. `current` → `history.front()`, `prefetch.front()` → `current`. Returns `false` if `prefetch` is empty.
- `pivot_backward(n)` — step the cursor `n` slots toward the past. Current and the `n-1` most-recent history slots slide **into prefetch front** (so a subsequent `pivot_forward(n)` replays them without re-decoding).
- `pivot_forward(n)` — symmetric; consumes `n` prefetched frames without going through the pipeline.
- `push_prefetch(frame)` — append a newly-decoded frame to the prefetch tail. Returns `false` if prefetch is at capacity.
- `set_current(frame)` — replace current without touching history/prefetch. Used to seed the cursor after a seek.
- `clear()` — drop all frames. Used at the start of a full seek.

Queries:

- `at(offset)` — offset 0 is current, negative reaches back into history, positive into prefetch. Returns nullptr out of range.
- `history_size()`, `prefetch_size()`, `history_plus_current_size()`, `browsable_span()` — used by the main loop, the HUD buffer counter, and the full-buffer auto-loop trigger.

Eviction: when a cursor step would overflow one side's capacity, the far-end frame on that side is evicted. That's a deliberate memory bound — a user bouncing beyond capacity is navigating outside the buffered window.

The ring is populated by a single thread (the main thread, draining the converter queue — see §3) and is never accessed from worker threads, so no locking is needed inside `FrameRing`.

---

## 3. Intake — topping up prefetch each tick

`intake_prefetch()` (`app/video_compare.cpp:1170-1183`) runs at the top of every main-loop iteration, and once more after the seek block completes (`:1696`). For every side:

```cpp
while (!ring.prefetch_full()) {
  if (!converted_frame_queues_[side]->try_pop(frame)) break;  // non-blocking
  if (!ring.push_prefetch(frame)) break;                      // full
}
```

The non-blocking `try_pop` is what keeps the main thread responsive — intake only drains what the converter thread has already produced; it never waits on decode. Because this runs before the seek block examines ring sizes, a forward `+N` pivot (§5) sees up to `prefetch_capacity_` frames available, not just the raw `QUEUE_SIZE=3` of the converter queue.

The post-seek call catches frames the converter produced during the seek drain itself — otherwise they would sit stranded in the converter queue until the _next_ main-loop iteration.

---

## 4. Playback cadence

After intake, the main loop around `app/video_compare.cpp:1662-1776` decides whether to advance each ring's cursor this iteration.

### 4.1 Timing gate

`timer_` holds a wall-clock target for the next frame. The gate (`:1667`):

```cpp
skip_update = skip_update
  || ((timer_->us_until_target() - refresh_time_deque.average()) > 0 && !paused_forward_step);
```

Meaning: if we're ahead of schedule by more than one refresh period, hold the current frame (skip advance this tick). If we're running late (`us_until_target() < 0`), `skip_update` stays false and advance fires on each iteration — this is how the pipeline "catches up" when decode transiently lags real time.

`fetch_next_frame` (`:1668`) gates advance by play state: `get_play() || forward_navigate_frames > 0`. Paused with no frame-step pending → no advance.

### 4.2 Per-side sync

`sync_frame_queue` (`:1706-1715`) runs before the regular playback block and catches the lagging side up by a single frame when its PTS is behind the other's:

```cpp
if (is_behind(side.pts_, other.pts_, min_delta)) {
  adjusting = true;
  advance_ring(side);
}
```

- `is_behind` (`:53-62`) has a small tolerance band so PTS jitter doesn't oscillate the sync.
- `min_delta = 0.8 × min(left.delta_pts_, right.delta_pts_)` (`:64-66`) — 80 % of the smaller average frame duration.
- The `adjusting` local gates the regular playback block (`:1734`): while one side is still catching up, the other does not advance; both sides re-examine timing on the next iteration.

### 4.3 Regular playback block

When `!skip_update && !adjusting && fetch_next_frame`, every side's ring advances. If `advance_ring` fails on any side (prefetch empty and converter queue stopped — EOF), no side advances and the timer holds. Otherwise `store_frames = true` and per-side PTS / effective-time-shift bookkeeping runs (`update_frame_timing`, `:1773-1817`).

If prefetch is empty but the converter queue is live, `advance_ring` falls back to a **blocking** `converted_frame_queues_[side]->pop()` (`:1704`). This is the only main-thread block in regular playback — it keeps us pipeline-paced when we can't get ahead of decode.

---

## 5. Seeking

Any iteration where `seek_relative != 0 || shift_right_frames != 0 || force_seek_current_position` enters the seek branch (`:1233-1669`). Three dispositions, in priority order:

1. Ring-pivot fast path (pure `+`/`-` with enough buffered frames) — no demuxer work.
2. Full seek with post-seek drain to exact target — for paused user scrubs.
3. Full seek with no drain — for playing seeks and for fall-through `+`/`-`.

### 5.1 Pure-right-frame-shift predicate (`:1252`)

```cpp
const bool pure_right_frame_shift =
    seek_relative == 0.0F && shift_right_frames != 0 && !force_seek_current_position;
```

True iff this iteration is _only_ a `+`/`-` keystroke — no scrub, no crop request, no HDR flip. Used to decide whether the fast path is even eligible.

### 5.2 Ring-pivot fast path (`:1254-1350`)

When `pure_right_frame_shift` is true, the main loop first checks whether the shift can be served by re-pointing the right side's cursor into its own history or prefetch, without touching any pipeline state:

- `backward_pivot_possible = shift_right_frames < 0 && every right ring.history_size() >= -shift_right_frames && delta_pts > 0` (`:1267-1281`).
- `forward_pivot_possible = shift_right_frames > 0 && every right ring.prefetch_size() >= shift_right_frames && delta_pts > 0` (`:1284-1304`). Because intake (§3) ran at the top of this iteration, "prefetch size" here is up to `prefetch_capacity_`, not just 5.

If either is true, `ring.pivot_backward(n)` / `pivot_forward(n)` does the work on each right side (`:1306-1350`) — **no demuxer seek, no filter reinit, no re-decode, no queue drain.** `skip_update = true` so the rest of this iteration doesn't also advance. The left side is untouched.

If pivot isn't possible (not enough history/prefetch), control falls through to the full-seek path.

### 5.3 Full seek: barrier, seek, restart

Full seek is heavyweight. Steps:

1. **ReadyToSeek barrier** via the shared `VideoCompare::enter_seek_barrier(should_walk)` helper (`app/video_compare.cpp`, around the helper definition after `quit_all_queues`). The helper: sets `seeking_per_side_` for each side where `should_walk(side)` is true; stops that side's packet queue; repeatedly `empty()`s every downstream queue while spin-waiting on `ready_to_seek_.all_are_idle_where(should_walk)`; clears the seeking flags once the barrier has idled so the decode worker's 10 ms flush loop stops racing the codec. `pure_right_frame_shift` scopes the barrier to the right sides only, so the left pipeline keeps running. The same helper is also called by loop-mode materialize (§8.1), L1 re-decode (§11), and the auto-align PacketRing walk (`docs/design/auto-align.md`).
2. **Consume pending filter changes** on seeking sides.
3. **Per-side demuxer seek**:
   - Target is in seconds on each side's own time axis; the right target includes `TimeShifter::static_shift() + dynamic_shift()` so the subsequent comparison lands aligned with left.
   - `backward = seek_from_start || seek_relative < 0 || shift_right_frames != 0 || (force_seek_current_position && all_multi_frame)`. `AVSEEK_FLAG_BACKWARD` is required on absolute scrubs because sparse-keyframe inputs (e.g. a single keyframe at PTS 0) reject at-or-after seeks.
   - On a forward seek that overshoots EOF, the code restores the pre-seek position with a backward seek for every seeking side and surfaces "Unable to seek past end of file".
4. **Restart packet/decoded/filtered/converted queues** on seeking sides so workers can resume pushing.

After the demuxer lands, the first frame that will arrive in the converter queue is the keyframe at or before the target, not the target itself.

### 5.4 `pop_and_reset` and `drain_to_target` (`:1594-1629`)

For each seeking side the code clears its `FrameRing`, then:

```cpp
pop(first_frame);                    // blocks — first post-seek frame (the keyframe)

if (drain_to_target) {
  while (first_frame->pts < target_pts) {
    if (!pop(next_frame)) break;      // EOF: keep last-known-good
    first_frame = std::move(next_frame);
  }
}

side_state.pts_ = first_frame->pts;
ring.set_current(std::move(first_frame));
```

The drain-to-target loop is there because `av_seek_frame` with `AVSEEK_FLAG_BACKWARD` lands on the keyframe at-or-before the target, not the target itself. Each `pop` inside the loop **blocks** on the converter thread, so a drain of N frames costs N frames of pipeline latency on the main thread.

The drain condition (`:1633`):

```cpp
const bool drain_to_target = !pure_right_frame_shift && !display_->get_play();
```

Three cases:

| Seek source                               | `pure_right_frame_shift` | `display_->get_play()` | Drain?  | Why
| ----------------------------------------- | ------------------------ | ---------------------- | ------- | ---
| Paused timeline click / arrow / Shift+A/D | false                    | false                  | **yes** | User sees the next paused frame; it must be the clicked frame, not a keyframe hundreds of ms earlier.
| Playing timeline click / arrow            | false                    | true                   | no      | The main-loop "behind schedule, advance fast" mechanism (§4.1) picks up the keyframe and rapidly advances through intermediate frames at decode-bound rate. Draining would block the UI for a whole GOP with stale overlay values visible.
| `+` / `-` fall-through to full seek       | true                     | either                 | no      | `+`/`-` keys are held down; blocking each press on a GOP-duration drain is unacceptable. Landing imprecision here matches the pre-drain behavior users already tolerate.

`skip_update = true` on the way out of the seek branch (`:1653`) so this iteration does not also try to advance the ring beyond the freshly-set current frame.

---

## 6. Frame-stepping variants

### 6.1 `+` / `-` — right-video time shift

Input (`display/display_input.cpp:479-490`): `+`/`-`, or `Ctrl +/-` (×10), or `Alt +/-` (×100). Calls `playback_.adjust_shift_right_frames(delta)`.

Consumption (`app/video_compare.cpp:1192`): pulls `shift_right_frames` out of the playback controller. Enters the seek branch. Ring-pivot fast path (§5.2) handles the common case; full-seek-without-drain (§5.4) handles the fall-through.

Affects the right side(s) only. `TimeShifter` accumulates the shift so subsequent seeks / playback stay consistent with the new right-vs-left offset.

### 6.2 Shift+A / Shift+D — one-frame paused step

Input (`display/display_input.cpp`): set `frame_navigation_delta = ±1`. Consumed at `app/video_compare.cpp:1158` where it's turned into `seek_relative += delta × frame_duration_seconds`. Both sides seek. Falls through to the full-seek-with-drain path (paused) so the displayed frame lands exactly one frame away.

### 6.3 Arrow keys — relative seek in seconds

- Left / Right: ±1 s
- Down / Up: ±10 s (seek direction per `handle_key_down`)
- Page Down / Page Up: ±600 s

Sets `seek_relative` directly (seconds). Drives a full seek on both sides. Drains if paused.

### 6.4 `,` and `.` — in-buffer loop playback

Input (`display/display_input.cpp`): `,` toggles `Loop::PingPong`, `.` toggles `Loop::ForwardOnly`. Sets `buffer_play_loop_mode_`; side-effect: `play_ = false`, `tick_playback_ = true` (`playback_controller.cpp:11-19`).

Main loop (`app/video_compare.cpp:1969-1993`): when the timer fires, advance a `frame_offset` index into each ring's `at(-frame_offset)`:

- ForwardOnly — decrement `frame_offset` (move cursor toward present); wrap to the end of history on reach 0.
- PingPong — flip direction at either bound.

No demuxer involvement, no decode: purely a cursor-browse over already-decoded frames. The HUD shows `[LOOP >]` or `[LOOP <>]` via the state badge described in `display/display_render_gpu.cpp`.

### 6.5 Timeline click — mouse-driven seek

`display/display_input.cpp:173-174`: left-click (outside selection mode) calls `playback_.set_seek_relative(mouse_x / window_width)` and `set_seek_from_start(true)`. Main loop (`app/video_compare.cpp:1436-1449`) interprets as an absolute seek: `next_position = shortest_duration_ × seek_relative + start_time_`. Full seek, drain-if-paused.

---

## 7. Buffer sizing

The decoded RGB buffering is now intentionally small: the heavy lifting lives in the encoded-packet `PacketRing` (§10). The FrameRing exists to give the L0 pivot fast path a few frames of local scrub history without paying decode cost per step.

`frame_buffer_size_` (`app/video_compare.h:224`):

- Sourced from `config.frame_buffer_size` (default `12`, `app/config.h:72`). Was 50 pre-Phase-3; dropped to 12 because the PacketRing is the real backing store and L1 re-decode (§11) reconstitutes deeper history when needed.
- CLI: `--frame-buffer-size N` (`app/main.cpp:764-777`), minimum 1.
- Passed to each `SideState`'s `FrameRing(history_capacity, prefetch_capacity)` at construction. Both halves get the same capacity.

Constants adjacent to it:

- `QUEUE_SIZE = 3` — inter-stage queue depth. Was 5 pre-Phase-3. Independent of `frame_buffer_size_`.
- `SLEEP_PERIOD_MS = 10` — poll interval for `ready_to_seek_` spin loops.
- `packet_buffer_bytes` (default 256 MiB, CLI `--packet-buffer-size`) — PacketRing byte budget per side.

Larger `frame_buffer_size_` means:

- More memory (RGB frames at full resolution × 2 sides × 2 halves + 1 current slot = `2N+1` per side). At 4K HDR that's ≈47.5 MiB × (2N+1) per 4K-HDR side.
- More scrub history served by L0 pivots (microseconds per step) before L1 re-decode kicks in.
- Deeper forward prefetch for `+` without a pipeline catch-up wait.
- Longer in-buffer loop range under `,`/`.` (until Phase 4 replaces this with eager decode of the PacketRing on loop entry).

Rough memory profile on a 4K HDR + 720p SDR comparison, `--packet-buffer-size 256M`:

| defaults                | peak RSS  |
| ----------------------- | --------- |
| Phase 3 (`-f 12`, Q=3)  | ≈1.95 GiB |
| Legacy (`-f 50`, Q=5)   | ≈4.7 GiB  |

The 4K HDR case has ≈47.5 MiB per decoded frame and an OS / SDL / libplacebo baseline of ~800 MiB, so the absolute floor on that workload is ~1.2 GiB regardless of ring size. The architectural win is replacing a growing decoded-RGB buffer with a bounded encoded-packet buffer.

---

## 8. Loop mode — auto-loop trigger and eager materialize

### 8.1 Mode entry and exit

Loop mode has three states (`app/video_compare.h`, `Display::Loop`): `Off`, `ForwardOnly`, `PingPong`. User toggles via `,` (PingPong) and `.` (ForwardOnly). Auto-loop (below) can also trigger an initial entry.

The main loop tracks the previous loop mode across iterations and fires two hooks on transition (`app/video_compare.cpp`, the loop-mode transition block after packet-ring eviction):

- **Off → non-Off (loop entry)**: eager materialize — barrier the pipeline, flush each decoder, reinit each filter, walk every seeking side's PacketRing from the keyframe ≤ `(current_pts - loop_cap_sec)` forward, decode through `current_pts`, and push each frame into the side's FrameRing. On exit, each ring has history populated with the N−1 pre-target frames and `current` set to the present frame. The FrameRing's history capacity is grown to fit.
- **non-Off → Off (loop exit)**: `ring.set_capacities(frame_buffer_size_, frame_buffer_size_)` shrinks each ring back to operating size. The most-recent-around-cursor frames are kept; excess is evicted.

Cap controlled by `VIDEO_COMPARE_LOOP_CAP_SEC` (default 5.0 s). At 4K HDR (≈47.5 MiB per decoded frame × 60 fps × 5 s = ≈14 GiB per side) this is memory-expensive; lower it on memory-constrained systems. At 1080p RGB24 5 s × 30 fps ≈ 940 MiB.

Diagnostic: `VIDEO_COMPARE_LOG_SEEK_TIMING=1` emits `[loop] mode transition X→Y` and `[loop] materialize done in Nms, left=K right=K frames`.

### 8.2 Auto-loop

`auto_loop_mode_` (`app/video_compare.h`) is set once at startup from the CLI `--auto-loop-mode` flag: `off`, `on` (→ `Loop::ForwardOnly`), or `pp` (→ `Loop::PingPong`). When non-`Off`, the main loop fires one automatic loop-mode entry as soon as the buffer has collected enough content:

```cpp
if (auto_loop_mode_ != Loop::Off && !auto_loop_triggered &&
    (buffer_is_full || end_of_file)) {
  display_->set_buffer_play_loop_mode(auto_loop_mode_);
  auto_loop_triggered = true;
}
```

`buffer_is_full` now measures PacketRing span against `loop_cap_sec` (Phase 4 rewire). Previously it measured FrameRing size against `frame_buffer_size_`, which fired in a fraction of a second after Phase 3's ring shrink. Fallback: if any side lacks a PacketRing, reverts to the old FrameRing-fill criterion.

`end_of_file` — every converter queue is stopped AND there's no activity this tick.

The auto-loop trigger sets `buffer_play_loop_mode` via the display, which the main loop's transition detector picks up and runs the materialize flow in 8.1. Fires once per process.

---

## 9. Playback-in-sync signal

`is_in_sync(left_pts, right_pts, left_delta, right_delta)` (`app/video_compare.cpp:68-72`) returns true iff neither side is behind the other by more than `min_delta`. The main loop computes it once per refresh (`:1845`) and pushes the value into Display (`:1846`, `display_->set_playback_in_sync(...)`).

Consequences:

- When `!is_playback_in_sync`, the main loop reduces UI refresh rate to ~10 Hz (`:1848`) so the pipeline gets more CPU to resync.
- `Display::playback_in_sync_` drives the **`[SEEK]`** state badge in the HUD (`display/display_render_gpu.cpp`), which takes priority over `[LOOP >]` / `[LOOP <>]` / `[PLAY]` / `[PAUSE]`. This is the only user-visible hint that a post-seek catch-up or sync-adjust pass is in progress — especially useful during a playing seek when the drain path is skipped (§5.4) and the user would otherwise wonder why playback appears to briefly rewind before racing forward.
- Live quality metrics (PSNR / SSIM / VMAF in `display_render_gpu.cpp` and `display_render_sdl.cpp`) are gated on `!playback_.play()` — not `playback_in_sync_` — so they only run while paused. During the sync-adjust catch-up ticks that happen inside paused seeks, the sides are out of sync and the overlay values stay frozen at their last stable computation, rather than recomputing on transient pairs.

---

## 10. PacketRing — encoded spill buffer

`PacketRing` (`media/buffering/packet_ring.h`) is the per-side byte-budgeted ring of cloned `AVPacket`s, fed from the demuxer thread. Its role is to cheaply retain seconds-to-minutes of encoded content around the playback cursor so that backward seeks and frame steps can re-decode from a nearby keyframe without going back out to disk / demuxer.

Shape (mirrors Chromium's `SourceBufferStream` + `SourceBufferRange`):

- A `std::list<std::unique_ptr<PacketRange>> ranges_`, sorted by `first_pts`. A new range opens on a demuxer-seek discontinuity; otherwise appends extend the tail range.
- Each `PacketRange` owns a deque of packets and a `KeyframeMap` (`std::map<int64_t pts, size_t buffer_index>`). The index uses a `keyframe_map_index_base_` offset so front-GOP evictions don't rewrite all existing entries.
- Fudge-room adjacency (`2 × max_inter_buffer_distance_`, time-base-floored) decides whether a new packet extends the tail range or opens a new one.

Producer (demuxer thread, `app/video_compare.cpp:562-568`): each video packet is `av_packet_clone`d and `append()`d to the side's PacketRing alongside the existing `packet_queues_` push. Cost is a memcpy of the encoded bytes — trivial compared to decode.

Consumer (main thread):

- `covers(target_pts)` — any live range includes the target.
- `keyframe_at_or_before(target_pts)` — O(log K) lookup across all ranges; returns the latest keyframe ≤ target along with its absolute buffer index.
- `iterate_from(index, visitor)` — walks packets in DTS/storage order; used by L1.
- `evict_to_budget(current_pts)` — called once per main-loop tick (`app/video_compare.cpp:1196-1216`). Drops farthest range first, then chops whole GOPs from the far end of the remaining range, protecting the GOP that contains `current_pts`.

Sizing: `--packet-buffer-size <N[K|M|G]>` (default 256 MiB per side). On a 1080p H.264 clip at ~5 Mbps that's ≈7 minutes of coverage; at 4K HDR HEVC 30 Mbps it's ≈70 seconds.

Diagnostic: set `VIDEO_COMPARE_SHOW_PACKET_RING=1` for periodic `[packet-ring <side>] bytes=… packets=… ranges=… keyframes=… pts=[…]` lines on stderr.

---

## 11. L1 re-decode — fast backward seek

`L1` is a seek tier that sits between the existing L0 ring pivot (§5.2) and L2 full seek (§5.3). It drives the decoder / filter / converter on the main thread, feeding packets out of the PacketRing starting from the keyframe ≤ target, until the target frame is captured. The pipeline is held idle behind a `ReadyToSeek` barrier for the duration.

Dispatch order inside the seek branch (`app/video_compare.cpp:1273-…`):

1. **L0 pivot** — target already in FrameRing history/prefetch → pointer swap only.
2. **L1 re-decode** — PacketRing covers target AND keyframe-to-target distance ≤ `VIDEO_COMPARE_L1_MAX_KF_DISTANCE_SEC` (default 0.5 s). Drives decode inline.
3. **L2 full seek** — existing path: stop queues, `av_seek_frame`, `pop_and_reset` drains pipeline to target.

L1 eligibility also requires `!force_seek_current_position` (no pending crop/HDR reconfig), `!single_decoder_mode_`, and non-single-frame media on all seeking sides. Any failure drops to L2; the landing frame is correct either way.

Landing behavior: L1 clears the ring, replays pre-target frames into history via `push_prefetch` + `advance()` loop, then sets the target as `current`. This seeds the ring so subsequent backward presses hit L0 pivots at microsecond cost, amortizing the L1 cost across up to `frame_buffer_size_` presses.

Post-L1 demuxer positioning: after inline decode the main thread seeks each seeking demuxer *forward* to the keyframe at-or-after target+0.001s (`backward=false`). Seeking backward (to keyframe ≤ target) would make the pipeline re-emit the pre-target frames already in history, corrupting prefetch ordering and causing the first `+` press to briefly jump backward. If the forward seek fails on a sparse-keyframe input with no subsequent keyframe, it falls back to the backward seek and relies on `sync_frame_queue` to self-correct within a frame or two.

Worker-thread race note: after the barrier idles, the seek flag is cleared (inside `enter_seek_barrier`) so the decoder worker's `ready_to_seek` sleep loop stops calling `avcodec_flush_buffers` — necessary because the main thread is about to flush and send packets on the same codec context. Queues stay stopped so workers don't wake into a decode attempt.

Diagnostic: `VIDEO_COMPARE_LOG_SEEK_TIMING=1` logs `tier=<L0back|L0forward|L1|L1->L2|L2>` and elapsed time per seek, plus `[l1-skip]` lines explaining any L1 eligibility failures. `VIDEO_COMPARE_LOG_L1_STAGES=1` adds per-step L1 trace.

---

## 12. Cheat sheet

| Action                             | Key             | Dispatch                     | Demuxer? | Decode?
| ---------------------------------- | --------------- | ---------------------------- | -------- | -------
| Play / pause                       | Space           | —                            | —        | —
| In-buffer loop (PP)                | ,               | —                            | —        | —
| In-buffer loop (FW)                | .               | —                            | —        | —
| Timeline click (paused)            | left-click      | L1 if kf ≤ 0.5 s, else L2    | L2: yes  | yes
| Timeline click (playing)           | left-click      | L2 (no drain)                | yes      | yes
| Relative seek                      | arrows, PgUp/Dn | L1 if kf ≤ 0.5 s, else L2    | L2: yes  | yes
| Frame step (paused)                | Shift+A         | L1 if kf ≤ 0.5 s, else L2    | L2: yes  | yes
| Forward frame step                 | Shift+D         | `advance_ring` (no seek)     | —        | —
| Right-shift ±N (in FrameRing)      | +/-             | L0 pivot                     | —        | —
| Right-shift ±N (out of FrameRing)  | +/-             | L1 if kf ≤ 0.5 s, else L2    | L2: yes  | yes
| Clear crop                         | Backspace       | L2 (filter rebuild)          | yes      | yes
| Auto-align (ring covers window)    | `` ` ``         | run_auto_align → L0 pivot    | —        | —
| Auto-align (walk needed)           | `[`, `]`        | run_auto_align + barriered PacketRing walk → L0/L1/L2 | right side only | yes (decode only, no filter/convert)

For the auto-align algorithm and the barriered PacketRing walk it runs, see [auto-align.md](auto-align.md).
