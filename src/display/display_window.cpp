#include "display/display.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include "assets/fonts/source_code_pro_regular_ttf.h"
#include "core/strings/string_utils.h"
#include "display_utils.h"

void Display::recreate_video_textures_for_current_mode() {
  // GPU renderer: no SDL textures needed for video frames.
  if (gpu_renderer_active_) return;

  for (int s = 0; s < kSideCount; s++) {
    if (side_textures_linear_[s] != nullptr) {
      SDL_DestroyTexture(side_textures_linear_[s]);
      side_textures_linear_[s] = nullptr;
    }
    if (side_textures_nn_[s] != nullptr) {
      SDL_DestroyTexture(side_textures_nn_[s]);
      side_textures_nn_[s] = nullptr;
    }
  }

  // Per-side textures: each is video_width_ × video_height_ (no HStack/VStack doubling)
  const bool use_hdr_textures = hdr_display_available_ && hdr_passthrough_;
  const SDL_PixelFormat pixel_format = (requires_10_bpc() || hdr_passthrough_) ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24;

  auto create_video_texture = [&](SDL_ScaleMode scale_mode, const std::string& label) {
    SDL_Texture* tex;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, pixel_format);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, use_hdr_textures ? SDL_COLORSPACE_HDR10 : SDL_COLORSPACE_SRGB);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, video_width_);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, video_height_);
    if (use_hdr_textures) {
      SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_SDR_WHITE_POINT_FLOAT, 100.0f);
      SDL_SetFloatProperty(props, SDL_PROP_TEXTURE_CREATE_HDR_HEADROOM_FLOAT, hdr_content_headroom_);
    }
    tex = check_sdl(SDL_CreateTextureWithProperties(renderer_, props), "video texture " + label);
    SDL_DestroyProperties(props);

    SDL_SetTextureScaleMode(tex, scale_mode);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    return tex;
  };

  for (int s = 0; s < kSideCount; s++) {
    const std::string label = std::string(s == 0 ? "left" : "right");
    side_textures_linear_[s] = create_video_texture(SDL_SCALEMODE_LINEAR, label + " linear");
    side_textures_nn_[s] = create_video_texture(SDL_SCALEMODE_NEAREST, label + " nearest");
  }
}

// Force a specific window size and re-derive the content layout; used by hot-keys like Shift+W and mode-switch resize.
void Display::apply_window_size_and_relayout(const int target_w, const int target_h, const bool force_layout_refresh) {
  SDL_SetWindowSize(window_, target_w, target_h);
  handle_window_resize(true, force_layout_refresh);
}

// Enter or leave fullscreen, remembering the windowed size for later restore.
void Display::set_fullscreen(const bool fullscreen) {
  int current_window_w = window_width_;
  int current_window_h = window_height_;
  SDL_GetWindowSize(window_, &current_window_w, &current_window_h);
  const Uint32 flags_before = SDL_GetWindowFlags(window_);
  const bool sdl_fullscreen_before = (flags_before & SDL_WINDOW_FULLSCREEN) != 0;

  if (fullscreen == sdl_fullscreen_before) {
    return;
  }

  if (fullscreen) {
    windowed_size_before_fullscreen_ = {current_window_w, current_window_h};
  }

  if (!SDL_SetWindowFullscreen(window_, fullscreen)) {
    set_pending_message(string_sprintf("Unable to %s fullscreen (%s)", fullscreen ? "enter" : "exit", SDL_GetError()));
    return;
  }

  const bool sdl_fullscreen_after = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;

  if (sdl_fullscreen_after != fullscreen) {
    set_pending_message(string_sprintf("Unable to %s fullscreen mode", fullscreen ? "enter" : "exit"));
    return;
  }

  if (!fullscreen && windowed_size_before_fullscreen_[0] > 0 && windowed_size_before_fullscreen_[1] > 0) {
    apply_window_size_and_relayout(std::max(MIN_WINDOW_WIDTH, windowed_size_before_fullscreen_[0]), std::max(MIN_WINDOW_HEIGHT, windowed_size_before_fullscreen_[1]), true);
  } else {
    handle_window_resize(true, true);
  }
}

// True when the window visually behaves like fullscreen (true fullscreen or borderless desktop-sized).
bool Display::detect_fullscreen_like_state() const {
  const Uint32 flags = SDL_GetWindowFlags(window_);
  if ((flags & SDL_WINDOW_FULLSCREEN) != 0) {
    return true;
  }

  if ((flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_BORDERLESS)) == 0) {
    return false;
  }

  int window_x = 0;
  int window_y = 0;
  int window_w = 0;
  int window_h = 0;
  SDL_GetWindowSize(window_, &window_w, &window_h);
  SDL_GetWindowPosition(window_, &window_x, &window_y);

  SDL_DisplayID display_index = SDL_GetDisplayForWindow(window_);
  if (display_index == 0) {
    display_index = display_id_for_index(display_number_);
  }

  SDL_Rect display_bounds{};
  SDL_Rect usable_bounds{};
  if (!SDL_GetDisplayBounds(display_index, &display_bounds) || !SDL_GetDisplayUsableBounds(display_index, &usable_bounds)) {
    return false;
  }

  constexpr int tolerance = 4;
  const auto near = [&](const int a, const int b) { return std::abs(a - b) <= tolerance; };
  const bool fills_display_bounds = near(window_x, display_bounds.x) && near(window_y, display_bounds.y) && near(window_w, display_bounds.w) && near(window_h, display_bounds.h);
  const bool fills_usable_bounds = near(window_x, usable_bounds.x) && near(window_y, usable_bounds.y) && near(window_w, usable_bounds.w) && near(window_h, usable_bounds.h);

  return fills_display_bounds || fills_usable_bounds;
}

std::array<int, 2> Display::compute_mode_switch_target_window_size() const {
  const float target_aspect_ratio = std::max(compute_active_content_aspect_ratio(), 0.001F);
  const double current_area = static_cast<double>(std::max(window_width_, MIN_WINDOW_WIDTH)) * static_cast<double>(std::max(window_height_, MIN_WINDOW_HEIGHT));

  int target_w = std::max(MIN_WINDOW_WIDTH, static_cast<int>(std::round(std::sqrt(current_area * target_aspect_ratio))));
  int target_h = std::max(MIN_WINDOW_HEIGHT, static_cast<int>(std::round(std::sqrt(current_area / target_aspect_ratio))));

  SDL_DisplayID display_index = SDL_GetDisplayForWindow(window_);
  if (display_index == 0) {
    display_index = display_id_for_index(display_number_);
  }

  SDL_Rect bounds;
  if (SDL_GetDisplayUsableBounds(display_index, &bounds)) {
    const int max_w = std::max(1, bounds.w);
    const int max_h = std::max(1, bounds.h);

    if (target_w > max_w || target_h > max_h) {
      const float scale = std::min(static_cast<float>(max_w) / static_cast<float>(std::max(1, target_w)), static_cast<float>(max_h) / static_cast<float>(std::max(1, target_h)));
      target_w = std::max(1, static_cast<int>(std::round(static_cast<float>(target_w) * scale)));
      target_h = std::max(1, static_cast<int>(std::round(static_cast<float>(target_h) * scale)));
    }

    const int min_w = std::min(MIN_WINDOW_WIDTH, max_w);
    const int min_h = std::min(MIN_WINDOW_HEIGHT, max_h);
    target_w = std::max(min_w, std::min(target_w, max_w));
    target_h = std::max(min_h, std::min(target_h, max_h));
  }

  return {target_w, target_h};
}

// Resize the window to match the aspect of the newly-selected display mode.
void Display::resize_window_for_mode_switch() {
  const auto target_size = compute_mode_switch_target_window_size();
  const int target_w = target_size[0];
  const int target_h = target_size[1];
  const float target_aspect_ratio = std::max(compute_active_content_aspect_ratio(), 0.001F);

  // Keep WINDOW aspect lock consistent with the newly selected display mode.
  if (aspect_lock_mode_ == AspectLockMode::Window) {
    window_aspect_ratio_ = target_aspect_ratio;
  }

  if (is_fullscreen_) {
    // Keep the future windowed restore size in sync with mode changes done in fullscreen.
    windowed_size_before_fullscreen_ = target_size;
    handle_window_resize(true, true);
    return;
  }

  apply_window_size_and_relayout(target_w, target_h, true);
}

// Rebuild size-dependent resources (textures, buffers, zoom state) after a crop or source dim change.
void Display::reinitialize_video_dimensions(const unsigned width, const unsigned height) {
  const int new_video_width = static_cast<int>(width);
  const int new_video_height = static_cast<int>(height);
  if (new_video_width <= 0 || new_video_height <= 0) {
    throw std::runtime_error("Video dimensions must be positive");
  }
  if (video_width_ == new_video_width && video_height_ == new_video_height) {
    return;
  }

  video_width_ = new_video_width;
  video_height_ = new_video_height;

  recreate_video_textures_for_current_mode();

  diff_processor_.resize(video_width_, video_height_, requires_10_bpc());

  // Drop any cached RGB conversion state — new video dims require fresh
  // FormatConverter and RGB destination frames.
  rgb_cache_.invalidate();

  view_transform_.sync_move_offset_to_center();

  // Force relayout because video dimensions changed even if window size did not.
  handle_window_resize(true, true);
  update_window_title_with_current_roi();
}

// Recreate small/big TTF fonts at the current font scale.
void Display::rebuild_fonts() {
  if (small_font_ != nullptr) {
    TTF_CloseFont(small_font_);
    small_font_ = nullptr;
  }
  if (big_font_ != nullptr) {
    TTF_CloseFont(big_font_);
    big_font_ = nullptr;
  }

  SDL_IOStream* embedded_font_small = check_sdl(SDL_IOFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN), "get pointer to font");
  SDL_IOStream* embedded_font_big = check_sdl(SDL_IOFromConstMem(SOURCE_CODE_PRO_REGULAR_TTF, SOURCE_CODE_PRO_REGULAR_TTF_LEN), "get pointer to font");

  small_font_ = check_sdl(TTF_OpenFontIO(embedded_font_small, true, 16 * font_scale_), "font open");
  big_font_ = check_sdl(TTF_OpenFontIO(embedded_font_big, true, 24 * font_scale_), "font open");
}

// Rebuild the per-side filename text textures used by the SDL path.
void Display::rebuild_side_ui_textures() {
  // file_stem is used by save_selected_area / save_image_frames in both
  // renderer paths — always refresh it, even in GPU mode where the SDL
  // label textures aren't rebuilt.
  side_ui_[LEFT.as_simple_index()].file_stem = strip_ffmpeg_patterns(get_file_stem(left_file_name_));
  side_ui_[RIGHT.as_simple_index()].file_stem = strip_ffmpeg_patterns(get_file_stem(right_file_name_));

  if (gpu_renderer_active_) return; // no SDL textures in GPU renderer mode
  auto rebuild_side = [&](Side side, const std::string& label) {
    auto& ui = side_ui_[side.as_simple_index()];
    if (ui.text_texture != nullptr) {
      SDL_DestroyTexture(ui.text_texture);
      ui.text_texture = nullptr;
    }

    SDL_Surface* text_surface = render_text_with_fallback(label);
    ui.text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    ui.text_width = text_surface->w;
    ui.text_height = text_surface->h;
    SDL_DestroySurface(text_surface);
  };

  rebuild_side(LEFT, left_file_name_);
  rebuild_side(RIGHT, format_right_file_label(left_file_name_, right_file_name_, active_right_index_ + 1));
}

// Aspect ratio of the video content layout with current mode + aspect-view adjustments.
float Display::compute_content_aspect_ratio() const {
  const float content_w = static_cast<float>(video_width_) * ((mode_ == Mode::HStack) ? 2.0F : 1.0F);
  const float content_h = static_cast<float>(video_height_) * ((mode_ == Mode::VStack) ? 2.0F : 1.0F);

  return content_w / std::max(content_h, 1.0F);
}

// Aspect ratio of the content area actually painted (may be locked to window aspect).
float Display::compute_active_content_aspect_ratio() const {
  auto apply_mode_layout_multiplier = [&](const float single_frame_ratio) {
    if (mode_ == Mode::HStack) {
      return single_frame_ratio * 2.0F;
    }
    if (mode_ == Mode::VStack) {
      return single_frame_ratio * 0.5F;
    }
    return single_frame_ratio;
  };

  switch (aspect_view_mode_) {
    case AspectViewMode::Stretch:
      return static_cast<float>(std::max(window_width_, 1)) / static_cast<float>(std::max(window_height_, 1));
    case AspectViewMode::Preset16x9:
      return apply_mode_layout_multiplier(16.0F / 9.0F);
    case AspectViewMode::Preset4x3:
      return apply_mode_layout_multiplier(4.0F / 3.0F);
    case AspectViewMode::Preset1x1:
      return apply_mode_layout_multiplier(1.0F);
    case AspectViewMode::Original:
    default:
      break;
  }

  return compute_content_aspect_ratio();
}

// Recompute content_window_ and the window/drawable/video scale factors from the current window state.
void Display::update_content_window_layout() {
  const int safe_window_w = std::max(1, window_width_);
  const int safe_window_h_raw = std::max(1, window_height_);

  // If the dock is visible, reserve its drawable-space height from the bottom
  // of the window. Mapping drawable→window via the height factor keeps the
  // reservation consistent on high-DPI displays.
  const int dock_h_drawable = dock_.dock_height_drawable();
  int dock_h_window = 0;
  if (dock_h_drawable > 0 && drawable_to_window_height_factor_ > 0.0f) {
    dock_h_window = static_cast<int>(std::round(static_cast<float>(dock_h_drawable) / drawable_to_window_height_factor_));
    dock_h_window = std::min(dock_h_window, safe_window_h_raw - 32);  // leave at least some video area
    if (dock_h_window < 0) dock_h_window = 0;
  }
  const int safe_window_h = std::max(1, safe_window_h_raw - dock_h_window);

  content_window_ = SDL_Rect{0, 0, safe_window_w, safe_window_h};

  // When the dock is reserving space at the bottom we always letterbox the
  // video to its natural content aspect inside the remaining stage — even in
  // Stretch mode — so opening the dock shrinks the canvas without squeezing
  // the picture vertically. The full-window Stretch behavior is preserved
  // exactly when the dock is hidden.
  const bool letterbox_to_content = aspect_view_mode_ != AspectViewMode::Stretch || dock_h_window > 0;

  if (letterbox_to_content) {
    const float content_aspect_ratio = std::max(
        (aspect_view_mode_ == AspectViewMode::Stretch) ? compute_content_aspect_ratio()
                                                       : compute_active_content_aspect_ratio(),
        0.001F);
    const float window_aspect_ratio = static_cast<float>(safe_window_w) / static_cast<float>(safe_window_h);

    if (window_aspect_ratio > content_aspect_ratio) {
      content_window_.h = safe_window_h;
      content_window_.w = std::max(1, static_cast<int>(std::round(static_cast<float>(content_window_.h) * content_aspect_ratio)));
      content_window_.x = (safe_window_w - content_window_.w) / 2;
    } else {
      content_window_.w = safe_window_w;
      content_window_.h = std::max(1, static_cast<int>(std::round(static_cast<float>(content_window_.w) / content_aspect_ratio)));
      content_window_.y = (safe_window_h - content_window_.h) / 2;
    }
  }

  const float content_w = static_cast<float>(std::max(1, video_width_)) * ((mode_ == Mode::HStack) ? 2.0F : 1.0F);
  const float content_h = static_cast<float>(std::max(1, video_height_)) * ((mode_ == Mode::VStack) ? 2.0F : 1.0F);
  video_to_window_width_factor_ = content_w / static_cast<float>(std::max(1, content_window_.w));
  video_to_window_height_factor_ = content_h / static_cast<float>(std::max(1, content_window_.h));
}

// Query the current display for HDR availability/headroom; flag changed state for next refresh.
void Display::update_hdr_display_state() {
  bool hdr_available = false;
  float hdr_headroom = 1.0f;

  SDL_PropertiesID window_props = SDL_GetWindowProperties(window_);
  if (window_props != 0) {
    hdr_available = SDL_GetBooleanProperty(window_props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    hdr_headroom = SDL_GetFloatProperty(window_props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
  }

  if (hdr_available != hdr_display_available_) {
    hdr_display_available_ = hdr_available;
    hdr_display_headroom_ = hdr_headroom;
    hdr_state_changed_ = true;

    std::cerr << "HDR display " << (hdr_available ? "available" : "not available") << " (headroom: " << hdr_headroom << ")" << std::endl;
  }
}

// One-shot: returns true when HDR state has changed since the last call and clears the flag.
bool Display::consume_hdr_state_change() {
  if (hdr_state_changed_) {
    hdr_state_changed_ = false;
    return true;
  }
  return false;
}

// React to an OS-driven window resize: re-derive layout, refresh HDR, optionally reset the forced-size guard.
void Display::handle_window_resize(const bool reset_forced_size_guard, const bool force_layout_refresh) {
  if (reset_forced_size_guard) {
    last_forced_window_size_ = {-1, -1};
  }

  int new_drawable_w = 0;
  int new_drawable_h = 0;
  int new_window_w = 0;
  int new_window_h = 0;

  // Query both logical window size and drawable size since they can diverge (e.g. high-DPI).
  SDL_GetWindowSizeInPixels(window_, &new_drawable_w, &new_drawable_h);
  SDL_GetWindowSize(window_, &new_window_w, &new_window_h);

  bool was_fullscreen = is_fullscreen_;
  is_fullscreen_ = detect_fullscreen_like_state();

  auto force_window_size = [&](int width, int height) {
    last_forced_window_size_ = {width, height};
    SDL_SetWindowSize(window_, width, height);
  };

  const bool skip_forced_size = (last_forced_window_size_[0] == new_window_w && last_forced_window_size_[1] == new_window_h);

  // Enforce minimum window size early to keep downstream math well-defined.
  if (!skip_forced_size && (new_window_w < MIN_WINDOW_WIDTH || new_window_h < MIN_WINDOW_HEIGHT)) {
    force_window_size(std::max(new_window_w, MIN_WINDOW_WIDTH), std::max(new_window_h, MIN_WINDOW_HEIGHT));
    return;
  }

  // If aspect-ratio locking is enabled, snap the resize to the selected ratio.
  if (!skip_forced_size && !is_fullscreen_ && aspect_lock_mode_ != AspectLockMode::Off) {
    const float target_aspect_ratio = (aspect_lock_mode_ == AspectLockMode::Window) ? window_aspect_ratio_ : compute_active_content_aspect_ratio();
    const float safe_target_aspect_ratio = std::max(target_aspect_ratio, 0.001F);
    const float current_ratio = static_cast<float>(new_window_w) / static_cast<float>(new_window_h);
    if (std::abs(current_ratio - safe_target_aspect_ratio) > 0.001F) {
      int target_w = new_window_w;
      int target_h = new_window_h;

      if (current_ratio > safe_target_aspect_ratio) {
        target_w = static_cast<int>(std::round(new_window_h * safe_target_aspect_ratio));
        if (target_w < MIN_WINDOW_WIDTH) {
          target_w = MIN_WINDOW_WIDTH;
          target_h = std::max(MIN_WINDOW_HEIGHT, static_cast<int>(std::round(static_cast<float>(target_w) / safe_target_aspect_ratio)));
        }
      } else {
        target_h = static_cast<int>(std::round(new_window_w / safe_target_aspect_ratio));
        if (target_h < MIN_WINDOW_HEIGHT) {
          target_h = MIN_WINDOW_HEIGHT;
          target_w = std::max(MIN_WINDOW_WIDTH, static_cast<int>(std::round(static_cast<float>(target_h) * safe_target_aspect_ratio)));
        }
      }

      if (target_w != new_window_w || target_h != new_window_h) {
        force_window_size(target_w, target_h);
        return;
      }
    }
  }

  // Ignore invalid sizes (can occur during platform-specific resize transitions).
  if (new_drawable_w <= 0 || new_drawable_h <= 0 || new_window_w <= 0 || new_window_h <= 0) {
    return;
  }

  // No change means we can skip the expensive rebuilds below.
  if (!force_layout_refresh && new_drawable_w == drawable_width_ && new_drawable_h == drawable_height_ && new_window_w == window_width_ && new_window_h == window_height_) {
    return;
  }

  drawable_width_ = new_drawable_w;
  drawable_height_ = new_drawable_h;
  window_width_ = new_window_w;
  window_height_ = new_window_h;

  if (gpu_renderer_active_) {
    gpu_renderer_.resize(drawable_width_, drawable_height_);
  }

  drawable_to_window_width_factor_ = static_cast<float>(drawable_width_) / static_cast<float>(window_width_);
  drawable_to_window_height_factor_ = static_cast<float>(drawable_height_) / static_cast<float>(window_height_);
  // The dock's layout has to be computed before update_content_window_layout
  // because the latter subtracts the dock-bar height from the renderable area.
  dock_.layout(drawable_width_, drawable_height_);
  update_content_window_layout();

  font_scale_ = (drawable_to_window_width_factor_ + drawable_to_window_height_factor_) / 2.0F;

  border_extension_ = 3 * font_scale_;
  double_border_extension_ = border_extension_ * 2;
  line1_y_ = 20;
  line2_y_ = line1_y_ + 30 * font_scale_;

  // Keep text clipping consistent with the new drawable width.
  if (mode_ != Mode::VStack) {
    max_text_width_ = drawable_width_ / 2 - double_border_extension_ - line1_y_;
  } else {
    max_text_width_ = drawable_width_ - double_border_extension_ - line1_y_;
  }

  // Rebuild cached UI assets that are size-dependent (fonts, help, metadata, labels).
  if (renderer_) {
    SDL_SetRenderLogicalPresentation(renderer_, drawable_width_, drawable_height_, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  }

  rebuild_fonts();
  rebuild_side_ui_textures();
  overlay_.rebuild_help(small_font_, big_font_, renderer_, gpu_renderer_active_, drawable_width_, drawable_height_);
  metadata_panel_.mark_dirty();

  // Clamp overlay scroll positions to the new size and refresh ROI-dependent title.
  clamp_overlay_offsets();
  update_window_title_with_current_roi();

  // Trigger a synthetic no-op mouse move after fullscreen transitions to force
  // slider/UI refresh without requiring physical mouse movement.
  if (was_fullscreen != is_fullscreen_) {
    float global_mouse_x = 0;
    float global_mouse_y = 0;
    SDL_GetGlobalMouseState(&global_mouse_x, &global_mouse_y);
    SDL_WarpMouseGlobal(global_mouse_x, global_mouse_y);

    set_pending_message(string_sprintf("Fullscreen mode set to '%s'", is_fullscreen_ ? "ON" : "OFF"));
  }
}

// Recompute swap-aware left/right side identifiers from swap_left_right_.
void Display::refresh_display_side_mapping() {
  displayed_left_side_ = swap_left_right_ ? RIGHT : LEFT;
  displayed_right_side_ = swap_left_right_ ? LEFT : RIGHT;
}

// Store per-side metadata strings and mark the metadata panel dirty so it rebuilds on next render.
void Display::update_metadata(const VideoMetadata left_metadata, const VideoMetadata right_metadata) {
  metadata_panel_.update(left_metadata, right_metadata);
}

// Switch the active right-side video, updating filename, metadata, title, and side-UI textures.
void Display::update_right_video(const std::string& right_file_name, const VideoMetadata right_metadata) {
  metadata_panel_.update(metadata_panel_.left(), right_metadata);
  right_file_name_ = right_file_name;

  // Update right file stem (used by both renderer paths for save filenames)
  side_ui_[RIGHT.as_simple_index()].file_stem = strip_ffmpeg_patterns(get_file_stem(right_file_name));

  // Destroy old right texture (SDL path only — GPU renders labels inline)
  if (side_ui_[RIGHT.as_simple_index()].text_texture != nullptr) {
    SDL_DestroyTexture(side_ui_[RIGHT.as_simple_index()].text_texture);
    side_ui_[RIGHT.as_simple_index()].text_texture = nullptr;
  }

  if (!gpu_renderer_active_) {
    SDL_Surface* text_surface = render_text_with_fallback(format_right_file_label(left_file_name_, right_file_name, active_right_index_ + 1));
    side_ui_[RIGHT.as_simple_index()].text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
    side_ui_[RIGHT.as_simple_index()].text_width = text_surface->w;
    side_ui_[RIGHT.as_simple_index()].text_height = text_surface->h;
    SDL_DestroySurface(text_surface);
  }

  // Update window title (may include ROI)
  update_window_title_with_current_roi();
}

// Set the window title to reflect the current files and (when zoomed) the visible ROI.
void Display::update_window_title_with_current_roi() {
  const std::string base_title = format_window_title(left_file_name_, right_file_name_);

  std::string title = base_title;

  auto format_roi_bbox = [](const char* label, const SDL_Rect& r) -> std::string { return string_sprintf("%s(%d,%d)-(%d,%d)", label, r.x, r.y, r.x + r.w - 1, r.y + r.h - 1); };

  if (mode_ == Mode::HStack || mode_ == Mode::VStack) {
    const auto rois = get_visible_rois_in_single_frame_coordinates();
    const SDL_Rect left_roi = rois.first;
    const SDL_Rect right_roi = rois.second;

    const bool left_off = left_roi.w <= 0 || left_roi.h <= 0;
    const bool right_off = right_roi.w <= 0 || right_roi.h <= 0;

    const bool left_full = !left_off && left_roi.x == 0 && left_roi.y == 0 && left_roi.w == video_width_ && left_roi.h == video_height_;
    const bool right_full = !right_off && right_roi.x == 0 && right_roi.y == 0 && right_roi.w == video_width_ && right_roi.h == video_height_;

    if (left_off || right_off || !left_full || !right_full) {
      title += "   ";

      const std::string left_roi_str = (!left_off && !left_full) ? format_roi_bbox("L", left_roi) : "";
      const std::string right_roi_str = (!right_off && !right_full) ? format_roi_bbox("R", right_roi) : "";

      title += left_roi_str;
      if (!left_roi_str.empty() && !right_roi_str.empty()) {
        title += " ";
      }
      title += right_roi_str;
    }
  } else {
    const SDL_Rect roi = get_visible_roi_in_single_frame_coordinates();
    const bool roi_is_full = (roi.x == 0 && roi.y == 0 && roi.w == video_width_ && roi.h == video_height_);
    if (!roi_is_full && roi.w > 0 && roi.h > 0) {
      title += string_sprintf("   %s", format_roi_bbox("", roi).c_str());
    }
  }

  if (title != last_window_title_) {
    SDL_SetWindowTitle(window_, title.c_str());
    last_window_title_ = title;
  }
}
