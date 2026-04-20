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
- All four created with `QUEUE_SIZE = 5` (`app/video_compare.cpp:25`).

These inter-stage queues are deliberately shallow. The user-visible buffering is _after_ the converter, in each side's `FrameRing`.

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

The non-blocking `try_pop` is what keeps the main thread responsive — intake only drains what the converter thread has already produced; it never waits on decode. Because this runs before the seek block examines ring sizes, a forward `+N` pivot (§5) sees up to `prefetch_capacity_` frames available, not just the raw `QUEUE_SIZE=5` of the converter queue.

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

Full seek is heavyweight. Steps (`:1355-1572`):

1. **ReadyToSeek barrier** (`:1355-1396`). Per-side flags in `seeking_per_side_` (`app/video_compare.h:262`) tell workers to exit their loops and idle at their barrier. Main thread `stop()`s the packet queue and repeatedly `empty()`s every downstream queue while spin-waiting on `ready_to_seek_.all_are_idle_where(should_seek)`. Empty-calls are repeated because workers can produce one more frame between the flag check and the barrier entry. `pure_right_frame_shift` scopes the barrier to the right sides only, so the left pipeline keeps running.
2. **Consume pending filter changes** on seeking sides (`:1401-1407`).
3. **Per-side demuxer seek** (`:1466-1521`):
   - Target is in seconds on each side's own time axis; the right target includes `TimeShifter::static_shift() + dynamic_shift()` so the subsequent comparison lands aligned with left.
   - `backward = seek_from_start || seek_relative < 0 || shift_right_frames != 0 || (force_seek_current_position && all_multi_frame)` (`:1459`). `AVSEEK_FLAG_BACKWARD` is required on absolute scrubs because sparse-keyframe inputs (e.g. a single keyframe at PTS 0) reject at-or-after seeks.
   - On a forward seek that overshoots EOF, the code restores the pre-seek position with a backward seek for every seeking side (`:1524-1538`) and surfaces "Unable to seek past end of file".
4. **Restart packet/decoded/filtered/converted queues** on seeking sides (`:1547-1564`) so workers can resume pushing.

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

| Seek source | `pure_right_frame_shift` | `display_->get_play()` | Drain? | Why |
|---|---|---|---|---|
| Paused timeline click / arrow / Shift+A/D | false | false | **yes** | User sees the next paused frame; it must be the clicked frame, not a keyframe hundreds of ms earlier. |
| Playing timeline click / arrow | false | true | no | The main-loop "behind schedule, advance fast" mechanism (§4.1) picks up the keyframe and rapidly advances through intermediate frames at decode-bound rate. Draining would block the UI for a whole GOP with stale overlay values visible. |
| `+` / `-` fall-through to full seek | true | either | no | `+`/`-` keys are held down; blocking each press on a GOP-duration drain is unacceptable. Landing imprecision here matches the pre-drain behavior users already tolerate. |

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

`frame_buffer_size_` (`app/video_compare.h:223`):

- Sourced from `config.frame_buffer_size` (default `50`, `app/config.h:72`).
- CLI: `--frame-buffer-size N` (`app/main.cpp:765-775`), minimum 1.
- Passed to each `SideState`'s `FrameRing(history_capacity, prefetch_capacity)` at construction (`app/video_compare.cpp:1020`). Both halves get the same capacity.

Constants adjacent to it:

- `QUEUE_SIZE = 5` — inter-stage queue depth. Independent of `frame_buffer_size_`.
- `SLEEP_PERIOD_MS = 10` — poll interval for `ready_to_seek_` spin loops.

Larger `frame_buffer_size_` means:

- More memory (RGB frames at full resolution × 2 sides × 2 halves).
- Larger backward history for ring-pivot to serve `-` without a full seek.
- Deeper forward prefetch for `+` without a full seek.
- Longer in-buffer loop range under `,`/`.`.
- The auto-loop trigger (§8) takes longer to fire.

---

## 8. Auto-loop

`auto_loop_mode_` (`app/video_compare.h:222`) is set once at startup from the CLI `--auto-loop-mode` flag: `off`, `on` (→ `Loop::ForwardOnly`), or `pp` (→ `Loop::PingPong`).

Triggered from the main loop (`app/video_compare.cpp:1996-2001`):

```cpp
if (auto_loop_mode_ != Loop::Off && !auto_loop_triggered &&
    (buffer_is_full || end_of_file)) {
  display_->set_buffer_play_loop_mode(auto_loop_mode_);
  auto_loop_triggered = true;
}
```

Where:

- `buffer_is_full` (`:1836`) — both sides' rings have filled their history + current (`history_plus_current_size() == frame_buffer_size_`).
- `end_of_file` (`:1835`) — every converter queue is stopped AND there's no activity this tick.

The effect is identical to the user pressing `,` or `.`: regular playback flips to in-buffer ring-browse (§6.4). Fires once per process.

---

## 9. Playback-in-sync signal

`is_in_sync(left_pts, right_pts, left_delta, right_delta)` (`app/video_compare.cpp:68-72`) returns true iff neither side is behind the other by more than `min_delta`. The main loop computes it once per refresh (`:1845`) and pushes the value into Display (`:1846`, `display_->set_playback_in_sync(...)`).

Consequences:

- When `!is_playback_in_sync`, the main loop reduces UI refresh rate to ~10 Hz (`:1848`) so the pipeline gets more CPU to resync.
- `Display::playback_in_sync_` drives the **`[SEEK]`** state badge in the HUD (`display/display_render_gpu.cpp`), which takes priority over `[LOOP >]` / `[LOOP <>]` / `[PLAY]` / `[PAUSE]`. This is the only user-visible hint that a post-seek catch-up or sync-adjust pass is in progress — especially useful during a playing seek when the drain path is skipped (§5.4) and the user would otherwise wonder why playback appears to briefly rewind before racing forward.
- Live quality metrics (PSNR / SSIM / VMAF in `display_render_gpu.cpp` and `display_render_sdl.cpp`) are gated on `!playback_.play()` — not `playback_in_sync_` — so they only run while paused. During the sync-adjust catch-up ticks that happen inside paused seeks, the sides are out of sync and the overlay values stay frozen at their last stable computation, rather than recomputing on transient pairs.

---

## 10. Cheat sheet

| Action                             | Key             | Seek branch | Drain?    | Demuxer? | Decode?
| ---------------------------------- | --------------- | ----------- | --------- | -------- | -------
| Play / pause                       | Space           | —           | —         | —        | —
| In-buffer loop (PP)                | ,               | —           | —         | —        | —
| In-buffer loop (FW)                | .               | —           | —         | —        | —
| Timeline click (paused)            | left-click      | full        | yes       | yes      | yes
| Timeline click (playing)           | left-click      | full        | no        | yes      | yes
| Relative seek                      | arrows, PgUp/Dn | full        | if paused | yes      | yes
| Frame step (paused)                | Shift+A/D       | full        | yes       | yes      | yes
| Right-shift ±N (pivot fits)        | +/-             | pivot       | —         | —        | —
| Right-shift ±N (pivot doesn't fit) | +/-             | full        | no        | yes      | yes
| Clear crop                         | Backspace       | full        | if paused | yes      | yes (filter rebuild)

