#include "display/subsystems/thumbnail_extractor.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace {

// RAII wrappers for the libav resources used by extract_thumbnail.
struct AVFormatCtxDeleter {
  void operator()(AVFormatContext* ctx) const { if (ctx) avformat_close_input(&ctx); }
};
struct AVCodecCtxDeleter {
  void operator()(AVCodecContext* ctx) const { if (ctx) avcodec_free_context(&ctx); }
};
struct AVFrameDeleter {
  void operator()(AVFrame* f) const { if (f) av_frame_free(&f); }
};
struct AVPacketDeleter {
  void operator()(AVPacket* p) const { if (p) av_packet_free(&p); }
};
struct SwsCtxDeleter {
  void operator()(SwsContext* s) const { if (s) sws_freeContext(s); }
};

using AVFormatCtxPtr = std::unique_ptr<AVFormatContext, AVFormatCtxDeleter>;
using AVCodecCtxPtr = std::unique_ptr<AVCodecContext, AVCodecCtxDeleter>;
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
using SwsCtxPtr = std::unique_ptr<SwsContext, SwsCtxDeleter>;

// Test whether the frame is a keyframe in a way that's portable across
// FFmpeg versions (the bit-flag form replaced the deprecated `key_frame` int
// around FFmpeg 6.0).
inline bool frame_is_key(const AVFrame* f) {
#if defined(AV_FRAME_FLAG_KEY)
  return (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
  return f->key_frame != 0;
#endif
}

}  // namespace

DockBitmap extract_thumbnail(const std::string& path, int max_width, int max_height) {
  if (path.empty() || max_width <= 0 || max_height <= 0) {
    return {};
  }

  AVFormatContext* fmt_raw = nullptr;
  if (avformat_open_input(&fmt_raw, path.c_str(), nullptr, nullptr) < 0) {
    std::cerr << "[thumbnail] avformat_open_input failed for: " << path << std::endl;
    return {};
  }
  AVFormatCtxPtr fmt(fmt_raw);

  if (avformat_find_stream_info(fmt.get(), nullptr) < 0) {
    std::cerr << "[thumbnail] avformat_find_stream_info failed for: " << path << std::endl;
    return {};
  }

  // Find the first video stream.
  int video_stream_index = -1;
  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index = static_cast<int>(i);
      break;
    }
  }
  if (video_stream_index < 0) {
    std::cerr << "[thumbnail] no video stream in: " << path << std::endl;
    return {};
  }
  AVStream* stream = fmt->streams[video_stream_index];

  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    std::cerr << "[thumbnail] no decoder available for: " << path << std::endl;
    return {};
  }
  AVCodecCtxPtr codec_ctx(avcodec_alloc_context3(codec));
  if (codec_ctx == nullptr) {
    return {};
  }
  if (avcodec_parameters_to_context(codec_ctx.get(), stream->codecpar) < 0) {
    return {};
  }
  // Single thread is plenty for one frame.
  codec_ctx->thread_count = 1;
  if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
    std::cerr << "[thumbnail] avcodec_open2 failed for: " << path << std::endl;
    return {};
  }

  // Seek to ~10% of the container duration. AV_SEEK_FLAG_BACKWARD lands us at
  // the keyframe at-or-before the requested timestamp, which is exactly what
  // we want for fast keyframe-based thumbnails.
  const int64_t total_duration = fmt->duration > 0 ? fmt->duration : 0;
  const int64_t target_us = static_cast<int64_t>(total_duration * 0.10);
  if (total_duration > 0) {
    av_seek_frame(fmt.get(), -1, target_us, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(codec_ctx.get());
  }

  AVPacketPtr packet(av_packet_alloc());
  AVFramePtr frame(av_frame_alloc());
  if (packet == nullptr || frame == nullptr) {
    return {};
  }

  // Decode up to ~120 packets looking for the first usable frame after the
  // seek. We prefer a keyframe but accept any decoded frame as a fallback so
  // sources whose first post-seek frame isn't flagged still produce a thumb.
  AVFramePtr fallback_frame;
  int packets_examined = 0;
  while (packets_examined < 240) {
    const int read_ret = av_read_frame(fmt.get(), packet.get());
    if (read_ret == AVERROR_EOF) {
      // Flush the decoder.
      avcodec_send_packet(codec_ctx.get(), nullptr);
    } else if (read_ret < 0) {
      break;
    } else {
      ++packets_examined;
      if (packet->stream_index != video_stream_index) {
        av_packet_unref(packet.get());
        continue;
      }
      const int send_ret = avcodec_send_packet(codec_ctx.get(), packet.get());
      av_packet_unref(packet.get());
      if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
        break;
      }
    }

    while (true) {
      const int recv_ret = avcodec_receive_frame(codec_ctx.get(), frame.get());
      if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
        break;
      }
      if (recv_ret < 0) {
        return {};
      }
      if (frame_is_key(frame.get())) {
        goto have_frame;
      }
      if (fallback_frame == nullptr) {
        // Stash a copy as fallback. av_frame_clone bumps refs on the buffers.
        fallback_frame.reset(av_frame_clone(frame.get()));
      }
      av_frame_unref(frame.get());
    }

    if (read_ret == AVERROR_EOF) {
      break;
    }
  }

  if (fallback_frame != nullptr) {
    frame = std::move(fallback_frame);
  } else {
    std::cerr << "[thumbnail] no frame decoded from: " << path << std::endl;
    return {};
  }

have_frame:
  // Resolve target dimensions preserving source aspect ratio. Pixel-aspect
  // (SAR) is folded in if non-trivial so anamorphic sources don't render
  // squashed.
  const int src_w = frame->width;
  const int src_h = frame->height;
  if (src_w <= 0 || src_h <= 0) {
    return {};
  }
  double effective_w = static_cast<double>(src_w);
  if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0) {
    effective_w *= static_cast<double>(frame->sample_aspect_ratio.num) /
                   static_cast<double>(frame->sample_aspect_ratio.den);
  }
  const double src_aspect = effective_w / static_cast<double>(src_h);
  int out_w = max_width;
  int out_h = static_cast<int>(std::round(static_cast<double>(out_w) / src_aspect));
  if (out_h > max_height) {
    out_h = max_height;
    out_w = static_cast<int>(std::round(static_cast<double>(out_h) * src_aspect));
  }
  out_w = std::max(8, out_w);
  out_h = std::max(8, out_h);

  SwsCtxPtr sws(sws_getContext(
      src_w, src_h, static_cast<AVPixelFormat>(frame->format),
      out_w, out_h, AV_PIX_FMT_RGBA,
      SWS_BICUBIC, nullptr, nullptr, nullptr));
  if (sws == nullptr) {
    std::cerr << "[thumbnail] sws_getContext failed for: " << path << std::endl;
    return {};
  }

  DockBitmap out;
  out.width = out_w;
  out.height = out_h;
  out.rgba.assign(static_cast<size_t>(out_w) * out_h * 4, 0);

  uint8_t* dst_planes[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
  int dst_strides[4] = {out_w * 4, 0, 0, 0};
  sws_scale(sws.get(), frame->data, frame->linesize, 0, src_h, dst_planes, dst_strides);
  return out;
}

// ---------------------------------------------------------------------------
// ThumbnailLoader

ThumbnailLoader::ThumbnailLoader(std::vector<std::string> paths,
                                 int max_width, int max_height,
                                 ReadyCallback on_ready) {
  worker_ = std::thread([this, paths = std::move(paths), max_width, max_height,
                          on_ready = std::move(on_ready)]() {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (cancel_.load(std::memory_order_relaxed)) {
        return;
      }
      DockBitmap bitmap = extract_thumbnail(paths[i], max_width, max_height);
      if (cancel_.load(std::memory_order_relaxed)) {
        return;
      }
      if (on_ready) {
        on_ready(i, std::move(bitmap));
      }
    }
  });
}

ThumbnailLoader::~ThumbnailLoader() {
  cancel_.store(true, std::memory_order_relaxed);
  if (worker_.joinable()) {
    worker_.join();
  }
}
