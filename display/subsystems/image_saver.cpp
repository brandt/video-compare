#include "image_saver.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include "display/jxl_saver.h"
#include "core/strings/string_utils.h"

namespace {

void save_frame_image(const AVFrame* frame, const std::string& filename, std::atomic_bool& error_occurred) {
  try {
    JxlSaver::save(frame, filename);
  } catch (const std::ios_base::failure&) {
    std::cerr << "Error saving image to file: " << filename << std::endl;
    error_occurred = true;
  } catch (const std::runtime_error& e) {
    std::cerr << "Error saving image: " << e.what() << std::endl;
    error_occurred = true;
  }
}

}  // namespace

void ImageSaver::save_frames_with_osd(const AVFrame* left, const AVFrame* right, const AVFrame* osd,
                                       const std::string& left_stem, const std::string& right_stem) {
  std::atomic_bool error_occurred(false);

  const bool stems_equal = (left_stem == right_stem);
  const std::string left_filename = string_sprintf("%s%s_%04d.jxl", left_stem.c_str(), stems_equal ? "_left" : "", saved_image_number_);
  const std::string right_filename = string_sprintf("%s%s_%04d.jxl", right_stem.c_str(), stems_equal ? "_right" : "", saved_image_number_);
  const std::string osd_filename = string_sprintf("%s_%s_osd_%04d.jxl", left_stem.c_str(), right_stem.c_str(), saved_image_number_);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) { save_frame_image(frame, filename, error_occurred); };

  std::thread save_left_thread(save_frame, left, left_filename);
  std::thread save_right_thread(save_frame, right, right_filename);
  std::thread save_osd_thread(save_frame, osd, osd_filename);

  save_left_thread.join();
  save_right_thread.join();
  save_osd_thread.join();

  if (!error_occurred) {
    const std::string msg = string_sprintf("Saved %s, %s and %s", left_filename.c_str(), right_filename.c_str(), osd_filename.c_str());
    if (notify_) notify_(msg);
    saved_image_number_++;
  }
}

void ImageSaver::save_selected_area(const AVFrame* left, const AVFrame* right, const SDL_Rect& selection_rect,
                                     const std::string& left_stem, const std::string& right_stem) {
  std::atomic_bool error_occurred(false);

  auto create_frame = [&](const int width, const int height, const AVFrame* source_frame) -> AVFrame* {
    AVFrame* frame = av_frame_alloc();
    frame->format = source_frame->format;
    frame->width = width;
    frame->height = height;
    frame->colorspace = source_frame->colorspace;
    frame->color_range = source_frame->color_range;
    av_frame_get_buffer(frame, 0);
    return frame;
  };

  AVFrame* left_selected = create_frame(selection_rect.w, selection_rect.h, left);
  AVFrame* right_selected = create_frame(selection_rect.w, selection_rect.h, right);
  AVFrame* concatenated = create_frame(selection_rect.w * 2, selection_rect.h, left);

  // Packed-RGB bytes per pixel derived from frame format so GPU-mode RGB
  // cache frames (RGB24 / RGB48LE) save correctly even when display flags
  // (hdr_passthrough_, use_10_bpc_) would have suggested a different size.
  int pixel_size;
  switch (left->format) {
    case AV_PIX_FMT_RGB24:      pixel_size = 3; break;
    case AV_PIX_FMT_RGB48LE:    pixel_size = 6; break;
    case AV_PIX_FMT_X2RGB10LE:  pixel_size = 4; break;
    default:
      std::cerr << "save_selected_area: unsupported pixel format " << left->format << std::endl;
      av_frame_free(&left_selected);
      av_frame_free(&right_selected);
      av_frame_free(&concatenated);
      return;
  }

  for (int y = 0; y < selection_rect.h; y++) {
    const int src_y = selection_rect.y + y;
    const int dst_y = y;

    std::memcpy(left_selected->data[0] + dst_y * left_selected->linesize[0],
                left->data[0] + src_y * left->linesize[0] + selection_rect.x * pixel_size,
                selection_rect.w * pixel_size);

    std::memcpy(right_selected->data[0] + dst_y * right_selected->linesize[0],
                right->data[0] + src_y * right->linesize[0] + selection_rect.x * pixel_size,
                selection_rect.w * pixel_size);

    std::memcpy(concatenated->data[0] + dst_y * concatenated->linesize[0],
                left->data[0] + src_y * left->linesize[0] + selection_rect.x * pixel_size,
                selection_rect.w * pixel_size);
    std::memcpy(concatenated->data[0] + dst_y * concatenated->linesize[0] + selection_rect.w * pixel_size,
                right->data[0] + src_y * right->linesize[0] + selection_rect.x * pixel_size,
                selection_rect.w * pixel_size);
  }

  const bool stems_equal = (left_stem == right_stem);
  const std::string left_filename = string_sprintf("%s%s_cutout_%04d.jxl", left_stem.c_str(), stems_equal ? "_left" : "", saved_selected_image_number_);
  const std::string right_filename = string_sprintf("%s%s_cutout_%04d.jxl", right_stem.c_str(), stems_equal ? "_right" : "", saved_selected_image_number_);
  const std::string concatenated_filename = string_sprintf("%s_%s_cutout_concat_%04d.jxl", left_stem.c_str(), right_stem.c_str(), saved_selected_image_number_);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) { save_frame_image(frame, filename, error_occurred); };

  std::thread save_left_thread(save_frame, left_selected, left_filename);
  std::thread save_right_thread(save_frame, right_selected, right_filename);
  std::thread save_concatenated_thread(save_frame, concatenated, concatenated_filename);

  save_left_thread.join();
  save_right_thread.join();
  save_concatenated_thread.join();

  av_frame_free(&left_selected);
  av_frame_free(&right_selected);
  av_frame_free(&concatenated);

  if (!error_occurred) {
    std::cout << "Saved " << string_sprintf("%s, %s and %s", left_filename.c_str(), right_filename.c_str(), concatenated_filename.c_str()) << std::endl;
    saved_selected_image_number_++;
  }
}
