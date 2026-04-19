#pragma once
#include <SDL3/SDL.h>
#include <iostream>
#include <map>
#include <string>
#include "display_modes.h"
#include "string_utils.h"

namespace MetadataProperties {
constexpr const char* RESOLUTION = "Resolution";
constexpr const char* SAMPLE_ASPECT_RATIO = "Sample Aspect Ratio";
constexpr const char* DISPLAY_ASPECT_RATIO = "Display Aspect Ratio";
constexpr const char* DURATION = "Duration";
constexpr const char* FRAME_RATE = "Frame Rate";
constexpr const char* FIELD_ORDER = "Field Order";
constexpr const char* CODEC = "Codec";
constexpr const char* HARDWARE_ACCELERATION = "Hardware Acceleration";
constexpr const char* PIXEL_FORMAT = "Pixel Format";
constexpr const char* COLOR_SPACE = "Color Space";
constexpr const char* COLOR_PRIMARIES = "Color Primaries";
constexpr const char* TRANSFER_CURVE = "Transfer Curve";
constexpr const char* COLOR_RANGE = "Color Range";
constexpr const char* CONTAINER = "Container";
constexpr const char* FILE_SIZE = "File Size";
constexpr const char* BIT_RATE = "Bit Rate";
constexpr const char* FILTERS = "Filters";

constexpr const char* const ALL[] = {RESOLUTION,      SAMPLE_ASPECT_RATIO, DISPLAY_ASPECT_RATIO, DURATION,  FRAME_RATE, FIELD_ORDER, CODEC,  HARDWARE_ACCELERATION, PIXEL_FORMAT, COLOR_SPACE,
                                     COLOR_PRIMARIES, TRANSFER_CURVE,      COLOR_RANGE,          CONTAINER, FILE_SIZE,  BIT_RATE,    FILTERS};

constexpr size_t COUNT = sizeof(ALL) / sizeof(ALL[0]);

const size_t LONGEST = ConstexprStringUtils::longest_string_length<COUNT>(ALL);
}  // namespace MetadataProperties

struct VideoMetadata {
  std::map<std::string, std::string> properties;

  std::string get(const std::string& key, const std::string& default_value = "N/A") const {
    auto it = properties.find(key);
    return it != properties.end() ? it->second : default_value;
  }

  void set(const std::string& key, const std::string& value) { properties[key] = value; }
};

class Vector2D {
 public:
  Vector2D(float px, float py) : x_(px), y_(py) {}

  float x() const { return x_; }
  float y() const { return y_; }

  Vector2D operator+(const Vector2D& v) const { return Vector2D(x_ + v.x_, y_ + v.y_); }

  Vector2D operator-(const Vector2D& v) const { return Vector2D(x_ - v.x_, y_ - v.y_); }

  Vector2D operator*(const Vector2D& v) const { return Vector2D(x_ * v.x_, y_ * v.y_); }

  Vector2D operator/(const Vector2D& v) const { return Vector2D(x_ / v.x_, y_ / v.y_); }

  Vector2D operator+(const float scalar) const { return Vector2D(x_ + scalar, y_ + scalar); }

  Vector2D operator-(const float scalar) const { return Vector2D(x_ - scalar, y_ - scalar); }

  Vector2D operator*(const float scalar) const { return Vector2D(x_ * scalar, y_ * scalar); }

  Vector2D operator/(const float scalar) const { return Vector2D(x_ / scalar, y_ / scalar); }

  friend std::ostream& operator<<(std::ostream& os, const Vector2D& v) { return os << "(" << v.x_ << ", " << v.y_ << ")"; }

 private:
  float x_, y_;
};

struct PendingCropRequest {
  SDL_Rect rect{0, 0, 0, 0};
  bool valid{false};
  bool clear_requested{false};
  bool apply_left{false};
  bool apply_right{false};
  size_t right_target_index{0};
};

enum class DisplayDiffMode { LegacyAbs, AbsLinear, AbsSqrt, SignedDiverging };
enum class SelectionState { None, Started, Completed };
enum class CropTargetSide { Undefined, Left, Right, Both };
