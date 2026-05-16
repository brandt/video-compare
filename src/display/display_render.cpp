#include "display/display.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include "core/strings/string_utils.h"
#include "display_utils.h"
#include "pixel_format_utils.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

// Clamp the metadata panel's scroll offset. The help overlay no longer
// scrolls — it's laid out to fit on a single page.
void Display::clamp_overlay_offsets() {
  metadata_panel_.clamp_scroll(drawable_height_, gpu_renderer_active_, HELP_TEXT_LINE_SPACING);
}

// SDL path: read back the rendered scene via SDL_RenderReadPixels and hand off to ImageSaver.
void Display::save_image_frames_sdl(const AVFrame* left_frame, const AVFrame* right_frame) {
  // SDL renderer path — build the OSD via SDL_RenderReadPixels, then defer
  // to the shared ImageSaver pipeline. The GPU renderer path builds its OSD
  // via GpuRenderer::capture_osd and calls the saver directly.
  const size_t pitch = requires_10_bpc() ? drawable_width_ * 3 * sizeof(uint16_t) : drawable_width_ * 3;
  uint8_t* pixels = reinterpret_cast<uint8_t*>(av_malloc(pitch * drawable_height_));

  SDL_Surface* read_surface = SDL_RenderReadPixels(renderer_, nullptr);
  if (read_surface) {
    if (requires_10_bpc()) {
      const uint32_t* src = reinterpret_cast<const uint32_t*>(read_surface->pixels);
      uint16_t* dest = reinterpret_cast<uint16_t*>(pixels);
      const int src_pitch_pixels = read_surface->pitch / sizeof(uint32_t);

      for (int row = 0; row < drawable_height_; row++) {
        const uint32_t* src_row = src + row * src_pitch_pixels;
        for (int col = 0; col < drawable_width_; col++) {
          const uint32_t argb = src_row[col];
          const uint32_t r10 = (argb >> 20) & 0x3FF;
          const uint32_t g10 = (argb >> 10) & 0x3FF;
          const uint32_t b10 = argb & 0x3FF;

          *(dest++) = static_cast<uint16_t>(r10 << 6);
          *(dest++) = static_cast<uint16_t>(g10 << 6);
          *(dest++) = static_cast<uint16_t>(b10 << 6);
        }
      }
    } else {
      SDL_Surface* rgb_surface = SDL_ConvertSurface(read_surface, SDL_PIXELFORMAT_RGB24);
      if (rgb_surface) {
        for (int row = 0; row < drawable_height_; row++) {
          memcpy(pixels + row * pitch,
                 reinterpret_cast<uint8_t*>(rgb_surface->pixels) + row * rgb_surface->pitch,
                 drawable_width_ * 3);
        }
        SDL_DestroySurface(rgb_surface);
      }
    }
    SDL_DestroySurface(read_surface);
  }

  AVFrame* osd = av_frame_alloc();
  osd->format = requires_10_bpc() ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;
  osd->width = drawable_width_;
  osd->height = drawable_height_;
  osd->data[0] = pixels;
  osd->linesize[0] = pitch;
  AVFramePtr osd_frame(osd);

  const std::string& left_stem = side_ui_[displayed_left_side_.as_simple_index()].file_stem;
  const std::string& right_stem = side_ui_[displayed_right_side_.as_simple_index()].file_stem;
  image_saver_.save_frames_with_osd(left_frame, right_frame, osd_frame.get(), left_stem, right_stem);
}

// Draw a text texture with a background rect and an optional clip+fade when it overflows max_text_width_.
void Display::render_text(const int x, const int y, SDL_Texture* texture, const int texture_width, const int texture_height, const int border_extension, const bool left_adjust) {
  // compute clip amount which ensures the filename does not extend more than half the display width
  const int clip_amount = std::max((texture_width + double_border_extension_) - max_text_width_, 0);
  const int gradient_amount = std::min(clip_amount, 24);

  SDL_FRect fill_rect = {static_cast<float>(x - border_extension + gradient_amount), static_cast<float>(y - border_extension), static_cast<float>(texture_width + double_border_extension_ - clip_amount - gradient_amount), static_cast<float>(texture_height + double_border_extension_)};

  SDL_FRect src_rect = {static_cast<float>(clip_amount + gradient_amount), 0, static_cast<float>(texture_width - clip_amount - gradient_amount), static_cast<float>(texture_height)};
  SDL_FRect text_rect = {static_cast<float>(x + gradient_amount), static_cast<float>(y), static_cast<float>(texture_width - clip_amount - gradient_amount), static_cast<float>(texture_height)};

  if (!left_adjust && (mode_ != Mode::VStack)) {
    fill_rect.x += clip_amount;
    text_rect.x += clip_amount;
  }

  SDL_RenderFillRect(renderer_, &fill_rect);
  SDL_RenderTexture(renderer_, texture, &src_rect, &text_rect);

  // render gradient
  if (gradient_amount > 0) {
    Uint8 draw_color_r;
    Uint8 draw_color_g;
    Uint8 draw_color_b;
    Uint8 draw_color_a;
    Uint8 alpha_mod;

    SDL_GetRenderDrawColor(renderer_, &draw_color_r, &draw_color_g, &draw_color_b, &draw_color_a);
    SDL_GetTextureAlphaMod(texture, &alpha_mod);

    fill_rect.x--;
    fill_rect.w = 1;

    src_rect.x--;
    src_rect.w = 1;
    text_rect.x--;
    text_rect.w = 1;

    for (int i = (gradient_amount - 1); i >= 0; i--, fill_rect.x--, src_rect.x--, text_rect.x--) {
      SDL_SetRenderDrawColor(renderer_, draw_color_r, draw_color_g, draw_color_b, draw_color_a * i / gradient_amount);
      SDL_RenderFillRect(renderer_, &fill_rect);

      SDL_SetTextureAlphaMod(texture, alpha_mod * i / gradient_amount);
      SDL_RenderTexture(renderer_, texture, &src_rect, &text_rect);
    }

    // reset
    SDL_SetRenderDrawColor(renderer_, draw_color_r, draw_color_g, draw_color_b, draw_color_a);
    SDL_SetTextureAlphaMod(texture, alpha_mod);
  }
}

// Draw the alternating yellow/black progress dot strip along the top or bottom edge.
void Display::render_progress_dots(const float position, const float progress, const bool is_top) {
  if (duration_ > 0) {
    const float dot_size = 2.f;

    const int dot_width = std::round(drawable_to_window_width_factor_ * dot_size);
    const int dot_height = std::round(drawable_to_window_height_factor_ * dot_size);

    const int y_offset = is_top ? 1 : hud_bottom_drawable_y() - 1 - dot_height;

    const int x_position = std::round(position * drawable_width_ / duration_);
    const int x_progress = std::round(progress * drawable_width_ / duration_);

    for (int x = 0; x < x_position; x++) {
      if (x % (2 * dot_width) < dot_width) {
        SDL_SetRenderDrawColor(renderer_, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, BACKGROUND_ALPHA * 3 / 2);
      } else {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
      }

      SDL_RenderLine(renderer_, x, y_offset, x, y_offset + dot_height - 1);
    }

    // draw current frame
    SDL_SetRenderDrawColor(renderer_, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, BACKGROUND_ALPHA * 2);

    const SDL_FRect current_frame = {static_cast<float>(x_position), static_cast<float>(is_top ? y_offset : y_offset - dot_height), static_cast<float>(x_progress - x_position), static_cast<float>(dot_height * 2)};
    SDL_RenderRect(renderer_, &current_frame);
  }
}

// Return the per-side video texture, honoring the current bilinear/nearest filtering choice.
SDL_Texture* Display::get_side_texture(int side) const {
  return bilinear_texture_filtering_ ? side_textures_linear_[side] : side_textures_nn_[side];
}

// Upload pixels to both the linear and nearest variants of the per-side video texture.
void Display::update_side_texture(int side, const void* pixels, int pitch) {
  SDL_Texture* tex = get_side_texture(side);
  void* locked_pixels = nullptr;
  int locked_pitch = 0;

  // Full-frame upload (no sub-rect — each side has its own texture)
  if (!SDL_LockTexture(tex, nullptr, &locked_pixels, &locked_pitch)) {
    check_sdl(SDL_UpdateTexture(tex, nullptr, pixels, pitch), "side texture update fallback");
    return;
  }

  const uint8_t* src = static_cast<const uint8_t*>(pixels);
  uint8_t* dst = static_cast<uint8_t*>(locked_pixels);

  if (pitch == locked_pitch) {
    memcpy(dst, src, static_cast<size_t>(locked_pitch) * video_height_);
  } else {
    const int row_bytes = std::min(pitch, locked_pitch);
    for (int y = 0; y < video_height_; y++) {
      memcpy(dst, src, row_bytes);
      src += pitch;
      dst += locked_pitch;
    }
  }

  SDL_UnlockTexture(tex);
}

// Round a float to int and clamp to [0, drawable_height_).
int Display::round_and_clamp(const float value) {
  const int result = static_cast<int>(std::roundf(value));

  return requires_10_bpc() ? clamp_int_to_10_bpc_range(result) : clamp_int_to_byte_range(result);
}

// Crop a packed RGB frame in-place by adjusting its data pointer and width/height.
// The returned frame shares storage with src and must be freed with av_frame_free.
AVFrame* crop_rgb_frame(const AVFrame* src, const SDL_Rect& roi, SDL_Rect* out_effective_roi) {
  AVFrame* cropped_frame = av_frame_clone(src);

  if (!cropped_frame) {
    throw std::runtime_error("Unable to clone source frame");
  }

  int bpp = 0;
  if (src->format == AV_PIX_FMT_RGB24) {
    bpp = 3;
  } else if (src->format == AV_PIX_FMT_RGB48LE) {
    bpp = 6;
  } else {
    throw std::runtime_error("Unknown packed RGB format");
  }

  const int x = clamp_range(roi.x, 0, src->width - 1);
  const int y = clamp_range(roi.y, 0, src->height - 1);
  const int w = clamp_range(roi.w, 1, src->width - x);
  const int h = clamp_range(roi.h, 1, src->height - y);

  if (out_effective_roi != nullptr) {
    *out_effective_roi = {x, y, w, h};
  }

  cropped_frame->data[0] = cropped_frame->data[0] + y * cropped_frame->linesize[0] + x * bpp;
  cropped_frame->width = w;
  cropped_frame->height = h;
  return cropped_frame;
}

// SDL path: render the PSNR/SSIM/VMAF overlay in the top-right corner.
void Display::render_quality_metrics_overlay() {
  const std::string vmaf_display = (last_vmaf_ == "n/a") ? std::string("n/a (pause to compute)") : last_vmaf_;
  const std::array<std::string, 3> lines = {
      std::string("PSNR: ") + last_psnr_ + " dB",
      std::string("SSIM: ") + last_ssim_,
      std::string("VMAF: ") + vmaf_display,
  };

  std::array<SDL_Texture*, 3> textures{{nullptr, nullptr, nullptr}};
  std::array<int, 3> widths{{0, 0, 0}};
  std::array<int, 3> heights{{0, 0, 0}};
  int max_w = 0;
  int total_h = 0;
  const int line_spacing = 4;

  for (size_t i = 0; i < lines.size(); i++) {
    SDL_Surface* surface = TTF_RenderText_Blended(small_font_, lines[i].c_str(), 0, POSITION_COLOR);
    if (surface == nullptr) {
      continue;
    }
    textures[i] = SDL_CreateTextureFromSurface(renderer_, surface);
    widths[i] = surface->w;
    heights[i] = surface->h;
    max_w = std::max(max_w, surface->w);
    total_h += surface->h + (i + 1 < lines.size() ? line_spacing : 0);
    SDL_DestroySurface(surface);
  }

  const int padding = border_extension_ * 2;
  const int right_margin = HELP_TEXT_HORIZONTAL_MARGIN;
  const int top_margin = line2_y_ * 2;

  SDL_FRect bg_rect = {static_cast<float>(drawable_width_ - right_margin - max_w - padding * 2), static_cast<float>(top_margin), static_cast<float>(max_w + padding * 2), static_cast<float>(total_h + padding * 2)};

  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 2);
  SDL_RenderFillRect(renderer_, &bg_rect);

  float y = bg_rect.y + padding;
  for (size_t i = 0; i < lines.size(); i++) {
    if (textures[i] == nullptr) {
      continue;
    }
    SDL_FRect dst = {bg_rect.x + padding, y, static_cast<float>(widths[i]), static_cast<float>(heights[i])};
    SDL_RenderTexture(renderer_, textures[i], nullptr, &dst);
    SDL_DestroyTexture(textures[i]);
    y += heights[i] + line_spacing;
  }
}

// Render text using big_font_, falling back to small_font_ if the big one is unavailable.
SDL_Surface* Display::render_text_with_fallback(const std::string& text) {
  SDL_Surface* surface = TTF_RenderText_Blended(small_font_, text.c_str(), 0, TEXT_COLOR);

  if (!surface) {
    std::cerr << "Falling back to lower-quality rendering for '" << text << "'" << std::endl;

    surface = check_sdl(TTF_RenderText_Solid(small_font_, text.c_str(), 0, TEXT_COLOR), "text surface");
  }

  return surface;
}

// Update the selection end point from the current mouse position (called when zoom/pan changes while selecting).
void Display::refresh_selection_end_from_mouse() {
  if (selection_.state() != SelectionState::Started) {
    return;
  }

  SDL_GetMouseState(&mouse_x_, &mouse_y_);
  Vector2D end_video_pos = view_transform_.window_to_video_position(mouse_x_, mouse_y_, view_transform_.compute_zoom_rect());

  if (selection_.wrap()) {
    end_video_pos = SelectionManager::wrap_to_left_frame(end_video_pos, mode_, video_width_, video_height_);
  }

  selection_.set_end(end_video_pos);
}

// SDL path: draw the selection/crop rectangle(s) with per-side colouring.
void Display::draw_selection_rect() {
  if (selection_.state() != SelectionState::Started) {
    return;
  }

  const auto zoom_rect = view_transform_.compute_zoom_rect();

  auto draw_rect = [this](const SDL_FRect& r, Uint8 r_val, Uint8 g_val, Uint8 b_val, int alpha_divider = 1) {
    SDL_SetRenderDrawColor(renderer_, r_val / 2, g_val / 2, b_val / 2, 128 / alpha_divider);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(renderer_, &r);

    SDL_SetRenderDrawColor(renderer_, r_val, g_val, b_val, 255 / alpha_divider);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_RenderRect(renderer_, &r);
  };

  SDL_Rect selection_rect = selection_.get_left_selection_rect(video_width_, video_height_);
  SDL_FRect drawable_rect = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(selection_rect, zoom_rect));

  const bool crop_mode = selection_.crop_mode();
  const CropTargetSide side = selection_.crop_target_side();

  if (mode_ == Mode::Split) {
    if (crop_mode) {
      switch (side) {
        case CropTargetSide::Right: draw_rect(drawable_rect, 128, 128, 255); break;
        case CropTargetSide::Both:  draw_rect(drawable_rect, 255, 255, 255); break;
        default:                    draw_rect(drawable_rect, 255, 128, 128); break;
      }
    } else {
      draw_rect(drawable_rect, 255, 255, 255);
    }
    return;
  } else {
    if (!crop_mode || side == CropTargetSide::Left || side == CropTargetSide::Both) {
      draw_rect(drawable_rect, 255, 128, 128);
    } else {
      draw_rect(drawable_rect, 96, 96, 96, 3);
    }
  }

  switch (mode_) {
    case Mode::HStack: selection_rect.x += video_width_; break;
    case Mode::VStack: selection_rect.y += video_height_; break;
    default: break;
  }

  drawable_rect = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(selection_rect, zoom_rect));

  if (!crop_mode || side == CropTargetSide::Right || side == CropTargetSide::Both) {
    draw_rect(drawable_rect, 128, 128, 255);
  } else {
    draw_rect(drawable_rect, 96, 96, 96, 3);
  }
}

// If save_selected_area was requested, write left/right JXLs for the current selection rect.
void Display::possibly_save_selected_area(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (selection_.state() != SelectionState::Completed) {
    return;
  }

  const SDL_Rect selection_rect = selection_.get_left_selection_rect(video_width_, video_height_);

  if (selection_rect.w <= 0 || selection_rect.h <= 0) {
    std::cerr << "Selection rectangle is empty. Please make a valid selection." << std::endl;
  } else {
    const std::string& left_stem = side_ui_[displayed_left_side_.as_simple_index()].file_stem;
    const std::string& right_stem = side_ui_[displayed_right_side_.as_simple_index()].file_stem;
    image_saver_.save_selected_area(left_frame, right_frame, selection_rect, left_stem, right_stem);
  }

  selection_.cancel_save_selected_area();
}

// If a crop was requested, build a PendingCropRequest from the selection and queue it for the main loop.
void Display::possibly_apply_crop() {
  if (selection_.state() != SelectionState::Completed) {
    return;
  }

  const SDL_Rect selection_rect = selection_.get_left_selection_rect(video_width_, video_height_);

  PendingCropRequest request;
  if (selection_rect.w <= 0 || selection_rect.h <= 0) {
    std::cerr << "Crop rectangle is empty. Please make a valid selection." << std::endl;
  } else {
    request.rect = selection_rect;
    request.valid = true;

    switch (selection_.crop_target_side()) {
      case CropTargetSide::Left:
        request.apply_left = true;
        break;
      case CropTargetSide::Right:
        request.apply_right = true;
        request.right_target_index = active_right_index_;
        break;
      case CropTargetSide::Both:
        request.apply_left = true;
        request.apply_right = true;
        request.right_target_index = active_right_index_;
        break;
      case CropTargetSide::Undefined:
        request.valid = false;
        break;
    }
  }

  selection_.set_pending_crop_request(request);
  selection_.reset_crop_mode();
}

// Compute the visible ROI on each side in single-frame (per-side) coordinates, accounting for zoom and mode.
std::pair<SDL_Rect, SDL_Rect> Display::get_visible_rois_in_single_frame_coordinates() const {
  const auto zoom_rect = view_transform_.compute_zoom_rect();

  // p0/p1 are in *layout* coordinates in hstack/vstack, single-frame in split.
  const Vector2D p0 = view_transform_.window_to_video_position(0, 0, zoom_rect, true);
  const Vector2D p1 = view_transform_.window_to_video_position(window_width_, window_height_, zoom_rect, false);

  const int lx0 = static_cast<int>(std::min(p0.x(), p1.x()));
  const int ly0 = static_cast<int>(std::min(p0.y(), p1.y()));
  const int lx1 = static_cast<int>(std::max(p0.x(), p1.x()));
  const int ly1 = static_cast<int>(std::max(p0.y(), p1.y()));

  auto clamp_x = [&](int x) { return clamp_range(x, 0, video_width_); };
  auto clamp_y = [&](int y) { return clamp_range(y, 0, video_height_); };

  auto intersect_1d = [&](int a0, int a1, int b0, int b1) -> std::pair<int, int> {
    const int lo = std::max(a0, b0);
    const int hi = std::min(a1, b1);
    return (hi > lo) ? std::make_pair(lo, hi) : std::make_pair(0, 0);
  };

  // Default: single-frame layout (split) => both sides share the same ROI.
  if (mode_ != Mode::HStack && mode_ != Mode::VStack) {
    const int x0 = clamp_x(lx0);
    const int y0 = clamp_y(ly0);
    const int x1 = clamp_x(lx1);
    const int y1 = clamp_y(ly1);
    const SDL_Rect roi = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    return {roi, roi};
  }

  if (mode_ == Mode::HStack) {
    const int w = video_width_;
    const auto left_x = intersect_1d(lx0, lx1, 0, w);
    const auto right_x_layout = intersect_1d(lx0, lx1, w, 2 * w);
    const std::pair<int, int> right_x = (right_x_layout.second > right_x_layout.first) ? std::make_pair(right_x_layout.first - w, right_x_layout.second - w) : std::make_pair(0, 0);

    const int y0 = clamp_y(ly0);
    const int y1 = clamp_y(ly1);

    SDL_Rect left{0, 0, 0, 0};
    if (left_x.second > left_x.first) {
      const int x0 = clamp_x(left_x.first);
      const int x1 = clamp_x(left_x.second);
      left = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    }

    SDL_Rect right{0, 0, 0, 0};
    if (right_x.second > right_x.first) {
      const int x0 = clamp_x(right_x.first);
      const int x1 = clamp_x(right_x.second);
      right = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    }

    return {left, right};
  }

  // VSTACK
  const int h = video_height_;
  const auto top_y = intersect_1d(ly0, ly1, 0, h);
  const auto bottom_y_layout = intersect_1d(ly0, ly1, h, 2 * h);
  const std::pair<int, int> bottom_y = (bottom_y_layout.second > bottom_y_layout.first) ? std::make_pair(bottom_y_layout.first - h, bottom_y_layout.second - h) : std::make_pair(0, 0);

  const int x0 = clamp_x(lx0);
  const int x1 = clamp_x(lx1);

  SDL_Rect left{0, 0, 0, 0};
  if (top_y.second > top_y.first) {
    const int y0 = clamp_y(top_y.first);
    const int y1 = clamp_y(top_y.second);
    left = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
  }

  SDL_Rect right{0, 0, 0, 0};
  if (bottom_y.second > bottom_y.first) {
    const int y0 = clamp_y(bottom_y.first);
    const int y1 = clamp_y(bottom_y.second);
    right = {x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
  }

  return {left, right};
}

// Intersection of the left and right visible ROIs — the region compared by similarity metrics.
SDL_Rect Display::get_visible_roi_in_single_frame_coordinates() const {
  const auto rois = get_visible_rois_in_single_frame_coordinates();
  const SDL_Rect left_roi = rois.first;
  const SDL_Rect right_roi = rois.second;

  const bool left_off = left_roi.w <= 0 || left_roi.h <= 0;
  const bool right_off = right_roi.w <= 0 || right_roi.h <= 0;

  if (mode_ == Mode::HStack || mode_ == Mode::VStack) {
    // Prefer the side that the view center is currently over, but fall back to the other
    // if that side is not visible.
    const auto zoom_rect = view_transform_.compute_zoom_rect();
    const Vector2D center_layout = view_transform_.window_to_video_position(window_width_ / 2, window_height_ / 2, zoom_rect, true);

    const bool prefer_right = (mode_ == Mode::HStack) ? (center_layout.x() >= static_cast<float>(video_width_)) : (center_layout.y() >= static_cast<float>(video_height_));
    const SDL_Rect preferred = prefer_right ? right_roi : left_roi;
    const SDL_Rect other = prefer_right ? left_roi : right_roi;

    const bool preferred_off = prefer_right ? right_off : left_off;
    const bool other_off = prefer_right ? left_off : right_off;

    if (!preferred_off) {
      return preferred;
    }
    if (!other_off) {
      return other;
    }
    return {0, 0, 0, 0};
  }

  // SPLIT (single-frame layout): both ROIs are identical.
  return left_roi;
}
