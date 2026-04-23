#include <doctest/doctest.h>
#include <functional>
#include <memory>
#include "media/buffering/packet_ring.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

// Helper: allocate an AVPacket with the given pts/dts, mark keyframe iff `is_kf`.
// The ring takes ownership via PacketPtr.
PacketRing::PacketPtr make_pkt(int64_t pts, int64_t dts, bool is_kf, int size = 32) {
  AVPacket* p = av_packet_alloc();
  REQUIRE(p != nullptr);
  // Allocate a small buffer so byte_size is nonzero (fudge_room + eviction
  // logic pays attention to sizes).
  av_new_packet(p, size);
  p->pts = pts;
  p->dts = dts;
  p->flags = is_kf ? AV_PKT_FLAG_KEY : 0;
  return PacketRing::PacketPtr{p, [](AVPacket* q) { av_packet_free(&q); }};
}

// 1/1000 time base: 1 unit = 1 ms. Convenient for 59.94 fps math where deltas
// are ~17 ms.
const AVRational kMsTimeBase = {1, 1000};

}  // namespace

TEST_CASE("PacketRing: fresh ring has zero bytes and no ranges") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  const auto s = ring.stats();
  CHECK(s.packet_count == 0);
  CHECK(s.range_count == 0);
  CHECK(s.bytes_used == 0);
  CHECK(s.pts_max == INT64_MIN);
  CHECK(s.pts_min == INT64_MIN);
  CHECK_FALSE(ring.covers(0));
}

TEST_CASE("PacketRing: contiguous appends populate a single range") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  for (int64_t pts = 0; pts < 100; pts += 17) {
    const bool is_kf = (pts == 0);
    ring.append(make_pkt(pts, pts, is_kf));
  }
  const auto s = ring.stats();
  CHECK(s.packet_count == 6);
  CHECK(s.range_count == 1);
  CHECK(s.pts_min == 0);
  CHECK(s.pts_max == 85);
  CHECK(ring.covers(50));
}

TEST_CASE("PacketRing: keyframe_at_or_before returns the latest keyframe at or before target") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  // Keyframes at 0, 50, 100. Non-key between.
  ring.append(make_pkt(0, 0, true));
  ring.append(make_pkt(17, 17, false));
  ring.append(make_pkt(34, 34, false));
  ring.append(make_pkt(50, 50, true));
  ring.append(make_pkt(67, 67, false));
  ring.append(make_pkt(84, 84, false));
  ring.append(make_pkt(100, 100, true));

  auto hit = ring.keyframe_at_or_before(49);
  REQUIRE(hit.has_value());
  CHECK(hit->kf_pts == 0);

  hit = ring.keyframe_at_or_before(50);
  REQUIRE(hit.has_value());
  CHECK(hit->kf_pts == 50);

  hit = ring.keyframe_at_or_before(75);
  REQUIRE(hit.has_value());
  CHECK(hit->kf_pts == 50);

  hit = ring.keyframe_at_or_before(200);
  REQUIRE(hit.has_value());
  CHECK(hit->kf_pts == 100);
}

TEST_CASE("PacketRing: keyframe_at_or_before returns nullopt before first keyframe") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  ring.append(make_pkt(100, 100, true));
  CHECK_FALSE(ring.keyframe_at_or_before(50).has_value());
}

TEST_CASE("PacketRing: clear wipes ranges, packets, and bytes") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  for (int64_t pts = 0; pts < 60; pts += 17) {
    ring.append(make_pkt(pts, pts, pts == 0));
  }
  CHECK(ring.stats().packet_count > 0);
  ring.clear();
  const auto s = ring.stats();
  CHECK(s.packet_count == 0);
  CHECK(s.range_count == 0);
  CHECK(s.bytes_used == 0);
  CHECK_FALSE(ring.covers(0));
}

TEST_CASE("PacketRing: mark_discontinuity forces the next append to open a new range") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  ring.append(make_pkt(0, 0, true));
  ring.append(make_pkt(17, 17, false));
  CHECK(ring.stats().range_count == 1);

  ring.mark_discontinuity();
  ring.append(make_pkt(34, 34, true));
  const auto s = ring.stats();
  CHECK(s.range_count == 2);
  CHECK(s.pts_max == 34);
}

TEST_CASE("PacketRing: large pts gap opens a new range automatically") {
  PacketRing ring(/*byte_budget=*/1024 * 1024, kMsTimeBase);
  // Seed a range with a tight packet cadence.
  for (int64_t pts = 0; pts < 100; pts += 17) {
    ring.append(make_pkt(pts, pts, pts == 0));
  }
  CHECK(ring.stats().range_count == 1);

  // Jump far forward — outside the fudge-room of the existing tail range.
  ring.append(make_pkt(5000, 5000, true));
  const auto s = ring.stats();
  CHECK(s.range_count == 2);
  CHECK(s.pts_max == 5000);
}
