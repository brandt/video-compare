#include "media/buffering/packet_ring.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace {

// Treat AV_NOPTS_VALUE as noval; otherwise take pkt->pts, falling back to
// pkt->dts so the range still tracks timestamps for codecs that only emit DTS.
// Returns INT64_MIN if both are noval.
int64_t effective_pts(const AVPacket* pkt) {
  if (pkt->pts != AV_NOPTS_VALUE) {
    return pkt->pts;
  }
  if (pkt->dts != AV_NOPTS_VALUE) {
    return pkt->dts;
  }
  return INT64_MIN;
}

int64_t effective_dts(const AVPacket* pkt) {
  if (pkt->dts != AV_NOPTS_VALUE) {
    return pkt->dts;
  }
  if (pkt->pts != AV_NOPTS_VALUE) {
    return pkt->pts;
  }
  return INT64_MIN;
}

std::size_t packet_byte_size(const AVPacket* pkt) {
  // Ignore side-data bytes; the caller's budget intent is about encoded-frame
  // bulk, and side data is small and bounded.
  return pkt ? static_cast<std::size_t>(std::max(pkt->size, 0)) : 0;
}

constexpr std::size_t kUnindexableLimit = 32;

}  // namespace

// ---------------- PacketRange ----------------

PacketRange::PacketRange(AVRational time_base) : time_base_(time_base) {}

bool PacketRange::can_append(const AVPacket* pkt) const {
  if (buffers_.empty()) {
    return true;
  }
  const int64_t pts = effective_pts(pkt);
  if (pts == INT64_MIN) {
    // Unindexable — accept it; the range's PTS bounds are frozen by the
    // indexable packet that preceded it. A subsequent indexable packet will
    // resume extending the bounds.
    return true;
  }
  // B-frame reorder means the next packet's PTS can be strictly less than the
  // current last_pts_; that's normal within a GOP. Only reject if PTS falls
  // before the range's FIRST pts (truly out of range) or overshoots by more
  // than the fudge room.
  if (pts < first_pts_ - fudge_room()) {
    return false;
  }
  if (pts > last_pts_ + fudge_room()) {
    return false;
  }
  return true;
}

int64_t PacketRange::fudge_room() const {
  // Mirror Chromium: 2 × max observed inter-buffer distance. Before we have
  // two indexable packets, we don't yet know the typical packet spacing, so
  // use a generous time_base-derived floor: 1/2 second. This is large enough
  // to forgive any realistic inter-frame gap but small enough that a true
  // discontinuity (post-seek gap) still opens a new range.
  //
  // half_second_in_tb = time_base.den / (2 * time_base.num).
  // For common time bases (1/90000, 1/24000, 1/1000, 1/600) this yields
  // 45000, 12000, 500, 300 ticks respectively.
  const int64_t observed = 2 * max_inter_buffer_distance_;
  int64_t floor_ticks = 1;
  if (time_base_.num > 0 && time_base_.den > 0) {
    floor_ticks = static_cast<int64_t>(time_base_.den) / (2 * static_cast<int64_t>(time_base_.num));
    if (floor_ticks < 1) floor_ticks = 1;
  }
  return std::max(observed, floor_ticks);
}

void PacketRange::append(PacketPtr pkt) {
  AVPacket* raw = pkt.get();
  const int64_t pts = effective_pts(raw);
  const int64_t dts = effective_dts(raw);
  const std::size_t bytes = packet_byte_size(raw);
  const bool is_keyframe = (raw->flags & AV_PKT_FLAG_KEY) != 0;

  if (buffers_.empty()) {
    first_pts_ = pts;
    last_pts_ = pts;
    first_dts_ = dts;
    last_dts_ = dts;
  } else {
    if (pts != INT64_MIN) {
      if (last_pts_ != INT64_MIN) {
        const int64_t delta = pts - last_pts_;
        if (delta > 0) {
          max_inter_buffer_distance_ = std::max(max_inter_buffer_distance_, delta);
        }
      }
      last_pts_ = std::max(last_pts_, pts);
      if (first_pts_ == INT64_MIN) {
        first_pts_ = pts;
      }
    }
    if (dts != INT64_MIN) {
      last_dts_ = std::max(last_dts_, dts);
      if (first_dts_ == INT64_MIN) {
        first_dts_ = dts;
      }
    }
  }

  const std::size_t absolute_index = buffers_.size() + keyframe_map_index_base_;
  if (is_keyframe && pts != INT64_MIN) {
    keyframe_map_.emplace(pts, absolute_index);
  }

  buffers_.push_back(std::move(pkt));
  byte_size_ += bytes;
}

PacketRange::KeyframeMap::const_iterator PacketRange::keyframe_at_or_before(int64_t target_pts) const {
  if (keyframe_map_.empty()) {
    return keyframe_map_.end();
  }
  // upper_bound gives first entry > target; decrement to get last entry <= target.
  auto it = keyframe_map_.upper_bound(target_pts);
  if (it == keyframe_map_.begin()) {
    return keyframe_map_.end();  // all entries are after target
  }
  --it;
  return it;
}

std::size_t PacketRange::iterate_from(std::size_t absolute_buffer_index, const std::function<bool(const AVPacket*)>& visit) const {
  if (absolute_buffer_index < keyframe_map_index_base_) {
    return 0;
  }
  const std::size_t local = absolute_buffer_index - keyframe_map_index_base_;
  if (local >= buffers_.size()) {
    return 0;
  }
  std::size_t visited = 0;
  for (std::size_t i = local; i < buffers_.size(); ++i) {
    ++visited;
    if (!visit(buffers_[i].get())) {
      break;
    }
  }
  return visited;
}

std::size_t PacketRange::delete_gop_from_front(int64_t protect_pts) {
  if (buffers_.empty() || keyframe_map_.empty()) {
    return 0;
  }
  // GOP runs from keyframe_map_.begin() to the next keyframe (exclusive), or
  // to buffers_.end() if no second keyframe.
  auto first_kf = keyframe_map_.begin();
  const std::size_t first_kf_abs = first_kf->second;
  const int64_t first_kf_pts = first_kf->first;

  auto second_kf = std::next(first_kf);
  const bool have_second = (second_kf != keyframe_map_.end());
  const std::size_t gop_end_abs = have_second ? second_kf->second : (buffers_.size() + keyframe_map_index_base_);

  // If protect_pts lies inside this GOP, refuse.
  if (protect_pts >= first_kf_pts && (!have_second || protect_pts < second_kf->first)) {
    return 0;
  }

  // Translate to local indices.
  assert(first_kf_abs >= keyframe_map_index_base_);
  const std::size_t local_start = first_kf_abs - keyframe_map_index_base_;
  const std::size_t local_end = gop_end_abs - keyframe_map_index_base_;
  if (local_start != 0) {
    // Shouldn't happen in normal operation: we always evict from front, so
    // the first keyframe is at local 0. Guard anyway.
    return 0;
  }

  std::size_t bytes_freed = 0;
  for (std::size_t i = local_start; i < local_end; ++i) {
    bytes_freed += packet_byte_size(buffers_[i].get());
  }

  buffers_.erase(buffers_.begin() + local_start, buffers_.begin() + local_end);
  byte_size_ -= bytes_freed;
  keyframe_map_.erase(first_kf);
  keyframe_map_index_base_ += (local_end - local_start);

  if (buffers_.empty()) {
    first_pts_ = INT64_MIN;
    last_pts_ = INT64_MIN;
    first_dts_ = INT64_MIN;
    last_dts_ = INT64_MIN;
  } else if (!keyframe_map_.empty()) {
    first_pts_ = keyframe_map_.begin()->first;
  } else {
    // Only non-keyframe packets remain — uncommon, but normalize first_pts_
    // to the earliest packet's effective PTS if available.
    first_pts_ = effective_pts(buffers_.front().get());
    if (first_pts_ == INT64_MIN) {
      first_pts_ = last_pts_;
    }
  }
  return bytes_freed;
}

std::size_t PacketRange::delete_gop_from_back(int64_t protect_pts) {
  if (buffers_.empty() || keyframe_map_.empty()) {
    return 0;
  }
  auto last_kf = std::prev(keyframe_map_.end());
  const std::size_t last_kf_abs = last_kf->second;
  const int64_t last_kf_pts = last_kf->first;

  if (protect_pts >= last_kf_pts && protect_pts <= last_pts_) {
    return 0;
  }

  const std::size_t local_start = last_kf_abs - keyframe_map_index_base_;
  const std::size_t local_end = buffers_.size();
  if (local_start >= local_end) {
    return 0;
  }

  std::size_t bytes_freed = 0;
  for (std::size_t i = local_start; i < local_end; ++i) {
    bytes_freed += packet_byte_size(buffers_[i].get());
  }
  buffers_.erase(buffers_.begin() + local_start, buffers_.end());
  byte_size_ -= bytes_freed;
  keyframe_map_.erase(last_kf);

  if (buffers_.empty()) {
    first_pts_ = INT64_MIN;
    last_pts_ = INT64_MIN;
    first_dts_ = INT64_MIN;
    last_dts_ = INT64_MIN;
  } else {
    // last_pts_ / last_dts_ shrink to whatever the new tail packet carries.
    int64_t new_last_pts = INT64_MIN;
    int64_t new_last_dts = INT64_MIN;
    // Conservative: scan the remaining tail in reverse for the first usable
    // PTS; buffers_ is small here (single GOP worth typically).
    for (auto it = buffers_.rbegin(); it != buffers_.rend(); ++it) {
      const int64_t p = effective_pts(it->get());
      const int64_t d = effective_dts(it->get());
      if (p != INT64_MIN) new_last_pts = std::max(new_last_pts, p);
      if (d != INT64_MIN) new_last_dts = std::max(new_last_dts, d);
      if (new_last_pts != INT64_MIN && new_last_dts != INT64_MIN) break;
    }
    if (new_last_pts != INT64_MIN) last_pts_ = new_last_pts;
    if (new_last_dts != INT64_MIN) last_dts_ = new_last_dts;
  }
  return bytes_freed;
}

// ---------------- PacketRing ----------------

PacketRing::PacketRing(std::size_t byte_budget, AVRational demuxer_time_base) : demuxer_time_base_(demuxer_time_base), byte_budget_(byte_budget) {}

void PacketRing::append(PacketPtr pkt) {
  std::lock_guard<std::mutex> lock(mutex_);
  append_locked(std::move(pkt));
}

void PacketRing::append_locked(PacketPtr pkt) {
  const AVPacket* raw = pkt.get();
  const int64_t pts = effective_pts(raw);
  const int64_t dts = effective_dts(raw);
  const std::size_t bytes = packet_byte_size(raw);

  if (pts == INT64_MIN && dts == INT64_MIN) {
    if (++consecutive_unindexable_ >= kUnindexableLimit) {
      l1_disabled_ = true;
    }
  } else {
    consecutive_unindexable_ = 0;
  }

  PacketRange* tail = ranges_.empty() ? nullptr : ranges_.back().get();
  const bool need_new_range = next_append_starts_new_range_ || tail == nullptr || !tail->can_append(raw);

  if (need_new_range) {
    ranges_.emplace_back(std::make_unique<PacketRange>(demuxer_time_base_));
    tail = ranges_.back().get();
    next_append_starts_new_range_ = false;
  }

  tail->append(std::move(pkt));
  bytes_used_ += bytes;
}

void PacketRing::mark_discontinuity() {
  std::lock_guard<std::mutex> lock(mutex_);
  next_append_starts_new_range_ = true;
}

std::optional<PacketRing::KeyframeHit> PacketRing::keyframe_at_or_before(int64_t target_pts) const {
  std::lock_guard<std::mutex> lock(mutex_);

  const PacketRange* best_range = nullptr;
  int64_t best_kf_pts = INT64_MIN;
  std::size_t best_absolute_index = 0;

  for (const auto& range_uptr : ranges_) {
    const PacketRange* range = range_uptr.get();
    if (range->empty() || range->first_pts() > target_pts) {
      continue;
    }
    auto it = range->keyframe_at_or_before(target_pts);
    if (it == range->keyframe_end()) {
      continue;
    }
    if (it->first > best_kf_pts) {
      best_range = range;
      best_kf_pts = it->first;
      best_absolute_index = it->second;
    }
  }

  if (!best_range) {
    return std::nullopt;
  }
  return KeyframeHit{best_range, best_kf_pts, best_absolute_index};
}

bool PacketRing::covers(int64_t target_pts) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& range_uptr : ranges_) {
    if (range_uptr->covers(target_pts)) {
      return true;
    }
  }
  return false;
}

std::list<std::unique_ptr<PacketRange>>::iterator PacketRing::farthest_range_locked(int64_t current_pts) {
  if (ranges_.size() <= 1) {
    return ranges_.end();
  }
  auto farthest = ranges_.end();
  int64_t farthest_distance = -1;
  for (auto it = ranges_.begin(); it != ranges_.end(); ++it) {
    const PacketRange* r = it->get();
    if (r->empty()) {
      return it;  // immediate candidate: empty range can go
    }
    if (r->covers(current_pts)) {
      continue;  // never evict current range
    }
    // Distance from current_pts to the nearer end of this range.
    const int64_t d_front = std::llabs(current_pts - r->first_pts());
    const int64_t d_back = std::llabs(current_pts - r->last_pts());
    const int64_t d = std::min(d_front, d_back);
    if (d > farthest_distance) {
      farthest_distance = d;
      farthest = it;
    }
  }
  return farthest;
}

void PacketRing::evict_to_budget(int64_t current_pts) {
  std::lock_guard<std::mutex> lock(mutex_);
  evict_locked(current_pts);
}

void PacketRing::evict_locked(int64_t current_pts) {
  // Phase 1: drop whole ranges that don't contain current_pts, farthest first.
  while (bytes_used_ > byte_budget_ && ranges_.size() > 1) {
    auto victim = farthest_range_locked(current_pts);
    if (victim == ranges_.end()) {
      break;
    }
    bytes_used_ -= (*victim)->byte_size();
    ranges_.erase(victim);
  }

  // Phase 2: single (or current-containing) range left. Chop GOPs from the
  // far end. Never evict the GOP containing current_pts.
  while (bytes_used_ > byte_budget_ && !ranges_.empty()) {
    // Find the range containing current_pts; operate on it. If current_pts
    // lies outside all ranges (rare; right after a seek before intake ran),
    // operate on the last range.
    PacketRange* target = nullptr;
    for (auto& rup : ranges_) {
      if (rup->covers(current_pts)) {
        target = rup.get();
        break;
      }
    }
    if (!target) {
      target = ranges_.back().get();
    }
    if (target->empty() || target->keyframe_count() < 2) {
      // Wedged: fewer than two keyframes means we can't drop a GOP without
      // potentially losing the current one.
      break;
    }
    const int64_t dist_front = std::llabs(current_pts - target->first_pts());
    const int64_t dist_back = std::llabs(current_pts - target->last_pts());
    std::size_t freed = 0;
    if (dist_front > dist_back) {
      freed = target->delete_gop_from_front(current_pts);
    } else {
      freed = target->delete_gop_from_back(current_pts);
    }
    if (freed == 0) {
      // The far-end GOP was protected; try the other end once.
      freed = (dist_front > dist_back) ? target->delete_gop_from_back(current_pts) : target->delete_gop_from_front(current_pts);
      if (freed == 0) {
        break;  // truly wedged
      }
    }
    bytes_used_ -= freed;
  }
}

void PacketRing::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  ranges_.clear();
  bytes_used_ = 0;
  next_append_starts_new_range_ = true;
  consecutive_unindexable_ = 0;
  // Leave l1_disabled_ sticky — if we've seen bad inputs once, don't let a
  // clear() forget that.
}

PacketRing::Stats PacketRing::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_locked();
}

PacketRing::Stats PacketRing::stats_locked() const {
  Stats s;
  s.byte_budget = byte_budget_;
  s.bytes_used = bytes_used_;
  s.range_count = ranges_.size();
  s.l1_disabled = l1_disabled_;
  for (const auto& r : ranges_) {
    s.packet_count += r->packet_count();
    s.keyframe_count += r->keyframe_count();
    if (r->empty()) continue;
    if (s.pts_min == INT64_MIN || r->first_pts() < s.pts_min) s.pts_min = r->first_pts();
    if (s.pts_max == INT64_MIN || r->last_pts() > s.pts_max) s.pts_max = r->last_pts();
  }
  return s;
}

bool PacketRing::l1_disabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return l1_disabled_;
}
