#include "display/display.h"
#include <libgen.h>
#include <algorithm>
#include <atomic>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include "display/controls.h"
#include "display_utils.h"
#include "core/ffmpeg/ffmpeg.h"
#include "display/jxl_saver.h"
#include "analysis/metrics/metrics_calculator.h"
#include "pixel_format_utils.h"
#include "analysis/scopes/scope_window.h"
#include "assets/fonts/source_code_pro_regular_ttf.h"
#include "app/version.h"
#include "assets/icons/video_compare_icon.h"
#include "analysis/metrics/vmaf_calculator.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

SDL::SDL() {
  check_sdl(SDL_Init(SDL_INIT_VIDEO), "SDL init");
  check_sdl(TTF_Init(), "TTF init");
}

SDL::~SDL() {
  SDL_Quit();
}

// Construct the main window, font resources, GPU renderer, and all collaborator units.
Display::Display(const int display_number,
                 const Mode mode,
                 const bool verbose,
                 const bool fit_window_to_usable_bounds,
                 const bool high_dpi_allowed,
                 const AspectLockMode aspect_lock_mode,
                 const AspectViewMode aspect_view_mode,
                 const bool use_10_bpc,
                 const bool fast_input_alignment,
                 const bool bilinear_texture_filtering,
                 const std::tuple<int, int> window_size,
                 const unsigned width,
                 const unsigned height,
                 const double duration,
                 const float wheel_sensitivity,
                 const bool start_in_subtraction_mode,
                 const bool start_in_fullscreen,
                 const bool start_paused,
                 const std::string& left_file_name,
                 const std::string& right_file_name)
    : display_number_{display_number},
      mode_{mode},
      fit_window_to_usable_bounds_{fit_window_to_usable_bounds},
      high_dpi_allowed_{high_dpi_allowed},
      aspect_lock_mode_{aspect_lock_mode},
      aspect_view_mode_{aspect_view_mode},
      use_10_bpc_{use_10_bpc},
      fast_input_alignment_{fast_input_alignment},
      bilinear_texture_filtering_{bilinear_texture_filtering},
      video_width_{0},
      video_height_{0},
      duration_{duration},
      start_in_fullscreen_{start_in_fullscreen},
      pending_verbose_print_{verbose},
      layout_adapter_(*this),
      view_transform_(layout_adapter_),
      diff_processor_(row_workers_, start_in_subtraction_mode),
      wheel_sensitivity_{wheel_sensitivity} {
  view_transform_.set_on_change([this]() { refresh_selection_end_from_mouse(); });
  image_saver_.set_notifier([this](const std::string& msg) { notify_user(msg); });
  // Honour --start-paused. Can't flip playback_ to paused right away because
  // the ring is still empty and the main loop only advances the ring when
  // play_ is true (or a forward-nav is pending); flipping early would leave
  // the display without a current frame to render. Instead, defer the pause
  // until after possibly_refresh actually rendered the first frame.
  pending_startup_pause_ = start_paused;
  const int auto_width = mode == Mode::HStack ? width * 2 : width;
  const int auto_height = mode == Mode::VStack ? height * 2 : height;

  int window_x;
  int window_y;
  int window_width;
  int window_height;

  // Account for window frame and title bar (in points — same units as
  // SDL_GetDisplayUsableBounds and SDL_GetWindowSize). Platform-tuned
  // fallbacks; SDL_GetWindowBordersSize replaces these at warn-time when
  // available (Windows, some X11), but reliably returns 0 on macOS.
#if defined(__APPLE__)
  constexpr int border_width = 0;   // macOS windows have no side chrome.
  constexpr int border_height = 28; // Standard macOS titlebar height.
#elif defined(__linux__)
  constexpr int border_width = 10;
  constexpr int border_height = 40;
#else
  constexpr int border_width = 10;
  constexpr int border_height = 34;
#endif

  SDL_Rect bounds;
  const SDL_DisplayID display_id = display_id_for_index(display_number);
  check_sdl(SDL_GetDisplayUsableBounds(display_id, &bounds), "get display usable bounds");

  if (!fit_window_to_usable_bounds) {
    if (std::get<0>(window_size) < 0 && std::get<1>(window_size) < 0) {
      window_width = auto_width;
      window_height = auto_height;
    } else {
      if (std::get<0>(window_size) < 0) {
        window_height = std::get<1>(window_size);
        window_width = static_cast<float>(auto_width) / static_cast<float>(auto_height) * window_height;
      } else if (std::get<1>(window_size) < 0) {
        window_width = std::get<0>(window_size);
        window_height = static_cast<float>(auto_height) / static_cast<float>(auto_width) * window_width;
      } else {
        window_width = std::get<0>(window_size);
        window_height = std::get<1>(window_size);
      }
    }

    window_x = SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number);
    window_y = SDL_WINDOWPOS_UNDEFINED_DISPLAY(display_number);

    if (high_dpi_allowed_) {
      window_width /= 2;
      window_height /= 2;
    }
  } else {
    const int usable_width = std::max(bounds.w - border_width, MIN_WINDOW_WIDTH);
    const int usable_height = std::max(bounds.h - border_height, MIN_WINDOW_HEIGHT);

    const float aspect_ratio = static_cast<float>(auto_width) / static_cast<float>(auto_height);
    const float usable_aspect_ratio = static_cast<float>(usable_width) / static_cast<float>(usable_height);

    if (usable_aspect_ratio > aspect_ratio) {
      window_height = usable_height;
      window_width = static_cast<int>(window_height * aspect_ratio);
    } else {
      window_width = usable_width;
      window_height = static_cast<int>(window_width / aspect_ratio);
    }

    window_x = bounds.x + (usable_width - window_width + border_width) / 2;
    window_y = bounds.y + (usable_height - window_height + border_height) / 2 + border_width;
#ifdef __linux__
    window_y -= 2 * border_width + 4;
#endif
  }

  if (window_width < MIN_WINDOW_WIDTH) {
    throw std::runtime_error{"Window width cannot be less than " + std::to_string(MIN_WINDOW_WIDTH)};
  }
  if (window_height < MIN_WINDOW_HEIGHT) {
    throw std::runtime_error{"Window height cannot be less than " + std::to_string(MIN_WINDOW_HEIGHT)};
  }

  // Try creating a Vulkan window for GPU-accelerated rendering.
  // Fall back to SDL_Renderer if Vulkan initialisation fails.
  SDL_WindowFlags create_window_flags = SDL_WINDOW_RESIZABLE | (high_dpi_allowed_ ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0);
  renderer_ = nullptr;

  // Attempt Vulkan path first.
  window_ = SDL_CreateWindow(format_window_title(left_file_name, right_file_name).c_str(),
                             window_width, window_height,
                             create_window_flags | SDL_WINDOW_VULKAN);
  if (window_ && gpu_renderer_.init(window_)) {
    gpu_renderer_active_ = true;
    std::cerr << "Display: using libplacebo GPU renderer" << std::endl;
  } else {
    // Vulkan failed — destroy the window (if created) and try without Vulkan.
    if (window_) {
      gpu_renderer_.destroy();
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    window_ = check_sdl(SDL_CreateWindow(format_window_title(left_file_name, right_file_name).c_str(),
                                         window_width, window_height, create_window_flags),
                        "window");
    gpu_renderer_active_ = false;
    std::cerr << "Display: Vulkan unavailable, using SDL_Renderer" << std::endl;
  }

  SDL_SetWindowPosition(window_, window_x, window_y);

  SDL_IOStream* embedded_icon = check_sdl(SDL_IOFromConstMem(VIDEO_COMPARE_ICON_BMP, VIDEO_COMPARE_ICON_BMP_LEN), "get pointer to icon");
  SDL_Surface* icon_surface = check_sdl(SDL_LoadBMP_IO(embedded_icon, true), "load icon");

#ifdef _WIN32
  SDL_Surface* resized_icon_surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_ARGB8888);
  SDL_BlitSurfaceScaled(icon_surface, nullptr, resized_icon_surface, nullptr, SDL_SCALEMODE_LINEAR);
  SDL_SetWindowIcon(window_, resized_icon_surface);
  SDL_DestroySurface(resized_icon_surface);
#else
  SDL_SetWindowIcon(window_, icon_surface);
#endif

  SDL_DestroySurface(icon_surface);

  if (!gpu_renderer_active_) {
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window_);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_OUTPUT_COLORSPACE_NUMBER, SDL_COLORSPACE_SRGB_LINEAR);
    renderer_ = check_sdl(SDL_CreateRendererWithProperties(props), "renderer");
    SDL_DestroyProperties(props);
    SDL_SetRenderVSync(renderer_, 1);

    // Detect HDR display capability (SDL renderer path)
    update_hdr_display_state();

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderPresent(renderer_);
  }

  SDL_GetWindowSizeInPixels(window_, &drawable_width_, &drawable_height_);
  SDL_GetWindowSize(window_, &window_width_, &window_height_);
  window_aspect_ratio_ = static_cast<float>(window_width_) / static_cast<float>(std::max(1, window_height_));
  startup_window_size_ = {window_width_, window_height_};
  saved_window_size_ = startup_window_size_;

  // Check if the window (including its frame/titlebar) is larger than the
  // display's usable area and warn the user. Prefer the window's actual
  // frame sizes from SDL over the hardcoded pre-creation estimates — on
  // macOS the side borders are 0 and the titlebar is ~28 pts (not 10/34),
  // so the estimates produce false-positive warnings when a legitimately-
  // fitting window is computed from a Retina-sized video.
  int frame_top = 0;
  int frame_left = 0;
  int frame_bottom = 0;
  int frame_right = 0;
  const bool have_frame = SDL_GetWindowBordersSize(window_, &frame_top, &frame_left, &frame_bottom, &frame_right);

  // Fallback to the pre-creation estimates when the platform can't report
  // actual chrome (e.g. Wayland, where the compositor owns the frame).
  const int chrome_w = have_frame ? (frame_left + frame_right) : border_width;
  const int chrome_h = have_frame ? (frame_top + frame_bottom) : border_height;

  const int total_window_width = window_width_ + chrome_w;
  const int total_window_height = window_height_ + chrome_h;

  if (total_window_width > bounds.w || total_window_height > bounds.h) {
    std::cout << "WARNING: Window size (" << total_window_width << "x" << total_window_height << ") exceeds display area (" << bounds.w << "x" << bounds.h
              << "). Consider reducing the window size (use -W flag to resize) or using a larger display." << std::endl;

    set_pending_message("Window exceeds display area (use -W flag to resize)");
  }

  drawable_to_window_width_factor_ = static_cast<float>(drawable_width_) / static_cast<float>(window_width_);
  drawable_to_window_height_factor_ = static_cast<float>(drawable_height_) / static_cast<float>(window_height_);
  content_window_ = SDL_Rect{0, 0, window_width_, window_height_};
  video_to_window_width_factor_ = 1.0F;
  video_to_window_height_factor_ = 1.0F;

  font_scale_ = (drawable_to_window_width_factor_ + drawable_to_window_height_factor_) / 2.0F;

  border_extension_ = 3 * font_scale_;
  double_border_extension_ = border_extension_ * 2;
  line1_y_ = 20;
  line2_y_ = line1_y_ + 30 * font_scale_;

  if (mode_ != Mode::VStack) {
    max_text_width_ = drawable_width_ / 2 - double_border_extension_ - line1_y_;
  } else {
    max_text_width_ = drawable_width_ - double_border_extension_ - line1_y_;
  }

  rebuild_fonts();

  normal_mode_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  pan_mode_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_MOVE);
  selection_mode_cursor_ = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);

  if (renderer_) {
    SDL_SetRenderLogicalPresentation(renderer_, drawable_width_, drawable_height_, SDL_LOGICAL_PRESENTATION_LETTERBOX);
  }

  // Store left/right names before reinitializing dimensions since it may refresh title text.
  left_file_name_ = left_file_name;
  right_file_name_ = right_file_name;
  last_window_title_.clear();

  reinitialize_video_dimensions(width, height);

  // Match startup behavior to manual aspect-mode switching by normalizing the
  // window size to the selected aspect mode before first interaction.
  if (aspect_view_mode_ != AspectViewMode::Stretch) {
    const auto target_size = compute_mode_switch_target_window_size();
    apply_window_size_and_relayout(target_size[0], target_size[1], true);
  }

  rebuild_side_ui_textures();

  refresh_display_side_mapping();

  overlay_.rebuild_help(small_font_, big_font_, renderer_, gpu_renderer_active_, drawable_width_);

  if (start_in_fullscreen_) {
    set_fullscreen(true);
  }
}

// Destroy textures, cursors, and window in reverse construction order.
Display::~Display() {
  if (gpu_renderer_active_) {
    gpu_renderer_.destroy();
  }

  for (int s = 0; s < kSideCount; s++) {
    if (side_textures_linear_[s]) SDL_DestroyTexture(side_textures_linear_[s]);
    if (side_textures_nn_[s]) SDL_DestroyTexture(side_textures_nn_[s]);
  }

  if (!gpu_renderer_active_) {
    SDL_DestroyTexture(side_ui_[LEFT.as_simple_index()].text_texture);
    SDL_DestroyTexture(side_ui_[RIGHT.as_simple_index()].text_texture);
  }

  destroy_dock_thumb_textures();

  // OverlayManager and MetadataPanel clean up their own textures/surfaces.

  TTF_CloseFont(small_font_);
  TTF_CloseFont(big_font_);

  SDL_DestroyCursor(normal_mode_cursor_);
  SDL_DestroyCursor(pan_mode_cursor_);
  SDL_DestroyCursor(selection_mode_cursor_);

  // DifferenceProcessor and RgbFrameCache clean themselves up via destructors.

  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
  }
  SDL_DestroyWindow(window_);
}

// Dump current window, layout, HDR, and playback state to stdout.
void Display::print_verbose_info() {
  std::cout << "Main program version:  " << VersionInfo::version << std::endl;
  std::cout << "Video size:            " << video_width_ << "x" << video_height_ << std::endl;
  std::cout << "Video duration:        " << format_duration(duration_) << std::endl;
  std::cout << "Display mode:          " << mode_to_string(mode_) << std::endl;
  std::cout << "Fit to usable bounds:  " << std::boolalpha << fit_window_to_usable_bounds_ << std::endl;
  std::cout << "High-DPI allowed:      " << std::boolalpha << high_dpi_allowed_ << std::endl;
  std::cout << "Aspect lock mode:      " << aspect_lock_mode_to_string(aspect_lock_mode_) << std::endl;
  std::cout << "Aspect view mode:      " << aspect_view_mode_to_string(aspect_view_mode_) << std::endl;
  std::cout << "Use 10 bpc:            " << std::boolalpha << use_10_bpc_ << std::endl;
  std::cout << "HDR passthrough:       " << std::boolalpha << hdr_passthrough_ << std::endl;
  std::cout << "Fast input alignment:  " << std::boolalpha << fast_input_alignment_ << std::endl;
  std::cout << "Bilinear filtering:    " << std::boolalpha << bilinear_texture_filtering_ << std::endl;
  std::cout << "Mouse whl sensitivity: " << wheel_sensitivity_ << std::endl;

  const int sdl_ver = SDL_GetVersion();
  std::cout << "SDL version:           " << string_sprintf("%u.%u.%u", SDL_VERSIONNUM_MAJOR(sdl_ver), SDL_VERSIONNUM_MINOR(sdl_ver), SDL_VERSIONNUM_MICRO(sdl_ver)) << std::endl;

  const int ttf_ver = TTF_Version();
  std::cout << "SDL_ttf version:       " << string_sprintf("%u.%u.%u", SDL_VERSIONNUM_MAJOR(ttf_ver), SDL_VERSIONNUM_MINOR(ttf_ver), SDL_VERSIONNUM_MICRO(ttf_ver)) << std::endl;

  if (gpu_renderer_active_) {
    std::cout << "SDL renderer:          libplacebo (Vulkan)" << std::endl;
  } else {
    const char* renderer_name = SDL_GetRendererName(renderer_);
    std::cout << "SDL renderer:          " << (renderer_name ? renderer_name : "unknown") << std::endl;
  }

  SDL_DisplayID current_display_id = SDL_GetDisplayForWindow(window_);
  std::cout << "SDL display ID:        " << current_display_id << std::endl;

  const SDL_DisplayMode* desktop_display_mode = SDL_GetDesktopDisplayMode(current_display_id);
  if (desktop_display_mode) {
    std::cout << "SDL desktop size:      " << desktop_display_mode->w << "x" << desktop_display_mode->h << std::endl;
  }

  std::cout << "SDL GL drawable size:  " << drawable_width_ << "x" << drawable_height_ << std::endl;
  std::cout << "SDL window size:       " << window_width_ << "x" << window_height_ << std::endl;

  auto stringify_format_and_bpp = [&](SDL_PixelFormat pixel_format) -> std::string { return string_sprintf("%s (%d bpp)", SDL_GetPixelFormatName(pixel_format), SDL_BITSPERPIXEL(pixel_format)); };

  const SDL_PixelFormat video_pixel_format = requires_10_bpc() ? SDL_PIXELFORMAT_ARGB2101010 : SDL_PIXELFORMAT_RGB24;
  std::cout << "SDL video px format:   " << stringify_format_and_bpp(video_pixel_format) << std::endl;

  std::cout << "FFmpeg version:        " << av_version_info() << std::endl;
  std::cout << "libavutil version:     " << format_libav_version(avutil_version()) << std::endl;
  std::cout << "libavcodec version:    " << format_libav_version(avcodec_version()) << std::endl;
  std::cout << "libavformat version:   " << format_libav_version(avformat_version()) << std::endl;
  std::cout << "libavfilter version:   " << format_libav_version(avfilter_version()) << std::endl;
  std::cout << "libswscale version:    " << format_libav_version(swscale_version()) << std::endl;
  std::cout << "libswresample version: " << format_libav_version(swresample_version()) << std::endl;
  std::cout << "libavcodec configuration: " << avcodec_configuration() << std::endl << std::endl;
}
// Main-loop entry: early-out if nothing changed, build a per-frame RenderContext, then dispatch to the GPU or SDL render path.
bool Display::possibly_refresh(const AVFrame* left_frame, const AVFrame* right_frame, const std::string& current_total_browsable) {
  const std::string left_frame_key = get_frame_key(left_frame);
  const std::string right_frame_key = get_frame_key(right_frame);

  const bool has_updated_left_frame = previous_left_frame_key_ != left_frame_key;
  const bool has_updated_right_frame = previous_right_frame_key_ != right_frame_key;

  if (!input_received_ && !has_updated_left_frame && !has_updated_right_frame && !timer_based_update_performed_ && overlay_.pending_message().empty()) {
    return false;
  }

  // Reset each frame; set below by animations that need a periodic refresh
  // (loop-mode blink, fading message) even when no new input arrives.
  timer_based_update_performed_ = false;

  const bool compare_mode = show_left_ && show_right_;
  const auto zoom_rect = view_transform_.compute_zoom_rect();
  const float content_mouse_x = static_cast<float>(mouse_x_ - content_window_.x);
  const float safe_content_window_w = static_cast<float>(std::max(1, content_window_.w));
  const float full_ws_mouse_video_x = (content_mouse_x * safe_content_window_w / std::max(1.0F, safe_content_window_w - 1.0F)) * video_to_window_width_factor_;
  const float video_mouse_x = (full_ws_mouse_video_x - zoom_rect.start.x()) * static_cast<float>(video_width_) / zoom_rect.size.x();
  const float video_texel_clamped_mouse_x = static_cast<float>(content_window_.x) +
      (std::round(video_mouse_x) * zoom_rect.size.x() / static_cast<float>(video_width_) + zoom_rect.start.x()) / video_to_window_width_factor_;
  const int split_x = (compare_mode && mode_ == Mode::Split)
                          ? clamp_range(std::round(video_mouse_x), 0.0F, float(video_width_))
                          : show_left_ ? video_width_ : 0;
  const int dst_zoomed_size = static_cast<int>(std::round(std::min(drawable_width_, drawable_height_) * 0.5F)) & -2;
  const RenderContext ctx{
      left_frame,
      right_frame,
      has_updated_left_frame,
      has_updated_right_frame,
      compare_mode,
      zoom_rect,
      video_mouse_x,
      video_texel_clamped_mouse_x,
      split_x,
      dst_zoomed_size,
      dst_zoomed_size / 2,
  };

  if (gpu_renderer_active_) {
    render_frame_gpu(ctx, current_total_browsable);
  } else {
    render_frame_sdl(ctx, current_total_browsable);
  }

  input_received_ = false;
  previous_left_frame_pts_ = left_frame->pts;
  previous_right_frame_pts_ = right_frame->pts;
  previous_left_frame_key_ = left_frame_key;
  previous_right_frame_key_ = right_frame_key;
  // First-frame-rendered hook for --start-paused: the ring now has a current
  // frame on both sides, so flipping to paused here is safe — subsequent
  // iterations will keep displaying this frame without further ring advances.
  if (pending_startup_pause_) {
    playback_.set_play(false);
    pending_startup_pause_ = false;
  }
  return true;
}
// Queue a transient center-screen message for display on the next frame.
void Display::set_pending_message(const std::string& message) {
  overlay_.set_pending_message(message);
}

// Print to stdout in windowed mode, or queue a toast message in fullscreen.
void Display::notify_user(const std::string& message) {
  if (!is_fullscreen_) {
    // Avoid cluttering the screen with messages in windowed mode
    std::cout << message << std::endl;
  } else {
    set_pending_message(message);
  }
}

// Raise the main window (counteracts scope windows stealing keyboard focus).
void Display::focus_main_window() {
  if (window_ != nullptr) {
    SDL_RaiseWindow(window_);
  }
}

// Whether the current display advertises HDR support.
bool Display::get_hdr_display_available() const {
  return hdr_display_available_;
}

// Measured HDR peak-to-SDR ratio for the current display.
float Display::get_hdr_display_headroom() const {
  return hdr_display_headroom_;
}

// Enable or disable HDR passthrough; recreates the side textures when the state flips.
void Display::set_hdr_passthrough(bool enabled) {
  if (hdr_passthrough_ != enabled) {
    hdr_passthrough_ = enabled;

    recreate_video_textures_for_current_mode();

    // Reallocate diff buffer for new bit depth and force packed-pixel buffers
    // to reallocate on next frame.
    diff_processor_.resize(video_width_, video_height_, requires_10_bpc());
  }
}

// Set the content's expected HDR headroom for libplacebo tone-mapping.
void Display::set_hdr_content_headroom(float headroom) {
  hdr_content_headroom_ = headroom;
}

// Whether the user has requested to quit.
bool Display::get_quit() const {
  return quit_;
}

// Whether playback is currently active.
bool Display::get_play() const {
  return playback_.play();
}

// High-level playback state (SEEK / LOOP > / LOOP <> / PLAY / PAUSE). The
// precedence ordering mirrors the HUD badge: a seek-in-flight masks loop
// mode, which masks play/pause.
Display::PlayState Display::get_play_state() const {
  if (!playback_in_sync_) {
    return PlayState::Seek;
  }
  const Loop loop_mode = playback_.loop_mode();
  if (loop_mode == Loop::ForwardOnly) {
    return PlayState::LoopForward;
  }
  if (loop_mode == Loop::PingPong) {
    return PlayState::LoopPingPong;
  }
  if (playback_.play()) {
    return PlayState::Play;
  }
  return PlayState::Pause;
}

// Human-readable label for a PlayState, without decoration. The HUD wraps
// the result in square brackets; other consumers may not want that.
const char* Display::play_state_label(PlayState state) {
  switch (state) {
    case PlayState::Seek:
      return "SEEK";
    case PlayState::LoopForward:
      return "LOOP >";
    case PlayState::LoopPingPong:
      return "LOOP <>";
    case PlayState::Play:
      return "PLAY";
    case PlayState::Pause:
      return "PAUSE";
  }
  return "UNKNOWN";
}

// Current loop mode for buffer playback (Off / ForwardOnly / PingPong).
Display::Loop Display::get_buffer_play_loop_mode() const {
  return playback_.loop_mode();
}

// Set the loop mode for buffer playback.
void Display::set_buffer_play_loop_mode(const Display::Loop& mode) {
  playback_.set_loop_mode(mode);
}

// Whether buffer playback is advancing forward (false = reverse).
bool Display::get_buffer_play_forward() const {
  return playback_.forward();
}

// Toggle forward/reverse buffer playback direction.
void Display::toggle_buffer_play_direction() {
  playback_.toggle_direction();
}

// Whether fast input alignment is enabled.
bool Display::get_fast_input_alignment() const {
  return fast_input_alignment_;
}

// Whether the left and right videos are logically swapped.
bool Display::get_swap_left_right() const {
  return swap_left_right_;
}

// Underlying pipeline side that maps to the user's "right video" intent.
// Normally RIGHT; flips to LEFT when swap is active because the visually-
// right side is then rendered by the underlying LEFT pipeline. Used by
// every swap-aware input path so the translation lives in one place.
Side Display::follower_side_for_input() const {
  return swap_left_right_ ? LEFT : RIGHT;
}

// Pending relative-seek offset (seconds), consumed by the main loop.
float Display::get_seek_relative() const {
  return playback_.seek_relative();
}

// Whether the pending seek is absolute (from start) rather than relative.
bool Display::get_seek_from_start() const {
  return playback_.seek_from_start();
}

// Pending frame-buffer offset delta consumed by the main loop.
int Display::get_frame_buffer_offset_delta() const {
  return playback_.frame_buffer_offset_delta();
}

// Pending frame-by-frame step delta consumed by the main loop.
int Display::get_frame_navigation_delta() const {
  return playback_.frame_navigation_delta();
}

// Pending shift count (+/- frames) to offset the right-side video by.
int Display::get_shift_right_frames() const {
  return playback_.shift_right_frames();
}

// Whether the user has requested automatic left/right alignment.
bool Display::get_auto_align_requested() const {
  return playback_.auto_align_requested();
}

// Direction / extent of the auto-align search window the user asked for.
AutoAlignMode Display::get_auto_align_mode() const {
  return playback_.auto_align_mode();
}

// True when the user's pending seek should move only the right video(s).
// Currently set by shift-click on the timeline.
bool Display::get_right_only_seek() const {
  return playback_.right_only_seek();
}

// Which side actually moves during a pending right-only seek. Meaningful
// only when get_right_only_seek() is true. Forwards PlaybackController.
Side Display::get_right_only_seek_follower() const {
  return playback_.right_only_seek_follower();
}

// Forward-setter used by the auto-align block when the follower is LEFT
// (swap) and it needs to dispatch via the absolute-seek path rather than
// shift_right_frames. Same plumbing the mouse-button handler uses for
// shift-click.
void Display::set_right_only_seek(bool value, Side follower) {
  playback_.set_right_only_seek(value, follower);
}

// Public wrapper around MetricsCalculator::compute_frame_ssim for alignment consumers in video_compare.cpp.
float Display::compute_frame_ssim(const AVFrame* left_frame, const AVFrame* right_frame) {
  return MetricsCalculator::compute_frame_ssim(left_frame, right_frame, requires_10_bpc());
}

// Current playback speed multiplier (1.0 = real-time).
float Display::get_playback_speed_factor() const {
  return playback_.playback_speed_factor();
}

// Whether the next frame should advance playback by one tick.
bool Display::get_tick_playback() const {
  return playback_.tick_playback();
}

// Whether playback may advance this frame (covers both play + single-step).
bool Display::get_possibly_tick_playback() const {
  return playback_.possibly_tick_playback();
}

// Whether the FPS counter is currently visible.
bool Display::get_show_fps() const {
  return show_fps_;
}

// Consume and return a pending scope-window toggle request for the given scope type.
bool Display::get_toggle_scope_window_requested(const ScopeWindow::Type type) const {
  return toggle_scope_window_requested_[ScopeWindow::index(type)];
}

// Consume and return any queued crop request (main loop applies it).
PendingCropRequest Display::get_and_clear_pending_crop_request() {
  return selection_.get_and_clear_pending_crop_request();
}

// Set the number of available right-side videos.
void Display::set_num_right_videos(const size_t num_right_videos) {
  num_right_videos_ = num_right_videos;
}

// Number of available right-side videos.
size_t Display::get_num_right_videos() const {
  return num_right_videos_;
}

// Index of the currently active right-side video.
size_t Display::get_active_right_index() const {
  return active_right_index_;
}

// Switch to a different right-side video by index.
void Display::set_active_right_index(const size_t index) {
  active_right_index_ = std::min(index, num_right_videos_ > 0 ? num_right_videos_ - 1 : 0UL);
}

// Initialize the dock with one entry per input video.
void Display::init_dock(std::vector<DockEntry> entries) {
  dock_.init(std::move(entries));
  // Reflect the current slot mapping in dock badge indices.
  const auto& dock_entries = dock_.entries();
  int left_idx = -1, right_idx = -1;
  for (size_t i = 0; i < dock_entries.size(); ++i) {
    if (dock_entries[i].pipeline_side == displayed_left_side_) left_idx = static_cast<int>(i);
    if (dock_entries[i].pipeline_side == displayed_right_side_) right_idx = static_cast<int>(i);
  }
  dock_.set_slot_indices(left_idx, right_idx);

  // If the dock starts visible (default), entries land after the initial
  // window-resize/layout pass, so the dock rects were last computed with
  // zero entries. Re-run the visibility-change hook to lay out against the
  // populated entry list and reserve content-area space for the bar.
  if (dock_.visible()) {
    on_dock_visibility_changed();
  }
}

std::pair<int, int> Display::dock_thumb_target_size() const {
  // Compute a representative target size from the current drawable.
  // Fall back to defaults if not yet laid out — the loader uses this once at
  // startup before the first layout pass.
  const int w = (dock_.thumb_target_max_width() > 0) ? dock_.thumb_target_max_width() : 160;
  const int h = (dock_.thumb_target_max_height() > 0) ? dock_.thumb_target_max_height() : 90;
  return {w, h};
}

void Display::set_dock_thumbnail(int entry_index, DockBitmap bitmap) {
  dock_.set_thumbnail(entry_index, std::move(bitmap));
}

std::vector<Dock::EntryResult> Display::get_dock_results() const {
  return dock_.snapshot_results();
}

// Return the pipeline Side currently occupying the requested visual slot.
// slot 0 = visual-left, 1 = visual-right.
Side Display::get_slot_side(int slot) const {
  return (slot == 0) ? displayed_left_side_ : displayed_right_side_;
}

// Assign a pipeline to a visual slot. Maintains the legacy swap_left_right_
// flag (true iff the slots are {LEFT, RIGHT} swapped to {RIGHT, LEFT}) and
// updates active_right_index_ to whichever slot now holds a RIGHT pipeline so
// existing right-targeted operations (frame shift, auto-align, crop "right",
// save filenames) continue to target a *visible* pipeline.
void Display::set_slot_side(int slot, Side side) {
  if (!side.is_valid()) {
    return;
  }
  if (slot == 0) {
    displayed_left_side_ = side;
  } else if (slot == 1) {
    displayed_right_side_ = side;
  } else {
    return;
  }

  // Legacy swap flag: only "true swap" when the two slots are LEFT↔RIGHT(0).
  // Any other arrangement (e.g. two Rights, or RIGHT(2) on visual-left)
  // doesn't fit the old binary swap semantics, so leave the flag false.
  swap_left_right_ = displayed_left_side_.is_right() && displayed_right_side_.is_left();

  // Keep active_right_index_ pointing at a *visible* right pipeline when
  // possible. Prefer the visual-right slot's right; fall back to visual-left's.
  size_t new_active = active_right_index_;
  if (displayed_right_side_.is_right()) {
    new_active = displayed_right_side_.right_index();
  } else if (displayed_left_side_.is_right()) {
    new_active = displayed_left_side_.right_index();
  }
  if (new_active != active_right_index_) {
    active_right_index_ = std::min(new_active, num_right_videos_ > 0 ? num_right_videos_ - 1 : 0UL);
  }

  // Sync the dock's L/R badge indices so the picker reflects current slots.
  const auto& dock_entries = dock_.entries();
  int left_idx = -1, right_idx = -1;
  for (size_t i = 0; i < dock_entries.size(); ++i) {
    if (dock_entries[i].pipeline_side == displayed_left_side_) left_idx = static_cast<int>(i);
    if (dock_entries[i].pipeline_side == displayed_right_side_) right_idx = static_cast<int>(i);
  }
  dock_.set_slot_indices(left_idx, right_idx);

  input_received_ = true;
}
