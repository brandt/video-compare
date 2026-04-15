#pragma once
#include <atomic>

// Sticky flag for the same-file optimization where only the left decoder runs and
// pushes each decoded frame into every right decoded_frame_queue. Starts enabled when
// eligible; disables permanently on the first activity that could introduce a right-
// left divergence (a frame shift, a scrub, a crop change, or a filter change).
//
// Replaces the earlier `single_decoder_mode_` atomic bool + `update_decoder_mode(int64_t)`
// dynamic re-evaluation. Those dynamic re-enables were the root cause of two deadlock
// bugs: the post-seek right pipeline would start depending on left's pushes to its
// decoded queue while left's pipeline was still draining its own queues, leaving
// `pop_and_reset(right)` waiting forever.
//
// Sticky-disabled means once disabled, the decision is final. Workers always read this
// flag; it only ever transitions enabled->disabled.
class SingleDecoderMode {
 public:
  // Initialize to `initial_enabled` iff all of:
  //   - both sides reference the same decoded source,
  //   - the `-t` multiplier is 1:1,
  //   - the `-t` offset is below the near-zero threshold.
  // When disabled at construction there is no path to enable it again.
  void init(bool initial_enabled) { enabled_.store(initial_enabled, std::memory_order_relaxed); }

  // Permanently disable. Idempotent.
  void disable_sticky() { enabled_.store(false, std::memory_order_relaxed); }

  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

 private:
  std::atomic_bool enabled_{false};
};
