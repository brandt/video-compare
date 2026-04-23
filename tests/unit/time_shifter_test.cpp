#include <doctest/doctest.h>
#include "app/config.h"
#include "media/playback/time_shifter.h"

namespace {

TimeShiftConfig zero_config() {
  TimeShiftConfig c;
  c.offset_ms = 0;
  c.multiplier = AVRational{1, 1};
  return c;
}

TimeShiftConfig offset_config(int64_t offset_ms) {
  TimeShiftConfig c;
  c.offset_ms = offset_ms;
  c.multiplier = AVRational{1, 1};
  return c;
}

}  // namespace

TEST_CASE("TimeShifter: fresh instance has zero static_shift without offset") {
  TimeShifter ts(zero_config());
  CHECK(ts.static_shift() == 0);
  CHECK(ts.total_frames_shifted() == 0);
}

TEST_CASE("TimeShifter: initial offset carries through to static_shift") {
  TimeShifter ts(offset_config(2000));  // +2 s
  CHECK(ts.static_shift() == 2'000'000);
}

TEST_CASE("TimeShifter: frame accumulator drives static_shift") {
  TimeShifter ts(zero_config());
  ts.set_frame_shift_accumulator(10, 16'667);  // 10 frames x 16.667 ms
  CHECK(ts.static_shift() == 166'670);
  CHECK(ts.total_frames_shifted() == 10);

  ts.set_frame_shift_accumulator(20, 16'667);
  CHECK(ts.static_shift() == 333'340);
}

TEST_CASE("TimeShifter: set_static_shift_us overrides frame-accumulator drift") {
  TimeShifter ts(zero_config());
  // Integer-truncated avg (like the 59.98 fps VP9 bug): 16 instead of 16.67.
  ts.set_frame_shift_accumulator(128, 16'000);
  CHECK(ts.static_shift() == 2'048'000);  // drift — 4 ms short of the intended 2.052 s

  // Auto-align applies the exact delta after landing on the target frame.
  ts.set_static_shift_us(2'052'000);
  CHECK(ts.static_shift() == 2'052'000);
}

TEST_CASE("TimeShifter: effective_shift == static_shift under 1:1 multiplier") {
  TimeShifter ts(offset_config(1500));
  // Multiplier 1:1 means dynamic_shift(pts) == 0 for any pts, so effective == static.
  CHECK(ts.effective_shift(0) == ts.static_shift());
  CHECK(ts.effective_shift(5'000'000) == ts.static_shift());
  CHECK(ts.effective_shift(-1'000'000) == ts.static_shift());
}

TEST_CASE("TimeShifter: set_static_shift_us doesn't desync total_frames_shifted") {
  // The exact override is meant for consumers that know the true right-vs-left
  // pts delta and want effective_shift to report it. The frame count is
  // preserved so a subsequent +/- press can still compute relative shifts.
  TimeShifter ts(zero_config());
  ts.set_frame_shift_accumulator(10, 16'000);
  CHECK(ts.total_frames_shifted() == 10);

  ts.set_static_shift_us(167'000);  // exact delta for 10 frames @ 16.7 ms
  CHECK(ts.static_shift() == 167'000);
  CHECK(ts.total_frames_shifted() == 10);  // unchanged
}

TEST_CASE("TimeShifter: to_common inverts effective_shift") {
  TimeShifter ts(offset_config(2000));
  // to_common(raw) = raw - effective_shift(raw).
  // For right_raw = 2'000'000 (2 s) and static_shift = 2'000'000, common = 0.
  CHECK(ts.to_common(2'000'000) == 0);
  // For right_raw = 3'000'000 and static_shift = 2'000'000, common = 1'000'000 (1 s).
  CHECK(ts.to_common(3'000'000) == 1'000'000);
}
