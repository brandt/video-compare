#include "display/jxl_saver.h"
#include <fstream>
extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

void JxlSaver::save(const AVFrame* frame, const std::string& filename) {
  const AVCodec* codec = avcodec_find_encoder_by_name("libjxl");
  if (!codec) {
    throw EncodingException("libjxl encoder not found; cannot save HDR frame");
  }

  // Convert to RGB48LE if needed (libjxl accepts rgb48le natively)
  const AVFrame* frame_to_save = frame;
  AVFramePtr converted_frame(nullptr);

  if (frame->format != AV_PIX_FMT_RGB48LE) {
    converted_frame.reset(convert(frame, AV_PIX_FMT_RGB48LE));
    frame_to_save = converted_frame.get();
  }

  AVCodecContextPtr codec_ctx(avcodec_alloc_context3(codec));
  if (!codec_ctx) {
    throw EncodingException("Could not allocate JXL codec context");
  }

  codec_ctx->width = frame_to_save->width;
  codec_ctx->height = frame_to_save->height;
  codec_ctx->pix_fmt = AV_PIX_FMT_RGB48LE;
  codec_ctx->time_base = {1, 25};
  codec_ctx->color_primaries = frame->color_primaries;
  codec_ctx->color_trc = frame->color_trc;
  codec_ctx->colorspace = frame->colorspace;
  codec_ctx->color_range = AVCOL_RANGE_JPEG;  // RGB data is always full range

  // Lossless mode
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "distance", "0", 0);
  av_dict_set(&opts, "effort", "3", 0);

  if (avcodec_open2(codec_ctx.get(), codec, &opts) < 0) {
    av_dict_free(&opts);
    throw EncodingException("Could not open JXL codec");
  }
  av_dict_free(&opts);

  AVPacketPtr packet(av_packet_alloc());
  if (!packet) {
    throw EncodingException("Could not allocate packet");
  }

  if (avcodec_send_frame(codec_ctx.get(), frame_to_save) < 0) {
    throw EncodingException("Error sending frame to JXL encoder");
  }

  if (avcodec_receive_packet(codec_ctx.get(), packet.get()) < 0) {
    throw EncodingException("Error during JXL encoding");
  }

  try {
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    if (!file.is_open()) {
      throw IOException("Could not open file: " + filename);
    }
    file.write(reinterpret_cast<const char*>(packet->data), packet->size);
    file.close();
  } catch (const std::ios_base::failure& e) {
    throw IOException("IO error while writing JXL file " + filename + ": " + e.what());
  }
}

AVFrame* JxlSaver::convert(const AVFrame* frame, const AVPixelFormat output_format) {
  struct SwsContext* sws_ctx = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, output_format, SWS_BILINEAR, nullptr, nullptr, nullptr);

  if (!sws_ctx) {
    throw EncodingException("Could not initialize the conversion context");
  }

  AVFrame* converted_frame = av_frame_alloc();
  if (!converted_frame) {
    sws_freeContext(sws_ctx);
    throw EncodingException("Could not allocate converted frame");
  }

  converted_frame->format = output_format;
  converted_frame->width = frame->width;
  converted_frame->height = frame->height;
  av_image_alloc(converted_frame->data, converted_frame->linesize, converted_frame->width, converted_frame->height, output_format, 32);

  sws_scale(sws_ctx, frame->data, frame->linesize, 0, frame->height, converted_frame->data, converted_frame->linesize);

  sws_freeContext(sws_ctx);
  return converted_frame;
}
