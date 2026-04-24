#include "display/display.h"
#include <SDL3_ttf/SDL_ttf.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include "analysis/metrics/metrics_calculator.h"
#include "analysis/metrics/vmaf_calculator.h"
#include "core/ffmpeg/ffmpeg.h"
#include "core/strings/string_utils.h"
#include "display_utils.h"
#include "display/gpu_renderer.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
}

// libplacebo GPU render path: build ops/overlays/text ops and hand them to GpuRenderer::render + present.
void Display::render_frame_gpu(const RenderContext& ctx, const std::string& current_total_browsable) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;
  const bool has_updated_left_frame = ctx.has_updated_left_frame;
  const bool has_updated_right_frame = ctx.has_updated_right_frame;
  const bool compare_mode = ctx.compare_mode;
  const auto& zoom_rect = ctx.zoom_rect;
  const float video_mouse_x = ctx.video_mouse_x;
  const float video_texel_clamped_mouse_x = ctx.video_texel_clamped_mouse_x;
  const int split_x = ctx.split_x;
  const int dst_zoomed_size = ctx.dst_zoomed_size;

  const bool have_rgb = gpu_run_cpu_work(ctx);
  gpu_upload_frames(ctx, have_rgb);

    // Main-view + zoom magnifier render ops share a single ops vector so
    // libplacebo gets both in one pass.
    std::vector<GpuRenderer::SideRenderOp> ops;
    ops.reserve(8);  // 2 main + up to 4 zoom (2 sides × up to 2 slices each)
    gpu_build_main_video_ops(ctx, ops);

    // Drawable-x of the split boundary inside each zoom box (-1 = don't draw).
    float zoom_left_slider_dx = -1.f;
    float zoom_right_slider_dx = -1.f;
    gpu_build_zoom_magnifier_ops(ctx, ops, zoom_left_slider_dx, zoom_right_slider_dx);

    // Build overlay list (Phase 2: primitives only, no text yet).
    std::vector<GpuRenderer::OverlayOp> overlays;

    auto push_rect = [&](float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
      GpuRenderer::OverlayOp op;
      op.dst_x0 = x0; op.dst_y0 = y0;
      op.dst_x1 = x1; op.dst_y1 = y1;
      op.color[0] = r / 255.0f;
      op.color[1] = g / 255.0f;
      op.color[2] = b / 255.0f;
      op.color[3] = a / 255.0f;
      overlays.push_back(op);
    };

    if (show_hud_) {
      // Split line — vertical white line at mouse-snapped video texel position.
      if (mode_ == Mode::Split && compare_mode) {
        const float split_drawable_x = std::round(video_texel_clamped_mouse_x * drawable_to_window_width_factor_);
        push_rect(split_drawable_x, 0, split_drawable_x + 1, static_cast<float>(drawable_height_),
                  255, 255, 255, 255);

        // Slider line within each active zoom window, placed at the mapped
        // split boundary (recorded from the zoom pass). This is typically
        // near the dst center but shifts by up to half a video pixel (×
        // dst-scale drawable pixels) so the slider sits exactly on the
        // left/right frame edge in the zoomed view.
        const float zoom_dst_y = static_cast<float>(drawable_height_ - dst_zoomed_size);
        const float zoom_dst_y1 = static_cast<float>(drawable_height_);
        if (view_transform_.zoom_left() && zoom_left_slider_dx >= 0.f) {
          const float sx = std::round(zoom_left_slider_dx);
          push_rect(sx, zoom_dst_y, sx + 1.f, zoom_dst_y1, 255, 255, 255, 255);
        }
        if (view_transform_.zoom_right() && zoom_right_slider_dx >= 0.f) {
          const float sx = std::round(zoom_right_slider_dx);
          push_rect(sx, zoom_dst_y, sx + 1.f, zoom_dst_y1, 255, 255, 255, 255);
        }
      }

      // Progress dots — alternating yellow / black strip per side showing
      // playback position, with a small outline rect at the current-frame
      // sub-range. Matches render_progress_dots() in the SDL path.
      if (duration_ > 0) {
        const float dot_size = 2.f;
        const int dot_width = std::round(drawable_to_window_width_factor_ * dot_size);
        const int dot_height = std::round(drawable_to_window_height_factor_ * dot_size);

        auto render_dots = [&](float position, float progress, bool is_top) {
          const int y_offset = is_top ? 1 : drawable_height_ - 1 - dot_height;
          const int x_position = std::round(position * drawable_width_ / duration_);
          const int x_progress = std::round(progress * drawable_width_ / duration_);

          for (int x = 0; x < x_position; x += dot_width) {
            const int x_end = std::min(x + dot_width, x_position);
            const bool yellow = (x % (2 * dot_width)) < dot_width;
            const uint8_t alpha = yellow ? static_cast<uint8_t>(BACKGROUND_ALPHA * 3 / 2) : static_cast<uint8_t>(BACKGROUND_ALPHA);
            const uint8_t r = yellow ? POSITION_COLOR.r : 0;
            const uint8_t g = yellow ? POSITION_COLOR.g : 0;
            const uint8_t b = yellow ? POSITION_COLOR.b : 0;
            push_rect(static_cast<float>(x), static_cast<float>(y_offset),
                      static_cast<float>(x_end), static_cast<float>(y_offset + dot_height),
                      r, g, b, alpha);
          }

          // Current-frame outline: 4 thin rects (top/bottom/left/right).
          const float cf_x0 = static_cast<float>(x_position);
          const float cf_y0 = static_cast<float>(is_top ? y_offset : y_offset - dot_height);
          const float cf_x1 = static_cast<float>(x_progress);
          const float cf_y1 = cf_y0 + static_cast<float>(dot_height * 2);
          const uint8_t cf_a = static_cast<uint8_t>(BACKGROUND_ALPHA * 2);
          if (cf_x1 > cf_x0 && cf_y1 > cf_y0) {
            push_rect(cf_x0, cf_y0, cf_x1, cf_y0 + 1, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, cf_a); // top
            push_rect(cf_x0, cf_y1 - 1, cf_x1, cf_y1, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, cf_a); // bottom
            push_rect(cf_x0, cf_y0, cf_x0 + 1, cf_y1, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, cf_a); // left
            push_rect(cf_x1 - 1, cf_y0, cf_x1, cf_y1, POSITION_COLOR.r, POSITION_COLOR.g, POSITION_COLOR.b, cf_a); // right
          }
        };

        const float left_position = ffmpeg::pts_in_secs(left_frame);
        const float right_position = ffmpeg::pts_in_secs(right_frame);
        const float left_progress = left_position + ffmpeg::frame_duration_in_secs(left_frame);
        const float right_progress = right_position + ffmpeg::frame_duration_in_secs(right_frame);
        render_dots(left_position, left_progress, true);
        render_dots(right_position, right_progress, false);
      }
    }

    // Build text overlays — file labels, position times, zoom factor, etc.
    // Each TTF_RenderText_Blended surface is kept alive through the render()
    // call; destroyed immediately after.
    std::vector<GpuRenderer::TextOverlayOp> text_ops;
    std::vector<SDL_Surface*> text_surfaces; // owns the per-frame surfaces

    if (show_hud_) {
      enum class TextAlign { Left, Right };

      // Render `text` to an RGBA surface, bake a left-edge alpha fade for any
      // portion that would exceed `max_text_width_`, and push background +
      // text overlay ops.  Matches the SDL `render_text` clip-and-fade logic:
      // the END of long file paths stays visible, the beginning fades out.
      // Returns the surface's (w, h) (before clipping).
      auto push_text = [&](const std::string& text, TTF_Font* font, SDL_Color color,
                            int x, int y, TextAlign align,
                            uint8_t bg_r = 0, uint8_t bg_g = 0, uint8_t bg_b = 0,
                            uint8_t bg_a = BACKGROUND_ALPHA) -> std::pair<int, int> {
        if (text.empty()) return {0, 0};
        SDL_Surface* raw = TTF_RenderText_Blended(font, text.c_str(), 0, color);
        if (!raw) return {0, 0};
        SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!rgba) return {0, 0};
        text_surfaces.push_back(rgba);

        const int clip_amount = std::max((rgba->w + double_border_extension_) - max_text_width_, 0);
        const int gradient = std::min(clip_amount, 24);

        // Bake alpha gradient into source pixels: alpha=0 for the clipped-out
        // left portion, smoothly ramping to original alpha over `gradient` px.
        if (clip_amount > 0) {
          uint8_t* pixels = static_cast<uint8_t*>(rgba->pixels);
          const int pitch = rgba->pitch;
          for (int py = 0; py < rgba->h; ++py) {
            uint8_t* row = pixels + py * pitch;
            const int zero_end = std::min(clip_amount, rgba->w);
            for (int px = 0; px < zero_end; ++px) {
              row[px * 4 + 3] = 0;  // RGBA32: A is the 4th byte
            }
            if (gradient > 0) {
              const int grad_end = std::min(clip_amount + gradient, rgba->w);
              for (int px = clip_amount; px < grad_end; ++px) {
                const int ramp = px - clip_amount;
                row[px * 4 + 3] = static_cast<uint8_t>((row[px * 4 + 3] * ramp) / gradient);
              }
            }
          }
        }

        // Position the texture so the visible (unclipped) portion lands at
        // the requested (x, y) for left-aligned text, or ends at (x + w)
        // for right-aligned text.  Clipped (transparent) pixels spill off
        // to the side; libplacebo ignores them.
        const int visible_w = rgba->w - clip_amount;
        int dst_x_left;   // visible-region left edge in FBO coords
        int tex_x;        // texture's own top-left in FBO coords
        if (align == TextAlign::Left) {
          dst_x_left = x;
          tex_x = x - clip_amount;
        } else {
          dst_x_left = x + clip_amount;
          tex_x = x;
        }

        // Background (hard edges, spans only the visible text region).
        push_rect(static_cast<float>(dst_x_left - border_extension_),
                  static_cast<float>(y - border_extension_),
                  static_cast<float>(dst_x_left + visible_w + border_extension_),
                  static_cast<float>(y + rgba->h + border_extension_),
                  bg_r, bg_g, bg_b, bg_a);

        GpuRenderer::TextOverlayOp t{};
        t.rgba_data = rgba->pixels;
        t.width = rgba->w;
        t.height = rgba->h;
        t.stride = rgba->pitch;
        t.dst_x = static_cast<float>(tex_x);
        t.dst_y = static_cast<float>(y);
        t.alpha = 1.0f;
        text_ops.push_back(t);
        return {rgba->w, rgba->h};
      };

      // File-name labels (top-left / top-right, matching SDL path layout).
      // Pick label string based on displayed_*_side_ so the `s` swap key
      // flips labels together with the video content.
      const float left_position = ffmpeg::pts_in_secs(left_frame);
      const float right_position = ffmpeg::pts_in_secs(right_frame);

      auto label_for_side = [&](Side s) -> std::string {
        return s.is_left() ? left_file_name_
                           : format_right_file_label(left_file_name_, right_file_name_, active_right_index_ + 1);
      };
      const std::string left_label = label_for_side(displayed_left_side_);
      const std::string right_label = label_for_side(displayed_right_side_);

      if (show_left_) {
        const std::string left_pic(1, av_get_picture_type_char(left_frame->pict_type));
        const std::string left_pos_str = format_position(left_position, true) + " " + left_pic + format_position_difference(left_position, right_position);

        if (mode_ == Mode::VStack) {
          push_text(left_pos_str, small_font_, POSITION_COLOR, line1_y_, line1_y_, TextAlign::Left);
          push_text(left_label, small_font_, TEXT_COLOR, line1_y_, line2_y_, TextAlign::Left);
        } else {
          push_text(left_label, small_font_, TEXT_COLOR, line1_y_, line1_y_, TextAlign::Left);
          push_text(left_pos_str, small_font_, POSITION_COLOR, line1_y_, line2_y_, TextAlign::Left);
        }
      }
      if (show_right_) {
        const std::string right_pic(1, av_get_picture_type_char(right_frame->pict_type));
        const std::string right_pos_str = format_position(right_position, true) + " " + right_pic + format_position_difference(right_position, left_position);

        // Pre-measure text widths for right-alignment in non-VStack modes.
        int w_label = 0, h_label = 0;
        int w_pos = 0, h_pos = 0;
        TTF_GetStringSize(small_font_, right_label.c_str(), 0, &w_label, &h_label);
        TTF_GetStringSize(small_font_, right_pos_str.c_str(), 0, &w_pos, &h_pos);

        if (mode_ == Mode::VStack) {
          push_text(right_label, small_font_, TEXT_COLOR, line1_y_, drawable_height_ - line2_y_ - h_label, TextAlign::Left);
          push_text(right_pos_str, small_font_, POSITION_COLOR, line1_y_, drawable_height_ - line1_y_ - h_pos, TextAlign::Left);
        } else {
          push_text(right_label, small_font_, TEXT_COLOR, drawable_width_ - line1_y_ - w_label, line1_y_, TextAlign::Right);
          push_text(right_pos_str, small_font_, POSITION_COLOR, drawable_width_ - line1_y_ - w_pos, line2_y_, TextAlign::Right);
        }
      }

      // Video/UI FPS counters (persistent when show_fps_). Positioned in the
      // bottom-center-right area, paired with a small gap between them.
      if (show_fps_) {
        const std::string vid_str = string_sprintf("Vid %.1f", current_video_fps_);
        const std::string ui_str = string_sprintf("UI %.1f", current_ui_fps_);

        int vid_w = 0, vid_h = 0, ui_w = 0, ui_h = 0;
        TTF_GetStringSize(small_font_, vid_str.c_str(), 0, &vid_w, &vid_h);
        TTF_GetStringSize(small_font_, ui_str.c_str(), 0, &ui_w, &ui_h);

        const int gap = double_border_extension_ * 2;
        const int pair_w = vid_w + double_border_extension_ + gap + ui_w + double_border_extension_;
        const int pair_anchor_x = drawable_width_ * 2 / 3; // ~67% across
        const int vid_x = pair_anchor_x - pair_w / 2 + border_extension_;
        const int ui_x = vid_x + vid_w + double_border_extension_ + gap;
        const int fps_y = drawable_height_ - line1_y_ - std::max(vid_h, ui_h);

        push_text(vid_str, small_font_, FPS_VIDEO_COLOR, vid_x, fps_y, TextAlign::Left);
        push_text(ui_str, small_font_, FPS_UI_COLOR, ui_x, fps_y, TextAlign::Left);

        // Play state badge, mirrored at ~33% across so it visually balances
        // the FPS readout on the right.
        //
        // The state itself is derived by Display::get_play_state() so that
        // external introspection (e.g. the debug input socket) and the HUD
        // can't drift. The HUD adds the bracket decoration and picks a color.
        const PlayState play_state = get_play_state();
        const std::string state_str = std::string("[") + play_state_label(play_state) + "]";
        SDL_Color state_color = TEXT_COLOR;
        switch (play_state) {
          case PlayState::Seek:
            state_color = ZOOM_COLOR;
            break;
          case PlayState::LoopForward:
            state_color = LOOP_FW_LABEL_COLOR;
            break;
          case PlayState::LoopPingPong:
            state_color = LOOP_PP_LABEL_COLOR;
            break;
          case PlayState::Play:
            state_color = POSITION_COLOR;
            break;
          case PlayState::Pause:
            state_color = TEXT_COLOR;
            break;
        }

        int state_w = 0, state_h = 0;
        TTF_GetStringSize(small_font_, state_str.c_str(), 0, &state_w, &state_h);

        const int left_anchor_x = drawable_width_ / 3;  // ~33% across (mirror of 2/3)
        const int state_x = left_anchor_x - state_w / 2 + border_extension_;
        const int left_y = drawable_height_ - line1_y_ - state_h;

        push_text(state_str, small_font_, state_color, state_x, left_y, TextAlign::Left);
      }

      // Zoom factor — bottom-left (top-right in VStack). Precision varies
      // with the value to avoid noisy fractional digits at common zooms.
      std::string zoom_factor_str;
      {
        const uint64_t zr = lrintf(view_transform_.global_zoom_factor() * 1000);
        int tz = ((zr % 10) > 0 ? 0 : 1) + ((zr % 100) > 0 ? 0 : 1) + ((zr % 1000) > 0 ? 0 : 1);
        if (view_transform_.global_zoom_factor() < 1e-1 || (tz == 0 && zr < 1000)) {
          zoom_factor_str = string_sprintf("x%1.3f", view_transform_.global_zoom_factor());
        } else if (tz <= 1 && zr < 10000) {
          zoom_factor_str = string_sprintf("x%1.2f", view_transform_.global_zoom_factor());
        } else if (tz <= 2 && zr < 100000) {
          zoom_factor_str = string_sprintf("x%1.1f", view_transform_.global_zoom_factor());
        } else {
          zoom_factor_str = string_sprintf("x%1.0f", view_transform_.global_zoom_factor());
        }
      }
      {
        int zw = 0, zh = 0;
        TTF_GetStringSize(small_font_, zoom_factor_str.c_str(), 0, &zw, &zh);
        const int zx = (mode_ == Mode::VStack) ? drawable_width_ - line1_y_ - zw : line1_y_;
        const int zy = (mode_ == Mode::VStack) ? line1_y_ : drawable_height_ - line1_y_ - zh;
        push_text(zoom_factor_str, small_font_, ZOOM_COLOR, zx, zy, TextAlign::Left,
                  0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * 2));
      }

      // Playback speed — bottom-center. "@<speed>" with optional "|<pct>%"
      // when a manual speed level has been applied.
      {
        std::string speed_str, speed_factor_str;
        const float playback_speed = 1000000.0f * playback_.playback_speed_factor() /
                                      float(std::max(ffmpeg::frame_duration(left_frame), ffmpeg::frame_duration(right_frame)));
        const uint64_t ps_r = lrintf(playback_speed * 1000);
        if (ps_r < 1000) {
          speed_str = string_sprintf("%1.2f", playback_speed);
        } else if (ps_r % 1000 && ps_r < 240000) {
          if (ps_r % 100 && ps_r < 60000) speed_str = string_sprintf("%1.2f", playback_speed);
          else speed_str = string_sprintf("%1.1f", playback_speed);
        } else {
          speed_str = string_sprintf("%1.0f", playback_speed);
        }
        if (playback_.playback_speed_modified()) {
          if (lrintf(playback_.playback_speed_factor() * 100) < 10)
            speed_factor_str = string_sprintf("|%1.1f%%", playback_.playback_speed_factor() * 100);
          else
            speed_factor_str = string_sprintf("|%1.0f%%", playback_.playback_speed_factor() * 100);
        }
        const std::string united = string_sprintf("@%s%s", speed_str.c_str(), speed_factor_str.c_str());
        int sw = 0, sh = 0;
        TTF_GetStringSize(small_font_, united.c_str(), 0, &sw, &sh);
        const int sx = drawable_width_ / 2 - sw / 2 - border_extension_;
        const int sy = drawable_height_ - line1_y_ - sh;
        push_text(united, small_font_, PLAYBACK_SPEED_COLOR, sx, sy, TextAlign::Left,
                  0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * 2));
      }

      // Current frame / total browsable — top-center. In loop mode the
      // background blinks with a mode-specific color.
      if (!current_total_browsable.empty()) {
        int cw = 0, ch = 0;
        TTF_GetStringSize(small_font_, current_total_browsable.c_str(), 0, &cw, &ch);
        const int cx = drawable_width_ / 2 - cw / 2;
        const int cy = (mode_ == Mode::VStack) ? line1_y_ : line2_y_;

        SDL_Color bg_color = LOOP_OFF_LABEL_COLOR;
        int bg_alpha = BACKGROUND_ALPHA;
        if (playback_.loop_mode() != Loop::Off) {
          bg_alpha = static_cast<int>(bg_alpha * (1.0 + std::sin(float(SDL_GetTicks()) / 180.0) * 0.6));
          bg_alpha = clamp_range(bg_alpha, 0, 255);
          switch (playback_.loop_mode()) {
            case Loop::ForwardOnly: bg_color = LOOP_FW_LABEL_COLOR; break;
            case Loop::PingPong:    bg_color = LOOP_PP_LABEL_COLOR; break;
            default: break;
          }
          timer_based_update_performed_ = true;
        }
        push_text(current_total_browsable, small_font_, BUFFER_COLOR, cx, cy, TextAlign::Left,
                  bg_color.r, bg_color.g, bg_color.b, static_cast<uint8_t>(bg_alpha));
      }

      // Target seek position — bottom-right, only when the cursor is in the
      // window and the content is seekable.
      if (mouse_is_inside_window_ && duration_ > 0) {
        const float target_position = static_cast<float>(mouse_x_) / static_cast<float>(window_width_) * duration_;
        const std::string target_str = format_position(target_position, true);
        int tw = 0, th = 0;
        TTF_GetStringSize(small_font_, target_str.c_str(), 0, &tw, &th);
        const int tx = drawable_width_ - line1_y_ - tw;
        const int ty = drawable_height_ - line1_y_ - th;
        push_text(target_str, small_font_, TARGET_COLOR, tx, ty, TextAlign::Right,
                  0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * 2));
      }
    }

    // Message toast — fading center-screen notification. Independent of
    // show_hud_.  On arrival, move pending_message_ to the "active" slot so
    // it persists through the fade even after pending_message_ is cleared.
    if (!overlay_.pending_message().empty()) {
      overlay_.set_gpu_active_message(overlay_.pending_message());
      overlay_.clear_pending_message();
      overlay_.set_message_shown_at(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()));
    }
    if (!overlay_.gpu_active_message().empty()) {
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
      const float elapsed_s = (now - overlay_.message_shown_at()).count() / 1000.0f;
      constexpr float kHoldSeconds = 3.0f;
      constexpr float kFadeSeconds = 0.5f;
      float keep_alpha;
      if (elapsed_s < kHoldSeconds) {
        keep_alpha = 1.0f;
      } else {
        keep_alpha = std::max(std::sqrt(1.0f - (elapsed_s - kHoldSeconds) / kFadeSeconds), 0.0f);
      }
      if (keep_alpha <= 0.0f) {
        overlay_.clear_gpu_active_message();
      } else {
        SDL_Surface* raw = TTF_RenderText_Blended(big_font_, overlay_.gpu_active_message().c_str(), 0, TEXT_COLOR);
        if (raw) {
          SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
          SDL_DestroySurface(raw);
          if (rgba) {
            // libplacebo's PL_OVERLAY_NORMAL ignores per-part color/alpha, so
            // bake the fade into the source alpha channel on CPU.
            if (keep_alpha < 1.0f) {
              uint8_t* pixels = static_cast<uint8_t*>(rgba->pixels);
              const int pitch = rgba->pitch;
              for (int py = 0; py < rgba->h; ++py) {
                uint8_t* row = pixels + py * pitch;
                for (int px = 0; px < rgba->w; ++px) {
                  row[px * 4 + 3] = static_cast<uint8_t>(row[px * 4 + 3] * keep_alpha);
                }
              }
            }
            text_surfaces.push_back(rgba);
            const int mx = drawable_width_ / 2 - rgba->w / 2;
            const int my = drawable_height_ / 2 - rgba->h / 2;
            push_rect(static_cast<float>(mx - 2), static_cast<float>(my - 2),
                      static_cast<float>(mx + rgba->w + 2), static_cast<float>(my + rgba->h + 2),
                      0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * keep_alpha));
            GpuRenderer::TextOverlayOp t{};
            t.rgba_data = rgba->pixels;
            t.width = rgba->w;
            t.height = rgba->h;
            t.stride = rgba->pitch;
            t.dst_x = static_cast<float>(mx);
            t.dst_y = static_cast<float>(my);
            t.alpha = 1.0f;  // applied via CPU alpha-multiply above
            text_ops.push_back(t);
          }
        }
        timer_based_update_performed_ = true;
      }
    }

    // Selection / crop rect — shown whenever a selection is in progress,
    // independent of show_hud_ (matching draw_selection_rect in the SDL path).
    if (selection_.state() == SelectionState::Started) {
      auto push_selection_rect = [&](const SDL_FRect& r, uint8_t r_val, uint8_t g_val, uint8_t b_val, int alpha_divider = 1) {
        push_rect(r.x, r.y, r.x + r.w, r.y + r.h,
                  r_val / 2, g_val / 2, b_val / 2,
                  static_cast<uint8_t>(128 / alpha_divider));

        const uint8_t border_a = static_cast<uint8_t>(255 / alpha_divider);
        push_rect(r.x, r.y,                r.x + r.w, r.y + 1,         r_val, g_val, b_val, border_a); // top
        push_rect(r.x, r.y + r.h - 1,      r.x + r.w, r.y + r.h,       r_val, g_val, b_val, border_a); // bottom
        push_rect(r.x, r.y,                r.x + 1,   r.y + r.h,       r_val, g_val, b_val, border_a); // left
        push_rect(r.x + r.w - 1, r.y,      r.x + r.w, r.y + r.h,       r_val, g_val, b_val, border_a); // right
      };

      SDL_Rect selection_rect = selection_.get_left_selection_rect(video_width_, video_height_);
      SDL_FRect drawable_rect = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(selection_rect, zoom_rect));

      const bool crop_mode = selection_.crop_mode();
      const CropTargetSide side = selection_.crop_target_side();

      if (mode_ == Mode::Split) {
        if (crop_mode) {
          switch (side) {
            case CropTargetSide::Right: push_selection_rect(drawable_rect, 128, 128, 255); break;
            case CropTargetSide::Both:  push_selection_rect(drawable_rect, 255, 255, 255); break;
            default:                    push_selection_rect(drawable_rect, 255, 128, 128); break;
          }
        } else {
          push_selection_rect(drawable_rect, 255, 255, 255);
        }
      } else {
        if (!crop_mode || side == CropTargetSide::Left || side == CropTargetSide::Both) {
          push_selection_rect(drawable_rect, 255, 128, 128);
        } else {
          push_selection_rect(drawable_rect, 96, 96, 96, 3);
        }

        if (mode_ == Mode::HStack) selection_rect.x += video_width_;
        else if (mode_ == Mode::VStack) selection_rect.y += video_height_;

        drawable_rect = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(selection_rect, zoom_rect));
        if (!crop_mode || side == CropTargetSide::Right || side == CropTargetSide::Both) {
          push_selection_rect(drawable_rect, 128, 128, 255);
        } else {
          push_selection_rect(drawable_rect, 96, 96, 96, 3);
        }
      }
    }

    // Live quality metrics overlay (PSNR / SSIM / VMAF) — rendered at the
    // top-right corner when show_quality_metrics_ is on. Suppressed while
    // a full-screen panel is visible (they'd otherwise peek through).
    if (show_quality_metrics_ && !overlay_.show_help() && !overlay_.show_metadata()) {
      const std::string vmaf_display = (last_vmaf_ == "n/a") ? std::string("n/a (pause to compute)") : last_vmaf_;
      const std::array<std::string, 3> metric_lines = {
          std::string("PSNR: ") + last_psnr_ + " dB",
          std::string("SSIM: ") + last_ssim_,
          std::string("VMAF: ") + vmaf_display,
      };

      std::array<SDL_Surface*, 3> metric_surfaces{{nullptr, nullptr, nullptr}};
      int max_w = 0;
      int total_h = 0;
      const int metric_line_spacing = 4;
      for (size_t i = 0; i < metric_lines.size(); ++i) {
        SDL_Surface* raw = TTF_RenderText_Blended(small_font_, metric_lines[i].c_str(), 0, POSITION_COLOR);
        if (!raw) continue;
        SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(raw);
        if (!rgba) continue;
        metric_surfaces[i] = rgba;
        text_surfaces.push_back(rgba);
        max_w = std::max(max_w, rgba->w);
        total_h += rgba->h + (i + 1 < metric_lines.size() ? metric_line_spacing : 0);
      }

      if (max_w > 0) {
        const int padding = border_extension_ * 2;
        const int right_margin = HELP_TEXT_HORIZONTAL_MARGIN;
        const int top_margin = line2_y_ * 2;

        const float bg_x = static_cast<float>(drawable_width_ - right_margin - max_w - padding * 2);
        const float bg_y = static_cast<float>(top_margin);
        const float bg_w = static_cast<float>(max_w + padding * 2);
        const float bg_h = static_cast<float>(total_h + padding * 2);
        push_rect(bg_x, bg_y, bg_x + bg_w, bg_y + bg_h, 0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * 2));

        float y = bg_y + padding;
        for (size_t i = 0; i < metric_lines.size(); ++i) {
          SDL_Surface* s = metric_surfaces[i];
          if (!s) continue;
          GpuRenderer::TextOverlayOp t{};
          t.rgba_data = s->pixels;
          t.width = s->w;
          t.height = s->h;
          t.stride = s->pitch;
          t.dst_x = bg_x + padding;
          t.dst_y = y;
          t.alpha = 1.0f;
          text_ops.push_back(t);
          y += s->h + metric_line_spacing;
        }
      }
    }

    // Help and metadata overlays — full-screen panels with multi-line text
    // honoring the scroll offset. Pushed after all other HUD so they layer
    // on top.  Because libplacebo draws primitive-overlays before text-
    // overlays in a single render pass, the full-screen dim rect would end
    // up UNDER the HUD text (wrong order). Simplest fix: suppress HUD text
    // while a full-screen panel is visible.  The panel's own background
    // fully dims the area where HUD would have been, so the result is
    // visually equivalent to the SDL path.
    if (overlay_.show_help() || overlay_.show_metadata()) {
      // Clear previously-accumulated text overlays (HUD labels, positions,
      // FPS, etc.) so they don't poke through the panel.
      text_ops.clear();
      for (SDL_Surface* s : text_surfaces) SDL_DestroySurface(s);
      text_surfaces.clear();
      overlays.clear();

      // Full-screen semi-transparent black background.
      push_rect(0, 0, static_cast<float>(drawable_width_), static_cast<float>(drawable_height_),
                0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * 3 / 2));

      if (overlay_.show_help()) {
        int y = overlay_.help_scroll_offset();
        for (SDL_Surface* s : overlay_.help_surfaces()) {
          if (!s) continue;
          if (y + s->h > 0 && y < drawable_height_) {
            GpuRenderer::TextOverlayOp t{};
            t.rgba_data = s->pixels;
            t.width = s->w;
            t.height = s->h;
            t.stride = s->pitch;
            t.dst_x = static_cast<float>(HELP_TEXT_HORIZONTAL_MARGIN);
            t.dst_y = static_cast<float>(y);
            t.alpha = 1.0f;
            text_ops.push_back(t);
          }
          y += s->h + HELP_TEXT_LINE_SPACING;
        }
      } else if (overlay_.show_metadata()) {
        metadata_panel_.ensure_current(swap_left_right_,
                                       displayed_left_side_.is_left(),
                                       displayed_right_side_.is_right(),
                                       small_font_, big_font_, renderer_,
                                       gpu_renderer_active_, drawable_width_);

        const int table_width = drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2;
        const int table_x = HELP_TEXT_HORIZONTAL_MARGIN;

        int y;
        const int meta_total_h = metadata_panel_.total_height();
        if (mode_ == Mode::VStack && meta_total_h < drawable_height_ / 2) {
          y = (drawable_height_ / 2 - meta_total_h) / 2;
        } else if (mode_ != Mode::VStack && meta_total_h < drawable_height_) {
          y = (drawable_height_ - meta_total_h) / 2;
        } else {
          y = metadata_panel_.scroll_offset() + 10;
        }

        for (SDL_Surface* s : metadata_panel_.surfaces()) {
          if (!s) continue;
          const int x_offset = (table_width - s->w) / 2;
          if (y + s->h > 0 && y < drawable_height_) {
            GpuRenderer::TextOverlayOp t{};
            t.rgba_data = s->pixels;
            t.width = s->w;
            t.height = s->h;
            t.stride = s->pitch;
            t.dst_x = static_cast<float>(table_x + x_offset);
            t.dst_y = static_cast<float>(y);
            t.alpha = 1.0f;
            text_ops.push_back(t);
          }
          y += s->h + HELP_TEXT_LINE_SPACING;
        }
      }
    }

    if (gpu_renderer_.render(ops.data(), static_cast<int>(ops.size()),
                              overlays.data(), static_cast<int>(overlays.size()),
                              text_ops.data(), static_cast<int>(text_ops.size()),
                              nullptr)) {
      gpu_renderer_.present();
    }

    // Capture the OSD while text_surfaces are still alive (TextOverlayOp
    // pixel pointers alias them), then destroy surfaces.
    AVFramePtr osd_frame = gpu_capture_osd(have_rgb, ops, overlays, text_ops);
    for (SDL_Surface* s : text_surfaces) SDL_DestroySurface(s);

    gpu_finalize_deferred(ctx, have_rgb, osd_frame.get());
}

// Convert YUV→RGB on demand and run CPU-side features (pixel inspector, similarity metrics, live quality). Returns whether the RGB cache is populated.
bool Display::gpu_run_cpu_work(const RenderContext& ctx) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;
  const auto& zoom_rect = ctx.zoom_rect;

  // Features that still need CPU pixel access drive an on-demand YUV→RGB
  // conversion. Skipped entirely when none are active to keep the pipeline
  // GPU-fast. `show_quality_metrics_` is gated on `!playback_.play()` to match
  // the metric-compute branch below — otherwise we'd pay the full-resolution
  // sws_scale cost on every played frame without using the result.
  const bool need_rgb = diff_processor_.subtraction_mode() || print_mouse_position_and_color_ ||
                        print_image_similarity_metrics_ || (show_quality_metrics_ && !playback_.play()) ||
                        selection_.save_selected_area_requested() || selection_.auto_crop_black_borders_requested() ||
                        image_saver_.save_frames_requested();
  bool have_rgb = false;
  if (need_rgb) {
    have_rgb = rgb_cache_.ensure(left_frame, right_frame, video_width_, video_height_, requires_10_bpc());
  }

  // Pixel inspector + similarity metrics consume RGB frames and run before
  // any visual update so a successful key press gets immediate feedback.
  if (have_rgb) {
    const Vector2D mouse_video_pos = view_transform_.window_to_video_position(mouse_x_, mouse_y_, zoom_rect);
    const int mouse_video_x = mouse_video_pos.x();
    const int mouse_video_y = mouse_video_pos.y();

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

        auto original_dims = [&](const AVFrame* frame) -> std::pair<int, int> {
          const int ow = get_metadata_int_value(frame, "original_width", frame->width);
          const int oh = get_metadata_int_value(frame, "original_height", frame->height);
          return {ow, oh};
        };
        const auto od_left = original_dims(left_frame);
        const auto od_right = original_dims(right_frame);

        const bool is_10bpc = requires_10_bpc();
        const auto print_side = [&](const char* label, const AVFrame* frame, const std::pair<int, int>& od) {
          std::cout << label << " "
                    << string_sprintf("[%4d,%4d]", pixel_video_x * od.first / video_width_, pixel_video_y * od.second / video_height_)
                    << ", "
                    << MetricsCalculator::get_and_format_rgb_yuv_pixel(frame->data[0], frame->linesize[0], frame, pixel_video_x, pixel_video_y, is_10bpc);
        };
        print_side("Left: ", rgb_cache_.left(), od_left);
        std::cout << " - ";
        print_side("Right:", rgb_cache_.right(), od_right);
        std::cout << std::endl;
      }
      print_mouse_position_and_color_ = false;
    }

    if (print_image_similarity_metrics_) {
      SDL_Rect roi = get_visible_roi_in_single_frame_coordinates();
      if (roi.w <= 0 || roi.h <= 0) {
        std::cerr << "ROI is empty, skipping metrics calculation" << std::endl;
      } else {
        SDL_Rect effective_roi_left{}, effective_roi_right{};
        AVFrame* left_crop = crop_rgb_frame(rgb_cache_.left(), roi, &effective_roi_left);
        AVFrame* right_crop = crop_rgb_frame(rgb_cache_.right(), roi, &effective_roi_right);
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
              (crop_width < video_width_ || crop_height < video_height_)
                  ? string_sprintf("  (%d,%d)-(%d,%d)", effective_roi_left.x, effective_roi_left.y, effective_roi_left.x + crop_width - 1, effective_roi_left.y + crop_height - 1)
                  : "";

          std::cout << string_sprintf("Metrics: [%s|%s] PSNR(%s), SSIM(%s), VMAF(%s)%s",
                                      format_position(ffmpeg::pts_in_secs(left_frame), false).c_str(),
                                      format_position(ffmpeg::pts_in_secs(right_frame), false).c_str(),
                                      psnr.c_str(), ssim.c_str(), vmaf.c_str(), roi_str.c_str())
                    << std::endl;

          delete[] left_gray;
          delete[] right_gray;
        }
        if (left_crop) av_frame_free(&left_crop);
        if (right_crop) av_frame_free(&right_crop);
      }
      print_image_similarity_metrics_ = false;
    }

    // Live on-screen quality metrics (rendered further down). Paused-only —
    // PSNR/SSIM/VMAF are expensive and running them during playback wastes
    // CPU on transient frame pairs the user isn't inspecting. When paused,
    // the pair is stable and all three values can be computed and held.
    if (show_quality_metrics_ && !playback_.play() && video_width_ > 0 && video_height_ > 0) {
      float* left_gray = MetricsCalculator::rgb_to_grayscale(rgb_cache_.left()->data[0], rgb_cache_.left()->linesize[0], video_width_, video_height_, requires_10_bpc());
      float* right_gray = MetricsCalculator::rgb_to_grayscale(rgb_cache_.right()->data[0], rgb_cache_.right()->linesize[0], video_width_, video_height_, requires_10_bpc());
      last_psnr_ = MetricsCalculator::compute_psnr(left_gray, right_gray, video_width_, video_height_);
      last_ssim_ = MetricsCalculator::compute_ssim(left_gray, right_gray, video_width_, video_height_);
      delete[] left_gray;
      delete[] right_gray;

      if (left_frame->pts != last_vmaf_left_pts_ || right_frame->pts != last_vmaf_right_pts_) {
        last_vmaf_ = VMAFCalculator::instance().compute(rgb_cache_.left(), rgb_cache_.right());
        last_vmaf_left_pts_ = left_frame->pts;
        last_vmaf_right_pts_ = right_frame->pts;
      }
    }

    // Auto-crop black borders: scan each cached RGB frame independently and
    // submit a per-side crop request through the existing crop pipeline so it
    // stacks in crop_history_ and is clearable via BACKSPACE.
    if (selection_.auto_crop_black_borders_requested()) {
      const SDL_Rect left_rect = detect_black_border_crop(rgb_cache_.left());
      const SDL_Rect right_rect = detect_black_border_crop(rgb_cache_.right());
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
  } else if (need_rgb) {
    // One-shot keys still need to be cleared so they don't re-fire next frame.
    if (print_mouse_position_and_color_) print_mouse_position_and_color_ = false;
    if (print_image_similarity_metrics_) print_image_similarity_metrics_ = false;
  }

  return have_rgb;
}

// Upload left/right frames (or the RGB-diff shell in subtraction mode) to the GpuRenderer when their keys change.
void Display::gpu_upload_frames(const RenderContext& ctx, const bool have_rgb) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;

  // Upload frames only when they've actually changed (matches SDL path logic).
  const bool gpu_subtraction = diff_processor_.subtraction_mode() && have_rgb;
  const bool right_needs_update = input_received_ || ctx.has_updated_right_frame || (gpu_subtraction && ctx.has_updated_left_frame);

  if (input_received_ || ctx.has_updated_left_frame) {
    gpu_renderer_.upload_frame(0, left_frame);
  }
  if (right_needs_update) {
    if (gpu_subtraction) {
      // Compute the RGB diff into diff_buffer_ and hand it to libplacebo via
      // a reusable AVFrame shell. When RGB conversion fails we fall back to
      // the normal YUV upload below so the video stays visible.
      std::array<uint8_t*, 3> rgb_l_planes{rgb_cache_.left()->data[0], nullptr, nullptr};
      std::array<uint8_t*, 3> rgb_r_planes{rgb_cache_.right()->data[0], nullptr, nullptr};
      std::array<size_t, 3> rgb_l_pitches{static_cast<size_t>(rgb_cache_.left()->linesize[0]), 0, 0};
      std::array<size_t, 3> rgb_r_pitches{static_cast<size_t>(rgb_cache_.right()->linesize[0]), 0, 0};
      diff_processor_.update_difference(rgb_l_planes, rgb_l_pitches, rgb_r_planes, rgb_r_pitches, 0);

      AVFrame* shell = diff_processor_.ensure_diff_upload_frame();
      shell->format = rgb_cache_.right()->format;
      shell->width = video_width_;
      shell->height = video_height_;
      shell->data[0] = diff_processor_.diff_buffer();
      for (int i = 1; i < AV_NUM_DATA_POINTERS; ++i) shell->data[i] = nullptr;
      shell->linesize[0] = static_cast<int>(diff_processor_.diff_pitches()[0]);
      for (int i = 1; i < AV_NUM_DATA_POINTERS; ++i) shell->linesize[i] = 0;
      shell->colorspace = rgb_cache_.right()->colorspace;
      shell->color_range = rgb_cache_.right()->color_range;
      gpu_renderer_.upload_frame(1, shell);
    } else {
      gpu_renderer_.upload_frame(1, right_frame);
    }
  }
}

// Build the main-view render ops for Split / HStack / VStack modes.
void Display::gpu_build_main_video_ops(const RenderContext& ctx, std::vector<GpuRenderer::SideRenderOp>& ops) {
  const bool compare_mode = ctx.compare_mode;
  (void)compare_mode;
  const auto& zoom_rect = ctx.zoom_rect;
  const int split_x = ctx.split_x;

  auto push_op = [&](int side, int src_x, int src_y, int src_w, int src_h, const SDL_Rect& video_quad) {
    const SDL_FRect screen_rect = video_rect_to_drawable_transform(view_transform_.video_to_zoom_space(video_quad, zoom_rect));
    GpuRenderer::SideRenderOp op{};
    op.side = side;
    op.src_x0 = static_cast<float>(src_x);
    op.src_y0 = static_cast<float>(src_y);
    op.src_x1 = static_cast<float>(src_x + src_w);
    op.src_y1 = static_cast<float>(src_y + src_h);
    op.dst_x0 = screen_rect.x;
    op.dst_y0 = screen_rect.y;
    op.dst_x1 = screen_rect.x + screen_rect.w;
    op.dst_y1 = screen_rect.y + screen_rect.h;
    ops.push_back(op);
  };

  if (!show_left_ && !show_right_) return;

  const int right_x_offset = (mode_ == Mode::HStack) ? video_width_ : 0;
  const int right_y_offset = (mode_ == Mode::VStack) ? video_height_ : 0;

  if (mode_ == Mode::Split) {
    // In Split mode, render the right video to its FULL area first (stable
    // target rect, independent of split position), then paint the left video
    // on top clipped at split_x. This keeps the right video's rendered pixels
    // stable while the split line moves.
    if (show_right_) {
      const SDL_Rect video_quad_right = {0, 0, video_width_, video_height_};
      push_op(1, 0, 0, video_width_, video_height_, video_quad_right);
    }
    if (show_left_ && split_x > 0) {
      const SDL_Rect video_quad_left = {0, 0, split_x, video_height_};
      push_op(0, 0, 0, split_x, video_height_, video_quad_left);
    }
  } else {
    // HStack / VStack: sides occupy disjoint screen areas; order doesn't matter.
    if (show_left_) {
      const SDL_Rect video_quad_left = {0, 0, video_width_, video_height_};
      push_op(0, 0, 0, video_width_, video_height_, video_quad_left);
    }
    if (show_right_) {
      const SDL_Rect video_quad_right = {right_x_offset, right_y_offset, video_width_, video_height_};
      push_op(1, 0, 0, video_width_, video_height_, video_quad_right);
    }
  }
}

// Zoom magnifier windows (bottom-left / bottom-right corners). Each active
// zoom renders a 64-drawable-pixel source block around the mouse, scaled up
// to fill half of the min(drawable_w, drawable_h). Matches the SDL path's
// composited view: Split mode shows right under left clipped at the split
// position; HStack/VStack show the per-side content on each side of the
// stack boundary that falls within the src rect.
void Display::gpu_build_zoom_magnifier_ops(const RenderContext& ctx,
                                            std::vector<GpuRenderer::SideRenderOp>& ops,
                                            float& zoom_left_slider_dx,
                                            float& zoom_right_slider_dx) {
  if (!view_transform_.zoom_left() && !view_transform_.zoom_right()) return;

  const bool compare_mode = ctx.compare_mode;
  const auto& zoom_rect = ctx.zoom_rect;
  const int split_x = ctx.split_x;
  const int dst_zoomed_size = ctx.dst_zoomed_size;

  const int src_zoomed_size = 64;
  const int src_half = src_zoomed_size / 2;

  const float mouse_drawable_x_f = static_cast<float>(mouse_x_) * drawable_to_window_width_factor_;
  const float mouse_drawable_y_f = static_cast<float>(mouse_y_) * drawable_to_window_height_factor_;

  // Clamp the 64-drawable-pixel src window to stay inside drawable bounds
  // (integer clamp preserves the SDL zoom's edge-behavior). The *size* stays
  // constant in drawable pixels — only the center moves.
  const float src_cx_draw = clamp_range(mouse_drawable_x_f,
                                         static_cast<float>(src_half),
                                         static_cast<float>(drawable_width_ - src_half));
  const float src_cy_draw = clamp_range(mouse_drawable_y_f,
                                         static_cast<float>(src_half),
                                         static_cast<float>(drawable_height_ - src_half));

  // Fixed src size in layout/video coords (independent of mouse position) —
  // 64 drawable pixels mapped through the current view zoom.
  const float video_per_draw_x = video_to_window_width_factor_ / (zoom_rect.zoom_factor * drawable_to_window_width_factor_);
  const float video_per_draw_y = video_to_window_height_factor_ / (zoom_rect.zoom_factor * drawable_to_window_height_factor_);
  const float video_src_half_w = static_cast<float>(src_half) * video_per_draw_x;
  const float video_src_half_h = static_cast<float>(src_half) * video_per_draw_y;

  // Convert clamped drawable center → layout-video coords (float, no
  // floor/ceil snapping — window_to_video_position discretises, so the math
  // is inlined here).
  const float center_win_x = src_cx_draw / drawable_to_window_width_factor_;
  const float center_win_y = src_cy_draw / drawable_to_window_height_factor_;
  const float center_layout_x = ((center_win_x - static_cast<float>(content_window_.x)) * video_to_window_width_factor_ - zoom_rect.start.x()) / zoom_rect.zoom_factor;
  const float center_layout_y = ((center_win_y - static_cast<float>(content_window_.y)) * video_to_window_height_factor_ - zoom_rect.start.y()) / zoom_rect.zoom_factor;

  const float sx0 = center_layout_x - video_src_half_w;
  const float sx1 = center_layout_x + video_src_half_w;
  const float sy0 = center_layout_y - video_src_half_h;
  const float sy1 = center_layout_y + video_src_half_h;

  // Push a side render op whose src rect is in *layout* coords (the per-side
  // frame offset is applied here), dst in FBO coords.
  auto push_zoom_slice = [&](int side,
                              float layout_x0, float layout_y0, float layout_x1, float layout_y1,
                              float dst_x0, float dst_y0, float dst_x1, float dst_y1) {
    const float x_off = (side == 1 && mode_ == Mode::HStack) ? static_cast<float>(video_width_) : 0.f;
    const float y_off = (side == 1 && mode_ == Mode::VStack) ? static_cast<float>(video_height_) : 0.f;
    const float fsx0 = clamp_range(layout_x0 - x_off, 0.f, static_cast<float>(video_width_));
    const float fsx1 = clamp_range(layout_x1 - x_off, 0.f, static_cast<float>(video_width_));
    const float fsy0 = clamp_range(layout_y0 - y_off, 0.f, static_cast<float>(video_height_));
    const float fsy1 = clamp_range(layout_y1 - y_off, 0.f, static_cast<float>(video_height_));
    if (fsx1 <= fsx0 || fsy1 <= fsy0 || dst_x1 <= dst_x0 || dst_y1 <= dst_y0) return;
    GpuRenderer::SideRenderOp op{};
    op.side = side;
    op.src_x0 = fsx0; op.src_y0 = fsy0;
    op.src_x1 = fsx1; op.src_y1 = fsy1;
    op.dst_x0 = dst_x0; op.dst_y0 = dst_y0;
    op.dst_x1 = dst_x1; op.dst_y1 = dst_y1;
    ops.push_back(op);
  };

  // Render the same logical src rect into the given dst box, reproducing the
  // main view's split / stack composition inside the zoom window.
  auto push_zoom_box = [&](float dx0, float dy0, float dx1, float dy1) {
    const float src_w = std::max(1e-3f, sx1 - sx0);
    const float src_h = std::max(1e-3f, sy1 - sy0);
    auto mx = [&](float lx) { return dx0 + (lx - sx0) / src_w * (dx1 - dx0); };
    auto my = [&](float ly) { return dy0 + (ly - sy0) / src_h * (dy1 - dy0); };

    if (mode_ == Mode::Split && compare_mode) {
      if (show_right_) push_zoom_slice(1, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
      if (show_left_) {
        const float sxl = static_cast<float>(split_x);
        if (sxl > sx0) {
          const float lx1 = std::min(sx1, sxl);
          push_zoom_slice(0, sx0, sy0, lx1, sy1, dx0, dy0, mx(lx1), dy1);
        }
      }
    } else if (mode_ == Mode::HStack) {
      const float boundary = static_cast<float>(video_width_);
      if (show_left_ && sx0 < boundary) {
        const float lx1 = std::min(sx1, boundary);
        push_zoom_slice(0, sx0, sy0, lx1, sy1, dx0, dy0, mx(lx1), dy1);
      }
      if (show_right_ && sx1 > boundary) {
        const float rx0 = std::max(sx0, boundary);
        push_zoom_slice(1, rx0, sy0, sx1, sy1, mx(rx0), dy0, dx1, dy1);
      }
    } else if (mode_ == Mode::VStack) {
      const float boundary = static_cast<float>(video_height_);
      if (show_left_ && sy0 < boundary) {
        const float ty1 = std::min(sy1, boundary);
        push_zoom_slice(0, sx0, sy0, sx1, ty1, dx0, dy0, dx1, my(ty1));
      }
      if (show_right_ && sy1 > boundary) {
        const float by0 = std::max(sy0, boundary);
        push_zoom_slice(1, sx0, by0, sx1, sy1, dx0, my(by0), dx1, dy1);
      }
    } else {
      // Split mode with only one side visible.
      if (show_left_) push_zoom_slice(0, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
      else if (show_right_) push_zoom_slice(1, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
    }
  };

  const float zoom_dst_y = static_cast<float>(drawable_height_ - dst_zoomed_size);
  if (view_transform_.zoom_left()) {
    push_zoom_box(0.f, zoom_dst_y, static_cast<float>(dst_zoomed_size), zoom_dst_y + dst_zoomed_size);
  }
  if (view_transform_.zoom_right()) {
    const float rx0 = static_cast<float>(drawable_width_ - dst_zoomed_size);
    push_zoom_box(rx0, zoom_dst_y, rx0 + dst_zoomed_size, zoom_dst_y + dst_zoomed_size);
  }

  // Record the exact mapped split-x inside each zoom box so the slider line
  // (drawn in the HUD pass) sits on the real boundary — split_x is snapped to
  // an integer video texel, while the src center tracks the raw mouse, so the
  // two can be offset by up to half a video pixel.
  if (mode_ == Mode::Split && compare_mode) {
    const float sxl = static_cast<float>(split_x);
    const float src_w = std::max(1e-3f, sx1 - sx0);
    if (sxl >= sx0 && sxl <= sx1) {
      const float frac = (sxl - sx0) / src_w;
      if (view_transform_.zoom_left()) {
        zoom_left_slider_dx = frac * static_cast<float>(dst_zoomed_size);
      }
      if (view_transform_.zoom_right()) {
        const float rx0 = static_cast<float>(drawable_width_ - dst_zoomed_size);
        zoom_right_slider_dx = rx0 + frac * static_cast<float>(dst_zoomed_size);
      }
    }
  }
}

// When a save is pending and CPU pixels are available, capture the OSD via GpuRenderer::capture_osd into an RGB24 AVFrame; otherwise skip the GPU readback and return nullptr.
AVFramePtr Display::gpu_capture_osd(const bool have_rgb,
                                     const std::vector<GpuRenderer::SideRenderOp>& ops,
                                     const std::vector<GpuRenderer::OverlayOp>& overlays,
                                     const std::vector<GpuRenderer::TextOverlayOp>& text_ops) {
  if (!image_saver_.save_frames_requested() || !have_rgb) {
    return AVFramePtr(nullptr);
  }
  const size_t pitch = static_cast<size_t>(drawable_width_) * 3;
  uint8_t* pixels = reinterpret_cast<uint8_t*>(av_malloc(pitch * static_cast<size_t>(drawable_height_)));
  if (!pixels) return AVFramePtr(nullptr);
  if (!gpu_renderer_.capture_osd(pixels, static_cast<int>(pitch), drawable_width_, drawable_height_,
                                  ops.data(), static_cast<int>(ops.size()),
                                  overlays.data(), static_cast<int>(overlays.size()),
                                  text_ops.data(), static_cast<int>(text_ops.size()))) {
    av_free(pixels);
    return AVFramePtr(nullptr);
  }
  AVFrame* osd = av_frame_alloc();
  osd->format = AV_PIX_FMT_RGB24;
  osd->width = drawable_width_;
  osd->height = drawable_height_;
  osd->data[0] = pixels;
  osd->linesize[0] = static_cast<int>(pitch);
  return AVFramePtr(osd);
}

// Consume deferred save-frames / save-selected-area / crop requests now that the frame has rendered.
void Display::gpu_finalize_deferred(const RenderContext& ctx, const bool have_rgb, AVFrame* osd_frame) {
  (void)ctx;
  if (image_saver_.save_frames_requested()) {
    if (have_rgb && osd_frame) {
      const std::string& left_stem = side_ui_[displayed_left_side_.as_simple_index()].file_stem;
      const std::string& right_stem = side_ui_[displayed_right_side_.as_simple_index()].file_stem;
      image_saver_.save_frames_with_osd(rgb_cache_.left(), rgb_cache_.right(), osd_frame, left_stem, right_stem);
    } else {
      std::cerr << "Save image frames: OSD or RGB capture unavailable." << std::endl;
    }
    image_saver_.clear_save_frames_request();
  }
  if (selection_.save_selected_area_requested()) {
    if (have_rgb) {
      possibly_save_selected_area(rgb_cache_.left(), rgb_cache_.right());
    } else {
      std::cerr << "Save selected area: RGB conversion unavailable." << std::endl;
      selection_.cancel_save_selected_area();
    }
  }
  if (selection_.crop_mode()) {
    possibly_apply_crop();
  }
}

// GPU path entry: upload a decoded native-format AVFrame to the libplacebo side texture.
void Display::upload_native_frame(int side, const AVFrame* frame) {
  if (gpu_renderer_active_) {
    gpu_renderer_.upload_frame(side, frame);
  }
}
