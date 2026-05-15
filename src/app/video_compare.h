#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "app/config.h"
#include "core/core_types.h"
#include "media/demuxer.h"
#include "display/display.h"
#include "display/subsystems/thumbnail_extractor.h"
#include "media/format_converter.h"
#include "media/buffering/frame_ring.h"
#include "media/buffering/packet_ring.h"
#include "core/data/queue.h"
#include "analysis/scope_manager.h"
#include "core/single_decoder_mode.h"
#include "media/playback/time_shifter.h"
#include "core/time/timer.h"
#include "media/video_decoder.h"
#include "media/video_filterer.h"
extern "C" {
#include <libavcodec/avcodec.h>
}

struct SwsContext;

class ScopeWindow;

using AVPacketUniquePtr = std::unique_ptr<AVPacket, std::function<void(AVPacket*)>>;
using AVFrameSharedPtr = std::shared_ptr<AVFrame>;
using AVFrameUniquePtr = std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>;

using PacketQueue = Queue<AVPacketUniquePtr>;
using DecodedFrameQueue = Queue<AVFrameSharedPtr>;
using FrameQueue = Queue<AVFrameUniquePtr>;

class ReadyToSeek {
 public:
  enum class ProcessorThread { Demultiplexer, Decoder, Filterer, Converter, Count };

  bool get(const ProcessorThread i, const Side& j) const {
    auto it = ready_to_seek_[to_index(i)].find(j);
    if (it != ready_to_seek_[to_index(i)].end()) {
      return load(it->second);
    }
    // If not found, return false (not ready)
    return false;
  }

  void init(const ProcessorThread i, const Side& j) { ready_to_seek_[to_index(i)][j].store(false, std::memory_order_relaxed); }

  void set(const ProcessorThread i, const Side& j) { store(ready_to_seek_[to_index(i)][j], true); }

  void reset_all() {
    for (auto& thread_map : ready_to_seek_) {
      for (auto& pair : thread_map) {
        store(pair.second, false);
      }
    }
  }

  bool all_are_idle() const {
    for (const auto& thread_map : ready_to_seek_) {
      for (const auto& pair : thread_map) {
        if (!load(pair.second)) {
          return false;
        }
      }
    }

    return true;
  }

  // Like all_are_idle(), but only considers sides for which `side_pred(side)` is true.
  template <typename Predicate>
  bool all_are_idle_where(Predicate side_pred) const {
    for (const auto& thread_map : ready_to_seek_) {
      for (const auto& pair : thread_map) {
        if (side_pred(pair.first) && !load(pair.second)) {
          return false;
        }
      }
    }

    return true;
  }

 private:
  static inline bool load(const std::atomic_bool& atomic_flag) { return atomic_flag.load(std::memory_order_relaxed); }

  static inline void store(std::atomic_bool& atomic_flag, const bool value) { atomic_flag.store(value, std::memory_order_relaxed); }

 private:
  static constexpr size_t to_index(const ProcessorThread thread) { return static_cast<size_t>(thread); }
  static constexpr size_t kProcessorThreadCount = static_cast<size_t>(ProcessorThread::Count);

  std::array<std::map<Side, std::atomic_bool>, kProcessorThreadCount> ready_to_seek_;
};

class ExceptionHolder {
 public:
  void store_current_exception() {
    std::unique_lock<std::shared_timed_mutex> lock(mutex_);
    if (!exception_) {
      exception_ = std::current_exception();
    }
  }

  bool has_exception() const {
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);
    return exception_ != nullptr;
  }

  void rethrow_stored_exception() const {
    std::shared_lock<std::shared_timed_mutex> lock(mutex_);
    if (exception_) {
      std::rethrow_exception(exception_);
    }
  }

 private:
  mutable std::shared_timed_mutex mutex_;
  std::exception_ptr exception_{nullptr};
};

struct RightVideoInfo {
  std::string file_name;
  VideoMetadata metadata;
};

// Custom deleter for SwsContext so it can live inside a std::unique_ptr.
// Forward-declaration-friendly: the operator() body is defined in the .cpp
// where sws_freeContext is available.
struct SwsContextDeleter {
  void operator()(SwsContext* ctx) const noexcept;
};

// Owning pointer for a cached SwsContext used by the auto-align fingerprinter.
using SwsContextUniquePtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

// Key identifying a cached fingerprint SwsContext. The tuple captures the
// source format and dimensions; the destination is always GRAY8 at
// kFingerprintSize x kFingerprintSize, so it's not part of the key.
using AutoAlignSwsKey = std::tuple<AVPixelFormat, int, int>;

// A single auto-align candidate: a follower-side PTS (microseconds, AV_TIME_BASE)
// and its precomputed structural fingerprint. Lifetime is decoupled from the
// underlying AVFrame so candidates can survive ring eviction and be reused
// across retry presses.
struct AutoAlignCandidate {
  int64_t pts{0};
  std::vector<float> fp;
};

// Cache carried across consecutive auto-align presses. Lets the user extend
// the search region incrementally — pressing any auto-align key (same or
// different) after a non-seek outcome expands the searched interval rather
// than resetting. Also survives a successful seek so that directional
// iteration (` / [ / ] stepping to equally-strong candidates) doesn't need
// to re-decode the same region. The cache invalidates when the user does
// something that implies different intent: master or follower position
// moved outside what the algorithm itself produced (manual seek, scrub,
// +/- frame shift), or follower-side swap.
struct AutoAlignRetryCache {
  bool valid{false};
  // Guards: positions we saw on both sides when the cache was last populated
  // or last updated post-seek. If either mismatches at the next press it means
  // something outside auto-align moved one of the sides, so the cache is stale.
  int64_t master_pts_at_cache{0};
  int64_t follower_pts_at_cache{0};
  // Follower side that participated. If the user swaps mid-search the cache
  // becomes invalid (the swap changes which side is the follower).
  Side follower_side{SideType::Right};
  // Searched interval on the follower's PTS axis (AV_TIME_BASE µs), absolute
  // (not relative to follower_current). Every candidate in `candidates` has
  // a pts inside [searched_low_pts, searched_high_pts]. The interval grows
  // monotonically across presses until a cache-invalidating event clears it.
  int64_t searched_low_pts{0};
  int64_t searched_high_pts{0};
  // Set when the interval's low/high edge is at the follower clip's start/end
  // boundary and further expansion in that direction can't gain new frames.
  bool low_saturated{false};
  bool high_saturated{false};
  // Set when a packet-ring walk was attempted but couldn't find a starting
  // keyframe in-coverage (distinct from clip-boundary saturation — the clip
  // may extend further, but the packet buffer doesn't hold it).
  bool packet_buffer_miss{false};
  // The candidate pool and master-side probe fingerprints, carried forward so
  // each press only decodes + fingerprints net-new territory.
  std::vector<AutoAlignCandidate> candidates;
  std::unordered_map<int64_t, std::vector<float>> master_probe_fingerprints;
};

// State handed from the auto-align scoring pass to the post-seek verification
// step. Populated when a seek is about to be dispatched; consumed (and cleared)
// after the follower ring's new current frame is in place.
struct PendingAutoAlignVerification {
  bool active{false};
  float expected_score{0.0f};
  int expected_shift_frames{0};
  // Side that participated in the seek. Normally RIGHT; becomes LEFT when
  // the auto-align key was pressed while `swap_left_right_` was active.
  // The rescore looks up the landed frame in this side's FrameRing.
  Side follower_side{SideType::Right};
  int64_t master_current_pts{0};
  int64_t probe_step_pts{0};
  int64_t delta_t_pts{0};
  // Fingerprints of the participating master-side probe frames, keyed by
  // the master frame's PTS. Master didn't move during the seek, so these
  // stay valid.
  std::unordered_map<int64_t, std::vector<float>> master_probe_fingerprints;
};

enum class MediaFrameCardinality { Unknown, SingleFrame, MultiFrame };

struct MediaFrameDetectionState {
  std::atomic<MediaFrameCardinality> cardinality{MediaFrameCardinality::Unknown};
  std::atomic_int decoded_count{0};
  std::atomic<int64_t> last_counted_pts{std::numeric_limits<int64_t>::min()};
};

/**
 * Lock-protected snapshot of per-side playback position, republished once per
 * main-loop iteration by VideoCompare::compare(). Exists because the
 * authoritative SideState struct is a stack-local inside compare() and cannot
 * be read from other threads. External inspectors (e.g. the debug input
 * socket) should call VideoCompare::get_playback_state_snapshot() to obtain a
 * consistent copy.
 */
struct PlaybackStateSnapshot {
  /** Left-side presentation timestamp, AV_TIME_BASE microseconds. */
  int64_t left_pts_us{0};
  /** Right-side presentation timestamp (active right video), AV_TIME_BASE microseconds. */
  int64_t right_pts_us{0};
  /** Right-to-left effective time shift in microseconds (user frame-shift + time-shifter). */
  int64_t effective_time_shift_us{0};
  /** Monotonic decoded-picture counter for left side; -1 if none decoded yet. */
  int32_t left_decoded_picture_number{-1};
  /** Monotonic decoded-picture counter for right side; -1 if none decoded yet. */
  int32_t right_decoded_picture_number{-1};
  /** compare() loop iteration at which this snapshot was published. */
  uint64_t frame_number{0};
  /** True once compare() has published at least one real update (i.e. the
   *  other fields reflect real pipeline state rather than defaults). */
  bool initialized{false};
};

class VideoCompare {
 public:
  VideoCompare(const VideoCompareConfig& config);
  ~VideoCompare();
  void operator()();

  /** Thread-safe copy of the latest playback-position snapshot published by compare(). */
  PlaybackStateSnapshot get_playback_state_snapshot() const;

  /** Absolute path of the left-side video as supplied on the command line. */
  const std::string& get_left_path() const;

  /** Absolute path of the currently-active right-side video (changes as the
   *  user cycles right videos). */
  std::string get_active_right_path() const;

  /** Seconds elapsed since the VideoCompare instance was constructed. */
  double get_uptime_seconds() const;

  /** Per-input keep/skip/toss results formatted as a JSON array string. Each
   *  entry is `{ "path": "...", "action": "keep|skip|toss" }`. Order matches
   *  the CLI input order (left first, then right_videos[0..N-1]). Safe to call
   *  after compare() returns. */
  std::string format_results_json() const;

 private:
  void recreate_format_converter_for_side(const Side& side, const int sws_flags);
  void recreate_format_converters(const int sws_flags);

  bool handle_hdr_state_change();

  void demultiplex(const Side& side);

  void decode_video(const Side& side);
  bool process_packet(const Side& side, AVPacket* packet);

  void filter_video(const Side& side);
  void filter_decoded_frame(const Side& side, AVFrameSharedPtr frame_decoded);

  void format_convert_video(const Side& side);

  bool keep_running() const;
  void quit_all_queues();

  // Enter the pipeline barrier for main-thread-driven per-side operations
  // (L1 re-decode, loop-mode materialize, auto-align candidate build).
  //
  // For each side where should_walk returns true: sets the seeking flag, stops
  // that side's packet queue, drains its downstream queues, and spin-waits
  // until every processor thread for that side has parked at its ReadyToSeek
  // flag. Once idle, the seeking flag is cleared so the main thread can drive
  // the codec without the decode worker's flush/is_seeking race; the queues
  // stay stopped. The corresponding restore (decoder flush, filterer reinit,
  // demuxer reseek, queue restart) varies by caller and is done inline.
  void enter_seek_barrier(const std::function<bool(const Side&)>& should_walk);

  void note_decoded_frame(const Side& side, const int64_t pts);

  void refresh_side_filter_metadata(const Side& side, const std::string& filters);

  bool handle_pending_crop_request(const Side& active_right);

  void dump_debug_info(const int frame_number, const int64_t effective_right_time_shift, const int average_refresh_time);

  void compare();

 private:
  class ScopeUpdateState {
   public:
    ScopeUpdateState() { reset(); }

    struct Sample {
      std::string left_frame_key;
      std::string right_frame_key;
      ScopeWindow::Roi roi;
      bool swapped;

      bool operator==(const Sample& other) const {
        const bool same_frame_keys = left_frame_key == other.left_frame_key && right_frame_key == other.right_frame_key;
        const bool same_roi = roi.x == other.roi.x && roi.y == other.roi.y && roi.w == other.roi.w && roi.h == other.roi.h;
        const bool same_swap = swapped == other.swapped;

        return same_frame_keys && same_roi && same_swap;
      }
    };

    static Sample capture(const AVFrame* left_frame, const AVFrame* right_frame, const ScopeWindow::Roi& roi, const bool swapped) {
      return Sample{
          get_frame_key(left_frame),
          get_frame_key(right_frame),
          roi,
          swapped,
      };
    }

    bool has_changed(const Sample& sample) const { return (!initialized_) || !(sample == state_); }

    void update(const Sample& sample) {
      state_ = sample;
      initialized_ = true;
    }

    void reset() { initialized_ = false; }

   private:
    Sample state_;
    bool initialized_{false};
  };

  const VideoCompareConfig& config_;
  const bool same_decoded_video_both_sides_;
  bool hdr_passthrough_active_{false};

  const Display::Loop auto_loop_mode_;
  const size_t frame_buffer_size_;
  const size_t packet_buffer_bytes_;
  TimeShifter time_shifter_;

  std::map<Side, std::unique_ptr<Demuxer>> demuxers_;
  std::map<Side, std::unique_ptr<VideoDecoder>> video_decoders_;
  std::map<Side, std::unique_ptr<VideoFilterer>> video_filterers_;
  std::map<Side, std::unique_ptr<FormatConverter>> format_converters_;

  VideoFilterContext video_filter_context_;

  std::map<Side, std::unique_ptr<PacketQueue>> packet_queues_;
  std::map<Side, std::unique_ptr<PacketRing>> packet_rings_;
  std::map<Side, std::shared_ptr<DecodedFrameQueue>> decoded_frame_queues_;
  std::map<Side, std::unique_ptr<FrameQueue>> filtered_frame_queues_;
  std::map<Side, std::unique_ptr<FrameQueue>> converted_frame_queues_;

  std::map<Side, std::vector<SDL_Rect>> crop_history_;

  size_t max_width_;
  size_t max_height_;
  double shortest_duration_;

  std::unique_ptr<Display> display_;
  std::unique_ptr<Timer> timer_;

  // Background thumbnail loader for the bottom-of-window dock. Spawned at
  // the end of the constructor (after dock entries are registered) and
  // joined on destruction. Lives as a unique_ptr so destruction order is
  // explicit — cancel-and-join before the Dock's mutex goes away.
  std::unique_ptr<ThumbnailLoader> thumbnail_loader_;

  size_t active_right_index_{0};
  std::map<Side, RightVideoInfo> right_video_info_;
  VideoMetadata left_video_metadata_;

  std::map<Side, MediaFrameDetectionState> media_frame_detection_states_;

  std::unique_ptr<ScopeManager> scope_manager_;
  ScopeUpdateState scope_update_state_;

  std::vector<std::thread> stages_;

  ExceptionHolder exception_holder_;

  // Per-side "this side's pipeline is currently seeking". Workers check their own
  // side so that a pure right-side frame shift doesn't disturb the left pipeline.
  std::map<Side, std::atomic_bool> seeking_per_side_;

  bool is_seeking(const Side& side) const {
    auto it = seeking_per_side_.find(side);
    return it != seeking_per_side_.end() && it->second.load(std::memory_order_relaxed);
  }

  bool any_seeking() const {
    for (const auto& pair : seeking_per_side_) {
      if (pair.second.load(std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  SingleDecoderMode single_decoder_mode_;
  ReadyToSeek ready_to_seek_;

  // Auto-align: SwsContext cache used to downscale frames to fingerprint size.
  // Keyed by source (format, width, height); output is always GRAY8 at
  // MetricsCalculator::kFingerprintSize. Created lazily on first press.
  std::map<AutoAlignSwsKey, SwsContextUniquePtr> auto_align_sws_cache_;

  // Auto-align: verification state carried across the seek dispatch.
  PendingAutoAlignVerification pending_auto_align_verification_;

  // Auto-align: cache carried across consecutive same-mode presses that
  // returned "low confidence". Each retry extends the search window by the
  // mode's base width without redoing fingerprint work for the previous span.
  // Invalidated the moment anything else happens (see can_reuse check in
  // compare()).
  AutoAlignRetryCache auto_align_retry_cache_;

  // Thread-safe playback-position snapshot. Written by compare() near the end
  // of each iteration under playback_state_snapshot_mutex_; read by external
  // inspectors (e.g. the debug input socket) via get_playback_state_snapshot().
  mutable std::mutex playback_state_snapshot_mutex_;
  PlaybackStateSnapshot playback_state_snapshot_;

  // Wall-clock origin for get_uptime_seconds(). Captured in the constructor
  // initializer list so "uptime" measures from object creation, not from the
  // moment the main loop starts.
  const std::chrono::steady_clock::time_point start_time_{std::chrono::steady_clock::now()};
};
