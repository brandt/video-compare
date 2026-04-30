#pragma once
#include <atomic>
#include <mutex>
#include "core/core_types.h"
#include "media/demuxer.h"
#include "core/side_aware.h"
#include "media/video_decoder.h"
#include "media/video_filter_context.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

struct CropRect {
  int x{0};
  int y{0};
  int w{0};
  int h{0};
};

class VideoFilterer : public SideAware {
 public:
  VideoFilterer(const Side& side,
                const Demuxer* demuxer,
                const VideoDecoder* video_decoder,
                const ToneMapping tone_mapping_mode,
                const float boost_tone,
                const std::string& custom_video_filters,
                const std::string& custom_color_space,
                const std::string& custom_color_range,
                const std::string& custom_color_primaries,
                const std::string& custom_color_trc,
                const VideoFilterContext* video_filter_context,
                const bool disable_auto_filters,
                const AVPixelFormat output_pixel_format = AV_PIX_FMT_NONE,
                const bool hdr_passthrough = false,
                const bool gpu_color_processing = false);
  ~VideoFilterer();

  void init();
  void free();
  void reinit();

  void close_src();

  bool send(AVFrame* decoded_frame);
  bool receive(AVFrame* filtered_frame);

  static std::string get_resolved_filters_from_frame(const AVFrame* frame);
  static int get_filter_generation_from_frame(const AVFrame* frame);

  std::string filter_description() const;
  std::string resolved_filter_description() const;

  size_t src_width() const;
  size_t src_height() const;
  AVPixelFormat src_pixel_format() const;
  size_t dest_width() const;
  size_t dest_height() const;
  AVPixelFormat dest_pixel_format() const;

  bool set_crop_rect(const CropRect* rect);
  bool consume_filter_change();

  // Non-consuming peek at the pending filter-change flag. Used by L1 eligibility
  // to skip re-decode when a filter rebuild is required; those cases fall
  // through to the full seek path which calls consume_filter_change() + reinit().
  bool has_pending_filter_change() const { return filter_changed_.load(std::memory_order_relaxed); }

 private:
  struct CropSnapshot {
    CropRect rect{};
    bool enabled{false};
  };

  int init_filters();

  void mark_filter_changed();

  const Demuxer* demuxer_;
  const VideoDecoder* video_decoder_;
  const ToneMapping tone_mapping_mode_;
  const AVPixelFormat output_pixel_format_;
  const bool hdr_passthrough_;
  const bool gpu_color_processing_;

  std::string pre_filter_description_;
  std::string post_filter_description_;

  int width_;
  int height_;
  AVPixelFormat pixel_format_;
  AVColorSpace color_space_;
  AVColorRange color_range_;
  AVRational sample_aspect_ratio_;
  AVRational time_base_;

  AVFilterContext* buffersrc_ctx_;
  AVFilterContext* buffersink_ctx_;
  AVFilterGraph* filter_graph_;

  DynamicRange dynamic_range_;
  unsigned peak_luminance_nits_;

  CropSnapshot crop_{};
  CropSnapshot pending_crop_{};
  mutable std::mutex pending_crop_mutex_;

  std::atomic_bool filter_changed_{false};
  std::atomic<int> filter_generation_{0};

  // True when the filter chain bakes the source's displaymatrix rotation
  // into pixels (via hflip/vflip/transpose/rotate). The filtered frame's
  // AV_FRAME_DATA_DISPLAYMATRIX is then stripped at the buffersink so a
  // downstream consumer that auto-applies it (e.g. libplacebo's GPU
  // renderer reading side data on map) does not double-rotate.
  bool rotation_baked_{false};
};
