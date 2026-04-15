#pragma once
#include <cmath>
#include <cstdint>
#include "config.h"
#include "ffmpeg.h"

extern "C" {
#include <libavutil/rational.h>
}

// Centralizes right-side PTS arithmetic. One `TimeShifter` owns the time shift state
// that used to be sprinkled across compare() as `static_right_time_shift`,
// `time_shift_offset_av_time_`, and the inline `calculate_dynamic_time_shift(...)`
// calls.
//
// Time-shift composition:
//
//   effective_shift(pts) = static_shift + dynamic_shift(pts)
//
// where:
//
//   static_shift     = time_shift_offset + total_right_time_shifted * right_delta
//                      (the constant-per-seek contribution, updated by +/- keys)
//   dynamic_shift(p) = p - (p * multiplier.den / multiplier.num)
//                      (the rational-multiplier contribution, grows with pts)
//
// The right side's PTS in the common (left) time frame is:
//
//   common(pts) = pts - effective_shift(pts)
//
// which is exactly what the sync code compares against `left.pts`.
class TimeShifter {
 public:
  explicit TimeShifter(const TimeShiftConfig& config) : multiplier_{config.multiplier}, offset_av_time_{static_cast<int64_t>(static_cast<double>(config.offset_ms) * MILLISEC_TO_AV_TIME)}, static_shift_{offset_av_time_} {}

  // --- State ---

  // Recompute `static_shift` from the `+`/`-` frame accumulator and the right-side
  // average frame duration.
  void set_frame_shift_accumulator(int total_frames_shifted, int64_t right_delta) {
    total_frames_shifted_ = total_frames_shifted;
    static_shift_ = offset_av_time_ + static_cast<int64_t>(total_frames_shifted) * right_delta;
  }

  // --- Getters ---
  int64_t static_shift() const { return static_shift_; }
  int total_frames_shifted() const { return total_frames_shifted_; }
  const AVRational& multiplier() const { return multiplier_; }
  int64_t offset_av_time() const { return offset_av_time_; }

  // Dynamic-shift contribution at the given raw PTS. The `inverse` form is used when
  // recovering the shift FROM a raw post-seek PTS (subtract from pts to reach common);
  // the forward form is used when computing the seek target.
  int64_t dynamic_shift(int64_t raw_pts, bool inverse = true) const {
    if (inverse) {
      return raw_pts - av_rescale_q(raw_pts, AVRational{multiplier_.den, multiplier_.num}, AVRational{1, 1});
    }
    return av_rescale_q(raw_pts, AVRational{multiplier_.num, multiplier_.den}, AVRational{1, 1}) - raw_pts;
  }

  // Full effective shift at the given raw PTS (static + dynamic, inverse form).
  int64_t effective_shift(int64_t raw_pts) const {
    return static_shift_ + dynamic_shift(raw_pts, true);
  }

  // Convert a raw right-side PTS into the common (left) time frame.
  int64_t to_common(int64_t raw_pts) const {
    return raw_pts - effective_shift(raw_pts);
  }

  // Nudge a near-zero static shift away from zero by half a frame, so a post-seek
  // right demuxer lands on a different frame than the left even when the accumulated
  // shift nearly cancels the `-t` offset. Exact integer-frame shifts pass through
  // unchanged.
  void nudge_away_from_zero(int64_t right_delta) {
    static constexpr int64_t FALLBACK_THRESHOLD = 2 * 1000;
    const int64_t threshold = (right_delta > 0) ? (right_delta / 2) : FALLBACK_THRESHOLD;
    if (static_shift_ != 0 && std::abs(static_shift_) < threshold) {
      static_shift_ = (static_shift_ > 0) ? threshold : -threshold;
    }
  }

 private:
  AVRational multiplier_;
  int64_t offset_av_time_;       // Constant offset from the `-t` flag.
  int total_frames_shifted_{0};  // Running `+`/`-` accumulator (in frames).
  int64_t static_shift_;         // offset_av_time_ + total_frames_shifted_ * right_delta.
};
