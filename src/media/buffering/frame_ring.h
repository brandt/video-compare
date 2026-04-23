#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>

extern "C" {
#include <libavutil/frame.h>
}

// Symmetric display buffer with a cursor.
//
//   history[N-1] ... history[1] history[0]  |CURRENT|  prefetch[0] prefetch[1] ... prefetch[M-1]
//                                       ^                ^
//                            most recent past       next to display
//
// history and prefetch are independently bounded; the cursor is implicit in the layout.
//
// Cursor operations (main-thread only):
//
//   - advance(): normal playback step. current -> history.front(); prefetch[0] ->
//     current. Returns false if prefetch is empty.
//   - pivot_backward(n): n steps into history. current and the n-1 front history slots
//     slide into prefetch in order, so a subsequent pivot_forward(n) sees them again.
//   - pivot_forward(n): symmetric, consuming n prefetched frames.
//
// When a cursor step would overflow a side's capacity, the far-end frame on that side
// is evicted. This is a deliberate memory bound, not a design bug: a user bouncing
// around beyond capacity is navigating outside the buffered window.
//
// The pipeline populates the prefetch tail by calling push_prefetch(). The only
// producer is the main thread (which drains a thread-safe intake queue fed by the
// converter thread), so FrameRing itself holds no lock.
class FrameRing {
 public:
  using FramePtr = std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>;

  FrameRing(size_t history_capacity, size_t prefetch_capacity) : history_capacity_(history_capacity), prefetch_capacity_(prefetch_capacity) {}

  // --- Queries ---

  bool has_current() const { return current_ != nullptr; }
  int history_size() const { return static_cast<int>(history_.size()); }
  int prefetch_size() const { return static_cast<int>(prefetch_.size()); }

  AVFrame* current_frame() { return current_.get(); }
  const AVFrame* current_frame() const { return current_.get(); }

  // offset 0 == current; negative == history (offset -1 is the most recent past frame);
  // positive == prefetch (offset +1 is the next frame to be displayed). Returns nullptr
  // if the offset is out of range.
  const AVFrame* at(int offset) const { return at_impl(offset); }
  AVFrame* at(int offset) { return at_impl(offset); }

  // Count of frames reachable via at(), including current if present.
  int browsable_span() const { return history_size() + (has_current() ? 1 : 0) + prefetch_size(); }

  // Count of history + current, i.e., the past-and-present browse span (excludes
  // prefetch). Matches the pre-Tier-3 `frames_.size()`.
  int history_plus_current_size() const { return history_size() + (has_current() ? 1 : 0); }

  size_t history_capacity() const { return history_capacity_; }
  size_t prefetch_capacity() const { return prefetch_capacity_; }

  // --- Mutators (main-thread only) ---

  // Replace the current frame, preserving history and prefetch. Used after a full seek
  // once the first post-seek frame has been popped from the pipeline.
  void set_current(FramePtr frame) { current_ = std::move(frame); }

  // Drop everything. Used on full seek.
  void clear() {
    history_.clear();
    prefetch_.clear();
    current_.reset();
  }

  // Normal playback advance: current -> history front, prefetch front -> current.
  // Returns false (and leaves state unchanged) if no prefetched frame is available.
  bool advance() {
    if (prefetch_.empty()) {
      return false;
    }
    if (current_) {
      push_history_front(std::move(current_));
    }
    current_ = std::move(prefetch_.front());
    prefetch_.pop_front();
    return true;
  }

  // Pivot backward by n. Precondition: history_size() >= n.
  void pivot_backward(int n) {
    if (n <= 0) {
      return;
    }
    if (static_cast<size_t>(n) > history_.size()) {
      throw std::out_of_range("FrameRing::pivot_backward: n exceeds history depth");
    }
    for (int i = 0; i < n; ++i) {
      if (current_) {
        push_prefetch_front(std::move(current_));
      }
      current_ = std::move(history_.front());
      history_.pop_front();
    }
  }

  // Pivot forward by n. Precondition: prefetch_size() >= n.
  void pivot_forward(int n) {
    if (n <= 0) {
      return;
    }
    if (static_cast<size_t>(n) > prefetch_.size()) {
      throw std::out_of_range("FrameRing::pivot_forward: n exceeds prefetch depth");
    }
    for (int i = 0; i < n; ++i) {
      if (current_) {
        push_history_front(std::move(current_));
      }
      current_ = std::move(prefetch_.front());
      prefetch_.pop_front();
    }
  }

  // Append a newly-decoded frame to the prefetch tail. Returns false (dropping the
  // frame) if prefetch is at capacity.
  bool push_prefetch(FramePtr frame) {
    if (prefetch_.size() >= prefetch_capacity_) {
      return false;
    }
    prefetch_.push_back(std::move(frame));
    return true;
  }

  bool prefetch_full() const { return prefetch_.size() >= prefetch_capacity_; }

  // Resize capacities (trims oldest on each side if shrinking).
  void set_capacities(size_t history_capacity, size_t prefetch_capacity) {
    history_capacity_ = history_capacity;
    prefetch_capacity_ = prefetch_capacity;
    while (history_.size() > history_capacity_) {
      history_.pop_back();
    }
    while (prefetch_.size() > prefetch_capacity_) {
      prefetch_.pop_back();
    }
  }

 private:
  AVFrame* at_impl(int offset) const {
    if (offset == 0) {
      return current_.get();
    }
    if (offset < 0) {
      const size_t idx = static_cast<size_t>(-offset - 1);
      if (idx >= history_.size()) {
        return nullptr;
      }
      return history_[idx].get();
    }
    const size_t idx = static_cast<size_t>(offset - 1);
    if (idx >= prefetch_.size()) {
      return nullptr;
    }
    return prefetch_[idx].get();
  }

  // Push to history front; evict oldest (back) if over capacity.
  void push_history_front(FramePtr frame) {
    history_.push_front(std::move(frame));
    while (history_.size() > history_capacity_) {
      history_.pop_back();
    }
  }

  // Push to prefetch front; evict farthest-future (back) if over capacity.
  void push_prefetch_front(FramePtr frame) {
    prefetch_.push_front(std::move(frame));
    while (prefetch_.size() > prefetch_capacity_) {
      prefetch_.pop_back();
    }
  }

  size_t history_capacity_;
  size_t prefetch_capacity_;
  std::deque<FramePtr> history_;   // [0] = most recent past
  FramePtr current_;
  std::deque<FramePtr> prefetch_;  // [0] = next to display
};
