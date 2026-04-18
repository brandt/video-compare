#pragma once
#include <SDL3/SDL.h>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
extern "C" {
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

inline const SDL_Color BACKGROUND_COLOR = {54, 69, 79, 0};
inline const SDL_Color LOOP_OFF_LABEL_COLOR = {0, 0, 0, 0};
inline const SDL_Color LOOP_FW_LABEL_COLOR = {80, 127, 255, 0};
inline const SDL_Color LOOP_PP_LABEL_COLOR = {191, 95, 60, 0};
inline const SDL_Color TEXT_COLOR = {255, 255, 255, 0};
inline const SDL_Color HELP_TEXT_PRIMARY_COLOR = {255, 255, 255, 0};
inline const SDL_Color HELP_TEXT_ALTERNATE_COLOR = {255, 255, 192, 0};
inline const SDL_Color POSITION_COLOR = {255, 255, 192, 0};
inline const SDL_Color TARGET_COLOR = {200, 200, 140, 0};
inline const SDL_Color ZOOM_COLOR = {255, 165, 0, 0};
inline const SDL_Color PLAYBACK_SPEED_COLOR = {0, 192, 160, 0};
inline const SDL_Color BUFFER_COLOR = {160, 225, 192, 0};
inline const SDL_Color FPS_VIDEO_COLOR = {255, 255, 192, 0};
inline const SDL_Color FPS_UI_COLOR = {255, 120, 200, 0};
inline constexpr int BACKGROUND_ALPHA = 100;

inline constexpr int MOUSE_WHEEL_SCROLL_STEPS_TO_DOUBLE = 12;
inline const float ZOOM_STEP_SIZE = std::pow(2.0F, 1.0F / float(MOUSE_WHEEL_SCROLL_STEPS_TO_DOUBLE));
inline constexpr float ZOOM_SLOWDOWN_RATIO = 3.0F;

inline constexpr int PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE = 6;
inline const float PLAYBACK_SPEED_STEP_SIZE = std::pow(2.0F, 1.0F / float(PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE));
inline constexpr float PLAYBACK_SPEED_SLOWDOWN_RATIO = 5.0F;

inline constexpr float RELATIVE_SEEK_SLOWDOWN_RATIO = 4.0F;

inline constexpr int HELP_TEXT_LINE_SPACING = 1;
inline constexpr int HELP_TEXT_HORIZONTAL_MARGIN = 26;

inline constexpr int MIN_WINDOW_WIDTH = 4;
inline constexpr int MIN_WINDOW_HEIGHT = 1;

struct FrameDeleter {
  void operator()(AVFrame* frame) const {
    av_freep(&frame->data[0]);
    av_frame_free(&frame);
  }
};
using AVFramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

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

inline int luma709(int r, int g, int b) {
  return (217 * r + 733 * g + 74 * b) >> 10;
}

inline SDL_FRect to_frect(const SDL_Rect& r) {
  return {static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w), static_cast<float>(r.h)};
}

inline SDL_FRect make_frect(int x, int y, int w, int h) {
  return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
}

inline float round_3(float value) {
  return std::round(value * 1000.0F) / 1000.0F;
}

inline int get_metadata_int_value(const AVFrame* frame, const std::string& key, const int default_value) {
  const AVDictionaryEntry* entry = av_dict_get(frame->metadata, key.c_str(), nullptr, 0);
  return entry ? std::atoi(entry->value) : default_value;
}

SDL_DisplayID display_id_for_index(int index);

std::string get_file_name_and_extension(const std::string& file_path);
std::string get_file_stem(const std::string& file_path);
std::string format_right_file_label(const std::string& left_file_name, const std::string& right_file_name, const size_t right_file_number);
std::string format_window_title(const std::string& left_file_name, const std::string& right_file_name);
std::string strip_ffmpeg_patterns(const std::string& input);
std::string format_position_difference(const float position1, const float position2);
std::string to_hex(const uint32_t value, const int width);
std::string format_libav_version(unsigned version);
