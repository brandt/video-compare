#include "display/display.h"
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include "analysis/metrics/metrics_calculator.h"
#include "analysis/metrics/vmaf_calculator.h"
#include "core/ffmpeg/ffmpeg.h"
#include "core/strings/string_utils.h"
#include "display_utils.h"
extern "C" {
#include <libavutil/frame.h>
}

// SDL_Renderer render path: upload per-side textures, render HUD via SDL primitives, present.
void Display::render_frame_sdl(const RenderContext& ctx, const std::string& current_total_browsable) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;
  const bool has_updated_left_frame = ctx.has_updated_left_frame;
  const bool has_updated_right_frame = ctx.has_updated_right_frame;
  const bool compare_mode = ctx.compare_mode;
  const auto& zoom_rect = ctx.zoom_rect;
  const float video_texel_clamped_mouse_x = ctx.video_texel_clamped_mouse_x;
  const int split_x = ctx.split_x;
  const int dst_zoomed_size = ctx.dst_zoomed_size;
  const int dst_half_zoomed_size = ctx.dst_half_zoomed_size;

  std::array<uint8_t*, 3> planes_left{left_frame->data[0], left_frame->data[1], left_frame->data[2]};
  std::array<uint8_t*, 3> planes_right{right_frame->data[0], right_frame->data[1], right_frame->data[2]};
  std::array<size_t, 3> pitches_left{static_cast<size_t>(left_frame->linesize[0]), static_cast<size_t>(left_frame->linesize[1]), static_cast<size_t>(left_frame->linesize[2])};
  std::array<size_t, 3> pitches_right{static_cast<size_t>(right_frame->linesize[0]), static_cast<size_t>(right_frame->linesize[1]), static_cast<size_t>(right_frame->linesize[2])};

  // init 10 bpc temp buffers
  if (requires_10_bpc()) {
    diff_processor_.ensure_left_planes(pitches_left[0]);
    diff_processor_.ensure_right_planes(pitches_right[0]);
  }

  update_window_title_with_current_roi();

  sdl_run_cpu_work(ctx, planes_left, pitches_left, planes_right, pitches_right);

  // Clear and draw the video textures, then the zoom magnifier on top.
  SDL_SetRenderDrawColor(renderer_, BACKGROUND_COLOR.r, BACKGROUND_COLOR.g, BACKGROUND_COLOR.b, BACKGROUND_COLOR.a);
  SDL_RenderClear(renderer_);

  sdl_render_video_textures(ctx, planes_left, pitches_left, planes_right, pitches_right);

  const int mouse_drawable_x = std::round(video_texel_clamped_mouse_x * drawable_to_window_width_factor_);
  const int mouse_drawable_y = std::round(static_cast<float>(mouse_y_) * drawable_to_window_height_factor_);
  sdl_render_zoom_magnifier(mouse_drawable_x, mouse_drawable_y, dst_zoomed_size);

  if (show_hud_) {
    sdl_render_hud(ctx, current_total_browsable);
  }

  sdl_render_message_toast();

  if (mode_ == Mode::Split && show_hud_ && compare_mode) {
    // Split slider line + zoom-window slider(s).
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer_, mouse_drawable_x, 0, mouse_drawable_x, drawable_height_);
    if (view_transform_.zoom_left()) {
      SDL_RenderLine(renderer_, dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, dst_half_zoomed_size, drawable_height_);
    }
    if (view_transform_.zoom_right()) {
      SDL_RenderLine(renderer_, drawable_width_ - dst_half_zoomed_size - 1, drawable_height_ - dst_zoomed_size, drawable_width_ - dst_half_zoomed_size - 1, drawable_height_);
    }
  }

  draw_selection_rect();

  if (show_quality_metrics_) {
    render_quality_metrics_overlay();
  }

  if (overlay_.show_metadata()) {
    metadata_panel_.ensure_current(swap_left_right_,
                                   displayed_left_side_.is_left(),
                                   displayed_right_side_.is_right(),
                                   small_font_, big_font_, renderer_,
                                   gpu_renderer_active_, drawable_width_);
    metadata_panel_.render_sdl(renderer_, drawable_width_, drawable_height_, mode_);
  }

  if (overlay_.show_help()) {
    overlay_.render_help_sdl(renderer_);
  }

  sdl_finalize_deferred(left_frame, right_frame);

  SDL_RenderPresent(renderer_);
}

// SDL path: run pixel inspector, similarity metrics, and live quality metrics directly on the native frame planes.
void Display::sdl_run_cpu_work(const RenderContext& ctx,
                                const std::array<uint8_t*, 3>& planes_left, const std::array<size_t, 3>& pitches_left,
                                const std::array<uint8_t*, 3>& planes_right, const std::array<size_t, 3>& pitches_right) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;
  const auto& zoom_rect = ctx.zoom_rect;

  const Vector2D mouse_video_pos = view_transform_.window_to_video_position(mouse_x_, mouse_y_, zoom_rect);
  const int mouse_video_x = mouse_video_pos.x();
  const int mouse_video_y = mouse_video_pos.y();

  // print pixel position in original video coordinates and RGB+YUV color value
  if (print_mouse_position_and_color_) {
    const bool print_left_pixel = mouse_video_x >= 0 && mouse_video_x < video_width_ && mouse_video_y >= 0 && mouse_video_y < video_height_;

    bool print_right_pixel;
    switch (mode_) {
      case Mode::HStack:
        print_right_pixel = mouse_video_x >= video_width_ && mouse_video_x < (2 * video_width_) && mouse_video_y >= 0 && mouse_video_y < video_height_;
        break;
      case Mode::VStack:
        print_right_pixel = mouse_video_x >= 0 && mouse_video_x < video_width_ && mouse_video_y >= video_height_ && mouse_video_y < (video_height_ * 2);
        break;
      default:
        print_right_pixel = print_left_pixel;
    }

    if (print_left_pixel || print_right_pixel) {
      const int pixel_video_x = mouse_video_x % video_width_;
      const int pixel_video_y = mouse_video_y % video_height_;

      auto get_original_dimensions = [&](const AVFrame* frame) -> std::pair<int, int> {
        const int original_width = get_metadata_int_value(frame, "original_width", frame->width);
        const int original_height = get_metadata_int_value(frame, "original_height", frame->height);
        return std::make_pair(original_width, original_height);
      };

      auto original_left_dims = get_original_dimensions(left_frame);
      auto original_right_dims = get_original_dimensions(right_frame);

      std::cout << "Left:  " << string_sprintf("[%4d,%4d]", pixel_video_x * original_left_dims.first / video_width_, pixel_video_y * original_left_dims.second / video_height_);
      std::cout << ", " << MetricsCalculator::get_and_format_rgb_yuv_pixel(planes_left[0], pitches_left[0], left_frame, pixel_video_x, pixel_video_y, requires_10_bpc());
      std::cout << " - ";
      std::cout << "Right: " << string_sprintf("[%4d,%4d]", pixel_video_x * original_right_dims.first / video_width_, pixel_video_y * original_right_dims.second / video_height_);
      std::cout << ", " << MetricsCalculator::get_and_format_rgb_yuv_pixel(planes_right[0], pitches_right[0], right_frame, pixel_video_x, pixel_video_y, requires_10_bpc());
      std::cout << std::endl;
    }

    print_mouse_position_and_color_ = false;
  }

  // print image similarity metrics
  if (print_image_similarity_metrics_) {
    SDL_Rect roi = get_visible_roi_in_single_frame_coordinates();
    if (roi.w <= 0 || roi.h <= 0) {
      std::cerr << "ROI is empty, skipping metrics calculation" << std::endl;
    } else {
      SDL_Rect effective_roi_left{}, effective_roi_right{};
      AVFrame* left_crop = crop_rgb_frame(left_frame, roi, &effective_roi_left);
      AVFrame* right_crop = crop_rgb_frame(right_frame, roi, &effective_roi_right);

      if (!SDL_RectsEqual(&effective_roi_left, &effective_roi_right)) {
        std::cerr << "Error: Left and right effective ROIs are different" << std::endl;
      } else {
        const int crop_width = effective_roi_left.w;
        const int crop_height = effective_roi_left.h;

        float* left_gray = MetricsCalculator::rgb_to_grayscale(left_crop->data[0], left_crop->linesize[0], crop_width, crop_height, requires_10_bpc());
        float* right_gray = MetricsCalculator::rgb_to_grayscale(right_crop->data[0], right_crop->linesize[0], crop_width, crop_height, requires_10_bpc());

        const std::string psnr = MetricsCalculator::compute_psnr(left_gray, right_gray, crop_width, crop_height);
        const std::string ssim = MetricsCalculator::compute_ssim(left_gray, right_gray, crop_width, crop_height);
        const std::string vmaf = (left_crop && right_crop) ? VMAFCalculator::instance().compute(left_crop, right_crop) : "n/a";

        const std::string roi_str =
            (crop_width < video_width_ || crop_height < video_height_) ? string_sprintf("  (%d,%d)-(%d,%d)", effective_roi_left.x, effective_roi_left.y, effective_roi_left.x + crop_width - 1, effective_roi_left.y + crop_height - 1) : "";

        std::cout << string_sprintf("Metrics: [%s|%s] PSNR(%s), SSIM(%s), VMAF(%s)%s", format_position(ffmpeg::pts_in_secs(left_frame), false).c_str(), format_position(ffmpeg::pts_in_secs(right_frame), false).c_str(), psnr.c_str(),
                                    ssim.c_str(), vmaf.c_str(), roi_str.c_str())
                  << std::endl;

        delete[] left_gray;
        delete[] right_gray;
      }

      if (left_crop) av_frame_free(&left_crop);
      if (right_crop) av_frame_free(&right_crop);
    }

    print_image_similarity_metrics_ = false;
  }

  // live on-screen quality metrics overlay (toggled by Q key). Paused-only —
  // PSNR/SSIM/VMAF are expensive and running them during playback wastes CPU
  // on transient frame pairs the user isn't inspecting.
  if (show_quality_metrics_ && !playback_.play() && left_frame != nullptr && right_frame != nullptr && video_width_ > 0 && video_height_ > 0) {
    float* left_gray = MetricsCalculator::rgb_to_grayscale(left_frame->data[0], left_frame->linesize[0], video_width_, video_height_, requires_10_bpc());
    float* right_gray = MetricsCalculator::rgb_to_grayscale(right_frame->data[0], right_frame->linesize[0], video_width_, video_height_, requires_10_bpc());

    last_psnr_ = MetricsCalculator::compute_psnr(left_gray, right_gray, video_width_, video_height_);
    last_ssim_ = MetricsCalculator::compute_ssim(left_gray, right_gray, video_width_, video_height_);

    delete[] left_gray;
    delete[] right_gray;

    if (left_frame->pts != last_vmaf_left_pts_ || right_frame->pts != last_vmaf_right_pts_) {
      last_vmaf_ = VMAFCalculator::instance().compute(left_frame, right_frame);
      last_vmaf_left_pts_ = left_frame->pts;
      last_vmaf_right_pts_ = right_frame->pts;
    }
  }
}

// SDL path: upload per-side frames (with optional packed 10-bpc conversion or subtraction diff) and render them into the on-screen regions.
void Display::sdl_render_video_textures(const RenderContext& ctx,
                                         const std::array<uint8_t*, 3>& planes_left, const std::array<size_t, 3>& pitches_left,
                                         const std::array<uint8_t*, 3>& planes_right, const std::array<size_t, 3>& pitches_right) {
  if (!show_left_ && !show_right_) return;

  const bool has_updated_left_frame = ctx.has_updated_left_frame;
  const bool has_updated_right_frame = ctx.has_updated_right_frame;
  const auto& zoom_rect = ctx.zoom_rect;
  const int split_x = ctx.split_x;

  // Upload full frames to per-side textures
  if (input_received_ || has_updated_left_frame) {
    if (requires_10_bpc()) {
      const SDL_Rect full_rect = {0, 0, video_width_, video_height_};
      const auto& left_planes = diff_processor_.ensure_left_planes(pitches_left[0]);
      diff_processor_.convert_to_packed_10_bpc(planes_left, pitches_left, left_planes, pitches_left, full_rect);
      update_side_texture(0, left_planes[0], pitches_left[0]);
    } else {
      update_side_texture(0, planes_left[0], pitches_left[0]);
    }
  }

  // Subtraction mode depends on both frames; refresh when either changes
  const bool right_needs_update = input_received_ || has_updated_right_frame || (diff_processor_.subtraction_mode() && has_updated_left_frame);

  if (right_needs_update) {
    if (diff_processor_.subtraction_mode()) {
      diff_processor_.update_difference(planes_left, pitches_left, planes_right, pitches_right, 0);

      if (requires_10_bpc()) {
        const SDL_Rect full_rect = {0, 0, video_width_, video_height_};
        const auto& right_planes = diff_processor_.ensure_right_planes(pitches_right[0]);
        diff_processor_.convert_to_packed_10_bpc(diff_processor_.diff_planes(), diff_processor_.diff_pitches(), right_planes, pitches_right, full_rect);
        update_side_texture(1, right_planes[0], pitches_right[0]);
      } else {
        update_side_texture(1, diff_processor_.diff_planes()[0], diff_processor_.diff_pitches()[0]);
      }
    } else {
      if (requires_10_bpc()) {
        const SDL_Rect full_rect = {0, 0, video_width_, video_height_};
        const auto& right_planes = diff_processor_.ensure_right_planes(pitches_right[0]);
        diff_processor_.convert_to_packed_10_bpc(planes_right, pitches_right, right_planes, pitches_right, full_rect);
        update_side_texture(1, right_planes[0], pitches_right[0]);
      } else {
        update_side_texture(1, planes_right[0], pitches_right[0]);
      }
    }
  }

  // Render from per-side textures to screen regions
  if (show_left_ && (split_x > 0)) {
    const SDL_FRect src_left = {0, 0, static_cast<float>(split_x), static_cast<float>(video_height_)};
    const SDL_Rect video_quad_left = {0, 0, split_x, video_height_};
    const SDL_FRect screen_quad_left = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(video_quad_left, zoom_rect));
    check_sdl(SDL_RenderTexture(renderer_, get_side_texture(0), &src_left, &screen_quad_left), "left video texture render");
  }
  if (show_right_ && ((split_x < video_width_) || mode_ != Mode::Split)) {
    const int start_right = (mode_ == Mode::Split) ? std::max(split_x, 0) : 0;
    const int right_x_offset = (mode_ == Mode::HStack) ? video_width_ : 0;
    const int right_y_offset = (mode_ == Mode::VStack) ? video_height_ : 0;

    const SDL_FRect src_right = {static_cast<float>(start_right), 0, static_cast<float>(video_width_ - start_right), static_cast<float>(video_height_)};
    const SDL_Rect video_quad_right = {right_x_offset + start_right, right_y_offset, video_width_ - start_right, video_height_};
    const SDL_FRect screen_quad_right = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(video_quad_right, zoom_rect));
    check_sdl(SDL_RenderTexture(renderer_, get_side_texture(1), &src_right, &screen_quad_right), "right video texture render");
  }
}

// SDL path: read back a 64-px src block around the mouse and blit it scaled up into each active zoom-magnifier corner.
void Display::sdl_render_zoom_magnifier(const int mouse_drawable_x, const int mouse_drawable_y, const int dst_zoomed_size) {
  if (!view_transform_.zoom_left() && !view_transform_.zoom_right()) return;

  const int src_zoomed_size = 64;
  const int src_half_zoomed_size = src_zoomed_size / 2;

  SDL_Rect src_zoomed_area = {clamp_range(mouse_drawable_x - src_half_zoomed_size, 0, drawable_width_ - src_zoomed_size - 1),
                              clamp_range(mouse_drawable_y - src_half_zoomed_size, 0, drawable_height_ - src_zoomed_size - 1),
                              src_zoomed_size, src_zoomed_size};

  SDL_Surface* render_surface = SDL_RenderReadPixels(renderer_, &src_zoomed_area);
  SDL_Texture* render_texture = render_surface ? SDL_CreateTextureFromSurface(renderer_, render_surface) : nullptr;

  if (render_texture) {
    if (view_transform_.zoom_left()) {
      const SDL_FRect dst_zoomed_area = {0, static_cast<float>(drawable_height_ - dst_zoomed_size), static_cast<float>(dst_zoomed_size), static_cast<float>(dst_zoomed_size)};
      SDL_RenderTexture(renderer_, render_texture, nullptr, &dst_zoomed_area);
    }
    if (view_transform_.zoom_right()) {
      const SDL_FRect dst_zoomed_area = {static_cast<float>(drawable_width_ - dst_zoomed_size), static_cast<float>(drawable_height_ - dst_zoomed_size), static_cast<float>(dst_zoomed_size), static_cast<float>(dst_zoomed_size)};
      SDL_RenderTexture(renderer_, render_texture, nullptr, &dst_zoomed_area);
    }
  }

  SDL_DestroyTexture(render_texture);
  SDL_DestroySurface(render_surface);
}

// SDL path: draw all HUD text (file labels, positions, seek target, zoom factor, playback speed, frame counter, progress dots).
void Display::sdl_render_hud(const RenderContext& ctx, const std::string& current_total_browsable) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;

  const float left_position = ffmpeg::pts_in_secs(left_frame);
  const float right_position = ffmpeg::pts_in_secs(right_frame);
  const float left_progress = left_position + ffmpeg::frame_duration_in_secs(left_frame);
  const float right_progress = right_position + ffmpeg::frame_duration_in_secs(right_frame);

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

  SDL_FRect fill_rect;
  SDL_FRect text_rect;
  SDL_Surface* text_surface;

  if (show_left_) {
    const std::string left_picture_type(1, av_get_picture_type_char(left_frame->pict_type));
    const std::string left_pos_str = format_position(left_position, true) + " " + left_picture_type + format_position_difference(left_position, right_position);
    text_surface = TTF_RenderText_Blended(small_font_, left_pos_str.c_str(), 0, POSITION_COLOR);
    SDL_Texture* left_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    const int left_position_text_width = text_surface->w;
    const int left_position_text_height = text_surface->h;
    SDL_DestroySurface(text_surface);

    if (mode_ == Mode::VStack) {
      render_text(line1_y_, line1_y_, left_position_text_texture, left_position_text_width, left_position_text_height, border_extension_, true);
      render_text(line1_y_, line2_y_, side_ui_[displayed_left_side_.as_simple_index()].text_texture, side_ui_[displayed_left_side_.as_simple_index()].text_width, side_ui_[displayed_left_side_.as_simple_index()].text_height,
                  border_extension_, true);
    } else {
      render_text(line1_y_, line1_y_, side_ui_[displayed_left_side_.as_simple_index()].text_texture, side_ui_[displayed_left_side_.as_simple_index()].text_width, side_ui_[displayed_left_side_.as_simple_index()].text_height,
                  border_extension_, true);
      render_text(line1_y_, line2_y_, left_position_text_texture, left_position_text_width, left_position_text_height, border_extension_, true);
    }

    SDL_DestroyTexture(left_position_text_texture);
  }
  if (show_right_) {
    const std::string right_picture_type(1, av_get_picture_type_char(right_frame->pict_type));
    const std::string right_pos_str = format_position(right_position, true) + " " + right_picture_type + format_position_difference(right_position, left_position);
    text_surface = TTF_RenderText_Blended(small_font_, right_pos_str.c_str(), 0, POSITION_COLOR);
    SDL_Texture* right_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    int right_position_text_width = text_surface->w;
    int right_position_text_height = text_surface->h;
    SDL_DestroySurface(text_surface);

    int text1_x, text1_y, text2_x, text2_y;
    if (mode_ == Mode::VStack) {
      text1_x = line1_y_;
      text1_y = drawable_height_ - line2_y_ - side_ui_[displayed_right_side_.as_simple_index()].text_height;
      text2_x = line1_y_;
      text2_y = drawable_height_ - line1_y_ - side_ui_[displayed_right_side_.as_simple_index()].text_height;
    } else {
      text1_x = drawable_width_ - line1_y_ - side_ui_[displayed_right_side_.as_simple_index()].text_width;
      text1_y = line1_y_;
      text2_x = drawable_width_ - line1_y_ - right_position_text_width;
      text2_y = line2_y_;
    }

    render_text(text1_x, text1_y, side_ui_[displayed_right_side_.as_simple_index()].text_texture, side_ui_[displayed_right_side_.as_simple_index()].text_width, side_ui_[displayed_right_side_.as_simple_index()].text_height,
                border_extension_, false);
    render_text(text2_x, text2_y, right_position_text_texture, right_position_text_width, right_position_text_height, border_extension_, false);

    SDL_DestroyTexture(right_position_text_texture);
  }
  if (mouse_is_inside_window_ && duration_ > 0) {
    float target_position = static_cast<float>(mouse_x_) / static_cast<float>(window_width_) * duration_;

    const std::string target_pos_str = format_position(target_position, true);
    text_surface = TTF_RenderText_Blended(small_font_, target_pos_str.c_str(), 0, TARGET_COLOR);
    SDL_Texture* target_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    const int target_position_text_width = text_surface->w;
    const int target_position_text_height = text_surface->h;
    SDL_DestroySurface(text_surface);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 2);
    render_text(drawable_width_ - line1_y_ - target_position_text_width, drawable_height_ - line1_y_ - target_position_text_height, target_position_text_texture, target_position_text_width, target_position_text_height, border_extension_,
                false);

    SDL_DestroyTexture(target_position_text_texture);
  }

  // zoom factor
  std::string zoom_factor_str;
  const uint64_t global_zoom_factor_rounded = lrintf(view_transform_.global_zoom_factor() * 1000);
  int global_zoom_factor_trailing_zeros = (global_zoom_factor_rounded % 10) > 0 ? 0 : 1;
  global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 100) > 0 ? 0 : 1;
  global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 1000) > 0 ? 0 : 1;

  if (view_transform_.global_zoom_factor() < 1e-1 || (global_zoom_factor_trailing_zeros == 0 && global_zoom_factor_rounded < 1000)) {
    zoom_factor_str = string_sprintf("x%1.3f", view_transform_.global_zoom_factor());
  } else if (global_zoom_factor_trailing_zeros <= 1 && global_zoom_factor_rounded < 10000) {
    zoom_factor_str = string_sprintf("x%1.2f", view_transform_.global_zoom_factor());
  } else if (global_zoom_factor_trailing_zeros <= 2 && global_zoom_factor_rounded < 100000) {
    zoom_factor_str = string_sprintf("x%1.1f", view_transform_.global_zoom_factor());
  } else {
    zoom_factor_str = string_sprintf("x%1.0f", view_transform_.global_zoom_factor());
  }

  text_surface = TTF_RenderText_Blended(small_font_, zoom_factor_str.c_str(), 0, ZOOM_COLOR);
  SDL_Texture* zoom_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
  const int zoom_position_text_width = text_surface->w;
  const int zoom_position_text_height = text_surface->h;
  SDL_DestroySurface(text_surface);

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 2);

  int text_x = (mode_ == Mode::VStack) ? drawable_width_ - line1_y_ - zoom_position_text_width : line1_y_;
  int text_y = (mode_ == Mode::VStack) ? line1_y_ : drawable_height_ - line1_y_ - zoom_position_text_height;

  render_text(text_x, text_y, zoom_position_text_texture, zoom_position_text_width, zoom_position_text_height, border_extension_, false);
  SDL_DestroyTexture(zoom_position_text_texture);

  // playback speed
  std::string playback_speed_str;
  std::string playback_speed_factor_str;

  const float playback_speed = 1000000.0f * playback_.playback_speed_factor() / float(std::max(ffmpeg::frame_duration(left_frame), ffmpeg::frame_duration(right_frame)));
  const uint64_t playback_speed_rounded = lrintf(playback_speed * 1000);

  if (playback_speed_rounded < 1000) {
    playback_speed_str = string_sprintf("%1.2f", playback_speed);
  } else if (playback_speed_rounded % 1000 && playback_speed_rounded < 240000) {
    if (playback_speed_rounded % 100 && playback_speed_rounded < 60000) {
      playback_speed_str = string_sprintf("%1.2f", playback_speed);
    } else {
      playback_speed_str = string_sprintf("%1.1f", playback_speed);
    }
  } else {
    playback_speed_str = string_sprintf("%1.0f", playback_speed);
  }

  if (playback_.playback_speed_modified()) {
    if (lrintf(playback_.playback_speed_factor() * 100) < 10) {
      playback_speed_factor_str = string_sprintf("|%1.1f%%", playback_.playback_speed_factor() * 100);
    } else {
      playback_speed_factor_str = string_sprintf("|%1.0f%%", playback_.playback_speed_factor() * 100);
    }
  } else {
    playback_speed_factor_str = "";
  }

  const std::string united_playback_speed_str = string_sprintf("@%s%s", playback_speed_str.c_str(), playback_speed_factor_str.c_str());
  text_surface = TTF_RenderText_Blended(small_font_, united_playback_speed_str.c_str(), 0, PLAYBACK_SPEED_COLOR);
  SDL_Texture* playack_speed_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
  const int playack_speed_text_width = text_surface->w;
  const int playack_speed_text_height = text_surface->h;
  SDL_DestroySurface(text_surface);

  text_x = drawable_width_ / 2 - playack_speed_text_width / 2 - border_extension_;
  text_y = drawable_height_ - line1_y_ - zoom_position_text_height;

  render_text(text_x, text_y, playack_speed_text_texture, playack_speed_text_width, playack_speed_text_height, border_extension_, false);
  SDL_DestroyTexture(playack_speed_text_texture);

  // current frame / number of frames in history buffer
  text_surface = TTF_RenderText_Blended(small_font_, current_total_browsable.c_str(), 0, BUFFER_COLOR);
  SDL_Texture* current_total_browsable_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
  const int current_total_browsable_text_width = text_surface->w;
  const int current_total_browsable_text_height = text_surface->h;
  SDL_DestroySurface(text_surface);

  text_y = (mode_ == Mode::VStack) ? line1_y_ : line2_y_;

  // blink label in loop mode
  fill_rect = make_frect(drawable_width_ / 2 - current_total_browsable_text_width / 2 - border_extension_, text_y - border_extension_, current_total_browsable_text_width + double_border_extension_,
                         current_total_browsable_text_height + double_border_extension_);

  SDL_Color label_color = LOOP_OFF_LABEL_COLOR;
  int label_alpha = BACKGROUND_ALPHA;

  if (playback_.loop_mode() != Display::Loop::Off) {
    label_alpha *= 1.0 + sin(float(SDL_GetTicks()) / 180.0) * 0.6;

    switch (playback_.loop_mode()) {
      case Display::Loop::ForwardOnly: label_color = LOOP_FW_LABEL_COLOR; break;
      case Display::Loop::PingPong:    label_color = LOOP_PP_LABEL_COLOR; break;
      default: break;
    }

    timer_based_update_performed_ = true;
  }

  SDL_SetRenderDrawColor(renderer_, label_color.r, label_color.g, label_color.b, label_alpha);
  SDL_RenderFillRect(renderer_, &fill_rect);

  text_rect = make_frect(drawable_width_ / 2 - current_total_browsable_text_width / 2, text_y, current_total_browsable_text_width, current_total_browsable_text_height);
  SDL_RenderTexture(renderer_, current_total_browsable_text_texture, nullptr, &text_rect);
  SDL_DestroyTexture(current_total_browsable_text_texture);

  render_progress_dots(left_position, left_progress, true);
  render_progress_dots(right_position, right_progress, false);
}

// SDL path: render the fading center-screen message overlay when one is active.
void Display::sdl_render_message_toast() {
  SDL_FRect fill_rect;
  SDL_FRect text_rect;
  SDL_Surface* text_surface;

  if (!overlay_.pending_message().empty()) {
    overlay_.set_message_shown_at(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()));
    text_surface = TTF_RenderText_Blended(big_font_, overlay_.pending_message().c_str(), 0, TEXT_COLOR);

    if (overlay_.message_texture() != nullptr) {
      SDL_DestroyTexture(overlay_.message_texture());
    }
    overlay_.message_texture() = SDL_CreateTextureFromSurface(renderer_, text_surface);

    overlay_.message_width() = text_surface->w;
    overlay_.message_height() = text_surface->h;
    SDL_DestroySurface(text_surface);

    overlay_.clear_pending_message();
  }
  if (overlay_.message_texture() != nullptr) {
    std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    const float elapsed_s = (now - overlay_.message_shown_at()).count() / 1000.0F;
    constexpr float kHoldSeconds = 2.0F;
    constexpr float kFadeSeconds = 1.0F;
    const float keep_alpha = (elapsed_s < kHoldSeconds)
                                 ? 1.0F
                                 : std::max(sqrtf(1.0F - (elapsed_s - kHoldSeconds) / kFadeSeconds), 0.0F);

    const int mw = overlay_.message_width();
    const int mh = overlay_.message_height();
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * keep_alpha);
    fill_rect = make_frect(drawable_width_ / 2 - mw / 2 - 2, drawable_height_ / 2 - mh / 2 - 2, mw + 4, mh + 4);
    SDL_RenderFillRect(renderer_, &fill_rect);

    SDL_SetTextureAlphaMod(overlay_.message_texture(), 255 * keep_alpha);
    text_rect = make_frect(drawable_width_ / 2 - mw / 2, drawable_height_ / 2 - mh / 2, mw, mh);
    SDL_RenderTexture(renderer_, overlay_.message_texture(), nullptr, &text_rect);

    timer_based_update_performed_ = timer_based_update_performed_ || (keep_alpha > 0.0F);
  }
}

// SDL path: consume deferred save-frames / save-selected-area / crop requests now that the frame has rendered.
void Display::sdl_finalize_deferred(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (image_saver_.save_frames_requested()) {
    save_image_frames_sdl(left_frame, right_frame);
    image_saver_.clear_save_frames_request();
  }
  if (selection_.save_selected_area_requested()) {
    possibly_save_selected_area(left_frame, right_frame);
  }
  if (selection_.auto_crop_black_borders_requested()) {
    // SDL path: the post-format_converter frames delivered here are already at
    // max_width_ × max_height_ in one of RGB24 / RGB48LE / X2RGB10LE — exactly
    // what detect_black_border_crop dispatches on. Run detection directly on
    // the delivered frames; route the result through the same PendingCropRequest
    // pipeline the GPU path uses, so stacking + BACKSPACE behave identically.
    const SDL_Rect left_rect = detect_black_border_crop(left_frame);
    const SDL_Rect right_rect = detect_black_border_crop(right_frame);
    const bool left_cropped = (left_rect.w != video_width_ || left_rect.h != video_height_);
    const bool right_cropped = (right_rect.w != video_width_ || right_rect.h != video_height_);
    if (!left_cropped && !right_cropped) {
      notify_user("No black borders detected");
    } else {
      PendingCropRequest req{};
      req.per_side_rects = true;
      req.rect_left = left_rect;
      req.rect_right = right_rect;
      req.valid = true;
      req.apply_left = left_cropped;
      req.apply_right = right_cropped;
      req.right_target_index = active_right_index_;
      selection_.set_pending_crop_request(req);
    }
    selection_.cancel_auto_crop_black_borders();
  }
  if (selection_.crop_mode()) {
    possibly_apply_crop();
  }
}
