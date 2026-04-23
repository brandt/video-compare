#include <doctest/doctest.h>
#include <memory>
#include "media/buffering/frame_ring.h"
extern "C" {
#include <libavutil/frame.h>
}

namespace {

// Helper: allocate an AVFrame, tag it with `pts`, return a FramePtr that frees it.
FrameRing::FramePtr make_frame(int64_t pts) {
  AVFrame* raw = av_frame_alloc();
  REQUIRE(raw != nullptr);
  raw->pts = pts;
  return FrameRing::FramePtr{raw, [](AVFrame* f) { av_frame_free(&f); }};
}

}  // namespace

TEST_CASE("FrameRing: fresh ring is empty, no current") {
  FrameRing ring(12, 12);
  CHECK(ring.history_size() == 0);
  CHECK(ring.prefetch_size() == 0);
  CHECK(ring.has_current() == false);
  CHECK(ring.current_frame() == nullptr);
}

TEST_CASE("FrameRing: set_current + at(0) returns the same frame") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(42));
  REQUIRE(ring.has_current());
  CHECK(ring.current_frame()->pts == 42);
  CHECK(ring.at(0)->pts == 42);
}

TEST_CASE("FrameRing: push_prefetch + advance moves frames through current") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(100));
  ring.push_prefetch(make_frame(200));
  ring.push_prefetch(make_frame(300));
  CHECK(ring.prefetch_size() == 2);
  CHECK(ring.at(1)->pts == 200);
  CHECK(ring.at(2)->pts == 300);

  ring.advance();
  CHECK(ring.current_frame()->pts == 200);
  CHECK(ring.history_size() == 1);
  CHECK(ring.at(-1)->pts == 100);
  CHECK(ring.prefetch_size() == 1);
  CHECK(ring.at(1)->pts == 300);
}

TEST_CASE("FrameRing: pivot_forward(n) moves current forward by n prefetch entries") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(0));
  for (int i = 1; i <= 6; ++i) {
    ring.push_prefetch(make_frame(i * 1000));
  }
  CHECK(ring.prefetch_size() == 6);

  ring.pivot_forward(3);
  CHECK(ring.current_frame()->pts == 3000);
  CHECK(ring.history_size() == 3);
  CHECK(ring.prefetch_size() == 3);
  CHECK(ring.at(-1)->pts == 2000);
  CHECK(ring.at(-3)->pts == 0);
  CHECK(ring.at(1)->pts == 4000);
  CHECK(ring.at(3)->pts == 6000);
}

TEST_CASE("FrameRing: pivot_forward throws when n exceeds prefetch depth") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(0));
  ring.push_prefetch(make_frame(1000));
  CHECK_THROWS_AS(ring.pivot_forward(5), std::out_of_range);
}

TEST_CASE("FrameRing: pivot_backward(n) pulls from history") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(0));
  for (int i = 1; i <= 4; ++i) {
    ring.push_prefetch(make_frame(i * 1000));
  }
  // Advance 3 times to put frames in history.
  ring.advance();
  ring.advance();
  ring.advance();
  CHECK(ring.current_frame()->pts == 3000);
  CHECK(ring.history_size() == 3);

  ring.pivot_backward(2);
  CHECK(ring.current_frame()->pts == 1000);
  CHECK(ring.history_size() == 1);
  CHECK(ring.prefetch_size() == 3);
  CHECK(ring.at(1)->pts == 2000);
  CHECK(ring.at(3)->pts == 4000);
}

TEST_CASE("FrameRing: pivot_backward throws when n exceeds history depth") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(0));
  ring.push_prefetch(make_frame(1000));
  ring.advance();
  // history = 1; trying to pivot back 2 should throw.
  CHECK_THROWS_AS(ring.pivot_backward(2), std::out_of_range);
}

TEST_CASE("FrameRing: push_prefetch rejects frames once prefetch is full") {
  FrameRing ring(/*history=*/12, /*prefetch=*/3);
  ring.set_current(make_frame(0));
  CHECK(ring.push_prefetch(make_frame(1)) == true);
  CHECK(ring.push_prefetch(make_frame(2)) == true);
  CHECK(ring.push_prefetch(make_frame(3)) == true);
  CHECK(ring.prefetch_full() == true);
  CHECK(ring.push_prefetch(make_frame(4)) == false);  // rejected
  CHECK(ring.prefetch_size() == 3);
  CHECK(ring.at(3)->pts == 3);  // last accepted frame is still at offset +3
}

TEST_CASE("FrameRing: set_capacities trims history oldest-first, prefetch newest-first") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(0));
  for (int i = 1; i <= 6; ++i) {
    ring.push_prefetch(make_frame(i));
  }
  ring.advance();
  ring.advance();  // history = [0, 1], prefetch = [3,4,5,6], current = 2

  // Shrink both sides.
  ring.set_capacities(1, 2);
  CHECK(ring.history_size() == 1);
  CHECK(ring.prefetch_size() == 2);
  CHECK(ring.at(-1)->pts == 1);   // oldest history (pts=0) was dropped
  CHECK(ring.at(1)->pts == 3);    // prefetch front preserved
  CHECK(ring.at(2)->pts == 4);    // prefetch[1]; newer ones dropped
}

TEST_CASE("FrameRing: browsable_span counts history + current + prefetch") {
  FrameRing ring(12, 12);
  CHECK(ring.browsable_span() == 0);
  ring.set_current(make_frame(0));
  CHECK(ring.browsable_span() == 1);
  ring.push_prefetch(make_frame(1));
  ring.push_prefetch(make_frame(2));
  CHECK(ring.browsable_span() == 3);
  ring.advance();
  CHECK(ring.browsable_span() == 3);  // one moved history, count preserved
  CHECK(ring.history_size() == 1);
  CHECK(ring.prefetch_size() == 1);
}

TEST_CASE("FrameRing: clear resets everything") {
  FrameRing ring(12, 12);
  ring.set_current(make_frame(0));
  ring.push_prefetch(make_frame(1));
  ring.advance();
  ring.push_prefetch(make_frame(2));

  ring.clear();
  CHECK(ring.has_current() == false);
  CHECK(ring.history_size() == 0);
  CHECK(ring.prefetch_size() == 0);
  CHECK(ring.browsable_span() == 0);
}
