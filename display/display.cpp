#include "display.h"
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
#include "controls.h"
#include "display_utils.h"
#include "ffmpeg.h"
#include "jxl_saver.h"
#include "metrics_calculator.h"
#include "pixel_format_utils.h"
#include "scope_window.h"
#include "source_code_pro_regular_ttf.h"
#include "version.h"
#include "video_compare_icon.h"
#include "vmaf_calculator.h"
extern "C" {
#include <libavfilter/avfilter.h>
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
  const int auto_width = mode == Mode::HStack ? width * 2 : width;
  const int auto_height = mode == Mode::VStack ? height * 2 : height;

  int window_x;
  int window_y;
  int window_width;
  int window_height;

  // account for window frame and title bar
  constexpr int border_width = 10;
#ifdef __linux__
  constexpr int border_height = 40;
#else
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

  // Check if window is larger than display and warn user
  const int usable_width = bounds.w - border_width;
  const int usable_height = bounds.h - border_height;

  if (window_width_ > usable_width || window_height_ > usable_height) {
    std::cout << "WARNING: Window size (" << window_width_ << "x" << window_height_ << ") exceeds display area (" << usable_width << "x" << usable_height
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

// Tear down and rebuild per-side video textures after a pixel format or filtering change.
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

// Clamp the help and metadata panel scroll offsets to their valid ranges.
void Display::clamp_overlay_offsets() {
  overlay_.clamp_help_scroll(drawable_height_, gpu_renderer_active_, HELP_TEXT_LINE_SPACING);
  metadata_panel_.clamp_scroll(drawable_height_, gpu_renderer_active_, HELP_TEXT_LINE_SPACING);
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
  const int safe_window_h = std::max(1, window_height_);

  content_window_ = SDL_Rect{0, 0, safe_window_w, safe_window_h};

  if (aspect_view_mode_ != AspectViewMode::Stretch) {
    const float content_aspect_ratio = std::max(compute_active_content_aspect_ratio(), 0.001F);
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
  overlay_.rebuild_help(small_font_, big_font_, renderer_, gpu_renderer_active_, drawable_width_);
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

    const int y_offset = is_top ? 1 : drawable_height_ - 1 - dot_height;

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

AVFrame* crop_rgb_frame(const AVFrame* src, const SDL_Rect& roi, SDL_Rect* out_effective_roi = nullptr) {
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

// Update displayed_left_side_ / displayed_right_side_ after a swap toggle.
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
  return true;
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

// ===== GPU render sub-phases =====

// Convert YUV→RGB on demand and run CPU-side features (pixel inspector, similarity metrics, live quality). Returns whether the RGB cache is populated.
bool Display::gpu_run_cpu_work(const RenderContext& ctx) {
  const AVFrame* left_frame = ctx.left_frame;
  const AVFrame* right_frame = ctx.right_frame;
  const auto& zoom_rect = ctx.zoom_rect;

  // Features that still need CPU pixel access drive an on-demand YUV→RGB
  // conversion. Skipped entirely when none are active to keep the pipeline
  // GPU-fast.
  const bool need_rgb = diff_processor_.subtraction_mode() || print_mouse_position_and_color_ ||
                        print_image_similarity_metrics_ || show_quality_metrics_ ||
                        selection_.save_selected_area_requested() || image_saver_.save_frames_requested();
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

        std::cout << "Left:  " << string_sprintf("[%4d,%4d]", pixel_video_x * od_left.first / video_width_, pixel_video_y * od_left.second / video_height_);
        std::cout << ", " << MetricsCalculator::get_and_format_rgb_yuv_pixel(rgb_cache_.left()->data[0], rgb_cache_.left()->linesize[0], rgb_cache_.left(), pixel_video_x, pixel_video_y, requires_10_bpc());
        std::cout << " - ";
        std::cout << "Right: " << string_sprintf("[%4d,%4d]", pixel_video_x * od_right.first / video_width_, pixel_video_y * od_right.second / video_height_);
        std::cout << ", " << MetricsCalculator::get_and_format_rgb_yuv_pixel(rgb_cache_.right()->data[0], rgb_cache_.right()->linesize[0], rgb_cache_.right(), pixel_video_x, pixel_video_y, requires_10_bpc());
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

    // Live on-screen quality metrics (rendered further down).
    if (show_quality_metrics_ && video_width_ > 0 && video_height_ > 0) {
      float* left_gray = MetricsCalculator::rgb_to_grayscale(rgb_cache_.left()->data[0], rgb_cache_.left()->linesize[0], video_width_, video_height_, requires_10_bpc());
      float* right_gray = MetricsCalculator::rgb_to_grayscale(rgb_cache_.right()->data[0], rgb_cache_.right()->linesize[0], video_width_, video_height_, requires_10_bpc());
      last_psnr_ = MetricsCalculator::compute_psnr(left_gray, right_gray, video_width_, video_height_);
      last_ssim_ = MetricsCalculator::compute_ssim(left_gray, right_gray, video_width_, video_height_);
      delete[] left_gray;
      delete[] right_gray;

      if (!playback_.play()) {
        if (left_frame->pts != last_vmaf_left_pts_ || right_frame->pts != last_vmaf_right_pts_) {
          last_vmaf_ = VMAFCalculator::instance().compute(rgb_cache_.left(), rgb_cache_.right());
          last_vmaf_left_pts_ = left_frame->pts;
          last_vmaf_right_pts_ = right_frame->pts;
        }
      }
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

// ===== SDL render sub-phases =====

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

  // live on-screen quality metrics overlay (toggled by Q key)
  if (show_quality_metrics_ && left_frame != nullptr && right_frame != nullptr && video_width_ > 0 && video_height_ > 0) {
    float* left_gray = MetricsCalculator::rgb_to_grayscale(left_frame->data[0], left_frame->linesize[0], video_width_, video_height_, requires_10_bpc());
    float* right_gray = MetricsCalculator::rgb_to_grayscale(right_frame->data[0], right_frame->linesize[0], video_width_, video_height_, requires_10_bpc());

    last_psnr_ = MetricsCalculator::compute_psnr(left_gray, right_gray, video_width_, video_height_);
    last_ssim_ = MetricsCalculator::compute_ssim(left_gray, right_gray, video_width_, video_height_);

    delete[] left_gray;
    delete[] right_gray;

    if (!playback_.play()) {
      if (left_frame->pts != last_vmaf_left_pts_ || right_frame->pts != last_vmaf_right_pts_) {
        last_vmaf_ = VMAFCalculator::instance().compute(left_frame, right_frame);
        last_vmaf_left_pts_ = left_frame->pts;
        last_vmaf_right_pts_ = right_frame->pts;
      }
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

// Main loop calls this before pumping events for a new frame.
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

// F, Shift+F (save selected area), Shift+R/L/B (crop per side), BACKSPACE (clear crop).
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
      playback_.request_auto_align();
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

// Public wrapper around MetricsCalculator::compute_frame_psnr for alignment consumers in video_compare.cpp.
float Display::compute_frame_psnr(const AVFrame* left_frame, const AVFrame* right_frame) {
  return MetricsCalculator::compute_frame_psnr(left_frame, right_frame, requires_10_bpc());
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
