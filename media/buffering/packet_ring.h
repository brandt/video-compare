#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

// Encoded-packet spill buffer for the L1 re-decode path.
//
// Modeled on Chromium's SourceBufferStream + SourceBufferRange split: one owning
// container holds a list of contiguous-in-demuxer-order ranges, each with its
// own keyframe index. A demuxer seek creates a discontinuity so existing ranges
// are preserved (they may still be useful for a subsequent re-decode) and the
// next append opens a new range.
//
// Ordering:
//   - Packets are stored in demuxer (DTS / append) order inside a range.
//   - The keyframe_map_ in each range is keyed by PTS, because seek targets are
//     expressed in presentation time and a keyframe decodes self-standing so
//     its PTS bounds the displayable output.
//
// Ownership model:
//   - The demuxer thread is the sole producer. It calls append() after cloning
//     an AVPacket, and mark_discontinuity() after av_seek_frame.
//   - The main thread is the sole consumer. It calls keyframe_at_or_before(),
//     covers(), iterate_from(), and evict_to_budget().
//   - An internal mutex serializes the two so external locking is not required.
//     Critical sections are short; the consumer never holds the lock while
//     running decode work.

class PacketRange {
 public:
  using PacketPtr = std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>;
  // PTS -> (buffers_ index + keyframe_map_index_base_).
  using KeyframeMap = std::map<int64_t, size_t>;

  // `time_base` is only used for logging/debugging; PTS comparisons inside the
  // range use raw demuxer units end-to-end.
  explicit PacketRange(AVRational time_base);

  // --- Mutators (demuxer thread) ---

  // Precondition: can_append(pkt) is true OR buffers_ is empty.
  void append(PacketPtr pkt);

  // A packet is appendable iff its PTS is within fudge_room() of last_pts_
  // (or the range is empty). Packets from a discontinuity should not be
  // appended; the PacketRing opens a new range instead.
  bool can_append(const AVPacket* pkt) const;

  // --- Queries (main thread) ---

  // Returns iterator into keyframe_map_ or end() if none. O(log K).
  KeyframeMap::const_iterator keyframe_at_or_before(int64_t target_pts) const;
  KeyframeMap::const_iterator keyframe_end() const { return keyframe_map_.end(); }

  // Visit packets in storage order starting at a keyframe_map iterator's
  // buffer index (already adjusted by keyframe_map_index_base_). Visitor
  // receives const AVPacket*. Returns count visited. Stops if visitor
  // returns false.
  std::size_t iterate_from(std::size_t absolute_buffer_index,
                           const std::function<bool(const AVPacket*)>& visit) const;

  bool covers(int64_t pts) const { return !buffers_.empty() && pts >= first_pts_ && pts <= last_pts_; }

  int64_t first_pts() const { return first_pts_; }
  int64_t last_pts() const { return last_pts_; }
  int64_t first_dts() const { return first_dts_; }
  int64_t last_dts() const { return last_dts_; }
  std::size_t byte_size() const { return byte_size_; }
  std::size_t packet_count() const { return buffers_.size(); }
  std::size_t keyframe_count() const { return keyframe_map_.size(); }
  bool empty() const { return buffers_.empty(); }

  // --- Eviction (main thread) ---

  // Delete the oldest GOP (front keyframe to next keyframe exclusive). Refuses
  // (returns 0) if the GOP contains protect_pts. Returns bytes freed.
  std::size_t delete_gop_from_front(int64_t protect_pts);

  // Delete the newest GOP (last keyframe to end). Refuses if protect_pts is in
  // that GOP. Returns bytes freed.
  std::size_t delete_gop_from_back(int64_t protect_pts);

  // Fudge room per Chromium SourceBufferRange: 2 × max_inter_buffer_distance_.
  // Exposed for tests and for PacketRing's adjacency probe.
  int64_t fudge_room() const;

 private:
  std::deque<PacketPtr> buffers_;
  KeyframeMap keyframe_map_;  // PTS -> buffers_[i] absolute index, where
                              // absolute == local_index + keyframe_map_index_base_
  std::size_t keyframe_map_index_base_{0};

  // PTS / DTS span. INT64_MIN while empty.
  int64_t first_pts_{INT64_MIN};
  int64_t last_pts_{INT64_MIN};
  int64_t first_dts_{INT64_MIN};
  int64_t last_dts_{INT64_MIN};

  std::size_t byte_size_{0};

  AVRational time_base_;

  // Rolling max of observed PTS deltas between successive packets; seeds the
  // fudge-room calc. Starts at 0 and grows monotonically as packets arrive.
  int64_t max_inter_buffer_distance_{0};
};

class PacketRing {
 public:
  using PacketPtr = PacketRange::PacketPtr;

  struct Stats {
    std::size_t byte_budget{0};
    std::size_t bytes_used{0};
    std::size_t packet_count{0};
    std::size_t range_count{0};
    std::size_t keyframe_count{0};
    int64_t pts_min{INT64_MIN};
    int64_t pts_max{INT64_MIN};
    bool l1_disabled{false};
  };

  // Result of a cross-range keyframe lookup.
  struct KeyframeHit {
    const PacketRange* range;
    int64_t kf_pts;
    std::size_t absolute_buffer_index;
  };

  PacketRing(std::size_t byte_budget, AVRational demuxer_time_base);

  PacketRing(const PacketRing&) = delete;
  PacketRing& operator=(const PacketRing&) = delete;

  // --- Producer (demuxer thread) ---

  // Takes ownership of `pkt`. If adjacent to tail range, extends it; else
  // opens a new range. If bytes_used exceeds budget after append, does not
  // evict here — eviction is the main thread's responsibility (it needs
  // current_pts to pick what to keep).
  void append(PacketPtr pkt);

  // Call after av_seek_frame succeeds: the next append opens a fresh range
  // rather than trying to extend the old tail.
  void mark_discontinuity();

  // --- Consumer (main thread) ---

  std::optional<KeyframeHit> keyframe_at_or_before(int64_t target_pts) const;
  bool covers(int64_t target_pts) const;

  // --- Eviction (main thread, once per tick) ---

  // Evict whole ranges farthest from current_pts first, then chop GOPs from
  // the far end of the remaining range. Never evicts the GOP containing
  // current_pts. If the budget cannot be met (wedged), stops with one GOP
  // left around current_pts.
  void evict_to_budget(int64_t current_pts);

  void clear();

  Stats stats() const;

  bool l1_disabled() const;

 private:
  // Mutex-free helpers (caller must hold mutex_).
  void append_locked(PacketPtr pkt);
  void evict_locked(int64_t current_pts);
  Stats stats_locked() const;

  // Pick the range farthest from current_pts. Ties broken by oldest range.
  // Returns ranges_.end() if only one range or all ranges contain current_pts.
  std::list<std::unique_ptr<PacketRange>>::iterator farthest_range_locked(int64_t current_pts);

  mutable std::mutex mutex_;
  const AVRational demuxer_time_base_;
  std::size_t byte_budget_;
  std::size_t bytes_used_{0};
  std::list<std::unique_ptr<PacketRange>> ranges_;  // insertion order == temporal demuxer order
  bool next_append_starts_new_range_{true};

  // Tripped when too many consecutive packets have no usable PTS/DTS. L1 is
  // unsafe in that state because keyframe targeting can't be done.
  std::size_t consecutive_unindexable_{0};
  bool l1_disabled_{false};
};
