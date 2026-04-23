#pragma once
#include <memory>
#include <string>
extern "C" {
#include <libavutil/frame.h>
}

class FormatConverter;

// Lazily materialize packed RGB copies of native YUV frames for features that
// still need CPU pixel access in GPU mode (subtraction, per-pixel inspector,
// live PSNR/SSIM/VMAF, save-selected-area). Caches per frame_key so multiple
// consumers in the same refresh share one conversion, and reuses allocations
// when video dimensions and bit-depth are unchanged.
class RgbFrameCache {
 public:
  RgbFrameCache();
  ~RgbFrameCache();

  RgbFrameCache(const RgbFrameCache&) = delete;
  RgbFrameCache& operator=(const RgbFrameCache&) = delete;

  // Ensure both sides are cached and match the requested dimensions / bit-depth.
  // Reallocates if any differ from the last call; otherwise reuses and only
  // re-runs the converter when frame_key changed. Returns true when both sides
  // are ready; callers must gate RGB-dependent work on this.
  bool ensure(const AVFrame* left_frame, const AVFrame* right_frame,
              int video_width, int video_height, bool is_10bpc);

  AVFrame* left() const { return frames_[0]; }
  AVFrame* right() const { return frames_[1]; }
  AVFrame* get(int side) const { return frames_[side]; }

  // Drop all cached state (e.g. on reinitialize_video_dimensions). The next
  // ensure() call will rebuild from scratch.
  void invalidate();

 private:
  static constexpr int kSideCount = 2;

  bool ensure_side(int side, const AVFrame* src, int video_width, int video_height, bool is_10bpc);

  std::unique_ptr<FormatConverter> converters_[kSideCount];
  AVFrame* frames_[kSideCount]{};
  std::string frame_keys_[kSideCount];
};
