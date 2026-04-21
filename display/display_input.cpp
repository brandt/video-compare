#include "display/display.h"
#include <algorithm>
#include <iostream>
#include <regex>
#include <string>
#include "core/ffmpeg/ffmpeg.h"
#include "core/strings/string_utils.h"
#include "display_utils.h"
#include "analysis/scopes/scope_window.h"

void Display::begin_input_frame() {
  playback_.clear_transient_state();
  toggle_scope_window_requested_.fill(false);
}

// Mark that an event occurred this frame so the next refresh isn't skipped by the early-out guard.
void Display::mark_input_received() {
  input_received_ = true;
}

// Dispatch a single SDL event: window events, mouse, keyboard, and global shortcuts.
void Display::handle_event(const SDL_Event& event) {
  event_ = event;
  input_received_ = true;

  switch (event.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
      handle_window_event(event);
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      handle_wheel_event(event);
      break;
    case SDL_EVENT_MOUSE_MOTION:
      handle_mouse_motion_event(event);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      handle_mouse_button_event(event);
      break;
    case SDL_EVENT_KEY_DOWN:
      handle_key_down(event);
      break;
    case SDL_EVENT_KEY_UP:
      handle_key_up(event);
      break;
    case SDL_EVENT_QUIT:
      quit_ = true;
      break;
    default:
      break;
  }
}

// Set the cursor to pan/selection/normal based on current input state.
void Display::update_cursor_mode() {
  SDL_Cursor* cursor;
  if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) {
    cursor = pan_mode_cursor_;
  } else if (selection_.has_active_cursor_mode() && selection_.state() != SelectionState::Completed) {
    cursor = selection_mode_cursor_;
  } else {
    cursor = normal_mode_cursor_;
  }
  SDL_SetCursor(cursor);
}

// Whether the clipboard modifier is held (Cmd on macOS, Ctrl elsewhere).
bool Display::is_clipboard_mod_pressed(SDL_Keymod keymod, bool is_ctrl_down) const {
#ifdef __APPLE__
  (void)is_ctrl_down;
  return (keymod & SDL_KMOD_GUI) != 0;
#else
  (void)keymod;
  return is_ctrl_down;
#endif
}

// Window events: close, enter/leave, HDR state change, and resizes.
void Display::handle_window_event(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (event.window.windowID == SDL_GetWindowID(window_)) {
        quit_ = true;
      }
      break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      mouse_is_inside_window_ = false;
      break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
      mouse_is_inside_window_ = true;
      break;
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
      update_hdr_display_state();
      break;
    default:
      handle_window_resize();
      if (pending_verbose_print_) {
        print_verbose_info();
        pending_verbose_print_ = false;
      }
      break;
  }
}

// Mouse-wheel zooms the view around the current mouse position.
void Display::handle_wheel_event(const SDL_Event& event) {
  if (!mouse_is_inside_window_ || event.wheel.y == 0) return;

  float delta_zoom = wheel_sensitivity_ * event.wheel.y * (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
  if (delta_zoom > 0) {
    delta_zoom /= 2.0F;
  }
  if (SDL_GetModState() & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) {
    delta_zoom /= ZOOM_SLOWDOWN_RATIO;
  }

  const float new_global_zoom_factor = view_transform_.compute_zoom_factor(view_transform_.global_zoom_level() - delta_zoom);

  // logic ported from YUView's MoveAndZoomableView.cpp with thanks :)
  if (new_global_zoom_factor >= 0.001 && new_global_zoom_factor <= 10000) {
    const Vector2D zoom_point = Vector2D(static_cast<float>(mouse_x_ - content_window_.x) * video_to_window_width_factor_, static_cast<float>(mouse_y_ - content_window_.y) * video_to_window_height_factor_);
    view_transform_.update_move_offset(view_transform_.compute_relative_move_offset(zoom_point, new_global_zoom_factor));
    view_transform_.update_zoom_factor(new_global_zoom_factor);
  }
}

// Mouse-motion updates mouse position, pans on right-drag, and scrolls the active overlay panel.
void Display::handle_mouse_motion_event(const SDL_Event& event) {
  SDL_GetMouseState(&mouse_x_, &mouse_y_);

  refresh_selection_end_from_mouse();

  if (event.motion.state & SDL_BUTTON_RMASK) {
    const auto pan_offset = Vector2D(event.motion.xrel, event.motion.yrel) * Vector2D(video_to_window_width_factor_, video_to_window_height_factor_) / Vector2D(drawable_to_window_width_factor_, drawable_to_window_height_factor_);
    view_transform_.update_move_offset(view_transform_.move_offset() + pan_offset);
  }

  auto apply_scroll = [&](int& y_offset, const int total_height, size_t count) {
    y_offset += (-event.motion.yrel * total_height * 3) / drawable_height_;
    y_offset = std::max(y_offset, drawable_height_ - total_height - static_cast<int>(count) * HELP_TEXT_LINE_SPACING);
    y_offset = std::min(y_offset, 0);
  };

  if (overlay_.show_metadata()) {
    int y = metadata_panel_.scroll_offset();
    apply_scroll(y, metadata_panel_.total_height(), metadata_panel_.item_count(gpu_renderer_active_));
    metadata_panel_.set_scroll_offset(y);
  }

  if (overlay_.show_help()) {
    int y = overlay_.help_scroll_offset();
    apply_scroll(y, overlay_.help_total_height(), overlay_.help_item_count(gpu_renderer_active_));
    overlay_.set_help_scroll_offset(y);
  }
}

// Mouse-button events: start/complete selection, seek on click, update cursor mode.
void Display::handle_mouse_button_event(const SDL_Event& event) {
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    if (event.button.button == SDL_BUTTON_LEFT && selection_.has_active_cursor_mode() && selection_.state() == SelectionState::None) {
      const Vector2D start_pos = view_transform_.window_to_video_position(mouse_x_, mouse_y_, view_transform_.compute_zoom_rect());
      selection_.begin_selection(start_pos, mode_, video_width_, video_height_);
    } else if (event.button.button != SDL_BUTTON_RIGHT) {
      playback_.set_seek_relative(static_cast<float>(mouse_x_) / static_cast<float>(window_width_));
      playback_.set_seek_from_start(true);
    }
  } else {  // SDL_EVENT_MOUSE_BUTTON_UP
    if (event.button.button == SDL_BUTTON_LEFT && selection_.state() == SelectionState::Started) {
      selection_.complete_selection();
    }
  }
  update_cursor_mode();
}

// Key-down: extract modifiers, then cascade through first-match-wins key-group helpers.
void Display::handle_key_down(const SDL_Event& event) {
  const SDL_Keymod keymod = event.key.mod;
  const SDL_Keycode keycode = event.key.key;
  const bool is_shift_down = (keymod & SDL_KMOD_SHIFT) != 0;
  const bool is_ctrl_down = (keymod & SDL_KMOD_CTRL) != 0;
  const bool is_alt_down = (keymod & SDL_KMOD_ALT) != 0;

  const float relative_seek_scale = (is_shift_down || is_ctrl_down) ? 1.0F / RELATIVE_SEEK_SLOWDOWN_RATIO : 1.0F;
  const float playback_speed_scale = (is_shift_down || is_ctrl_down) ? 1.0F / PLAYBACK_SPEED_SLOWDOWN_RATIO : 1.0F;

  if (handle_right_video_index_shortcut(keycode, is_ctrl_down, is_shift_down)) return;
  if (handle_crop_save_keys(keycode, is_shift_down)) return;
  if (handle_scope_window_keys(keycode, is_shift_down)) return;
  if (handle_window_size_keys(keycode, is_shift_down, is_ctrl_down, is_alt_down)) return;
  if (handle_view_mode_keys(keycode, is_shift_down, is_ctrl_down)) return;
  if (handle_zoom_pan_keys(keycode, is_shift_down)) return;
  if (handle_playback_keys(keycode, relative_seek_scale, playback_speed_scale, is_shift_down, is_ctrl_down, is_alt_down)) return;
  if (handle_diff_keys(keycode, is_shift_down)) return;
  handle_misc_keys(keycode, keymod, is_shift_down);
}

// Key-up: release the transient zoom-left / zoom-right / show-fps flags.
void Display::handle_key_up(const SDL_Event& event) {
  switch (event.key.key) {
    case SDLK_Z: view_transform_.set_zoom_left(false); break;
    case SDLK_C: view_transform_.set_zoom_right(false); break;
    case SDLK_X: show_fps_ = false; break;
    default: break;
  }
}

// Ctrl+Shift+1..9/0 selects a specific right video by index.
bool Display::handle_right_video_index_shortcut(const SDL_Keycode keycode, const bool is_ctrl_down, const bool is_shift_down) {
  if (!is_ctrl_down || !is_shift_down) return false;

  size_t target_index = SIZE_MAX;
  if (keycode >= SDLK_1 && keycode <= SDLK_9) {
    target_index = keycode - SDLK_1;
  } else if (keycode >= SDLK_KP_1 && keycode <= SDLK_KP_9) {
    target_index = keycode - SDLK_KP_1;
  } else if (keycode == SDLK_KP_0 || keycode == SDLK_0) {
    target_index = 9;
  }

  if (target_index == SIZE_MAX) return false;
  if (target_index < num_right_videos_) {
    active_right_index_ = target_index;
    notify_user(string_sprintf("Active right video: %d/%d", active_right_index_ + 1, num_right_videos_));
  }
  return true;
}

// F, Shift+F (save selected area), Shift+R/L/B (crop per side), Shift+K (auto-crop black borders), BACKSPACE (clear crop).
bool Display::handle_crop_save_keys(const SDL_Keycode keycode, const bool is_shift_down) {
  auto toggle_crop_mode_for_side = [&](const CropTargetSide side) {
    selection_.toggle_crop_for_side(side);
    update_cursor_mode();
  };

  switch (keycode) {
    case SDLK_F:
      if (is_shift_down) {
        if (!selection_.save_selected_area_requested()) {
          selection_.reset_crop_mode();
          selection_.request_save_selected_area();
        } else {
          selection_.cancel_save_selected_area();
        }
        update_cursor_mode();
      } else {
        image_saver_.request_save_frames();
      }
      return true;
    case SDLK_K:
      if (is_shift_down) {
        selection_.reset_crop_mode();
        selection_.request_auto_crop_black_borders();
        return true;
      }
      return false;
    case SDLK_R:
      if (is_shift_down) { toggle_crop_mode_for_side(CropTargetSide::Right); return true; }
      return false;
    case SDLK_L:
      if (is_shift_down) { toggle_crop_mode_for_side(CropTargetSide::Left); return true; }
      return false;
    case SDLK_B:
      if (is_shift_down) { toggle_crop_mode_for_side(CropTargetSide::Both); return true; }
      return false;
    case SDLK_BACKSPACE:
      selection_.request_clear_crop();
      update_cursor_mode();
      return true;
    default:
      return false;
  }
}

// F1/F2/F3 and Shift+1/2/3 toggle the histogram / vectorscope / waveform scope windows.
bool Display::handle_scope_window_keys(const SDL_Keycode keycode, const bool is_shift_down) {
  auto request = [&](ScopeWindow::Type type) {
    toggle_scope_window_requested_[ScopeWindow::index(type)] = true;
  };
  switch (keycode) {
    case SDLK_F1: request(ScopeWindow::Type::Histogram);   return true;
    case SDLK_F2: request(ScopeWindow::Type::Vectorscope); return true;
    case SDLK_F3: request(ScopeWindow::Type::Waveform);    return true;
    case SDLK_1: case SDLK_KP_1:
      if (is_shift_down) { request(ScopeWindow::Type::Histogram); return true; }
      return false;
    case SDLK_2: case SDLK_KP_2:
      if (is_shift_down) { request(ScopeWindow::Type::Vectorscope); return true; }
      return false;
    case SDLK_3: case SDLK_KP_3:
      if (is_shift_down) { request(ScopeWindow::Type::Waveform); return true; }
      return false;
    default:
      return false;
  }
}

// Ctrl+W / Shift+W / Ctrl+Shift+W window-size save/restore, Alt+Enter fullscreen toggle.
bool Display::handle_window_size_keys(const SDL_Keycode keycode, const bool is_shift_down, const bool is_ctrl_down, const bool is_alt_down) {
  auto restore_window_size = [&](const std::array<int, 2>& size) {
    const int target_w = std::max(MIN_WINDOW_WIDTH, size[0]);
    const int target_h = std::max(MIN_WINDOW_HEIGHT, size[1]);
    // Route restore operations through the normal resize path so active
    // aspect-lock constraints (window/content) are always enforced.
    apply_window_size_and_relayout(target_w, target_h, false);
  };

  switch (keycode) {
    case SDLK_W:
      if (is_ctrl_down && is_shift_down) {
        saved_window_size_ = {window_width_, window_height_};
        std::cout << string_sprintf("Saved window size (%dx%d)", saved_window_size_[0], saved_window_size_[1]) << std::endl;
        return true;
      }
      if (is_ctrl_down) {
        restore_window_size(startup_window_size_);
        std::cout << string_sprintf("Restored startup window size (%dx%d)", startup_window_size_[0], startup_window_size_[1]) << std::endl;
        return true;
      }
      if (is_shift_down) {
        restore_window_size(saved_window_size_);
        std::cout << string_sprintf("Restored saved window size (%dx%d)", saved_window_size_[0], saved_window_size_[1]) << std::endl;
        return true;
      }
      return true;  // swallow bare W
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (is_alt_down) {
        const bool sdl_fullscreen_now = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
        if (sdl_fullscreen_now) {
          set_fullscreen(false);
        } else if (detect_fullscreen_like_state()) {
          set_pending_message("Alt+Enter cannot toggle native fullscreen; use window fullscreen button");
        } else {
          set_fullscreen(true);
        }
        return true;
      }
      return false;
    default:
      return false;
  }
}

// 1/2/3 show toggles, 0 subtraction, M/Shift+M mode cycle, S/Shift+S swap/aspect, T, I.
bool Display::handle_view_mode_keys(const SDL_Keycode keycode, const bool is_shift_down, const bool is_ctrl_down) {
  switch (keycode) {
    case SDLK_1: case SDLK_KP_1:
      show_left_ = !show_left_;
      return true;
    case SDLK_2: case SDLK_KP_2:
      show_right_ = !show_right_;
      return true;
    case SDLK_3: case SDLK_KP_3:
      show_hud_ = !show_hud_;
      return true;
    case SDLK_0: case SDLK_KP_0:
      diff_processor_.toggle_subtraction_mode();
      return true;
    case SDLK_M:
      if (is_shift_down) {
        constexpr int kModeCount = 3;
        const int delta = is_ctrl_down ? -1 : 1;
        mode_ = static_cast<Mode>((static_cast<int>(mode_) + delta + kModeCount) % kModeCount);
        recreate_video_textures_for_current_mode();
        resize_window_for_mode_switch();
        const std::string mode_name = mode_to_string(mode_);
        notify_user(string_sprintf("Display mode set to '%s'", to_upper_case(mode_name).c_str()));
      } else {
        print_image_similarity_metrics_ = true;
      }
      return true;
    case SDLK_S:
      if (is_shift_down) {
        constexpr int kModeCount = 5;
        const int delta = is_ctrl_down ? -1 : 1;
        aspect_view_mode_ = static_cast<AspectViewMode>((static_cast<int>(aspect_view_mode_) + delta + kModeCount) % kModeCount);
        if (is_fullscreen_) {
          handle_window_resize(true, true);
        } else {
          const auto target_size = compute_mode_switch_target_window_size();
          apply_window_size_and_relayout(target_size[0], target_size[1], true);
        }
        notify_user(string_sprintf("Aspect view mode set to '%s'", to_upper_case(aspect_view_mode_to_string(aspect_view_mode_)).c_str()));
      } else {
        swap_left_right_ = !swap_left_right_;
        refresh_display_side_mapping();
      }
      return true;
    case SDLK_T:
      bilinear_texture_filtering_ = !bilinear_texture_filtering_;
      notify_user(string_sprintf("Video texture filter set to '%s'", bilinear_texture_filtering_ ? "BILINEAR" : "NEAREST NEIGHBOR"));
      return true;
    case SDLK_I:
      fast_input_alignment_ = !fast_input_alignment_;
      notify_user(string_sprintf("Input alignment resizing filter set to '%s' (takes effect for the next decoded frame)", fast_input_alignment_ ? "BILINEAR (fast)" : "BICUBIC (high-quality)"));
      return true;
    default:
      return false;
  }
}

// 4-9 preset zooms, E mouse-centered pan, R reset pan, Z/C transient zoom magnifiers.
bool Display::handle_zoom_pan_keys(const SDL_Keycode keycode, const bool is_shift_down) {
  switch (keycode) {
    case SDLK_4: case SDLK_KP_4:
      view_transform_.update_zoom_factor_and_move_offset(std::min(video_to_window_width_factor_ / drawable_to_window_width_factor_, video_to_window_height_factor_ / drawable_to_window_height_factor_));
      return true;
    case SDLK_5: case SDLK_KP_5: view_transform_.update_zoom_factor_and_move_offset(0.5F); return true;
    case SDLK_6: case SDLK_KP_6: view_transform_.update_zoom_factor_and_move_offset(1.0F); return true;
    case SDLK_7: case SDLK_KP_7: view_transform_.update_zoom_factor_and_move_offset(2.0F); return true;
    case SDLK_8: case SDLK_KP_8: view_transform_.update_zoom_factor_and_move_offset(4.0F); return true;
    case SDLK_9: case SDLK_KP_9: view_transform_.update_zoom_factor_and_move_offset(8.0F); return true;
    case SDLK_E: {
      SDL_GetMouseState(&mouse_x_, &mouse_y_);
      const auto zoom_rect = view_transform_.compute_zoom_rect();
      const Vector2D mouse_video = view_transform_.window_to_video_position(mouse_x_, mouse_y_, zoom_rect);
      const Vector2D center_video = view_transform_.window_to_video_position(content_window_.x + content_window_.w / 2, content_window_.y + content_window_.h / 2, zoom_rect);
      view_transform_.update_move_offset(view_transform_.move_offset() + (center_video - mouse_video) * view_transform_.global_zoom_factor());
      return true;
    }
    case SDLK_R:
      if (!is_shift_down) {
        view_transform_.reset_pan();
        view_transform_.update_zoom_factor(1.0F);
        return true;
      }
      return false;
    case SDLK_Z:
      view_transform_.set_zoom_left(true);
      return true;
    default:
      return false;
  }
}

// SPACE play/pause, COMMA/PERIOD loop modes, J/L speed, A/D frame nav, arrows/PAGE seek, PLUS/MINUS shift right, GRAVE auto-align.
bool Display::handle_playback_keys(const SDL_Keycode keycode, const float relative_seek_scale, const float playback_speed_scale, const bool is_shift_down, const bool is_ctrl_down, const bool is_alt_down) {
  switch (keycode) {
    case SDLK_SPACE:
      playback_.toggle_play();
      return true;
    case SDLK_COMMA: case SDLK_KP_COMMA:
      set_buffer_play_loop_mode(playback_.loop_mode() != Loop::PingPong ? Loop::PingPong : Loop::Off);
      return true;
    case SDLK_PERIOD:
      set_buffer_play_loop_mode(playback_.loop_mode() != Loop::ForwardOnly ? Loop::ForwardOnly : Loop::Off);
      return true;
    case SDLK_A:
      if (is_shift_down) playback_.adjust_frame_navigation_delta(-1);
      else               playback_.adjust_frame_buffer_offset_delta(1);
      return true;
    case SDLK_D:
      if (is_shift_down) playback_.adjust_frame_navigation_delta(1);
      else               playback_.adjust_frame_buffer_offset_delta(-1);
      return true;
    case SDLK_LEFT:     playback_.add_seek_relative(-1.0F * relative_seek_scale);    return true;
    case SDLK_DOWN:     playback_.add_seek_relative(-10.0F * relative_seek_scale);   return true;
    case SDLK_PAGEDOWN: playback_.add_seek_relative(-600.0F * relative_seek_scale);  return true;
    case SDLK_RIGHT:    playback_.add_seek_relative(1.0F * relative_seek_scale);     return true;
    case SDLK_UP:       playback_.add_seek_relative(10.0F * relative_seek_scale);    return true;
    case SDLK_PAGEUP:   playback_.add_seek_relative(600.0F * relative_seek_scale);   return true;
    case SDLK_J:
      playback_.update_playback_speed(-1.0F * playback_speed_scale);
      playback_.set_possibly_tick_playback(true);
      return true;
    case SDLK_L:
      playback_.update_playback_speed(1.0F * playback_speed_scale);
      playback_.set_tick_playback(true);
      return true;
    case SDLK_PLUS: case SDLK_KP_PLUS: case SDLK_EQUALS:
      if (is_alt_down)       playback_.adjust_shift_right_frames(100);
      else if (is_ctrl_down) playback_.adjust_shift_right_frames(10);
      else                   playback_.adjust_shift_right_frames(1);
      return true;
    case SDLK_MINUS: case SDLK_KP_MINUS:
      if (is_alt_down)       playback_.adjust_shift_right_frames(-100);
      else if (is_ctrl_down) playback_.adjust_shift_right_frames(-10);
      else                   playback_.adjust_shift_right_frames(-1);
      return true;
    case SDLK_GRAVE:
      // Symmetric window around left's current position (typically served
      // entirely from the ring; no decode).
      playback_.request_auto_align(AutoAlignMode::Symmetric);
      return true;
    case SDLK_LEFTBRACKET:
      // Biased backward: search the 1s range before left's current position.
      playback_.request_auto_align(AutoAlignMode::Backward);
      return true;
    case SDLK_RIGHTBRACKET:
      // Biased forward: search the 1s range after left's current position.
      playback_.request_auto_align(AutoAlignMode::Forward);
      return true;
    default:
      return false;
  }
}

// Y cycles subtraction modes (reverse with Shift), U toggles luma-only.
bool Display::handle_diff_keys(const SDL_Keycode keycode, const bool is_shift_down) {
  switch (keycode) {
    case SDLK_Y: {
      const bool forward = !is_shift_down;
      DiffMode new_mode = diff_processor_.diff_mode();
      switch (new_mode) {
        case DiffMode::LegacyAbs:       new_mode = forward ? DiffMode::AbsLinear       : DiffMode::SignedDiverging; break;
        case DiffMode::AbsLinear:       new_mode = forward ? DiffMode::AbsSqrt         : DiffMode::LegacyAbs;       break;
        case DiffMode::AbsSqrt:         new_mode = forward ? DiffMode::SignedDiverging : DiffMode::AbsLinear;       break;
        case DiffMode::SignedDiverging: new_mode = forward ? DiffMode::LegacyAbs       : DiffMode::AbsSqrt;         break;
      }
      diff_processor_.set_diff_mode(new_mode);

      std::string diff_mode_name;
      switch (new_mode) {
        case DiffMode::LegacyAbs:       diff_mode_name = "ABSOLUTE LINEAR (FIXED GAIN)"; break;
        case DiffMode::AbsLinear:       diff_mode_name = "ABSOLUTE LINEAR (ADAPTIVE)";   break;
        case DiffMode::AbsSqrt:         diff_mode_name = "ABSOLUTE SQUARE ROOT";         break;
        case DiffMode::SignedDiverging: diff_mode_name = "SIGNED DIVERGING";             break;
      }
      notify_user(string_sprintf("Subtraction mode set to '%s'", diff_mode_name.c_str()));
      return true;
    }
    case SDLK_U:
      diff_processor_.toggle_diff_luma_only();
      notify_user(string_sprintf("Subtraction luminance-only set to '%s'", diff_processor_.diff_luma_only() ? "ON" : "OFF"));
      return true;
    default:
      return false;
  }
}

// H help, ESCAPE quit, P pixel print, Q quality overlay, X fps, TAB next right video, clipboard C/V, V metadata toggle.
bool Display::handle_misc_keys(const SDL_Keycode keycode, const SDL_Keymod keymod, const bool is_shift_down) {
  const bool is_ctrl_down = (keymod & SDL_KMOD_CTRL) != 0;

  switch (keycode) {
    case SDLK_H:
      overlay_.toggle_help();
      return true;
    case SDLK_ESCAPE:
      quit_ = true;
      return true;
    case SDLK_P:
      print_mouse_position_and_color_ = mouse_is_inside_window_;
      return true;
    case SDLK_Q:
      show_quality_metrics_ = !show_quality_metrics_;
      return true;
    case SDLK_X:
      if (is_shift_down) {
        notify_user(string_sprintf("Display state: window=%dx%d aspect=%s", window_width_, window_height_, aspect_view_mode_to_string(aspect_view_mode_).c_str()));
      } else {
        show_fps_ = true;
      }
      return true;
    case SDLK_TAB:
      if (is_shift_down) active_right_index_ = (active_right_index_ + num_right_videos_ - 1) % num_right_videos_;
      else               active_right_index_ = (active_right_index_ + 1) % num_right_videos_;
      notify_user(string_sprintf("Active right video: %d/%d", active_right_index_ + 1, num_right_videos_));
      return true;
    case SDLK_C:
      if (is_clipboard_mod_pressed(keymod, is_ctrl_down)) {
        const float previous_left_frame_secs = previous_left_frame_pts_ * AV_TIME_TO_SEC;
        const std::string previous_left_frame_secs_str = format_position(previous_left_frame_secs, false);
        SDL_SetClipboardText(previous_left_frame_secs_str.c_str());
        notify_user(string_sprintf("Copied to clipboard: %s", previous_left_frame_secs_str.c_str()));
      } else {
        view_transform_.set_zoom_right(true);
      }
      return true;
    case SDLK_V:
      if (is_clipboard_mod_pressed(keymod, is_ctrl_down)) {
        char* clip_text = SDL_GetClipboardText();
        if (!clip_text) {
          std::cerr << "Failed to get clipboard text: " << SDL_GetError() << std::endl;
          return true;
        }
        std::string clipboard_str(clip_text);
        SDL_free(clip_text);

        static const std::regex timestamp_regex(R"((?:(\d+):)?(?:(\d+):)?(\d+(?:\.\d+)?))");
        std::smatch match;
        if (std::regex_search(clipboard_str, match, timestamp_regex)) {
          std::string timestamp = match.str();
          notify_user(string_sprintf("Timestamp pasted: %s", timestamp.c_str()));
          playback_.set_seek_relative(parse_timestamps_to_seconds(timestamp) / static_cast<float>(duration_));
          playback_.set_seek_from_start(true);
        } else {
          notify_user("No valid timestamp found in clipboard.");
        }
      } else {
        overlay_.toggle_metadata();
      }
      return true;
    default:
      return false;
  }
}
