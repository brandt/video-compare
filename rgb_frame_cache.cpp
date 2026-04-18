#include "rgb_frame_cache.h"
#include "core_types.h"
#include "format_converter.h"
extern "C" {
#include <libavutil/imgutils.h>
}

RgbFrameCache::RgbFrameCache() = default;

RgbFrameCache::~RgbFrameCache() {
  invalidate();
}

void RgbFrameCache::invalidate() {
  for (int s = 0; s < kSideCount; ++s) {
    if (frames_[s] != nullptr) {
      av_freep(&frames_[s]->data[0]);
      av_frame_free(&frames_[s]);
    }
    converters_[s].reset();
    frame_keys_[s].clear();
  }
}

bool RgbFrameCache::ensure(const AVFrame* left_frame, const AVFrame* right_frame,
                           const int video_width, const int video_height, const bool is_10bpc) {
  const bool ok_left = ensure_side(0, left_frame, video_width, video_height, is_10bpc);
  const bool ok_right = ensure_side(1, right_frame, video_width, video_height, is_10bpc);
  return ok_left && ok_right;
}

bool RgbFrameCache::ensure_side(const int side, const AVFrame* src,
                                 const int video_width, const int video_height, const bool is_10bpc) {
  if (!src || src->data[0] == nullptr || src->width <= 0 || src->height <= 0) return false;

  // Target format matches what CPU pixel helpers expect based on is_10bpc:
  // RGB48LE (10-bit) or RGB24 (8-bit).
  const AVPixelFormat dst_fmt = is_10bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;

  const std::string new_key = get_frame_key(src);
  if (!new_key.empty() && new_key == frame_keys_[side] && frames_[side] != nullptr) {
    return true;
  }

  // Allocate or (re)allocate the destination RGB frame if size/format changed.
  if (frames_[side] == nullptr || frames_[side]->width != video_width ||
      frames_[side]->height != video_height || frames_[side]->format != dst_fmt) {
    if (frames_[side] != nullptr) {
      av_freep(&frames_[side]->data[0]);
      av_frame_free(&frames_[side]);
    }
    AVFrame* fr = av_frame_alloc();
    if (!fr) return false;
    fr->format = dst_fmt;
    fr->width = video_width;
    fr->height = video_height;
    if (av_image_alloc(fr->data, fr->linesize, video_width, video_height, dst_fmt, 64) < 0) {
      av_frame_free(&fr);
      return false;
    }
    frames_[side] = fr;
    // Force converter rebuild on the next conversion.
    converters_[side].reset();
  }

  // Lazily construct / reuse the per-side FormatConverter (handles format
  // changes itself via reinit on operator()). Initial params are seeded from
  // the first frame we see.
  if (!converters_[side]) {
    converters_[side] = std::make_unique<FormatConverter>(
        src->width, src->height, video_width, video_height,
        static_cast<AVPixelFormat>(src->format), dst_fmt,
        src->colorspace, src->color_range);
  }

  // Copy color props and invoke the converter. The converter sets frame_key
  // metadata on dst from src automatically. color_primaries and color_trc
  // are propagated so that downstream savers (JxlSaver) can preserve HDR
  // metadata on saved output.
  frames_[side]->colorspace = src->colorspace;
  frames_[side]->color_range = src->color_range;
  frames_[side]->color_primaries = src->color_primaries;
  frames_[side]->color_trc = src->color_trc;
  (*converters_[side])(const_cast<AVFrame*>(src), frames_[side]);
  frame_keys_[side] = new_key;
  return true;
}
