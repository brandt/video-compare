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
#include "ffmpeg.h"
#include "format_converter.h"
#include "jxl_saver.h"
#include "png_saver.h"
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

static const SDL_Color BACKGROUND_COLOR = {54, 69, 79, 0};
static const SDL_Color LOOP_OFF_LABEL_COLOR = {0, 0, 0, 0};
static const SDL_Color LOOP_FW_LABEL_COLOR = {80, 127, 255, 0};
static const SDL_Color LOOP_PP_LABEL_COLOR = {191, 95, 60, 0};
static const SDL_Color TEXT_COLOR = {255, 255, 255, 0};
static const SDL_Color HELP_TEXT_PRIMARY_COLOR = {255, 255, 255, 0};
static const SDL_Color HELP_TEXT_ALTERNATE_COLOR = {255, 255, 192, 0};
static const SDL_Color POSITION_COLOR = {255, 255, 192, 0};
static const SDL_Color TARGET_COLOR = {200, 200, 140, 0};
static const SDL_Color ZOOM_COLOR = {255, 165, 0, 0};
static const SDL_Color PLAYBACK_SPEED_COLOR = {0, 192, 160, 0};
static const SDL_Color BUFFER_COLOR = {160, 225, 192, 0};
static const SDL_Color FPS_VIDEO_COLOR = {255, 255, 192, 0};
static const SDL_Color FPS_UI_COLOR = {255, 120, 200, 0};
static const int BACKGROUND_ALPHA = 100;

static const int MOUSE_WHEEL_SCROLL_STEPS_TO_DOUBLE = 12;
static const float ZOOM_STEP_SIZE = pow(2.0F, 1.0F / float(MOUSE_WHEEL_SCROLL_STEPS_TO_DOUBLE));
static const float ZOOM_SLOWDOWN_RATIO = 3.0F;

static const int PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE = 6;
static const float PLAYBACK_SPEED_STEP_SIZE = pow(2.0F, 1.0F / float(PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE));
static const float PLAYBACK_SPEED_SLOWDOWN_RATIO = 5.0F;

static const float RELATIVE_SEEK_SLOWDOWN_RATIO = 4.0F;

static const int HELP_TEXT_LINE_SPACING = 1;
static const int HELP_TEXT_HORIZONTAL_MARGIN = 26;

static const int MIN_WINDOW_WIDTH = 4;
static const int MIN_WINDOW_HEIGHT = 1;

auto frame_deleter = [](AVFrame* frame) {
  av_freep(&frame->data[0]);
  av_frame_free(&frame);
};
using AVFramePtr = std::unique_ptr<AVFrame, decltype(frame_deleter)>;

template <typename T>
inline T check_sdl(T value, const std::string& message) {
  if (!value) {
    throw std::runtime_error{"SDL " + message + " - " + SDL_GetError()};
  }
  return value;
}

template <typename T>
inline T clamp_range(T v, T lo, T hi) {
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

inline uint32_t clamp_u32(int v, uint32_t hi) {
  return (v < 0) ? 0u : (v > (int)hi ? hi : (uint32_t)v);
}

inline int clamp_int_to_byte_range(int value) {
  return clamp_range(value, 0, 255);
}

inline int clamp_int_to_10_bpc_range(int value) {
  return clamp_range(value, 0, 1023);
}

inline uint8_t clamp_int_to_byte(int value) {
  return static_cast<uint8_t>(clamp_int_to_byte_range(value));
}

inline uint16_t clamp_int_to_10_bpc(int value) {
  return static_cast<uint16_t>(clamp_int_to_10_bpc_range(value));
}

inline int luma709(int r, int g, int b) {
  return (217 * r + 733 * g + 74 * b) >> 10;
}

inline SDL_FRect to_frect(const SDL_Rect& r) {
  return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
}

inline SDL_FRect make_frect(int x, int y, int w, int h) {
  return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
}

static SDL_DisplayID display_id_for_index(int index) {
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  if (!displays || count == 0) {
    return SDL_GetPrimaryDisplay();
  }
  if (index < 0 || index >= count) {
    index = 0;
  }
  SDL_DisplayID id = displays[index];
  SDL_free(displays);
  return id;
}

template <int Bpc>
struct BitDepthTraits;
template <>
struct BitDepthTraits<8> {
  using P = uint8_t;
  static constexpr uint32_t MaxCode = 255u;
  static constexpr int PackShift = 0;          // stored as 8b
  static inline int to10(int v) { return v; }  // already 8-bit working domain
  static inline P from10(uint32_t v) { return (P)clamp_u32((int)v, MaxCode); }
};

template <>
struct BitDepthTraits<10> {
  using P = uint16_t;
  static constexpr uint32_t MaxCode = 1023u;
  static constexpr int PackShift = 6;          // stored as 16b with <<6
  static inline int to10(int v) { return v; }  // values in working domain are 10b
  static inline P from10(uint32_t v) { return (P)(clamp_u32((int)v, MaxCode) << PackShift); }
};

// Credits to Kemin Zhou for this approach which does not require Boost or C++17
// https://stackoverflow.com/questions/4430780/how-can-i-extract-the-file-name-and-extension-from-a-path-in-c
std::string get_file_name_and_extension(const std::string& file_path) {
  char* buff = new char[file_path.size() + 1];
  strcpy(buff, file_path.c_str());

  const std::string result = std::string(basename(buff));

  delete[] buff;

  return result;
}

std::string get_file_stem(const std::string& file_path) {
  std::string tmp = get_file_name_and_extension(file_path);

  const std::string::size_type i = tmp.rfind('.');

  if (i != std::string::npos) {
    tmp = tmp.substr(0, i);
  }

  return tmp;
}

static bool should_suffix_right_file_number_1(const std::string& left_file_name, const std::string& right_file_name) {
  return get_file_name_and_extension(left_file_name) == get_file_name_and_extension(right_file_name);
}

static std::string format_right_file_label(const std::string& left_file_name, const std::string& right_file_name, const size_t right_file_number) {
  if (right_file_number >= 2 || (right_file_number == 1 && should_suffix_right_file_number_1(left_file_name, right_file_name))) {
    return right_file_name + string_sprintf(" <%zu>", right_file_number);
  }

  return right_file_name;
}

std::string format_window_title(const std::string& left_file_name, const std::string& right_file_name) {
  return string_sprintf("%s  |  %s", get_file_name_and_extension(left_file_name).c_str(), get_file_name_and_extension(right_file_name).c_str());
}

std::string strip_ffmpeg_patterns(const std::string& input) {
  static const std::regex pattern_regex(R"(%\d*d|\*|\?)");

  return std::regex_replace(input, pattern_regex, "");
};

inline float round_3(float value) {
  return std::round(value * 1000.0F) / 1000.0F;
}

static std::string format_position_difference(const float position1, const float position2) {
  // round both for the sake of consistency with the displayed positions
  const float position1_rounded = round_3(position1);
  const float position2_rounded = round_3(position2);

  // absolute difference very close to 0.001 -> we are in sync!
  if (std::abs(position1_rounded - position2_rounded) < 9.99e-4) {
    return "";
  } else if (position1 < position2) {
    return " (-" + format_position(position2_rounded - position1_rounded, true) + ")";
  }

  return " (+" + format_position(position1_rounded - position2_rounded, true) + ")";
}

static std::string to_hex(const uint32_t value, const int width) {
  std::stringstream sstream;
  sstream << std::setfill('0') << std::setw(width) << std::hex << value;

  return sstream.str();
}

static std::string format_libav_version(unsigned version) {
  int major = (version >> 16) & 0xff;
  int minor = (version >> 8) & 0xff;
  int micro = version & 0xff;
  return string_sprintf("%2u.%2u.%3u", major, minor, micro);
}

auto get_metadata_int_value = [](const AVFrame* frame, const std::string& key, const int default_value) -> int {
  const AVDictionaryEntry* entry = av_dict_get(frame->metadata, key.c_str(), nullptr, 0);

  return entry ? std::atoi(entry->value) : default_value;
};

SDL::SDL() {
  check_sdl(SDL_Init(SDL_INIT_VIDEO), "SDL init");
  check_sdl(TTF_Init(), "TTF init");
}

SDL::~SDL() {
  SDL_Quit();
}

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
      subtraction_mode_{start_in_subtraction_mode},
      pending_verbose_print_{verbose},
      wheel_sensitivity_{wheel_sensitivity} {
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

  rebuild_help_textures();

  if (start_in_fullscreen_) {
    set_fullscreen(true);
  }
}

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

    if (message_texture_ != nullptr) {
      SDL_DestroyTexture(message_texture_);
    }

    for (auto help_texture : help_textures_) {
      SDL_DestroyTexture(help_texture);
    }
  }

  for (auto s : help_surfaces_) SDL_DestroySurface(s);
  help_surfaces_.clear();
  for (auto s : metadata_surfaces_) SDL_DestroySurface(s);
  metadata_surfaces_.clear();

  TTF_CloseFont(small_font_);
  TTF_CloseFont(big_font_);

  SDL_DestroyCursor(normal_mode_cursor_);
  SDL_DestroyCursor(pan_mode_cursor_);
  SDL_DestroyCursor(selection_mode_cursor_);

  delete[] diff_buffer_;

  if (left_buffer_ != nullptr) {
    delete[] left_buffer_;
  }
  if (right_buffer_ != nullptr) {
    delete[] right_buffer_;
  }

  // GPU-mode RGB cache: release per-side FormatConverters and their packed
  // RGB destination frames. diff_upload_frame_ is an AVFrame shell that
  // aliases diff_buffer_ (freed above) — clear data[0] first so av_frame_free
  // doesn't try to walk into memory it doesn't own.
  for (int s = 0; s < kSideCount; ++s) {
    if (rgb_frames_[s] != nullptr) {
      av_freep(&rgb_frames_[s]->data[0]);
      av_frame_free(&rgb_frames_[s]);
    }
    rgb_converter_[s].reset();
  }
  if (diff_upload_frame_ != nullptr) {
    diff_upload_frame_->data[0] = nullptr;  // diff_buffer_ is owned separately
    av_frame_free(&diff_upload_frame_);
  }

  if (renderer_) {
    SDL_DestroyRenderer(renderer_);
  }
  SDL_DestroyWindow(window_);
}

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

void Display::apply_window_size_and_relayout(const int target_w, const int target_h, const bool force_layout_refresh) {
  SDL_SetWindowSize(window_, target_w, target_h);
  handle_window_resize(true, force_layout_refresh);
}

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

  if (diff_buffer_ != nullptr) {
    delete[] diff_buffer_;
    diff_buffer_ = nullptr;
  }
  diff_buffer_ = new uint8_t[video_width_ * video_height_ * 3 * (requires_10_bpc() ? sizeof(uint16_t) : sizeof(uint8_t))];
  diff_planes_ = {diff_buffer_, nullptr, nullptr};
  diff_pitches_ = {video_width_ * 3 * (requires_10_bpc() ? sizeof(uint16_t) : sizeof(uint8_t)), 0, 0};

  if (left_buffer_ != nullptr) {
    delete[] left_buffer_;
    left_buffer_ = nullptr;
  }
  if (right_buffer_ != nullptr) {
    delete[] right_buffer_;
    right_buffer_ = nullptr;
  }
  left_planes_ = {nullptr, nullptr, nullptr};
  right_planes_ = {nullptr, nullptr, nullptr};

  // Drop any cached RGB conversion state — new video dims require fresh
  // FormatConverter and RGB destination frames.
  for (int s = 0; s < kSideCount; ++s) {
    if (rgb_frames_[s] != nullptr) {
      av_freep(&rgb_frames_[s]->data[0]);
      av_frame_free(&rgb_frames_[s]);
    }
    rgb_converter_[s].reset();
    rgb_frame_keys_[s].clear();
  }

  move_offset_ = Vector2D((global_center_.x() - 0.5F) * static_cast<float>(video_width_), (global_center_.y() - 0.5F) * static_cast<float>(video_height_));

  // Force relayout because video dimensions changed even if window size did not.
  handle_window_resize(true, true);
  update_window_title_with_current_roi();
}

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

void Display::rebuild_side_ui_textures() {
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

  side_ui_[LEFT.as_simple_index()].file_stem = strip_ffmpeg_patterns(get_file_stem(left_file_name_));
  side_ui_[RIGHT.as_simple_index()].file_stem = strip_ffmpeg_patterns(get_file_stem(right_file_name_));

  rebuild_side(LEFT, left_file_name_);
  rebuild_side(RIGHT, format_right_file_label(left_file_name_, right_file_name_, active_right_index_ + 1));
}

void Display::rebuild_help_textures() {
  // Wrapping and layout depend on drawable width, so everything is rebuilt.
  for (auto help_texture : help_textures_) SDL_DestroyTexture(help_texture);
  help_textures_.clear();
  for (auto s : help_surfaces_) SDL_DestroySurface(s);
  help_surfaces_.clear();
  help_total_height_ = 0;

  bool primary_color = true;

  // Helper to render one line and track its height for scrolling math.
  auto add_help_texture = [&](TTF_Font* font, const std::string& text) {
    SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, primary_color ? HELP_TEXT_PRIMARY_COLOR : HELP_TEXT_ALTERNATE_COLOR, drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2);
    if (!surface) return;

    if (gpu_renderer_active_) {
      SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(surface);
      if (!rgba) return;
      help_total_height_ += rgba->h;
      help_surfaces_.push_back(rgba);
    } else {
      SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
      SDL_DestroySurface(surface);
      float tw, th; SDL_GetTextureSize(texture, &tw, &th);
      help_total_height_ += static_cast<int>(th);
      help_textures_.push_back(texture);
    }
  };

  add_help_texture(small_font_, " ");

  const auto& sections = get_control_sections();
  for (size_t i = 0; i < sections.size(); ++i) {
    const auto& section = sections[i];

    // Force section headers to white, underlined, and uppercase.
    primary_color = true;

    TTF_SetFontStyle(big_font_, TTF_STYLE_BOLD | TTF_STYLE_UNDERLINE);
    add_help_texture(big_font_, to_upper_case(section.title));
    TTF_SetFontStyle(big_font_, TTF_STYLE_NORMAL);

    // Reset so the first section item toggles to secondary color (light yellow).
    primary_color = true;

    for (size_t j = 0; j < section.entries.size(); ++j) {
      const auto& entry = section.entries[j];
      primary_color = !primary_color;

      if (entry.key.empty()) {
        add_help_texture(small_font_, entry.description);
        add_help_texture(small_font_, " ");
      } else {
        add_help_texture(small_font_, string_sprintf(" %-16s %s", entry.key.c_str(), entry.description.c_str()));
      }
    }

    if (i + 1 < sections.size()) {
      add_help_texture(small_font_, " ");
    }
  }
}

void Display::clamp_overlay_offsets() {
  auto clamp_offset = [&](int& y_offset, const int total_height, const size_t count) {
    const int min_offset = drawable_height_ - total_height - static_cast<int>(count) * HELP_TEXT_LINE_SPACING;
    y_offset = std::max(y_offset, min_offset);
    y_offset = std::min(y_offset, 0);
  };

  const size_t help_count = gpu_renderer_active_ ? help_surfaces_.size() : help_textures_.size();
  const size_t meta_count = gpu_renderer_active_ ? metadata_surfaces_.size() : metadata_textures_.size();
  clamp_offset(help_y_offset_, help_total_height_, help_count);
  clamp_offset(metadata_y_offset_, metadata_total_height_, meta_count);
}

float Display::compute_content_aspect_ratio() const {
  const float content_w = static_cast<float>(video_width_) * ((mode_ == Mode::HStack) ? 2.0F : 1.0F);
  const float content_h = static_cast<float>(video_height_) * ((mode_ == Mode::VStack) ? 2.0F : 1.0F);

  return content_w / std::max(content_h, 1.0F);
}

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

bool Display::consume_hdr_state_change() {
  if (hdr_state_changed_) {
    hdr_state_changed_ = false;
    return true;
  }
  return false;
}

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
  rebuild_help_textures();
  metadata_dirty_ = true;

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

void Display::convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches, std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches, const SDL_Rect& roi) {
  row_workers_.run_dynamic(
      roi.h,
      [=](const int start_row, const int end_row) {
        uint16_t* p_in = reinterpret_cast<uint16_t*>(in_planes[0] + roi.x * 6 + in_pitches[0] * (roi.y + start_row));
        uint32_t* p_out = out_planes[0] + roi.x + out_pitches[0] * (roi.y + start_row) / sizeof(uint32_t);

        for (int y = start_row; y < end_row; y++) {
          for (int in_x = 0, out_x = 0; out_x < roi.w; in_x += 3, out_x++) {
            const uint32_t r = p_in[in_x] >> 6;
            const uint32_t g = p_in[in_x + 1] >> 6;
            const uint32_t b = p_in[in_x + 2] >> 6;

            p_out[out_x] = (r << 20) | (g << 10) | (b);
          }

          p_in += in_pitches[0] / sizeof(uint16_t);
          p_out += out_pitches[0] / sizeof(uint32_t);
        }
      },
      suggest_block_rows_by_bytes(roi.w, roi.h, sizeof(uint16_t), 3));
}

template <int Bpc>
inline void process_difference_scanline(const typename BitDepthTraits<Bpc>::P* plane_left,
                                        const typename BitDepthTraits<Bpc>::P* plane_right,
                                        typename BitDepthTraits<Bpc>::P* plane_difference,
                                        const int pixels,
                                        const Display::DiffMode mode,
                                        const bool luma_only,
                                        const std::vector<uint32_t>& mag_u,
                                        const std::vector<uint32_t>& mag_s) {
  using T = BitDepthTraits<Bpc>;
  constexpr uint32_t MAX = T::MaxCode;
  constexpr uint32_t MID = MAX >> 1;

  auto load = [](typename T::P v) -> int { return (int)(v >> T::PackShift); };

  for (int i = 0; i < pixels; i++) {
    const int idx = i * 3;
    const int rl = load(plane_left[idx]), gl = load(plane_left[idx + 1]), bl = load(plane_left[idx + 2]);
    const int rr = load(plane_right[idx]), gr = load(plane_right[idx + 1]), br = load(plane_right[idx + 2]);

    if (mode == Display::DiffMode::LegacyAbs) {
      // Original: per-channel abs * AMPLIFICATION, clamped to bit depth
      constexpr int AMPLIFICATION = 2;

      if (luma_only) {
        const int dl = luma709(rl, gl, bl) - luma709(rr, gr, br);
        const uint32_t Y = clamp_u32(std::abs(dl) * AMPLIFICATION, MAX);
        auto y_p = T::from10(Y);

        plane_difference[idx] = y_p;
        plane_difference[idx + 1] = y_p;
        plane_difference[idx + 2] = y_p;
      } else {
        const uint32_t R = clamp_u32(std::abs(rl - rr) * AMPLIFICATION, MAX);
        const uint32_t G = clamp_u32(std::abs(gl - gr) * AMPLIFICATION, MAX);
        const uint32_t B = clamp_u32(std::abs(bl - br) * AMPLIFICATION, MAX);

        plane_difference[idx + 0] = T::from10(R);
        plane_difference[idx + 1] = T::from10(G);
        plane_difference[idx + 2] = T::from10(B);
      }
      continue;
    }

    // Adaptive mapping with optional sign and luma-only
    if (luma_only) {
      const int dl = luma709(rl, gl, bl) - luma709(rr, gr, br);
      const uint32_t a = (uint32_t)std::min<int>(MAX, std::abs(dl));

      if (mode == Display::DiffMode::SignedDiverging) {
        const uint32_t m = mag_s[a];
        const uint32_t Y = (dl >= 0) ? (MID + m) : (MID - m);
        auto y_p = T::from10(Y);
        plane_difference[idx + 0] = plane_difference[idx + 1] = plane_difference[idx + 2] = y_p;
      } else {
        const uint32_t Y = mag_u[a];
        auto y_p = T::from10(Y);
        plane_difference[idx + 0] = plane_difference[idx + 1] = plane_difference[idx + 2] = y_p;
      }
    } else {
      const int dr = rl - rr, dg = gl - gr, db = bl - br;

      if (mode == Display::DiffMode::SignedDiverging) {
        const uint32_t ar = (uint32_t)std::min<int>(MAX, std::abs(dr));
        const uint32_t ag = (uint32_t)std::min<int>(MAX, std::abs(dg));
        const uint32_t ab = (uint32_t)std::min<int>(MAX, std::abs(db));

        plane_difference[idx + 0] = T::from10(dr >= 0 ? (MID + mag_s[ar]) : (MID - mag_s[ar]));
        plane_difference[idx + 1] = T::from10(dg >= 0 ? (MID + mag_s[ag]) : (MID - mag_s[ag]));
        plane_difference[idx + 2] = T::from10(db >= 0 ? (MID + mag_s[ab]) : (MID - mag_s[ab]));
      } else {
        const uint32_t ar = (uint32_t)std::min<int>(MAX, std::abs(dr));
        const uint32_t ag = (uint32_t)std::min<int>(MAX, std::abs(dg));
        const uint32_t ab = (uint32_t)std::min<int>(MAX, std::abs(db));

        plane_difference[idx + 0] = T::from10(mag_u[ar]);
        plane_difference[idx + 1] = T::from10(mag_u[ag]);
        plane_difference[idx + 2] = T::from10(mag_u[ab]);
      }
    }
  }
}

template <int Bpc>
float Display::calculate_frame_p99(const typename BitDepthTraits<Bpc>::P* plane_left, const typename BitDepthTraits<Bpc>::P* plane_right, const size_t pitch_left, const size_t pitch_right, const int width_right) const {
  using T = BitDepthTraits<Bpc>;
  static_assert(Bpc == 8 || Bpc == 10, "Bpc must be 8 or 10");
  constexpr int CHANNELS = 3;

  const size_t stride_l = pitch_left / sizeof(typename T::P);
  const size_t stride_r = pitch_right / sizeof(typename T::P);

  const int bins = static_cast<int>(T::MaxCode) + 1;
  const int num_threads = row_workers_.size();

  std::vector<std::vector<uint32_t>> thread_histograms(num_threads, std::vector<uint32_t>(bins, 0u));

  // Use RowWorkers to compute histograms for different row ranges
  auto histograms_ptr = std::make_shared<std::vector<std::vector<uint32_t>>>(std::move(thread_histograms));

  row_workers_.run_dynamic_indexed(
      video_height_,
      [=](const int start_row, const int end_row, const int worker_index) {
        auto& hist = (*histograms_ptr)[worker_index];

        for (int y = start_row; y < end_row; y++) {
          const typename T::P* row_l = plane_left + y * stride_l;
          const typename T::P* row_r = plane_right + y * stride_r;

          for (int x = 0; x < width_right; x++) {
            const int idx = x * CHANNELS;

            const int rl = row_l[idx + 0] >> T::PackShift;
            const int gl = row_l[idx + 1] >> T::PackShift;
            const int bl = row_l[idx + 2] >> T::PackShift;

            const int rr = row_r[idx + 0] >> T::PackShift;
            const int gr = row_r[idx + 1] >> T::PackShift;
            const int br = row_r[idx + 2] >> T::PackShift;

            int d;
            if (diff_luma_only_) {
              const int yl = luma709(rl, gl, bl);
              const int yr = luma709(rr, gr, br);
              d = std::abs(yl - yr);
            } else {
              const int dr = std::abs(rl - rr);
              const int dg = std::abs(gl - gr);
              const int db = std::abs(bl - br);
              d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            }

            const int bin = clamp_range(d, 0, bins - 1);
            hist[static_cast<size_t>(bin)]++;
          }
        }
      },
      suggest_block_rows_by_bytes(video_width_, video_height_, sizeof(typename BitDepthTraits<Bpc>::P), 3));

  // Merge histograms
  std::vector<uint32_t> hist(bins, 0u);
  for (const auto& thread_hist : *histograms_ptr) {
    for (size_t i = 0; i < bins; ++i) {
      hist[i] += thread_hist[i];
    }
  }

  // Sum of histogram counts
  uint64_t total = std::accumulate(hist.begin(), hist.end(), 0);

  if (total == 0) {
    return 1.f;
  }

  // Linear-interpolated 99th percentile
  const double target_f = 0.99 * (double)(total - 1);
  const uint64_t r0 = (uint64_t)std::floor(target_f);
  const uint64_t r1 = (uint64_t)std::ceil(target_f);
  const double frac = target_f - (double)r0;

  int v0 = bins - 1, v1 = bins - 1;
  uint64_t acc = 0;

  // Find the values at ranks r0 and r1
  for (int k = 0; k < bins; k++) {
    const uint64_t next = acc + hist[static_cast<size_t>(k)];
    if (acc <= r0 && r0 < next) {
      v0 = k;
    }
    if (acc <= r1 && r1 < next) {
      v1 = k;
      break;
    }
    acc = next;
  }

  const float p = (float)v0 + frac * (float)(v1 - v0);
  return p;
}

std::pair<std::vector<uint32_t>, std::vector<uint32_t>> make_diff_lut(uint32_t max_code, Display::DiffMode mode, uint32_t scale_max) {
  std::vector<uint32_t> mag_u(max_code + 1);
  std::vector<uint32_t> mag_s(max_code + 1);

  if (mode != Display::DiffMode::LegacyAbs) {
    if (scale_max == 0) {
      scale_max = 1;
    }

    const uint32_t MID = max_code >> 1;
    const uint32_t Q = 16;
    const uint32_t ONE_Q = 1u << Q;
    const uint64_t HALF = uint64_t(1) << (Q - 1);

    for (uint32_t a = 0; a <= max_code; a++) {
      // x_q = clamp(a/scale, 0..1) in Q16
      uint32_t x_q = (uint32_t)std::min<uint64_t>(ONE_Q, ((uint64_t)a << Q) / scale_max);

      // map_unit(x): Linear / Sqrt (SignedDiverging uses sqrt magnitude)
      uint32_t y_q;
      switch (mode) {
        case Display::DiffMode::AbsLinear:
          y_q = x_q;
          break;
        case Display::DiffMode::AbsSqrt:
        case Display::DiffMode::SignedDiverging: {
          const double x = double(x_q) / double(ONE_Q);
          const double y = std::sqrt(x);
          y_q = (uint32_t)std::llround(y * double(ONE_Q));
          break;
        }
        default:  // LegacyAbs not expected here; fall back to linear
          y_q = x_q;
          break;
      }

      // Scale back to code domain with Q16 rounding
      mag_u[a] = (uint32_t)(((uint64_t)y_q * max_code + HALF) >> Q);  // [0..MAX]
      mag_s[a] = (uint32_t)(((uint64_t)y_q * MID + HALF) >> Q);       // [0..MID]
    }
  }

  return std::pair<std::vector<uint32_t>, std::vector<uint32_t>>(std::move(mag_u), std::move(mag_s));
};

template <int Bpc>
void Display::process_difference_planes(const typename BitDepthTraits<Bpc>::P* plane_left0,
                                        const typename BitDepthTraits<Bpc>::P* plane_right0,
                                        typename BitDepthTraits<Bpc>::P* plane_difference0,
                                        const size_t pitch_left,
                                        const size_t pitch_right,
                                        const size_t pitch_difference,
                                        const int width_right,
                                        const float diff_max) const {
  using T = BitDepthTraits<Bpc>;
  constexpr uint32_t MAX = T::MaxCode;

  const float scale_max = (diff_mode_ == Display::DiffMode::LegacyAbs) ? -1.f : clamp_range(diff_max, 4.f, (float)MAX);

  // Integerize/clip scale once
  const uint32_t scale_max_i = (uint32_t)std::max<double>(1.0, std::min<double>(double(MAX), std::round(std::fabs(scale_max))));

  // Build LUTs (only for adaptive mapping)
  auto luts = make_diff_lut(MAX, diff_mode_, scale_max_i);
  const std::vector<uint32_t> mag_u = std::move(luts.first);
  const std::vector<uint32_t> mag_s = std::move(luts.second);

  row_workers_.run_dynamic(
      video_height_,
      [=](const int start_row, const int end_row) {
        auto plane_left = plane_left0 + start_row * (pitch_left / sizeof(typename T::P));
        auto plane_right = plane_right0 + start_row * (pitch_right / sizeof(typename T::P));
        auto plane_difference = plane_difference0 + start_row * (pitch_difference / sizeof(typename T::P));

        for (int y = start_row; y < end_row; y++) {
          process_difference_scanline<Bpc>(plane_left, plane_right, plane_difference, width_right, diff_mode_, diff_luma_only_, mag_u, mag_s);
          plane_left += pitch_left / sizeof(typename T::P);
          plane_right += pitch_right / sizeof(typename T::P);
          plane_difference += pitch_difference / sizeof(typename T::P);
        }
      },
      suggest_block_rows_by_bytes(video_width_, video_height_, sizeof(typename BitDepthTraits<Bpc>::P), 3));
}

void Display::update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left, std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right, int split_x) {
  constexpr int CHANNELS = 3;

  const int width_right = (video_width_ - split_x);
  if (width_right <= 0) {
    return;
  }

  const bool update_frame_max = diff_mode_ != DiffMode::LegacyAbs;
  float frame_max = 1.f;

  // row starts after split_x pixels, i.e., split_x * 3 samples
  if (requires_10_bpc()) {
    auto plane_left0 = reinterpret_cast<uint16_t*>(planes_left[0]) + split_x * CHANNELS;
    auto plane_right0 = reinterpret_cast<uint16_t*>(planes_right[0]) + split_x * CHANNELS;
    auto plane_difference0 = reinterpret_cast<uint16_t*>(diff_planes_[0]) + split_x * CHANNELS;

    if (update_frame_max) {
      frame_max = calculate_frame_p99<10>(plane_left0, plane_right0, pitches_left[0], pitches_right[0], width_right);
    }

    process_difference_planes<10>(plane_left0, plane_right0, plane_difference0, pitches_left[0], pitches_right[0], diff_pitches_[0], width_right, frame_max);
  } else {
    auto plane_left0 = planes_left[0] + split_x * CHANNELS;
    auto plane_right0 = planes_right[0] + split_x * CHANNELS;
    auto plane_difference0 = diff_planes_[0] + split_x * CHANNELS;

    if (update_frame_max) {
      frame_max = calculate_frame_p99<8>(plane_left0, plane_right0, pitches_left[0], pitches_right[0], width_right);
    }

    process_difference_planes<8>(plane_left0, plane_right0, plane_difference0, pitches_left[0], pitches_right[0], diff_pitches_[0], width_right, frame_max);
  }
}

void save_frame_image(const AVFrame* frame, const std::string& filename, std::atomic_bool& error_occurred) {
  try {
    if (frame->format == AV_PIX_FMT_X2RGB10LE) {
      JxlSaver::save(frame, filename);
    } else {
      PngSaver::save(frame, filename);
    }
  } catch (const std::ios_base::failure& e) {
    std::cerr << "Error saving image to file: " << filename << std::endl;
    error_occurred = true;
  } catch (const std::runtime_error& e) {
    std::cerr << "Error saving image: " << e.what() << std::endl;
    error_occurred = true;
  }
};

void Display::save_image_frames(const AVFrame* left_frame, const AVFrame* right_frame) {
  std::atomic_bool error_occurred(false);

  const auto create_onscreen_display_avframe = [&]() -> AVFramePtr {
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
        // Convert surface to RGB24 format
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

    AVFrame* renderer_frame = av_frame_alloc();
    renderer_frame->format = requires_10_bpc() ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;
    renderer_frame->width = drawable_width_;
    renderer_frame->height = drawable_height_;
    renderer_frame->data[0] = pixels;
    renderer_frame->linesize[0] = pitch;

    return AVFramePtr(renderer_frame, frame_deleter);
  };

  const auto osd_frame = create_onscreen_display_avframe();

  const std::string& left_stem = side_ui_[displayed_left_side_.as_simple_index()].file_stem;
  const std::string& right_stem = side_ui_[displayed_right_side_.as_simple_index()].file_stem;
  const bool stems_equal = (left_stem == right_stem);
  const char* frame_ext = (left_frame->format == AV_PIX_FMT_X2RGB10LE) ? "jxl" : "png";
  const std::string left_filename = string_sprintf("%s%s_%04d.%s", left_stem.c_str(), stems_equal ? "_left" : "", saved_image_number_, frame_ext);
  const std::string right_filename = string_sprintf("%s%s_%04d.%s", right_stem.c_str(), stems_equal ? "_right" : "", saved_image_number_, frame_ext);
  const std::string osd_filename = string_sprintf("%s_%s_osd_%04d.png", left_stem.c_str(), right_stem.c_str(), saved_image_number_);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) { return save_frame_image(frame, filename, error_occurred); };

  std::thread save_left_frame_thread(save_frame, left_frame, left_filename);
  std::thread save_right_frame_thread(save_frame, right_frame, right_filename);
  std::thread save_osd_frame_thread(save_frame, osd_frame.get(), osd_filename);

  save_left_frame_thread.join();
  save_right_frame_thread.join();
  save_osd_frame_thread.join();

  if (!error_occurred) {
    notify_user(string_sprintf("Saved %s, %s and %s", left_filename.c_str(), right_filename.c_str(), osd_filename.c_str()));

    saved_image_number_++;
  }
}

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

SDL_Texture* Display::get_side_texture(int side) const {
  return bilinear_texture_filtering_ ? side_textures_linear_[side] : side_textures_nn_[side];
}

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

int Display::round_and_clamp(const float value) {
  const int result = static_cast<int>(std::roundf(value));

  return requires_10_bpc() ? clamp_int_to_10_bpc_range(result) : clamp_int_to_byte_range(result);
}

const std::array<int, 3> Display::get_rgb_pixel(uint8_t* rgb_plane, const size_t pitch, const int x, const int y) {
  int r, g, b;

  if (requires_10_bpc()) {
    uint16_t* rgb_pixel = reinterpret_cast<uint16_t*>(rgb_plane + x * 6 + y * pitch);

    r = *(rgb_pixel) >> 6;
    g = *(rgb_pixel + 1) >> 6;
    b = *(rgb_pixel + 2) >> 6;

  } else {
    uint8_t* rgb_pixel = rgb_plane + x * 3 + y * pitch;

    r = *(rgb_pixel);
    g = *(rgb_pixel + 1);
    b = *(rgb_pixel + 2);
  }

  return {r, g, b};
}

const std::array<int, 3> Display::convert_rgb_to_yuv(const std::array<int, 3> rgb, const AVPixelFormat rgb_format, const AVColorSpace color_space, const AVColorRange color_range) {
  auto allocate_frame = [&](const AVPixelFormat format) -> AVFramePtr {
    AVFrame* raw_frame = av_frame_alloc();

    if (raw_frame == nullptr) {
      throw ffmpeg::Error("Couldn't allocate frame");
    }

    raw_frame->format = format;
    raw_frame->width = 1;
    raw_frame->height = 1;
    raw_frame->colorspace = color_space;
    raw_frame->color_range = color_range;

    ffmpeg::check(av_image_alloc(raw_frame->data, raw_frame->linesize, raw_frame->width, raw_frame->height, format, 64));

    return AVFramePtr(raw_frame, frame_deleter);
  };

  const AVPixelFormat yuv_format = requires_10_bpc() ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P;

  auto rgb_pixel_frame = allocate_frame(rgb_format);
  auto yuv_pixel_frame = allocate_frame(yuv_format);

  if (requires_10_bpc()) {
    uint16_t* rgb_data = reinterpret_cast<uint16_t*>(rgb_pixel_frame->data[0]);

    auto extend_10_to_16_bit = [](const int value) {
      return (value * 1025) >> 4;  // 1023->65535
    };

    rgb_data[0] = extend_10_to_16_bit(rgb[0]);
    rgb_data[1] = extend_10_to_16_bit(rgb[1]);
    rgb_data[2] = extend_10_to_16_bit(rgb[2]);
  } else {
    uint8_t* rgb_data = reinterpret_cast<uint8_t*>(rgb_pixel_frame->data[0]);

    rgb_data[0] = rgb[0];
    rgb_data[1] = rgb[1];
    rgb_data[2] = rgb[2];
  }

  FormatConverter rgb_to_yuv_converter(1, 1, 1, 1, rgb_format, yuv_format, color_space, color_range);
  rgb_to_yuv_converter(rgb_pixel_frame.get(), yuv_pixel_frame.get());

  if (requires_10_bpc()) {
    auto y_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[0]);
    auto u_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[1]);
    auto v_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[2]);

    return {y_data[0], u_data[0], v_data[0]};
  } else {
    return {yuv_pixel_frame->data[0][0], yuv_pixel_frame->data[1][0], yuv_pixel_frame->data[2][0]};
  }
}

std::string Display::format_pixel(const std::array<int, 3>& pixel) {
  std::string hex_pixel = requires_10_bpc() ? to_hex((pixel[0] << 20) | (pixel[1] << 10) | pixel[2], 8) : to_hex((pixel[0] << 16) | (pixel[1] << 8) | pixel[2], 6);

  return requires_10_bpc() ? string_sprintf("(%4d,%4d,%4d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str()) : string_sprintf("(%3d,%3d,%3d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str());
}

std::string Display::get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, const size_t pitch, const AVFrame* frame, const int x, const int y) {
  auto rgb_format = static_cast<AVPixelFormat>(frame->format);

  const std::array<int, 3> rgb = get_rgb_pixel(rgb_plane, pitch, x, y);
  const std::array<int, 3> yuv = convert_rgb_to_yuv(rgb, rgb_format, frame->colorspace, frame->color_range);

  return "RGB" + format_pixel(rgb) + ", YUV" + format_pixel(yuv);
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

float* Display::rgb_to_grayscale(const uint8_t* plane, const size_t pitch, const int width, const int height) {
  float* grayscale_image = new float[width * height];
  float* p_out = grayscale_image;

  auto to_grayscale = [](const float r, const float g, const float b, const float normalization_factor) -> float { return (r * 0.299f + g * 0.587f + b * 0.114f) * normalization_factor; };

  if (requires_10_bpc()) {
    for (int y = 0; y < height; y++) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(plane + y * pitch);
      for (int x = 0; x < (width * 3); x += 3) {
        const float r = row[x] >> 6;
        const float g = row[x + 1] >> 6;
        const float b = row[x + 2] >> 6;
        *(p_out++) = to_grayscale(r, g, b, 1.f / 1023.f);
      }
    }
  } else {
    for (int y = 0; y < height; y++) {
      const uint8_t* row = plane + y * pitch;
      for (int x = 0; x < (width * 3); x += 3) {
        const float r = row[x];
        const float g = row[x + 1];
        const float b = row[x + 2];
        *(p_out++) = to_grayscale(r, g, b, 1.f / 255.f);
      }
    }
  }

  return grayscale_image;
}

float Display::compute_ssim_block(const float* left_plane, const float* right_plane, const int width, const int x_offset, const int y_offset, const int block_size) {
  const int block_elements = block_size * block_size;

  auto compute_mean = [&](const float* plane) {
    float sum = 0;

    for (int y = y_offset; y < (y_offset + block_size); y++) {
      const float* row = plane + y * width + x_offset;

      for (int x = 0; x < block_size; x++) {
        sum += *(row++);
      }
    }

    return sum / block_elements;
  };

  float mean1 = compute_mean(left_plane);
  float mean2 = compute_mean(right_plane);

  // compute variance and convariance
  float sum_var1 = 0, sum_var2 = 0, sum_covar = 0;

  for (int y = y_offset; y < (y_offset + block_size); y++) {
    const float* row1 = left_plane + y * width + x_offset;
    const float* row2 = right_plane + y * width + x_offset;

    for (int x = 0; x < block_size; x++) {
      float diff1 = *(row1++) - mean1;
      float diff2 = *(row2++) - mean2;

      sum_var1 += diff1 * diff1;
      sum_var2 += diff2 * diff2;
      sum_covar += diff1 * diff2;
    }
  }

  float variance1 = sum_var1 / block_elements;
  float variance2 = sum_var2 / block_elements;
  float covariance = sum_covar / block_elements;

  float geomtric_mean_variance12 = sqrtf(variance1 * variance2);

  // compute SSIM metrics
  static constexpr float k1 = 0.01f;
  static constexpr float k2 = 0.03f;
  static constexpr float c1 = k1 * k1;
  static constexpr float c2 = k2 * k2;
  static constexpr float c3 = c2 / 2.f;

  float luminance = (2.f * mean1 * mean2 + c1) / (mean1 * mean1 + mean2 * mean2 + c1);
  float contrast = (2.f * geomtric_mean_variance12 + c2) / (variance1 + variance2 + c2);
  float structure = (covariance + c3) / (geomtric_mean_variance12 + c3);

  return luminance * contrast * structure;
}

std::string Display::compute_ssim(const float* left_plane, const float* right_plane, const int width, const int height) {
  static constexpr int overlap = 4;
  static constexpr int block_size = 8;

  float ssim_sum = 0.0;
  int count = 0;

  for (int y = 0; y < height - (block_size - 1); y += block_size - overlap) {
    for (int x = 0; x < width - (block_size - 1); count++, x += block_size - overlap) {
      ssim_sum += compute_ssim_block(left_plane, right_plane, width, x, y, block_size);
    }
  }

  if (count == 0) {
    return "n/a";
  }

  const float ssim = ssim_sum / static_cast<float>(count);
  return string_sprintf("%.5f", ssim);
}

std::string Display::compute_psnr(const float* left_plane, const float* right_plane, const int width, const int height) {
  // compute MSE
  double mse = 0.0;

  for (int i = 0; i < (width * height); i++) {
    const float diff = *(left_plane++) - *(right_plane++);
    mse += static_cast<double>(diff) * static_cast<double>(diff);
  }

  mse /= static_cast<double>(width) * static_cast<double>(height);

  if (mse == 0) {
    return "inf";
  }

  // compute PSNR
  return string_sprintf("%.3f", -10.f * log10f(static_cast<float>(mse)));
}

void Display::render_help() {
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer_, nullptr);

  int y = help_y_offset_;

  for (size_t i = 0; i < help_textures_.size(); i++) {
    float fw, fh;
    SDL_GetTextureSize(help_textures_[i], &fw, &fh);

    SDL_FRect screen_area = {static_cast<float>(HELP_TEXT_HORIZONTAL_MARGIN), static_cast<float>(y), fw, fh};
    SDL_RenderTexture(renderer_, help_textures_[i], nullptr, &screen_area);

    y += static_cast<int>(fh) + HELP_TEXT_LINE_SPACING;
  }
}

void Display::render_metadata_overlay() {
  ensure_metadata_textures_current();

  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer_, nullptr);

  const int table_width = drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2;
  const int table_x = HELP_TEXT_HORIZONTAL_MARGIN;

  // Calculate the starting Y position to center the table vertically
  int y;

  if (mode_ == Mode::VStack && metadata_total_height_ < drawable_height_ / 2) {
    y = (drawable_height_ / 2 - metadata_total_height_) / 2;
  } else if (mode_ != Mode::VStack && metadata_total_height_ < drawable_height_) {
    y = (drawable_height_ - metadata_total_height_) / 2;
  } else {
    y = metadata_y_offset_ + 10;
  }

  for (size_t i = 0; i < metadata_textures_.size(); i++) {
    float fw, fh;
    SDL_GetTextureSize(metadata_textures_[i], &fw, &fh);
    int w = static_cast<int>(fw), h = static_cast<int>(fh);

    int x_offset = (table_width - w) / 2;

    SDL_FRect screen_area = {static_cast<float>(table_x + x_offset), static_cast<float>(y), fw, fh};
    SDL_RenderTexture(renderer_, metadata_textures_[i], nullptr, &screen_area);

    y += h + HELP_TEXT_LINE_SPACING;
  }
}

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

void Display::refresh_display_side_mapping() {
  displayed_left_side_ = swap_left_right_ ? RIGHT : LEFT;
  displayed_right_side_ = swap_left_right_ ? LEFT : RIGHT;
}

void Display::build_metadata_textures(const VideoMetadata& left_metadata, const VideoMetadata& right_metadata) {
  constexpr char TOKENIZER = ',';

  for (auto texture : metadata_textures_) SDL_DestroyTexture(texture);
  metadata_textures_.clear();
  for (auto s : metadata_surfaces_) SDL_DestroySurface(s);
  metadata_surfaces_.clear();
  metadata_total_height_ = 0;

  auto add_metadata_texture = [&](TTF_Font* font, const std::string& text, bool primary_color, bool is_header) {
    // choose text color based on content type and alternating pattern
    SDL_Color text_color = is_header ? HELP_TEXT_PRIMARY_COLOR : (primary_color ? HELP_TEXT_PRIMARY_COLOR : HELP_TEXT_ALTERNATE_COLOR);

    // render text with word wrapping to fit available width
    SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, text_color, drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2);
    if (!surface) return;

    if (gpu_renderer_active_) {
      SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(surface);
      if (!rgba) return;
      metadata_total_height_ += rgba->h + HELP_TEXT_LINE_SPACING;
      metadata_surfaces_.push_back(rgba);
    } else {
      SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
      SDL_DestroySurface(surface);
      float tw, th; SDL_GetTextureSize(texture, &tw, &th);
      metadata_total_height_ += static_cast<int>(th) + HELP_TEXT_LINE_SPACING;
      metadata_textures_.push_back(texture);
    }
  };

  // Calculate max length for column sizing
  auto calculate_max_length = [](const VideoMetadata& metadata) -> size_t {
    size_t max_length = 0;
    for (const auto& kv : metadata.properties) {
      // comma tokenize value and find max length among all tokens
      std::vector<std::string> tokens = string_split(kv.second, TOKENIZER);
      for (const auto& token : tokens) {
        max_length = std::max(max_length, token.length());
      }
    }
    return max_length;
  };

  auto left_max_length = calculate_max_length(left_metadata);
  auto right_max_length = calculate_max_length(right_metadata);

  const std::vector<std::string> properties(MetadataProperties::ALL, MetadataProperties::ALL + MetadataProperties::COUNT);

  // calculate available display width (accounting for margins)
  const int available_width = drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2;

  // dynamic column width calculation
  constexpr int spacing = 2;

  // calculate initial column widths based on content
  int prop_cols = MetadataProperties::LONGEST + spacing;
  int left_cols = left_max_length + spacing;
  int right_cols = right_max_length + spacing;
  int total_cols = prop_cols + left_cols + right_cols;

  // determine character widths for both font sizes to choose optimal font
  const std::string test_text = "FOR COMPUTING THE AVERAGE CHARACTER WIDTHS, WE NEED TO TEST THE WIDTH OF A STRING";

  int char_width_small = 10;
  int char_width_big = 14;

  int text_width, text_height;

  if (TTF_GetStringSize(small_font_, test_text.c_str(), 0, &text_width, &text_height)) {
    char_width_small = text_width / test_text.length() + 1;
  }
  if (TTF_GetStringSize(big_font_, test_text.c_str(), 0, &text_width, &text_height)) {
    char_width_big = text_width / test_text.length() + 1;
  }

  // calculate how many characters can fit per line with each font
  const int max_cols_per_line_big = available_width / char_width_big;
  const int max_cols_per_line_small = available_width / char_width_small;

  // choose the largest font that can accommodate all columns
  const int char_width = max_cols_per_line_big >= total_cols ? char_width_big : char_width_small;
  auto font = max_cols_per_line_big >= total_cols ? big_font_ : small_font_;

  const int max_cols_per_line = available_width / char_width;

  // if content is too wide for the window, proportionally reduce column widths
  if (total_cols > max_cols_per_line) {
    const int overshoot = total_cols - max_cols_per_line;

    // distribute the overshoot proportionally across columns
    // property column gets priority (2x weight) since it's the least important
    const int prop_cols_overshoot = std::min(prop_cols, overshoot * prop_cols / total_cols * 2);
    const int left_cols_overshoot = std::max(0, overshoot - prop_cols_overshoot) * left_cols / (left_cols + right_cols);
    const int right_cols_overshoot = overshoot - prop_cols_overshoot - left_cols_overshoot;

    prop_cols -= prop_cols_overshoot;
    left_cols -= left_cols_overshoot;
    right_cols -= right_cols_overshoot;
  }

  // generate table header
  TTF_SetFontStyle(font, TTF_STYLE_ITALIC | TTF_STYLE_UNDERLINE);
  add_metadata_texture(font, string_sprintf("%-*s%-*s%-*s", prop_cols, "", left_cols, "LEFT", right_cols, "RIGHT"), true, false);
  TTF_SetFontStyle(font, TTF_STYLE_NORMAL);

  bool primary_color = false;

  for (const auto& prop : properties) {
    std::string prop_value = to_upper_case(prop);

    // extract values for both videos
    std::string left_value = left_metadata.get(prop);
    std::string right_value = right_metadata.get(prop);

    // tokenize values by comma
    std::vector<std::string> left_tokens = string_split(left_value, TOKENIZER);
    std::vector<std::string> right_tokens = string_split(right_value, TOKENIZER);

    // determine how many lines we need for this property
    size_t max_tokens = std::max(left_tokens.size(), right_tokens.size());

    for (size_t i = 0; i < max_tokens; i++) {
      std::string current_prop_value = (i == 0) ? prop_value : "";
      std::string current_left_value = (i < left_tokens.size()) ? left_tokens[i] : "";
      std::string current_right_value = (i < right_tokens.size()) ? right_tokens[i] : "";

      // text truncation for narrow columns
      if (static_cast<int>(current_prop_value.length()) >= prop_cols) {
        current_prop_value = prop_cols > 1 ? current_prop_value.substr(0, prop_cols - 2) + "… " : "";
      }
      if (static_cast<int>(current_left_value.length()) >= left_cols) {
        current_left_value = "…" + current_left_value.substr(current_left_value.length() - left_cols + 2) + " ";
      }
      if (static_cast<int>(current_right_value.length()) >= right_cols) {
        current_right_value = "…" + current_right_value.substr(current_right_value.length() - right_cols + 2) + " ";
      }

      add_metadata_texture(font, string_sprintf("%-*s%-*s%-*s", prop_cols, current_prop_value.c_str(), left_cols, current_left_value.c_str(), right_cols, current_right_value.c_str()), primary_color, false);

      primary_color = !primary_color;
    }
  }
}

void Display::update_metadata(const VideoMetadata left_metadata, const VideoMetadata right_metadata) {
  left_metadata_ = left_metadata;
  right_metadata_ = right_metadata;

  metadata_dirty_ = true;
}

void Display::update_right_video(const std::string& right_file_name, const VideoMetadata right_metadata) {
  // Update right metadata
  right_metadata_ = right_metadata;
  metadata_dirty_ = true;
  right_file_name_ = right_file_name;

  // Update right file stem
  side_ui_[RIGHT.as_simple_index()].file_stem = strip_ffmpeg_patterns(get_file_stem(right_file_name));

  // Destroy old right texture
  if (side_ui_[RIGHT.as_simple_index()].text_texture != nullptr) {
    SDL_DestroyTexture(side_ui_[RIGHT.as_simple_index()].text_texture);
  }

  // Create new right texture
  SDL_Surface* text_surface = render_text_with_fallback(format_right_file_label(left_file_name_, right_file_name, active_right_index_ + 1));
  side_ui_[RIGHT.as_simple_index()].text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
  side_ui_[RIGHT.as_simple_index()].text_width = text_surface->w;
  side_ui_[RIGHT.as_simple_index()].text_height = text_surface->h;
  SDL_DestroySurface(text_surface);

  // Update window title (may include ROI)
  update_window_title_with_current_roi();
}

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

SDL_Surface* Display::render_text_with_fallback(const std::string& text) {
  SDL_Surface* surface = TTF_RenderText_Blended(small_font_, text.c_str(), 0, TEXT_COLOR);

  if (!surface) {
    std::cerr << "Falling back to lower-quality rendering for '" << text << "'" << std::endl;

    surface = check_sdl(TTF_RenderText_Solid(small_font_, text.c_str(), 0, TEXT_COLOR), "text surface");
  }

  return surface;
}

void Display::ensure_metadata_textures_current() {
  if (metadata_dirty_ || (swap_left_right_ != last_swap_left_right_state_)) {
    last_swap_left_right_state_ = swap_left_right_;

    const VideoMetadata& left_meta = (displayed_left_side_.is_left()) ? left_metadata_ : right_metadata_;
    const VideoMetadata& right_meta = (displayed_right_side_.is_right()) ? right_metadata_ : left_metadata_;
    build_metadata_textures(left_meta, right_meta);

    metadata_dirty_ = false;
  }
}

SDL_Rect Display::get_left_selection_rect() const {
  const int x = std::min(selection_start_.x(), selection_end_.x());
  const int y = std::min(selection_start_.y(), selection_end_.y());
  const int w = std::abs(selection_end_.x() - selection_start_.x());
  const int h = std::abs(selection_end_.y() - selection_start_.y());

  const int clipped_x = std::max(0, x);
  const int clipped_y = std::max(0, y);
  const int clipped_w = std::min(w - (clipped_x - x), video_width_ - clipped_x);
  const int clipped_h = std::min(h - (clipped_y - y), video_height_ - clipped_y);

  return {clipped_x, clipped_y, clipped_w, clipped_h};
}

Vector2D Display::wrap_to_left_frame(const Vector2D& video_position) const {
  switch (mode_) {
    case Mode::HStack:
      return video_position - Vector2D(video_width_, 0);
    case Mode::VStack:
      return video_position - Vector2D(0, video_height_);
    default:
      break;
  }

  return video_position;
}

void Display::refresh_selection_end_from_mouse() {
  if (selection_state_ != SelectionState::Started) {
    return;
  }

  SDL_GetMouseState(&mouse_x_, &mouse_y_);
  selection_end_ = window_to_video_position(mouse_x_, mouse_y_, compute_zoom_rect());

  if (selection_wrap_) {
    selection_end_ = wrap_to_left_frame(selection_end_);
  }
}

void Display::on_view_transform_changed() {
  refresh_selection_end_from_mouse();
}

void Display::draw_selection_rect() {
  if (selection_state_ != SelectionState::Started) {
    return;
  }

  const auto zoom_rect = compute_zoom_rect();

  auto draw_rect = [this](const SDL_FRect& r, Uint8 r_val, Uint8 g_val, Uint8 b_val, int alpha_divider = 1) {
    // Draw semi-transparent overlay
    SDL_SetRenderDrawColor(renderer_, r_val / 2, g_val / 2, b_val / 2, 128 / alpha_divider);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_RenderFillRect(renderer_, &r);

    // Draw border
    SDL_SetRenderDrawColor(renderer_, r_val, g_val, b_val, 255 / alpha_divider);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_RenderRect(renderer_, &r);
  };

  SDL_Rect selection_rect = get_left_selection_rect();
  SDL_FRect drawable_rect = video_rect_to_drawable_transform(video_to_zoom_space(selection_rect, zoom_rect));

  if (mode_ == Mode::Split) {
    if (crop_mode_) {
      switch (crop_target_side_) {
        case CropTargetSide::Right:
          draw_rect(drawable_rect, 128, 128, 255);
          break;
        case CropTargetSide::Both:
          draw_rect(drawable_rect, 255, 255, 255);
          break;
        default:
          draw_rect(drawable_rect, 255, 128, 128);
          break;
      }
    } else {
      draw_rect(drawable_rect, 255, 255, 255);
    }
    return;
  } else {
    if (!crop_mode_ || crop_target_side_ == CropTargetSide::Left || crop_target_side_ == CropTargetSide::Both) {
      draw_rect(drawable_rect, 255, 128, 128);
    } else {
      draw_rect(drawable_rect, 96, 96, 96, 3);
    }
  }

  // Draw right rectangle with appropriate offset
  switch (mode_) {
    case Mode::HStack:
      selection_rect.x += video_width_;
      break;
    case Mode::VStack:
      selection_rect.y += video_height_;
      break;
    default:
      break;
  }

  drawable_rect = video_rect_to_drawable_transform(video_to_zoom_space(selection_rect, zoom_rect));

  if (!crop_mode_ || crop_target_side_ == CropTargetSide::Right || crop_target_side_ == CropTargetSide::Both) {
    draw_rect(drawable_rect, 128, 128, 255);
  } else {
    draw_rect(drawable_rect, 96, 96, 96, 3);
  }
}

void Display::possibly_save_selected_area(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (selection_state_ != SelectionState::Completed) {
    return;
  }

  const SDL_Rect selection_rect = get_left_selection_rect();

  if (selection_rect.w <= 0 || selection_rect.h <= 0) {
    std::cerr << "Selection rectangle is empty. Please make a valid selection." << std::endl;
  } else {
    save_selected_area(left_frame, right_frame, selection_rect);
  }

  selection_state_ = SelectionState::None;
  save_selected_area_ = false;
}

void Display::possibly_apply_crop() {
  if (selection_state_ != SelectionState::Completed) {
    return;
  }

  const SDL_Rect selection_rect = get_left_selection_rect();

  pending_crop_request_ = PendingCropRequest{};

  if (selection_rect.w <= 0 || selection_rect.h <= 0) {
    std::cerr << "Crop rectangle is empty. Please make a valid selection." << std::endl;
  } else {
    pending_crop_request_.rect = selection_rect;
    pending_crop_request_.valid = true;

    switch (crop_target_side_) {
      case CropTargetSide::Left:
        pending_crop_request_.apply_left = true;
        break;
      case CropTargetSide::Right:
        pending_crop_request_.apply_right = true;
        pending_crop_request_.right_target_index = active_right_index_;
        break;
      case CropTargetSide::Both:
        pending_crop_request_.apply_left = true;
        pending_crop_request_.apply_right = true;
        pending_crop_request_.right_target_index = active_right_index_;
        break;
      case CropTargetSide::Undefined:
        pending_crop_request_.valid = false;
        break;
    }
  }

  selection_state_ = SelectionState::None;
  crop_mode_ = false;
  crop_target_side_ = CropTargetSide::Undefined;
}

void Display::save_selected_area(const AVFrame* left_frame, const AVFrame* right_frame, const SDL_Rect& selection_rect) {
  std::atomic_bool error_occurred(false);

  // Lambda for creating and initializing frames
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

  AVFrame* left_selected = create_frame(selection_rect.w, selection_rect.h, left_frame);
  AVFrame* right_selected = create_frame(selection_rect.w, selection_rect.h, right_frame);
  AVFrame* concatenated = create_frame(selection_rect.w * 2, selection_rect.h, left_frame);

  const int pixel_size = hdr_passthrough_ ? 4 : (requires_10_bpc() ? 3 * sizeof(uint16_t) : 3);

  for (int y = 0; y < selection_rect.h; y++) {
    const int src_y = selection_rect.y + y;
    const int dst_y = y;

    // Copy left frame data
    memcpy(left_selected->data[0] + dst_y * left_selected->linesize[0], left_frame->data[0] + src_y * left_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);

    // Copy right frame data
    memcpy(right_selected->data[0] + dst_y * right_selected->linesize[0], right_frame->data[0] + src_y * right_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);

    // Copy to concatenated frame
    memcpy(concatenated->data[0] + dst_y * concatenated->linesize[0], left_frame->data[0] + src_y * left_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);
    memcpy(concatenated->data[0] + dst_y * concatenated->linesize[0] + selection_rect.w * pixel_size, right_frame->data[0] + src_y * right_frame->linesize[0] + selection_rect.x * pixel_size, selection_rect.w * pixel_size);
  }

  const std::string& left_stem = side_ui_[displayed_left_side_.as_simple_index()].file_stem;
  const std::string& right_stem = side_ui_[displayed_right_side_.as_simple_index()].file_stem;
  const bool stems_equal = (left_stem == right_stem);
  const char* cutout_ext = (left_frame->format == AV_PIX_FMT_X2RGB10LE) ? "jxl" : "png";
  const std::string left_filename = string_sprintf("%s%s_cutout_%04d.%s", left_stem.c_str(), stems_equal ? "_left" : "", saved_selected_image_number_, cutout_ext);
  const std::string right_filename = string_sprintf("%s%s_cutout_%04d.%s", right_stem.c_str(), stems_equal ? "_right" : "", saved_selected_image_number_, cutout_ext);
  const std::string concatenated_filename = string_sprintf("%s_%s_cutout_concat_%04d.%s", left_stem.c_str(), right_stem.c_str(), saved_selected_image_number_, cutout_ext);

  auto save_frame = [&](const AVFrame* frame, const std::string& filename) { return save_frame_image(frame, filename, error_occurred); };

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

// Lazily materialize packed RGB copies of the current native YUV  frames for
// features that still need CPU pixel access (subtraction mode, per-pixel
// inspector, live PSNR/SSIM/VMAF). Cached per frame_key so multiple consumers
// in the same refresh share one conversion, and skipped entirely when none of
// those features are active -- the normal GPU path stays YUV-only. Target
// format mirrors what requires_10_bpc() selects so the existing RGB helpers
// (update_difference, get_rgb_pixel, rgb_to_grayscale) interpret the pixels
// correctly. Returns true when both sides are ready; callers must gate RGB-
// dependent work on this.
bool Display::ensure_rgb_frames(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (!gpu_renderer_active_) return true;  // SDL path frames are already RGB

  // Target format matches what the existing CPU pixel helpers expect based on
  // requires_10_bpc(): RGB48LE (10-bit) or RGB24 (8-bit).
  const AVPixelFormat dst_fmt = requires_10_bpc() ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24;

  auto ensure_side = [&](int side, const AVFrame* src, std::string& cache_key) -> bool {
    if (!src || src->data[0] == nullptr || src->width <= 0 || src->height <= 0) return false;

    const std::string new_key = get_frame_key(src);
    if (!new_key.empty() && new_key == cache_key && rgb_frames_[side] != nullptr) {
      return true;
    }

    // Allocate or (re)allocate the destination RGB frame if size/format changed.
    if (rgb_frames_[side] == nullptr || rgb_frames_[side]->width != video_width_ ||
        rgb_frames_[side]->height != video_height_ || rgb_frames_[side]->format != dst_fmt) {
      if (rgb_frames_[side] != nullptr) {
        av_freep(&rgb_frames_[side]->data[0]);
        av_frame_free(&rgb_frames_[side]);
      }
      AVFrame* fr = av_frame_alloc();
      if (!fr) return false;
      fr->format = dst_fmt;
      fr->width = video_width_;
      fr->height = video_height_;
      if (av_image_alloc(fr->data, fr->linesize, video_width_, video_height_, dst_fmt, 64) < 0) {
        av_frame_free(&fr);
        return false;
      }
      rgb_frames_[side] = fr;
      // Force converter rebuild on the next conversion.
      rgb_converter_[side].reset();
    }

    // Lazily construct / reuse the per-side FormatConverter (handles format
    // changes itself via reinit on operator()). Initial params are seeded from
    // the first frame we see.
    if (!rgb_converter_[side]) {
      rgb_converter_[side] = std::make_unique<FormatConverter>(
          src->width, src->height, video_width_, video_height_,
          static_cast<AVPixelFormat>(src->format), dst_fmt,
          src->colorspace, src->color_range);
    }

    // Copy color/CLL props and invoke the converter. The converter sets
    // frame_key metadata on dst from src automatically.
    rgb_frames_[side]->colorspace = src->colorspace;
    rgb_frames_[side]->color_range = src->color_range;
    (*rgb_converter_[side])(const_cast<AVFrame*>(src), rgb_frames_[side]);
    cache_key = new_key;
    return true;
  };

  const bool ok_left = ensure_side(0, left_frame, rgb_frame_keys_[0]);
  const bool ok_right = ensure_side(1, right_frame, rgb_frame_keys_[1]);
  return ok_left && ok_right;
}

bool Display::possibly_refresh(const AVFrame* left_frame, const AVFrame* right_frame, const std::string& current_total_browsable) {
  const std::string left_frame_key = get_frame_key(left_frame);
  const std::string right_frame_key = get_frame_key(right_frame);

  const bool has_updated_left_frame = previous_left_frame_key_ != left_frame_key;
  const bool has_updated_right_frame = previous_right_frame_key_ != right_frame_key;

  if (!input_received_ && !has_updated_left_frame && !has_updated_right_frame && !timer_based_update_performed_ && pending_message_.empty()) {
    return false;
  }

  // --- GPU renderer path ---
  if (gpu_renderer_active_) {
    const bool compare_mode = show_left_ && show_right_;
    const auto zoom_rect = compute_zoom_rect();

    // Reset each frame; set below by animations that need a periodic refresh
    // (loop-mode blink, fading message) even when no new input arrives.
    timer_based_update_performed_ = false;

    // Features that still need CPU pixel access drive an on-demand YUV→RGB
    // conversion. Skipped entirely when none are active to keep the pipeline
    // GPU-fast.
    const bool need_rgb = subtraction_mode_ || print_mouse_position_and_color_ ||
                          print_image_similarity_metrics_ || show_quality_metrics_;
    bool have_rgb = false;
    if (need_rgb) {
      have_rgb = ensure_rgb_frames(left_frame, right_frame);
    }

    // Pixel inspector + similarity metrics consume RGB frames and run before
    // any visual update so a successful key press gets immediate feedback.
    if (have_rgb) {
      const Vector2D mouse_video_pos = window_to_video_position(mouse_x_, mouse_y_, zoom_rect);
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
          std::cout << ", " << get_and_format_rgb_yuv_pixel(rgb_frames_[0]->data[0], rgb_frames_[0]->linesize[0], rgb_frames_[0], pixel_video_x, pixel_video_y);
          std::cout << " - ";
          std::cout << "Right: " << string_sprintf("[%4d,%4d]", pixel_video_x * od_right.first / video_width_, pixel_video_y * od_right.second / video_height_);
          std::cout << ", " << get_and_format_rgb_yuv_pixel(rgb_frames_[1]->data[0], rgb_frames_[1]->linesize[0], rgb_frames_[1], pixel_video_x, pixel_video_y);
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
          AVFrame* left_crop = crop_rgb_frame(rgb_frames_[0], roi, &effective_roi_left);
          AVFrame* right_crop = crop_rgb_frame(rgb_frames_[1], roi, &effective_roi_right);
          if (!SDL_RectsEqual(&effective_roi_left, &effective_roi_right)) {
            std::cerr << "Error: Left and right effective ROIs are different" << std::endl;
          } else {
            const int crop_width = effective_roi_left.w;
            const int crop_height = effective_roi_left.h;

            float* left_gray = rgb_to_grayscale(left_crop->data[0], left_crop->linesize[0], crop_width, crop_height);
            float* right_gray = rgb_to_grayscale(right_crop->data[0], right_crop->linesize[0], crop_width, crop_height);

            const std::string psnr = compute_psnr(left_gray, right_gray, crop_width, crop_height);
            const std::string ssim = compute_ssim(left_gray, right_gray, crop_width, crop_height);
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
        float* left_gray = rgb_to_grayscale(rgb_frames_[0]->data[0], rgb_frames_[0]->linesize[0], video_width_, video_height_);
        float* right_gray = rgb_to_grayscale(rgb_frames_[1]->data[0], rgb_frames_[1]->linesize[0], video_width_, video_height_);
        last_psnr_ = compute_psnr(left_gray, right_gray, video_width_, video_height_);
        last_ssim_ = compute_ssim(left_gray, right_gray, video_width_, video_height_);
        delete[] left_gray;
        delete[] right_gray;

        if (!play_) {
          if (left_frame->pts != last_vmaf_left_pts_ || right_frame->pts != last_vmaf_right_pts_) {
            last_vmaf_ = VMAFCalculator::instance().compute(rgb_frames_[0], rgb_frames_[1]);
            last_vmaf_left_pts_ = left_frame->pts;
            last_vmaf_right_pts_ = right_frame->pts;
          }
        }
      }
    } else {
      // One-shot keys still need to be cleared so they don't re-fire next frame.
      if (need_rgb) {
        if (print_mouse_position_and_color_) print_mouse_position_and_color_ = false;
        if (print_image_similarity_metrics_) print_image_similarity_metrics_ = false;
      }
    }

    // Upload frames only when they've actually changed (matches SDL path logic).
    const bool gpu_subtraction = subtraction_mode_ && have_rgb;
    const bool right_needs_update = input_received_ || has_updated_right_frame || (gpu_subtraction && has_updated_left_frame);

    if (input_received_ || has_updated_left_frame) {
      gpu_renderer_.upload_frame(0, left_frame);
    }
    if (right_needs_update) {
      if (gpu_subtraction) {
        // Compute the RGB diff into diff_buffer_ and hand it to libplacebo via
        // a reusable AVFrame shell. When RGB conversion fails we fall back to
        // the normal YUV upload below so the video stays visible.
        std::array<uint8_t*, 3> rgb_l_planes{rgb_frames_[0]->data[0], nullptr, nullptr};
        std::array<uint8_t*, 3> rgb_r_planes{rgb_frames_[1]->data[0], nullptr, nullptr};
        std::array<size_t, 3> rgb_l_pitches{static_cast<size_t>(rgb_frames_[0]->linesize[0]), 0, 0};
        std::array<size_t, 3> rgb_r_pitches{static_cast<size_t>(rgb_frames_[1]->linesize[0]), 0, 0};
        update_difference(rgb_l_planes, rgb_l_pitches, rgb_r_planes, rgb_r_pitches, 0);

        if (diff_upload_frame_ == nullptr) {
          diff_upload_frame_ = av_frame_alloc();
        }
        diff_upload_frame_->format = rgb_frames_[1]->format;
        diff_upload_frame_->width = video_width_;
        diff_upload_frame_->height = video_height_;
        diff_upload_frame_->data[0] = diff_buffer_;
        for (int i = 1; i < AV_NUM_DATA_POINTERS; ++i) diff_upload_frame_->data[i] = nullptr;
        diff_upload_frame_->linesize[0] = static_cast<int>(diff_pitches_[0]);
        for (int i = 1; i < AV_NUM_DATA_POINTERS; ++i) diff_upload_frame_->linesize[i] = 0;
        diff_upload_frame_->colorspace = rgb_frames_[1]->colorspace;
        diff_upload_frame_->color_range = rgb_frames_[1]->color_range;
        gpu_renderer_.upload_frame(1, diff_upload_frame_);
      } else {
        gpu_renderer_.upload_frame(1, right_frame);
      }
    }

    // Compute mouse-x in video coordinates — identical to the SDL path.
    const float content_mouse_x = static_cast<float>(mouse_x_ - content_window_.x);
    const float safe_content_window_w = static_cast<float>(std::max(1, content_window_.w));
    const float full_ws_mouse_video_x = (content_mouse_x * safe_content_window_w / std::max(1.0F, safe_content_window_w - 1.0F)) * video_to_window_width_factor_;
    const float video_mouse_x = (full_ws_mouse_video_x - zoom_rect.start.x()) * static_cast<float>(video_width_) / zoom_rect.size.x();

    const int split_x = (compare_mode && mode_ == Mode::Split)
                             ? clamp_range(std::round(video_mouse_x), 0.0F, float(video_width_))
                             : show_left_ ? video_width_ : 0;

    // Build render ops using the exact same coordinate chain as the SDL path:
    // video rect -> video_to_zoom_space -> video_rect_to_drawable_transform.
    // A vector (not a fixed array) — zoom-magnifier mode pushes extra ops on
    // top of the main-view ops.
    std::vector<GpuRenderer::SideRenderOp> ops;
    ops.reserve(8);  // 2 main + up to 4 zoom (2 sides × up to 2 slices each)

    auto push_op = [&](int side, int src_x, int src_y, int src_w, int src_h,
                        const SDL_Rect& video_quad) {
      const SDL_FRect screen_rect = video_rect_to_drawable_transform(video_to_zoom_space(video_quad, zoom_rect));
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

    if (show_left_ || show_right_) {
      const int right_x_offset = (mode_ == Mode::HStack) ? video_width_ : 0;
      const int right_y_offset = (mode_ == Mode::VStack) ? video_height_ : 0;

      if (mode_ == Mode::Split) {
        // In Split mode, render the right video to its FULL area first (stable
        // target rect, independent of split position), then paint the left
        // video on top clipped at split_x. This keeps the right video's
        // rendered pixels stable while the split line moves.
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

    // Zoom magnifier windows (bottom-left / bottom-right corners). Each
    // active zoom renders a 64-drawable-pixel source block around the mouse,
    // scaled up to fill half of the min(drawable_w, drawable_h). Matches the
    // SDL path's composited view: Split mode shows right under left clipped
    // at the split position; HStack/VStack show the per-side content on each
    // side of the stack boundary that falls within the src rect. This lets
    // the user switch to the opposite corner when the zoom box itself covers
    // the area they want to inspect.
    const int dst_zoomed_size = static_cast<int>(std::round(std::min(drawable_width_, drawable_height_) * 0.5F)) & -2;
    const int dst_half_zoomed_size = dst_zoomed_size / 2;

    if (zoom_left_ || zoom_right_) {
      const int src_zoomed_size = 64;
      const int src_half = src_zoomed_size / 2;

      const int mouse_drawable_x = std::round(static_cast<float>(mouse_x_) * drawable_to_window_width_factor_);
      const int mouse_drawable_y = std::round(static_cast<float>(mouse_y_) * drawable_to_window_height_factor_);

      const int src_x0_draw = clamp_range(mouse_drawable_x - src_half, 0, drawable_width_ - src_zoomed_size);
      const int src_y0_draw = clamp_range(mouse_drawable_y - src_half, 0, drawable_height_ - src_zoomed_size);
      const int src_x1_draw = src_x0_draw + src_zoomed_size;
      const int src_y1_draw = src_y0_draw + src_zoomed_size;

      // Drawable corners → video-layout coords (layout coords include any
      // HStack/VStack offsets; split_x and video_{width,height} are the
      // boundary coordinates we split on below).
      const Vector2D tl_layout = window_to_video_position(
          static_cast<int>(std::floor(src_x0_draw / drawable_to_window_width_factor_)),
          static_cast<int>(std::floor(src_y0_draw / drawable_to_window_height_factor_)), zoom_rect, true);
      const Vector2D br_layout = window_to_video_position(
          static_cast<int>(std::ceil(src_x1_draw / drawable_to_window_width_factor_)),
          static_cast<int>(std::ceil(src_y1_draw / drawable_to_window_height_factor_)), zoom_rect, false);
      const float sx0 = tl_layout.x(), sy0 = tl_layout.y();
      const float sx1 = br_layout.x(), sy1 = br_layout.y();

      // Push a side render op whose src rect is in *layout* coords (the
      // per-side frame offset is applied here), dst in FBO coords.
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

      // Render the same logical src rect into the given dst box, reproducing
      // the main view's split / stack composition inside the zoom window.
      auto push_zoom_box = [&](float dx0, float dy0, float dx1, float dy1) {
        const float src_w = std::max(1e-3f, sx1 - sx0);
        const float src_h = std::max(1e-3f, sy1 - sy0);
        auto mx = [&](float lx) { return dx0 + (lx - sx0) / src_w * (dx1 - dx0); };
        auto my = [&](float ly) { return dy0 + (ly - sy0) / src_h * (dy1 - dy0); };

        if (mode_ == Mode::Split && compare_mode) {
          // Right full, then left overlaid clipped at split_x (matches the
          // main-view layering so left-of-split shows left, right-of-split
          // shows right).
          if (show_right_) {
            push_zoom_slice(1, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
          }
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
      if (zoom_left_) {
        push_zoom_box(0.f, zoom_dst_y, static_cast<float>(dst_zoomed_size), zoom_dst_y + dst_zoomed_size);
      }
      if (zoom_right_) {
        const float rx0 = static_cast<float>(drawable_width_ - dst_zoomed_size);
        push_zoom_box(rx0, zoom_dst_y, rx0 + dst_zoomed_size, zoom_dst_y + dst_zoomed_size);
      }
    }

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
        const float video_texel_clamped_mouse_x = static_cast<float>(content_window_.x) + (std::round(video_mouse_x) * zoom_rect.size.x() / static_cast<float>(video_width_) + zoom_rect.start.x()) / video_to_window_width_factor_;
        const float split_drawable_x = std::round(video_texel_clamped_mouse_x * drawable_to_window_width_factor_);
        push_rect(split_drawable_x, 0, split_drawable_x + 1, static_cast<float>(drawable_height_),
                  255, 255, 255, 255);

        // Center "slider" line within each active zoom window — matches the
        // SDL path's Split-mode zoom indicator.
        const float zoom_dst_y = static_cast<float>(drawable_height_ - dst_zoomed_size);
        const float zoom_dst_y1 = static_cast<float>(drawable_height_);
        if (zoom_left_) {
          push_rect(static_cast<float>(dst_half_zoomed_size), zoom_dst_y,
                    static_cast<float>(dst_half_zoomed_size + 1), zoom_dst_y1,
                    255, 255, 255, 255);
        }
        if (zoom_right_) {
          const int cx = drawable_width_ - dst_half_zoomed_size - 1;
          push_rect(static_cast<float>(cx), zoom_dst_y,
                    static_cast<float>(cx + 1), zoom_dst_y1,
                    255, 255, 255, 255);
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
        const uint64_t zr = lrintf(global_zoom_factor_ * 1000);
        int tz = ((zr % 10) > 0 ? 0 : 1) + ((zr % 100) > 0 ? 0 : 1) + ((zr % 1000) > 0 ? 0 : 1);
        if (global_zoom_factor_ < 1e-1 || (tz == 0 && zr < 1000)) {
          zoom_factor_str = string_sprintf("x%1.3f", global_zoom_factor_);
        } else if (tz <= 1 && zr < 10000) {
          zoom_factor_str = string_sprintf("x%1.2f", global_zoom_factor_);
        } else if (tz <= 2 && zr < 100000) {
          zoom_factor_str = string_sprintf("x%1.1f", global_zoom_factor_);
        } else {
          zoom_factor_str = string_sprintf("x%1.0f", global_zoom_factor_);
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
        const float playback_speed = 1000000.0f * playback_speed_factor_ /
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
        if (playback_speed_level_ != 0) {
          if (lrintf(playback_speed_factor_ * 100) < 10)
            speed_factor_str = string_sprintf("|%1.1f%%", playback_speed_factor_ * 100);
          else
            speed_factor_str = string_sprintf("|%1.0f%%", playback_speed_factor_ * 100);
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
        if (buffer_play_loop_mode_ != Loop::Off) {
          bg_alpha = static_cast<int>(bg_alpha * (1.0 + std::sin(float(SDL_GetTicks()) / 180.0) * 0.6));
          bg_alpha = clamp_range(bg_alpha, 0, 255);
          switch (buffer_play_loop_mode_) {
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
    if (!pending_message_.empty()) {
      gpu_active_message_ = pending_message_;
      pending_message_.clear();
      message_shown_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    }
    if (!gpu_active_message_.empty()) {
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
      const float elapsed_s = (now - message_shown_at_).count() / 1000.0f;
      constexpr float kHoldSeconds = 3.0f;
      constexpr float kFadeSeconds = 0.5f;
      float keep_alpha;
      if (elapsed_s < kHoldSeconds) {
        keep_alpha = 1.0f;
      } else {
        keep_alpha = std::max(std::sqrt(1.0f - (elapsed_s - kHoldSeconds) / kFadeSeconds), 0.0f);
      }
      if (keep_alpha <= 0.0f) {
        gpu_active_message_.clear();
      } else {
        SDL_Surface* raw = TTF_RenderText_Blended(big_font_, gpu_active_message_.c_str(), 0, TEXT_COLOR);
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
    if (selection_state_ == SelectionState::Started) {
      auto push_selection_rect = [&](const SDL_FRect& r, uint8_t r_val, uint8_t g_val, uint8_t b_val, int alpha_divider = 1) {
        // Semi-transparent fill (half-intensity color).
        push_rect(r.x, r.y, r.x + r.w, r.y + r.h,
                  r_val / 2, g_val / 2, b_val / 2,
                  static_cast<uint8_t>(128 / alpha_divider));

        // Outline: four 1-pixel-thick edges.
        const uint8_t border_a = static_cast<uint8_t>(255 / alpha_divider);
        push_rect(r.x, r.y,                r.x + r.w, r.y + 1,         r_val, g_val, b_val, border_a); // top
        push_rect(r.x, r.y + r.h - 1,      r.x + r.w, r.y + r.h,       r_val, g_val, b_val, border_a); // bottom
        push_rect(r.x, r.y,                r.x + 1,   r.y + r.h,       r_val, g_val, b_val, border_a); // left
        push_rect(r.x + r.w - 1, r.y,      r.x + r.w, r.y + r.h,       r_val, g_val, b_val, border_a); // right
      };

      SDL_Rect selection_rect = get_left_selection_rect();
      SDL_FRect drawable_rect = video_rect_to_drawable_transform(video_to_zoom_space(selection_rect, zoom_rect));

      if (mode_ == Mode::Split) {
        if (crop_mode_) {
          switch (crop_target_side_) {
            case CropTargetSide::Right: push_selection_rect(drawable_rect, 128, 128, 255); break;
            case CropTargetSide::Both:  push_selection_rect(drawable_rect, 255, 255, 255); break;
            default:                    push_selection_rect(drawable_rect, 255, 128, 128); break;
          }
        } else {
          push_selection_rect(drawable_rect, 255, 255, 255);
        }
      } else {
        if (!crop_mode_ || crop_target_side_ == CropTargetSide::Left || crop_target_side_ == CropTargetSide::Both) {
          push_selection_rect(drawable_rect, 255, 128, 128);
        } else {
          push_selection_rect(drawable_rect, 96, 96, 96, 3);
        }

        // Right-side rect offset by the stack gap.
        if (mode_ == Mode::HStack) selection_rect.x += video_width_;
        else if (mode_ == Mode::VStack) selection_rect.y += video_height_;

        drawable_rect = video_rect_to_drawable_transform(video_to_zoom_space(selection_rect, zoom_rect));
        if (!crop_mode_ || crop_target_side_ == CropTargetSide::Right || crop_target_side_ == CropTargetSide::Both) {
          push_selection_rect(drawable_rect, 128, 128, 255);
        } else {
          push_selection_rect(drawable_rect, 96, 96, 96, 3);
        }
      }
    }

    // Live quality metrics overlay (PSNR / SSIM / VMAF) — rendered at the
    // top-right corner when show_quality_metrics_ is on. Suppressed while
    // a full-screen panel is visible (they'd otherwise peek through).
    if (show_quality_metrics_ && !show_help_ && !show_metadata_) {
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
    if (show_help_ || show_metadata_) {
      // Clear previously-accumulated text overlays (HUD labels, positions,
      // FPS, etc.) so they don't poke through the panel.
      text_ops.clear();
      for (SDL_Surface* s : text_surfaces) SDL_DestroySurface(s);
      text_surfaces.clear();
      overlays.clear();

      // Full-screen semi-transparent black background.
      push_rect(0, 0, static_cast<float>(drawable_width_), static_cast<float>(drawable_height_),
                0, 0, 0, static_cast<uint8_t>(BACKGROUND_ALPHA * 3 / 2));

      if (show_help_) {
        int y = help_y_offset_;
        for (SDL_Surface* s : help_surfaces_) {
          if (!s) continue;
          // Skip lines fully outside the visible area.
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
      } else if (show_metadata_) {
        ensure_metadata_textures_current();

        const int table_width = drawable_width_ - HELP_TEXT_HORIZONTAL_MARGIN * 2;
        const int table_x = HELP_TEXT_HORIZONTAL_MARGIN;

        int y;
        if (mode_ == Mode::VStack && metadata_total_height_ < drawable_height_ / 2) {
          y = (drawable_height_ / 2 - metadata_total_height_) / 2;
        } else if (mode_ != Mode::VStack && metadata_total_height_ < drawable_height_) {
          y = (drawable_height_ - metadata_total_height_) / 2;
        } else {
          y = metadata_y_offset_ + 10;
        }

        for (SDL_Surface* s : metadata_surfaces_) {
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

    // Surfaces are no longer referenced by libplacebo after render() returns.
    for (SDL_Surface* s : text_surfaces) SDL_DestroySurface(s);

    // Deferred actions. `save_image_frames` needs `SDL_RenderReadPixels` and
    // `save_selected_area` expects RGB frame data — both are SDL-path only
    // for now. `possibly_apply_crop` just sets a pending request that the
    // main loop consumes, so it works in both paths.
    if (save_image_frames_) {
      std::cerr << "Save image frames: not supported in GPU renderer mode yet." << std::endl;
      save_image_frames_ = false;
    }
    if (save_selected_area_) {
      std::cerr << "Save selected area: not supported in GPU renderer mode yet." << std::endl;
      save_selected_area_ = false;
      selection_state_ = SelectionState::None;
    }
    if (crop_mode_) {
      possibly_apply_crop();
    }

    input_received_ = false;
    previous_left_frame_pts_ = left_frame->pts;
    previous_right_frame_pts_ = right_frame->pts;
    previous_left_frame_key_ = left_frame_key;
    previous_right_frame_key_ = right_frame_key;
    return true;
  }
  // --- End GPU renderer path ---

  std::array<uint8_t*, 3> planes_left{left_frame->data[0], left_frame->data[1], left_frame->data[2]};
  std::array<uint8_t*, 3> planes_right{right_frame->data[0], right_frame->data[1], right_frame->data[2]};
  std::array<size_t, 3> pitches_left{static_cast<size_t>(left_frame->linesize[0]), static_cast<size_t>(left_frame->linesize[1]), static_cast<size_t>(left_frame->linesize[2])};
  std::array<size_t, 3> pitches_right{static_cast<size_t>(right_frame->linesize[0]), static_cast<size_t>(right_frame->linesize[1]), static_cast<size_t>(right_frame->linesize[2])};

  // init 10 bpc temp buffers
  if (requires_10_bpc()) {
    if (left_buffer_ == nullptr) {
      left_buffer_ = new uint32_t[pitches_left[0] * video_height_ / 4];
      left_planes_ = {left_buffer_, nullptr, nullptr};
    }
    if (right_buffer_ == nullptr) {
      right_buffer_ = new uint32_t[pitches_right[0] * video_height_ / 4];
      right_planes_ = {right_buffer_, nullptr, nullptr};
    }
  }

  const bool compare_mode = show_left_ && show_right_;

  const auto zoom_rect = compute_zoom_rect();

  update_window_title_with_current_roi();

  const Vector2D mouse_video_pos = window_to_video_position(mouse_x_, mouse_y_, zoom_rect);
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
      std::cout << ", " << get_and_format_rgb_yuv_pixel(planes_left[0], pitches_left[0], left_frame, pixel_video_x, pixel_video_y);
      std::cout << " - ";
      std::cout << "Right: " << string_sprintf("[%4d,%4d]", pixel_video_x * original_right_dims.first / video_width_, pixel_video_y * original_right_dims.second / video_height_);
      std::cout << ", " << get_and_format_rgb_yuv_pixel(planes_right[0], pitches_right[0], right_frame, pixel_video_x, pixel_video_y);
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

      // assert dimensions are the same
      if (!SDL_RectsEqual(&effective_roi_left, &effective_roi_right)) {
        std::cerr << "Error: Left and right effective ROIs are different" << std::endl;
      } else {
        // compute metrics
        const int crop_width = effective_roi_left.w;
        const int crop_height = effective_roi_left.h;

        float* left_gray = rgb_to_grayscale(left_crop->data[0], left_crop->linesize[0], crop_width, crop_height);
        float* right_gray = rgb_to_grayscale(right_crop->data[0], right_crop->linesize[0], crop_width, crop_height);

        const std::string psnr = compute_psnr(left_gray, right_gray, crop_width, crop_height);
        const std::string ssim = compute_ssim(left_gray, right_gray, crop_width, crop_height);
        const std::string vmaf = (left_crop && right_crop) ? VMAFCalculator::instance().compute(left_crop, right_crop) : "n/a";

        const std::string roi_str =
            (crop_width < video_width_ || crop_height < video_height_) ? string_sprintf("  (%d,%d)-(%d,%d)", effective_roi_left.x, effective_roi_left.y, effective_roi_left.x + crop_width - 1, effective_roi_left.y + crop_height - 1) : "";

        std::cout << string_sprintf("Metrics: [%s|%s] PSNR(%s), SSIM(%s), VMAF(%s)%s", format_position(ffmpeg::pts_in_secs(left_frame), false).c_str(), format_position(ffmpeg::pts_in_secs(right_frame), false).c_str(), psnr.c_str(),
                                    ssim.c_str(), vmaf.c_str(), roi_str.c_str())
                  << std::endl;

        delete[] left_gray;
        delete[] right_gray;
      }

      if (left_crop) {
        av_frame_free(&left_crop);
      }
      if (right_crop) {
        av_frame_free(&right_crop);
      }
    }

    print_image_similarity_metrics_ = false;
  }

  // live on-screen quality metrics overlay (toggled by Q key)
  if (show_quality_metrics_ && left_frame != nullptr && right_frame != nullptr && video_width_ > 0 && video_height_ > 0) {
    float* left_gray = rgb_to_grayscale(left_frame->data[0], left_frame->linesize[0], video_width_, video_height_);
    float* right_gray = rgb_to_grayscale(right_frame->data[0], right_frame->linesize[0], video_width_, video_height_);

    last_psnr_ = compute_psnr(left_gray, right_gray, video_width_, video_height_);
    last_ssim_ = compute_ssim(left_gray, right_gray, video_width_, video_height_);

    delete[] left_gray;
    delete[] right_gray;

    if (!play_) {
      if (left_frame->pts != last_vmaf_left_pts_ || right_frame->pts != last_vmaf_right_pts_) {
        last_vmaf_ = VMAFCalculator::instance().compute(left_frame, right_frame);
        last_vmaf_left_pts_ = left_frame->pts;
        last_vmaf_right_pts_ = right_frame->pts;
      }
    }
  }

  // clear everything
  SDL_SetRenderDrawColor(renderer_, BACKGROUND_COLOR.r, BACKGROUND_COLOR.g, BACKGROUND_COLOR.b, BACKGROUND_COLOR.a);
  SDL_RenderClear(renderer_);

  // mouse video x-position stretched to the active content area (letterboxed in fullscreen)
  const float content_mouse_x = static_cast<float>(mouse_x_ - content_window_.x);
  const float safe_content_window_w = static_cast<float>(std::max(1, content_window_.w));
  const float full_ws_mouse_video_x = (content_mouse_x * safe_content_window_w / std::max(1.0F, safe_content_window_w - 1.0F)) * video_to_window_width_factor_;

  // mouse x-position in video coordinates
  const float video_mouse_x = (full_ws_mouse_video_x - zoom_rect.start.x()) * static_cast<float>(video_width_) / zoom_rect.size.x();

  // the nearest texel border to the mouse x-position in window coordinates
  const float video_texel_clamped_mouse_x = static_cast<float>(content_window_.x) + (std::round(video_mouse_x) * zoom_rect.size.x() / static_cast<float>(video_width_) + zoom_rect.start.x()) / video_to_window_width_factor_;

  if (show_left_ || show_right_) {
    const int split_x = (compare_mode && mode_ == Mode::Split) ? clamp_range(std::round(video_mouse_x), 0.0F, float(video_width_)) : show_left_ ? video_width_ : 0;

    // Upload full frames to per-side textures
    if (input_received_ || has_updated_left_frame) {
      if (requires_10_bpc()) {
        const SDL_Rect full_rect = {0, 0, video_width_, video_height_};
        convert_to_packed_10_bpc(planes_left, pitches_left, left_planes_, pitches_left, full_rect);
        update_side_texture(0, left_planes_[0], pitches_left[0]);
      } else {
        update_side_texture(0, planes_left[0], pitches_left[0]);
      }
    }

    // Subtraction mode depends on both frames; refresh when either changes
    const bool right_needs_update = input_received_ || has_updated_right_frame || (subtraction_mode_ && has_updated_left_frame);

    if (right_needs_update) {
      if (subtraction_mode_) {
        update_difference(planes_left, pitches_left, planes_right, pitches_right, 0);

        if (requires_10_bpc()) {
          const SDL_Rect full_rect = {0, 0, video_width_, video_height_};
          convert_to_packed_10_bpc(diff_planes_, diff_pitches_, right_planes_, pitches_right, full_rect);
          update_side_texture(1, right_planes_[0], pitches_right[0]);
        } else {
          update_side_texture(1, diff_planes_[0], diff_pitches_[0]);
        }
      } else {
        if (requires_10_bpc()) {
          const SDL_Rect full_rect = {0, 0, video_width_, video_height_};
          convert_to_packed_10_bpc(planes_right, pitches_right, right_planes_, pitches_right, full_rect);
          update_side_texture(1, right_planes_[0], pitches_right[0]);
        } else {
          update_side_texture(1, planes_right[0], pitches_right[0]);
        }
      }
    }

    // Render from per-side textures to screen regions
    if (show_left_ && (split_x > 0)) {
      const SDL_FRect src_left = {0, 0, static_cast<float>(split_x), static_cast<float>(video_height_)};
      const SDL_Rect video_quad_left = {0, 0, split_x, video_height_};
      const SDL_FRect screen_quad_left = video_rect_to_drawable_transform(video_to_zoom_space(video_quad_left, zoom_rect));
      check_sdl(SDL_RenderTexture(renderer_, get_side_texture(0), &src_left, &screen_quad_left), "left video texture render");
    }
    if (show_right_ && ((split_x < video_width_) || mode_ != Mode::Split)) {
      const int start_right = (mode_ == Mode::Split) ? std::max(split_x, 0) : 0;
      const int right_x_offset = (mode_ == Mode::HStack) ? video_width_ : 0;
      const int right_y_offset = (mode_ == Mode::VStack) ? video_height_ : 0;

      const SDL_FRect src_right = {static_cast<float>(start_right), 0, static_cast<float>(video_width_ - start_right), static_cast<float>(video_height_)};
      const SDL_Rect video_quad_right = {right_x_offset + start_right, right_y_offset, video_width_ - start_right, video_height_};
      const SDL_FRect screen_quad_right = video_rect_to_drawable_transform(video_to_zoom_space(video_quad_right, zoom_rect));
      check_sdl(SDL_RenderTexture(renderer_, get_side_texture(1), &src_right, &screen_quad_right), "right video texture render");
    }
  }

  const int mouse_drawable_x = std::round(video_texel_clamped_mouse_x * drawable_to_window_width_factor_);
  const int mouse_drawable_y = std::round(static_cast<float>(mouse_y_) * drawable_to_window_height_factor_);

  // zoomed area
  const int dst_zoomed_size = static_cast<int>(std::round(std::min(drawable_width_, drawable_height_) * 0.5F)) & -2;  // size must be an even number of pixels
  const int dst_half_zoomed_size = dst_zoomed_size / 2;

  if (zoom_left_ || zoom_right_) {
    const int src_zoomed_size = 64;
    const int src_half_zoomed_size = src_zoomed_size / 2;

    SDL_Rect src_zoomed_area = {clamp_range(mouse_drawable_x - src_half_zoomed_size, 0, drawable_width_ - src_zoomed_size - 1), clamp_range(mouse_drawable_y - src_half_zoomed_size, 0, drawable_height_ - src_zoomed_size - 1),
                                src_zoomed_size, src_zoomed_size};

    SDL_Surface* render_surface = SDL_RenderReadPixels(renderer_, &src_zoomed_area);
    SDL_Texture* render_texture = render_surface ? SDL_CreateTextureFromSurface(renderer_, render_surface) : nullptr;

    if (render_texture) {
      if (zoom_left_) {
        const SDL_FRect dst_zoomed_area = {0, static_cast<float>(drawable_height_ - dst_zoomed_size), static_cast<float>(dst_zoomed_size), static_cast<float>(dst_zoomed_size)};
        SDL_RenderTexture(renderer_, render_texture, nullptr, &dst_zoomed_area);
      }
      if (zoom_right_) {
        const SDL_FRect dst_zoomed_area = {static_cast<float>(drawable_width_ - dst_zoomed_size), static_cast<float>(drawable_height_ - dst_zoomed_size), static_cast<float>(dst_zoomed_size), static_cast<float>(dst_zoomed_size)};
        SDL_RenderTexture(renderer_, render_texture, nullptr, &dst_zoomed_area);
      }
    }

    SDL_DestroyTexture(render_texture);
    SDL_DestroySurface(render_surface);
  }

  timer_based_update_performed_ = false;

  SDL_FRect fill_rect;
  SDL_FRect text_rect;
  SDL_Surface* text_surface;

  if (show_hud_) {
    const float left_position = ffmpeg::pts_in_secs(left_frame);
    const float right_position = ffmpeg::pts_in_secs(right_frame);
    const float left_progress = left_position + ffmpeg::frame_duration_in_secs(left_frame);
    const float right_progress = right_position + ffmpeg::frame_duration_in_secs(right_frame);

    // render background rectangles and text on top
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);

    if (show_left_) {
      // file name and current position of left video
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
      // file name and current position of right video
      const std::string right_picture_type(1, av_get_picture_type_char(right_frame->pict_type));
      const std::string right_pos_str = format_position(right_position, true) + " " + right_picture_type + format_position_difference(right_position, left_position);
      text_surface = TTF_RenderText_Blended(small_font_, right_pos_str.c_str(), 0, POSITION_COLOR);
      SDL_Texture* right_position_text_texture = SDL_CreateTextureFromSurface(renderer_, text_surface);
      int right_position_text_width = text_surface->w;
      int right_position_text_height = text_surface->h;
      SDL_DestroySurface(text_surface);

      int text1_x;
      int text1_y;
      int text2_x;
      int text2_y;

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
      // target seek position
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
    const uint64_t global_zoom_factor_rounded = lrintf(global_zoom_factor_ * 1000);
    int global_zoom_factor_trailing_zeros = (global_zoom_factor_rounded % 10) > 0 ? 0 : 1;
    global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 100) > 0 ? 0 : 1;
    global_zoom_factor_trailing_zeros += (global_zoom_factor_rounded % 1000) > 0 ? 0 : 1;

    if (global_zoom_factor_ < 1e-1 || (global_zoom_factor_trailing_zeros == 0 && global_zoom_factor_rounded < 1000)) {
      zoom_factor_str = string_sprintf("x%1.3f", global_zoom_factor_);
    } else if (global_zoom_factor_trailing_zeros <= 1 && global_zoom_factor_rounded < 10000) {
      zoom_factor_str = string_sprintf("x%1.2f", global_zoom_factor_);
    } else if (global_zoom_factor_trailing_zeros <= 2 && global_zoom_factor_rounded < 100000) {
      zoom_factor_str = string_sprintf("x%1.1f", global_zoom_factor_);
    } else {
      zoom_factor_str = string_sprintf("x%1.0f", global_zoom_factor_);
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

    const float playback_speed = 1000000.0f * playback_speed_factor_ / float(std::max(ffmpeg::frame_duration(left_frame), ffmpeg::frame_duration(right_frame)));
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

    if (playback_speed_level_ != 0) {
      if (lrintf(playback_speed_factor_ * 100) < 10) {
        playback_speed_factor_str = string_sprintf("|%1.1f%%", playback_speed_factor_ * 100);
      } else {
        playback_speed_factor_str = string_sprintf("|%1.0f%%", playback_speed_factor_ * 100);
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

    if (buffer_play_loop_mode_ != Display::Loop::Off) {
      label_alpha *= 1.0 + sin(float(SDL_GetTicks()) / 180.0) * 0.6;

      switch (buffer_play_loop_mode_) {
        case Display::Loop::ForwardOnly:
          label_color = LOOP_FW_LABEL_COLOR;
          break;
        case Display::Loop::PingPong:
          label_color = LOOP_PP_LABEL_COLOR;
          break;
        default:
          break;
      }

      timer_based_update_performed_ = true;
    }

    SDL_SetRenderDrawColor(renderer_, label_color.r, label_color.g, label_color.b, label_alpha);
    SDL_RenderFillRect(renderer_, &fill_rect);

    text_rect = make_frect(drawable_width_ / 2 - current_total_browsable_text_width / 2, text_y, current_total_browsable_text_width, current_total_browsable_text_height);
    SDL_RenderTexture(renderer_, current_total_browsable_text_texture, nullptr, &text_rect);
    SDL_DestroyTexture(current_total_browsable_text_texture);

    // display progress as dot lines
    render_progress_dots(left_position, left_progress, true);
    render_progress_dots(right_position, right_progress, false);
  }

  // render (optional) message
  if (!pending_message_.empty()) {
    message_shown_at_ = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    text_surface = TTF_RenderText_Blended(big_font_, pending_message_.c_str(), 0, TEXT_COLOR);

    if (message_texture_ != nullptr) {
      SDL_DestroyTexture(message_texture_);
    }
    message_texture_ = SDL_CreateTextureFromSurface(renderer_, text_surface);

    message_width_ = text_surface->w;
    message_height_ = text_surface->h;
    SDL_DestroySurface(text_surface);

    pending_message_.clear();
  }
  if (message_texture_ != nullptr) {
    std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    const float elapsed_s = (now - message_shown_at_).count() / 1000.0F;
    constexpr float kHoldSeconds = 2.0F;
    constexpr float kFadeSeconds = 1.0F;
    const float keep_alpha = (elapsed_s < kHoldSeconds)
                                 ? 1.0F
                                 : std::max(sqrtf(1.0F - (elapsed_s - kHoldSeconds) / kFadeSeconds), 0.0F);

    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, BACKGROUND_ALPHA * keep_alpha);
    fill_rect = make_frect(drawable_width_ / 2 - message_width_ / 2 - 2, drawable_height_ / 2 - message_height_ / 2 - 2, message_width_ + 4, message_height_ + 4);
    SDL_RenderFillRect(renderer_, &fill_rect);

    SDL_SetTextureAlphaMod(message_texture_, 255 * keep_alpha);
    text_rect = make_frect(drawable_width_ / 2 - message_width_ / 2, drawable_height_ / 2 - message_height_ / 2, message_width_, message_height_);
    SDL_RenderTexture(renderer_, message_texture_, nullptr, &text_rect);

    timer_based_update_performed_ = timer_based_update_performed_ || (keep_alpha > 0.0F);
  }

  if (mode_ == Mode::Split && show_hud_ && compare_mode) {
    // render movable slider(s)
    SDL_SetRenderDrawColor(renderer_, 255, 255, 255, SDL_ALPHA_OPAQUE);
    SDL_RenderLine(renderer_, mouse_drawable_x, 0, mouse_drawable_x, drawable_height_);

    if (zoom_left_) {
      SDL_RenderLine(renderer_, dst_half_zoomed_size, drawable_height_ - dst_zoomed_size, dst_half_zoomed_size, drawable_height_);
    }
    if (zoom_right_) {
      SDL_RenderLine(renderer_, drawable_width_ - dst_half_zoomed_size - 1, drawable_height_ - dst_zoomed_size, drawable_width_ - dst_half_zoomed_size - 1, drawable_height_);
    }
  }

  draw_selection_rect();

  if (show_quality_metrics_) {
    render_quality_metrics_overlay();
  }

  if (show_metadata_) {
    render_metadata_overlay();
  }

  if (show_help_) {
    render_help();
  }

  if (save_image_frames_) {
    save_image_frames(left_frame, right_frame);
    save_image_frames_ = false;
  }

  if (save_selected_area_) {
    possibly_save_selected_area(left_frame, right_frame);
  }
  if (crop_mode_) {
    possibly_apply_crop();
  }

  SDL_RenderPresent(renderer_);

  input_received_ = false;
  previous_left_frame_pts_ = left_frame->pts;
  previous_right_frame_pts_ = right_frame->pts;
  previous_left_frame_key_ = left_frame_key;
  previous_right_frame_key_ = right_frame_key;

  return true;
}

void Display::upload_native_frame(int side, const AVFrame* frame) {
  if (gpu_renderer_active_) {
    gpu_renderer_.upload_frame(side, frame);
  }
}

void Display::set_pending_message(const std::string& message) {
  pending_message_ = message;
}

void Display::notify_user(const std::string& message) {
  if (!is_fullscreen_) {
    // Avoid cluttering the screen with messages in windowed mode
    std::cout << message << std::endl;
  } else {
    set_pending_message(message);
  }
}

void Display::focus_main_window() {
  if (window_ != nullptr) {
    SDL_RaiseWindow(window_);
  }
}

float Display::compute_zoom_factor(const float zoom_level) const {
  return pow(ZOOM_STEP_SIZE, zoom_level);
}

Vector2D Display::compute_relative_move_offset(const Vector2D& zoom_point, const float zoom_factor) const {
  const float zoom_factor_change = zoom_factor / global_zoom_factor_;

  const Vector2D view_center((static_cast<float>(content_window_.x) + static_cast<float>(content_window_.w) / 2.0F) * video_to_window_width_factor_,
                             (static_cast<float>(content_window_.y) + static_cast<float>(content_window_.h) / 2.0F) * video_to_window_height_factor_);

  // the center point has to be moved relative to the zoom point
  const Vector2D new_move_offset = move_offset_ - (view_center + move_offset_ - zoom_point) * (1.0F - zoom_factor_change);

  return new_move_offset;
}

void Display::update_zoom_factor_and_move_offset(const float zoom_factor) {
  const Vector2D zoom_point(static_cast<float>(video_width_) * (mode_ == Mode::HStack ? 1.0F : 0.5F), static_cast<float>(video_height_) * (mode_ == Mode::VStack ? 1.0F : 0.5F));
  update_move_offset(compute_relative_move_offset(zoom_point, zoom_factor));

  update_zoom_factor(zoom_factor);
}

void Display::update_zoom_factor(const float zoom_factor) {
  global_zoom_factor_ = zoom_factor;
  global_zoom_level_ = log(zoom_factor) / log(ZOOM_STEP_SIZE);
  on_view_transform_changed();
}

void Display::update_move_offset(const Vector2D& move_offset) {
  move_offset_ = move_offset;
  global_center_ = Vector2D(move_offset_.x() / video_width_ + 0.5F, move_offset_.y() / video_height_ + 0.5F);
  on_view_transform_changed();
}

Display::ZoomRect Display::compute_zoom_rect() const {
  const Vector2D video_extent(video_width_, video_height_);
  const Vector2D zoom_rect_start((global_center_ - global_zoom_factor_ * 0.5F) * video_extent);
  const Vector2D zoom_rect_end((global_center_ + global_zoom_factor_ * 0.5F) * video_extent);
  const Vector2D zoom_rect_size(zoom_rect_end - zoom_rect_start);
  return {zoom_rect_start, zoom_rect_end, zoom_rect_size, global_zoom_factor_};
}

Vector2D Display::window_to_video_position(const int window_x_position, const int window_y_position, const Display::ZoomRect& zoom_rect, const bool floor_result) const {
  auto floor_or_ceil = [&](const float value) -> int { return floor_result ? std::floor(value) : std::ceil(value); };

  const float window_x_in_content = static_cast<float>(window_x_position - content_window_.x);
  const float window_y_in_content = static_cast<float>(window_y_position - content_window_.y);
  const int video_x = floor_or_ceil((window_x_in_content * video_to_window_width_factor_ - zoom_rect.start.x()) * static_cast<float>(video_width_) / zoom_rect.size.x());
  const int video_y = floor_or_ceil((window_y_in_content * video_to_window_height_factor_ - zoom_rect.start.y()) * static_cast<float>(video_height_) / zoom_rect.size.y());

  return Vector2D(video_x, video_y);
}

SDL_FRect Display::video_to_zoom_space(const SDL_Rect& video_rect, const Display::ZoomRect& zoom_rect) const {
  // transform video coordinates to the currently zoomed area space
  return SDL_FRect({zoom_rect.start.x() + float(video_rect.x) * zoom_rect.zoom_factor, zoom_rect.start.y() + float(video_rect.y) * zoom_rect.zoom_factor, std::min(float(video_rect.w) * zoom_rect.zoom_factor, zoom_rect.size.x()),
                    std::min(float(video_rect.h) * zoom_rect.zoom_factor, zoom_rect.size.y())});
};

std::pair<SDL_Rect, SDL_Rect> Display::get_visible_rois_in_single_frame_coordinates() const {
  const auto zoom_rect = compute_zoom_rect();

  // p0/p1 are in *layout* coordinates in hstack/vstack, single-frame in split.
  const Vector2D p0 = window_to_video_position(0, 0, zoom_rect, true);
  const Vector2D p1 = window_to_video_position(window_width_, window_height_, zoom_rect, false);

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

SDL_Rect Display::get_visible_roi_in_single_frame_coordinates() const {
  const auto rois = get_visible_rois_in_single_frame_coordinates();
  const SDL_Rect left_roi = rois.first;
  const SDL_Rect right_roi = rois.second;

  const bool left_off = left_roi.w <= 0 || left_roi.h <= 0;
  const bool right_off = right_roi.w <= 0 || right_roi.h <= 0;

  if (mode_ == Mode::HStack || mode_ == Mode::VStack) {
    // Prefer the side that the view center is currently over, but fall back to the other
    // if that side is not visible.
    const auto zoom_rect = compute_zoom_rect();
    const Vector2D center_layout = window_to_video_position(window_width_ / 2, window_height_ / 2, zoom_rect, true);

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

void Display::update_playback_speed(const float playback_speed_level_delta) {
  const float playback_speed_level = playback_speed_level_ + playback_speed_level_delta;

  // allow 128x change of playback speed
  if (std::abs(playback_speed_level) <= static_cast<float>(PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE * 7)) {
    playback_speed_level_ = playback_speed_level;
    playback_speed_factor_ = pow(PLAYBACK_SPEED_STEP_SIZE, playback_speed_level);
  }
}

void Display::begin_input_frame() {
  seek_relative_ = 0.0F;
  seek_from_start_ = false;
  frame_buffer_offset_delta_ = 0;
  frame_navigation_delta_ = 0;
  shift_right_frames_ = 0;
  auto_align_requested_ = false;
  tick_playback_ = false;
  possibly_tick_playback_ = false;
  toggle_scope_window_requested_.fill(false);
}

void Display::mark_input_received() {
  input_received_ = true;
}

void Display::handle_event(const SDL_Event& event) {
  event_ = event;
  input_received_ = true;

  auto update_cursor = [&]() {
    SDL_Cursor* cursor;

    if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK) {
      cursor = pan_mode_cursor_;
    } else if ((save_selected_area_ || crop_mode_) && selection_state_ != SelectionState::Completed) {
      cursor = selection_mode_cursor_;
    } else {
      cursor = normal_mode_cursor_;
    }

    SDL_SetCursor(cursor);
  };

  auto handle_scroll = [&](int& y_offset, const int total_height, size_t count) {
    y_offset += (-event_.motion.yrel * total_height * 3) / drawable_height_;
    y_offset = std::max(y_offset, drawable_height_ - total_height - static_cast<int>(count) * HELP_TEXT_LINE_SPACING);
    y_offset = std::min(y_offset, 0);
  };

  switch (event_.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
      // If the main application window is being closed, request application quit
      if (event_.window.windowID == SDL_GetWindowID(window_)) {
        quit_ = true;
      }
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      mouse_is_inside_window_ = false;
      break;
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
      mouse_is_inside_window_ = true;
      break;
    case SDL_EVENT_WINDOW_HDR_STATE_CHANGED:
      update_hdr_display_state();
      break;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
      handle_window_resize();

      if (pending_verbose_print_) {
        print_verbose_info();
        pending_verbose_print_ = false;
      }
      break;
    case SDL_EVENT_MOUSE_WHEEL:
      if (mouse_is_inside_window_ && event_.wheel.y != 0) {
        float delta_zoom = wheel_sensitivity_ * event_.wheel.y * (event_.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
        if (delta_zoom > 0) {
          delta_zoom /= 2.0F;
        }
        if (SDL_GetModState() & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL)) {
          delta_zoom /= ZOOM_SLOWDOWN_RATIO;
        }

        const float new_global_zoom_factor = compute_zoom_factor(global_zoom_level_ - delta_zoom);

        // logic ported from YUView's MoveAndZoomableView.cpp with thanks :)
        if (new_global_zoom_factor >= 0.001 && new_global_zoom_factor <= 10000) {
          const Vector2D zoom_point = Vector2D(static_cast<float>(mouse_x_ - content_window_.x) * video_to_window_width_factor_, static_cast<float>(mouse_y_ - content_window_.y) * video_to_window_height_factor_);
          update_move_offset(compute_relative_move_offset(zoom_point, new_global_zoom_factor));
          update_zoom_factor(new_global_zoom_factor);
        }
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      SDL_GetMouseState(&mouse_x_, &mouse_y_);

      refresh_selection_end_from_mouse();

      if (event_.motion.state & SDL_BUTTON_RMASK) {
        const auto pan_offset = Vector2D(event_.motion.xrel, event_.motion.yrel) * Vector2D(video_to_window_width_factor_, video_to_window_height_factor_) / Vector2D(drawable_to_window_width_factor_, drawable_to_window_height_factor_);

        update_move_offset(move_offset_ + pan_offset);
      }

      if (show_metadata_) {
        handle_scroll(metadata_y_offset_, metadata_total_height_,
                       gpu_renderer_active_ ? metadata_surfaces_.size() : metadata_textures_.size());
      }

      if (show_help_) {
        handle_scroll(help_y_offset_, help_total_height_,
                       gpu_renderer_active_ ? help_surfaces_.size() : help_textures_.size());
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event_.button.button == SDL_BUTTON_LEFT && (save_selected_area_ || crop_mode_) && selection_state_ == SelectionState::None) {
        selection_state_ = SelectionState::Started;
        selection_start_ = window_to_video_position(mouse_x_, mouse_y_, compute_zoom_rect());

        // Check if the selection is outside the left video frame
        selection_wrap_ = (mode_ == Mode::HStack && selection_start_.x() >= video_width_) || (mode_ == Mode::VStack && selection_start_.y() >= video_height_);

        if (selection_wrap_) {
          selection_start_ = wrap_to_left_frame(selection_start_);
        }

        selection_end_ = selection_start_;
      } else if (event_.button.button != SDL_BUTTON_RIGHT) {
        seek_relative_ = static_cast<float>(mouse_x_) / static_cast<float>(window_width_);
        seek_from_start_ = true;
      }
      update_cursor();
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event_.button.button == SDL_BUTTON_LEFT && selection_state_ == SelectionState::Started) {
        selection_state_ = SelectionState::Completed;
      }
      update_cursor();
      break;
    case SDL_EVENT_KEY_DOWN: {
      const SDL_Keymod keymod = event_.key.mod;
      const SDL_Keycode keycode = event_.key.key;
      const bool is_shift_down = (keymod & SDL_KMOD_SHIFT) != 0;
      const bool is_ctrl_down = (keymod & SDL_KMOD_CTRL) != 0;
      const bool is_alt_down = (keymod & SDL_KMOD_ALT) != 0;

      const float relative_seek_scale = (is_shift_down || is_ctrl_down) ? 1.0F / RELATIVE_SEEK_SLOWDOWN_RATIO : 1.0F;
      const float playback_speed_scale = (is_shift_down || is_ctrl_down) ? 1.0F / PLAYBACK_SPEED_SLOWDOWN_RATIO : 1.0F;

      auto is_clipboard_mod_pressed = [is_ctrl_down, keymod]() -> bool {
#ifdef __APPLE__
        return (keymod & SDL_KMOD_GUI);
#else
        return is_ctrl_down;
#endif
      };

      auto start_crop_mode_for_side = [&](const CropTargetSide side) {
        save_selected_area_ = false;
        crop_target_side_ = side;
        crop_mode_ = true;
        selection_state_ = SelectionState::None;
        update_cursor();
      };
      auto reset_crop_mode = [&]() {
        crop_mode_ = false;
        crop_target_side_ = CropTargetSide::Undefined;
        selection_state_ = SelectionState::None;
        update_cursor();
      };
      auto toggle_crop_mode_for_side = [&](const CropTargetSide side) {
        if (crop_mode_) {
          reset_crop_mode();
        } else {
          start_crop_mode_for_side(side);
        }
      };
      auto restore_window_size = [&](const std::array<int, 2>& size) {
        const int target_w = std::max(MIN_WINDOW_WIDTH, size[0]);
        const int target_h = std::max(MIN_WINDOW_HEIGHT, size[1]);
        // Route restore operations through the normal resize path so active
        // aspect-lock constraints (window/content) are always enforced.
        apply_window_size_and_relayout(target_w, target_h, false);
      };

      // Handle CTRL+SHIFT+1..0 for direct right video selection
      if (is_ctrl_down && is_shift_down) {
        size_t target_index = SIZE_MAX;

        if (keycode >= SDLK_1 && keycode <= SDLK_9) {
          target_index = keycode - SDLK_1;
        } else if (keycode >= SDLK_KP_1 && keycode <= SDLK_KP_9) {
          target_index = keycode - SDLK_KP_1;
        } else if ((keycode == SDLK_KP_0) || (keycode == SDLK_0)) {
          target_index = 9;
        }

        if (target_index != SIZE_MAX) {
          if (target_index < num_right_videos_) {
            active_right_index_ = target_index;
            notify_user(string_sprintf("Active right video: %d/%d", active_right_index_ + 1, num_right_videos_));
          }
          break;
        }
      }

      switch (keycode) {
        case SDLK_H:
          show_help_ = !show_help_;
          break;
        case SDLK_ESCAPE:
          quit_ = true;
          break;
        case SDLK_BACKSPACE:
          pending_crop_request_ = PendingCropRequest{};
          pending_crop_request_.clear_requested = true;
          reset_crop_mode();
          break;
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
          }
          break;
        case SDLK_W: {
          if (is_ctrl_down && is_shift_down) {
            saved_window_size_ = {window_width_, window_height_};
            std::cout << string_sprintf("Saved window size (%dx%d)", saved_window_size_[0], saved_window_size_[1]) << std::endl;
          } else if (is_ctrl_down) {
            restore_window_size(startup_window_size_);
            std::cout << string_sprintf("Restored startup window size (%dx%d)", startup_window_size_[0], startup_window_size_[1]) << std::endl;
          } else if (is_shift_down) {
            restore_window_size(saved_window_size_);
            std::cout << string_sprintf("Restored saved window size (%dx%d)", saved_window_size_[0], saved_window_size_[1]) << std::endl;
          }
          break;
        }
        case SDLK_SPACE:
          play_ = !play_;
          buffer_play_loop_mode_ = Loop::Off;
          tick_playback_ = play_;
          break;
        case SDLK_COMMA:
        case SDLK_KP_COMMA:
          set_buffer_play_loop_mode(buffer_play_loop_mode_ != Loop::PingPong ? Loop::PingPong : Loop::Off);
          break;
        case SDLK_PERIOD:
          set_buffer_play_loop_mode(buffer_play_loop_mode_ != Loop::ForwardOnly ? Loop::ForwardOnly : Loop::Off);
          break;
        case SDLK_F1:
          toggle_scope_window_requested_[ScopeWindow::index(ScopeWindow::Type::Histogram)] = true;
          break;
        case SDLK_1:
        case SDLK_KP_1:
          if (is_shift_down) {
            // Fallback for layouts where F-keys are inconvenient
            toggle_scope_window_requested_[ScopeWindow::index(ScopeWindow::Type::Histogram)] = true;
          } else {
            show_left_ = !show_left_;
          }
          break;
        case SDLK_F2:
          toggle_scope_window_requested_[ScopeWindow::index(ScopeWindow::Type::Vectorscope)] = true;
          break;
        case SDLK_2:
        case SDLK_KP_2:
          if (is_shift_down) {
            // Fallback for layouts where F-keys are inconvenient
            toggle_scope_window_requested_[ScopeWindow::index(ScopeWindow::Type::Vectorscope)] = true;
          } else {
            show_right_ = !show_right_;
          }
          break;
        case SDLK_F3:
          toggle_scope_window_requested_[ScopeWindow::index(ScopeWindow::Type::Waveform)] = true;
          break;
        case SDLK_3:
        case SDLK_KP_3:
          if (is_shift_down) {
            // Fallback for layouts where F-keys are inconvenient
            toggle_scope_window_requested_[ScopeWindow::index(ScopeWindow::Type::Waveform)] = true;
          } else {
            show_hud_ = !show_hud_;
          }
          break;
        case SDLK_0:
        case SDLK_KP_0:
          subtraction_mode_ = !subtraction_mode_;
          break;
        case SDLK_Z:
          zoom_left_ = true;
          break;
        case SDLK_C: {
          if (is_clipboard_mod_pressed()) {
            const float previous_left_frame_secs = previous_left_frame_pts_ * AV_TIME_TO_SEC;
            const std::string previous_left_frame_secs_str = format_position(previous_left_frame_secs, false);

            SDL_SetClipboardText(previous_left_frame_secs_str.c_str());

            notify_user(string_sprintf("Copied to clipboard: %s", previous_left_frame_secs_str.c_str()));
          } else {
            zoom_right_ = true;
          }
          break;
        }
        case SDLK_V: {
          if (is_clipboard_mod_pressed()) {
            char* clip_text = SDL_GetClipboardText();

            if (!clip_text) {
              std::cerr << "Failed to get clipboard text: " << SDL_GetError() << std::endl;
              return;
            }

            std::string clipboard_str(clip_text);
            SDL_free(clip_text);

            static const std::regex timestamp_regex(R"((?:(\d+):)?(?:(\d+):)?(\d+(?:\.\d+)?))");
            std::smatch match;

            if (std::regex_search(clipboard_str, match, timestamp_regex)) {
              std::string timestamp = match.str();
              notify_user(string_sprintf("Timestamp pasted: %s", timestamp.c_str()));

              seek_relative_ = parse_timestamps_to_seconds(timestamp) / static_cast<float>(duration_);
              seek_from_start_ = true;
            } else {
              notify_user("No valid timestamp found in clipboard.");
            }
          } else {
            show_metadata_ = !show_metadata_;
          }
          break;
        }
        case SDLK_A:
          if (is_shift_down) {
            --frame_navigation_delta_;
          } else {
            frame_buffer_offset_delta_++;
          }
          break;
        case SDLK_D:
          if (is_shift_down) {
            frame_navigation_delta_++;
          } else {
            frame_buffer_offset_delta_--;
          }
          break;
        case SDLK_I:
          fast_input_alignment_ = !fast_input_alignment_;
          notify_user(string_sprintf("Input alignment resizing filter set to '%s' (takes effect for the next decoded frame)", fast_input_alignment_ ? "BILINEAR (fast)" : "BICUBIC (high-quality)"));
          break;
        case SDLK_T:
          bilinear_texture_filtering_ = !bilinear_texture_filtering_;
          notify_user(string_sprintf("Video texture filter set to '%s'", bilinear_texture_filtering_ ? "BILINEAR" : "NEAREST NEIGHBOR"));
          break;
        case SDLK_S: {
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
          break;
        }
        case SDLK_F:
          if (is_shift_down) {
            if (!save_selected_area_) {
              reset_crop_mode();
              save_selected_area_ = true;
            } else {
              save_selected_area_ = false;
              selection_state_ = SelectionState::None;
            }
            update_cursor();
          } else {
            save_image_frames_ = true;
          }
          break;
        case SDLK_P:
          print_mouse_position_and_color_ = mouse_is_inside_window_;
          break;
        case SDLK_TAB:
          if (is_shift_down) {
            active_right_index_ = (active_right_index_ + num_right_videos_ - 1) % num_right_videos_;
          } else {
            active_right_index_ = (active_right_index_ + 1) % num_right_videos_;
          }
          notify_user(string_sprintf("Active right video: %d/%d", active_right_index_ + 1, num_right_videos_));
          break;
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
          break;
        case SDLK_Q:
          show_quality_metrics_ = !show_quality_metrics_;
          break;
        case SDLK_GRAVE:
          auto_align_requested_ = true;
          break;
        case SDLK_4:
        case SDLK_KP_4:
          update_zoom_factor_and_move_offset(std::min(video_to_window_width_factor_ / drawable_to_window_width_factor_, video_to_window_height_factor_ / drawable_to_window_height_factor_));
          break;
        case SDLK_5:
        case SDLK_KP_5:
          update_zoom_factor_and_move_offset(0.5F);
          break;
        case SDLK_6:
        case SDLK_KP_6:
          update_zoom_factor_and_move_offset(1.0F);
          break;
        case SDLK_7:
        case SDLK_KP_7:
          update_zoom_factor_and_move_offset(2.0F);
          break;
        case SDLK_8:
        case SDLK_KP_8:
          update_zoom_factor_and_move_offset(4.0F);
          break;
        case SDLK_9:
        case SDLK_KP_9:
          update_zoom_factor_and_move_offset(8.0F);
          break;
        case SDLK_E: {
          SDL_GetMouseState(&mouse_x_, &mouse_y_);

          const auto zoom_rect = compute_zoom_rect();
          const Vector2D mouse_video = window_to_video_position(mouse_x_, mouse_y_, zoom_rect);
          const Vector2D center_video = window_to_video_position(content_window_.x + content_window_.w / 2, content_window_.y + content_window_.h / 2, zoom_rect);

          update_move_offset(move_offset_ + (center_video - mouse_video) * global_zoom_factor_);
          break;
        }
        case SDLK_R:
          if (is_shift_down) {
            toggle_crop_mode_for_side(CropTargetSide::Right);
          } else {
            move_offset_ = Vector2D(0.0F, 0.0F);
            global_center_ = Vector2D(0.5F, 0.5F);
            update_zoom_factor(1.0F);
          }
          break;
        case SDLK_LEFT:
          seek_relative_ -= 1.0F * relative_seek_scale;
          break;
        case SDLK_DOWN:
          seek_relative_ -= 10.0F * relative_seek_scale;
          break;
        case SDLK_PAGEDOWN:
          seek_relative_ -= 600.0F * relative_seek_scale;
          break;
        case SDLK_RIGHT:
          seek_relative_ += 1.0F * relative_seek_scale;
          break;
        case SDLK_UP:
          seek_relative_ += 10.0F * relative_seek_scale;
          break;
        case SDLK_PAGEUP:
          seek_relative_ += 600.0F * relative_seek_scale;
          break;
        case SDLK_J:
          update_playback_speed(-1.0F * playback_speed_scale);
          possibly_tick_playback_ = true;
          break;
        case SDLK_L:
          if (is_shift_down) {
            toggle_crop_mode_for_side(CropTargetSide::Left);
          } else {
            update_playback_speed(1.0F * playback_speed_scale);
            tick_playback_ = true;
          }
          break;
        case SDLK_B:
          if (is_shift_down) {
            toggle_crop_mode_for_side(CropTargetSide::Both);
          }
          break;
        case SDLK_X:
          if (is_shift_down) {
            notify_user(string_sprintf("Display state: window=%dx%d aspect=%s", window_width_, window_height_, aspect_view_mode_to_string(aspect_view_mode_).c_str()));
          } else {
            show_fps_ = true;
          }
          break;
        case SDLK_PLUS:
        case SDLK_KP_PLUS:
        case SDLK_EQUALS:  // for tenkeyless keyboards
          if (is_alt_down) {
            shift_right_frames_ += 100;
          } else if (is_ctrl_down) {
            shift_right_frames_ += 10;
          } else {
            shift_right_frames_++;
          }
          break;
        case SDLK_MINUS:
        case SDLK_KP_MINUS:
          if (is_alt_down) {
            shift_right_frames_ -= 100;
          } else if (is_ctrl_down) {
            shift_right_frames_ -= 10;
          } else {
            shift_right_frames_--;
          }
          break;
        case SDLK_Y: {
          // Cycle through subtraction modes
          const bool forward = !is_shift_down;

          switch (diff_mode_) {
            case DiffMode::LegacyAbs:
              diff_mode_ = forward ? DiffMode::AbsLinear : DiffMode::SignedDiverging;
              break;
            case DiffMode::AbsLinear:
              diff_mode_ = forward ? DiffMode::AbsSqrt : DiffMode::LegacyAbs;
              break;
            case DiffMode::AbsSqrt:
              diff_mode_ = forward ? DiffMode::SignedDiverging : DiffMode::AbsLinear;
              break;
            case DiffMode::SignedDiverging:
              diff_mode_ = forward ? DiffMode::LegacyAbs : DiffMode::AbsSqrt;
              break;
          }

          std::string diff_mode_name;
          switch (diff_mode_) {
            case DiffMode::LegacyAbs:
              diff_mode_name = "ABSOLUTE LINEAR (FIXED GAIN)";
              break;
            case DiffMode::AbsLinear:
              diff_mode_name = "ABSOLUTE LINEAR (ADAPTIVE)";
              break;
            case DiffMode::AbsSqrt:
              diff_mode_name = "ABSOLUTE SQUARE ROOT";
              break;
            case DiffMode::SignedDiverging:
              diff_mode_name = "SIGNED DIVERGING";
              break;
          }
          notify_user(string_sprintf("Subtraction mode set to '%s'", diff_mode_name.c_str()));
          break;
        }
        case SDLK_U:
          diff_luma_only_ = !diff_luma_only_;
          notify_user(string_sprintf("Subtraction luminance-only set to '%s'", diff_luma_only_ ? "ON" : "OFF"));
          break;
        default:
          break;
      }
      break;
    }
    case SDL_EVENT_KEY_UP:
      switch (event_.key.key) {
        case SDLK_Z:
          zoom_left_ = false;
          break;
        case SDLK_C:
          zoom_right_ = false;
          break;
        case SDLK_X:
          show_fps_ = false;
          break;
      }
      break;
    case SDL_EVENT_QUIT:
      quit_ = true;
      break;
    default:
      break;
  }
}

bool Display::get_hdr_display_available() const {
  return hdr_display_available_;
}

float Display::get_hdr_display_headroom() const {
  return hdr_display_headroom_;
}

void Display::set_hdr_passthrough(bool enabled) {
  if (hdr_passthrough_ != enabled) {
    hdr_passthrough_ = enabled;

    recreate_video_textures_for_current_mode();

    // Reallocate diff buffer for new bit depth
    if (diff_buffer_ != nullptr) {
      delete[] diff_buffer_;
    }
    diff_buffer_ = new uint8_t[video_width_ * video_height_ * 3 * (requires_10_bpc() ? sizeof(uint16_t) : sizeof(uint8_t))];
    diff_planes_ = {diff_buffer_, nullptr, nullptr};
    diff_pitches_ = {video_width_ * 3 * (requires_10_bpc() ? sizeof(uint16_t) : sizeof(uint8_t)), 0, 0};

    // Force reallocation of packed-pixel buffers on next frame
    if (left_buffer_ != nullptr) {
      delete[] left_buffer_;
      left_buffer_ = nullptr;
    }
    if (right_buffer_ != nullptr) {
      delete[] right_buffer_;
      right_buffer_ = nullptr;
    }
    left_planes_ = {nullptr, nullptr, nullptr};
    right_planes_ = {nullptr, nullptr, nullptr};
  }
}

void Display::set_hdr_content_headroom(float headroom) {
  hdr_content_headroom_ = headroom;
}

bool Display::get_quit() const {
  return quit_;
}

bool Display::get_play() const {
  return play_;
}

Display::Loop Display::get_buffer_play_loop_mode() const {
  return buffer_play_loop_mode_;
}

void Display::set_buffer_play_loop_mode(const Display::Loop& mode) {
  buffer_play_loop_mode_ = mode;
  play_ = false;
  tick_playback_ = true;

  if (mode == Loop::ForwardOnly) {
    buffer_play_forward_ = true;
  }
}

bool Display::get_buffer_play_forward() const {
  return buffer_play_forward_;
}

void Display::toggle_buffer_play_direction() {
  buffer_play_forward_ = !buffer_play_forward_;
}

bool Display::get_fast_input_alignment() const {
  return fast_input_alignment_;
}

bool Display::get_swap_left_right() const {
  return swap_left_right_;
}

float Display::get_seek_relative() const {
  return seek_relative_;
}

bool Display::get_seek_from_start() const {
  return seek_from_start_;
}

int Display::get_frame_buffer_offset_delta() const {
  return frame_buffer_offset_delta_;
}

int Display::get_frame_navigation_delta() const {
  return frame_navigation_delta_;
}

int Display::get_shift_right_frames() const {
  return shift_right_frames_;
}

bool Display::get_auto_align_requested() const {
  return auto_align_requested_;
}

float Display::compute_frame_psnr(const AVFrame* left_frame, const AVFrame* right_frame) {
  if (left_frame == nullptr || right_frame == nullptr) {
    return -std::numeric_limits<float>::max();
  }
  if (left_frame->width != right_frame->width || left_frame->height != right_frame->height || left_frame->width <= 0 || left_frame->height <= 0) {
    return -std::numeric_limits<float>::max();
  }

  const int width = left_frame->width;
  const int height = left_frame->height;

  float* left_gray = rgb_to_grayscale(left_frame->data[0], left_frame->linesize[0], width, height);
  float* right_gray = rgb_to_grayscale(right_frame->data[0], right_frame->linesize[0], width, height);

  double mse = 0.0;
  const float* lp = left_gray;
  const float* rp = right_gray;
  for (int i = 0; i < width * height; i++) {
    const float diff = *(lp++) - *(rp++);
    mse += static_cast<double>(diff) * static_cast<double>(diff);
  }
  mse /= static_cast<double>(width) * static_cast<double>(height);

  delete[] left_gray;
  delete[] right_gray;

  if (mse == 0.0) {
    return std::numeric_limits<float>::max();
  }
  return -10.f * log10f(static_cast<float>(mse));
}

float Display::get_playback_speed_factor() const {
  return playback_speed_factor_;
}

bool Display::get_tick_playback() const {
  return tick_playback_;
}

bool Display::get_possibly_tick_playback() const {
  return possibly_tick_playback_;
}

bool Display::get_show_fps() const {
  return show_fps_;
}

bool Display::get_toggle_scope_window_requested(const ScopeWindow::Type type) const {
  return toggle_scope_window_requested_[ScopeWindow::index(type)];
}

PendingCropRequest Display::get_and_clear_pending_crop_request() {
  const PendingCropRequest request = pending_crop_request_;
  pending_crop_request_ = PendingCropRequest{};
  return request;
}

void Display::set_num_right_videos(const size_t num_right_videos) {
  num_right_videos_ = num_right_videos;
}

size_t Display::get_num_right_videos() const {
  return num_right_videos_;
}

size_t Display::get_active_right_index() const {
  return active_right_index_;
}

void Display::set_active_right_index(const size_t index) {
  active_right_index_ = std::min(index, num_right_videos_ > 0 ? num_right_videos_ - 1 : 0UL);
}
