#include "display_utils.h"
#include <libgen.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <regex>
#include <sstream>
#include "core/strings/string_utils.h"

SDL_DisplayID display_id_for_index(int index) {
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

std::string format_right_file_label(const std::string& left_file_name, const std::string& right_file_name, const size_t right_file_number) {
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
}

std::string format_position_difference(const float position1, const float position2) {
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

std::string to_hex(const uint32_t value, const int width) {
  std::stringstream sstream;
  sstream << std::setfill('0') << std::setw(width) << std::hex << value;

  return sstream.str();
}

std::string format_libav_version(unsigned version) {
  int major = (version >> 16) & 0xff;
  int minor = (version >> 8) & 0xff;
  int micro = version & 0xff;
  return string_sprintf("%2u.%2u.%3u", major, minor, micro);
}

SDL_Rect detect_black_border_crop(const AVFrame* rgb) {
  const int w = rgb ? rgb->width : 0;
  const int h = rgb ? rgb->height : 0;
  const SDL_Rect full = {0, 0, std::max(0, w), std::max(0, h)};
  if (!rgb || w <= 0 || h <= 0 || rgb->data[0] == nullptr) {
    return full;
  }

  // Threshold / pass-fraction picked for robustness: luma ≤ 16 in 8-bit scale
  // tolerates legal-range black (YUV 16–235 → RGB 0–255 bottoms out around
  // 0–16 in practice); 99% per row/col absorbs stray bright pixels from
  // compression noise. Per-format branches shift the threshold into the
  // container scale.
  constexpr int kLumaThreshold8bit = 16;
  constexpr float kPassFraction = 0.99F;
  constexpr float kMaxScanFraction = 0.40F;  // Don't consume more than 40% of each dim.

  // Supported formats:
  //   RGB24     — 3 B/px, 8-bit components
  //   RGB48LE   — 6 B/px, 10-bit payload in 16-bit LE containers (GPU rgb_cache and
  //               SDL path with --10-bpc but no HDR passthrough)
  //   X2RGB10LE — 4 B/px, packed 10:10:10 in a 32-bit LE word (SDL path with HDR
  //               passthrough — see determine_pixel_format in app/video_compare.cpp)
  int bytes_per_pixel = 0;
  int luma_threshold = 0;
  auto read_luma_rgb24 = [](const uint8_t* p) { return luma709(p[0], p[1], p[2]); };
  auto read_luma_rgb48le = [](const uint8_t* p) {
    const uint16_t* p16 = reinterpret_cast<const uint16_t*>(p);
    return luma709(p16[0], p16[1], p16[2]);
  };
  auto read_luma_x2rgb10le = [](const uint8_t* p) {
    uint32_t val = 0;
    std::memcpy(&val, p, sizeof(val));
    const int b = static_cast<int>(val & 0x3FF);
    const int g = static_cast<int>((val >> 10) & 0x3FF);
    const int r = static_cast<int>((val >> 20) & 0x3FF);
    return luma709(r, g, b);
  };

  using ReadLumaFn = int (*)(const uint8_t*);
  ReadLumaFn read_luma = nullptr;
  switch (rgb->format) {
    case AV_PIX_FMT_RGB24:
      bytes_per_pixel = 3;
      luma_threshold = kLumaThreshold8bit;
      read_luma = read_luma_rgb24;
      break;
    case AV_PIX_FMT_RGB48LE:
      bytes_per_pixel = 6;
      luma_threshold = kLumaThreshold8bit << 8;  // 16-bit container scale
      read_luma = read_luma_rgb48le;
      break;
    case AV_PIX_FMT_X2RGB10LE:
      bytes_per_pixel = 4;
      luma_threshold = kLumaThreshold8bit << 2;  // 10-bit component scale
      read_luma = read_luma_x2rgb10le;
      break;
    default:
      // Unsupported format — return "no borders" so callers fall back to a no-op
      // rather than producing a bogus crop.
      return full;
  }

  const int stride = rgb->linesize[0];
  const uint8_t* base = rgb->data[0];

  auto row_is_black = [&](int y) {
    const uint8_t* p = base + static_cast<ptrdiff_t>(y) * stride;
    int black_px = 0;
    for (int x = 0; x < w; ++x, p += bytes_per_pixel) {
      if (read_luma(p) <= luma_threshold) ++black_px;
    }
    return black_px >= static_cast<int>(std::ceil(kPassFraction * static_cast<float>(w)));
  };

  auto col_is_black = [&](int x, int y_start, int y_end) {
    const ptrdiff_t px_offset = static_cast<ptrdiff_t>(x) * bytes_per_pixel;
    const int span = y_end - y_start;
    int black_px = 0;
    for (int y = y_start; y < y_end; ++y) {
      const uint8_t* p = base + static_cast<ptrdiff_t>(y) * stride + px_offset;
      if (read_luma(p) <= luma_threshold) ++black_px;
    }
    return black_px >= static_cast<int>(std::ceil(kPassFraction * static_cast<float>(span)));
  };

  const int max_top = static_cast<int>(std::floor(static_cast<float>(h) * kMaxScanFraction));
  const int max_bottom = static_cast<int>(std::floor(static_cast<float>(h) * kMaxScanFraction));
  const int max_left = static_cast<int>(std::floor(static_cast<float>(w) * kMaxScanFraction));
  const int max_right = static_cast<int>(std::floor(static_cast<float>(w) * kMaxScanFraction));

  int top = 0;
  while (top < max_top && row_is_black(top)) ++top;
  int bottom = h;
  while (bottom > h - max_bottom && row_is_black(bottom - 1)) --bottom;
  if (bottom <= top) return full;

  int left = 0;
  while (left < max_left && col_is_black(left, top, bottom)) ++left;
  int right = w;
  while (right > w - max_right && col_is_black(right - 1, top, bottom)) --right;
  if (right <= left) return full;

  return SDL_Rect{left, top, right - left, bottom - top};
}
