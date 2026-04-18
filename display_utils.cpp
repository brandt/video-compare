#include "display_utils.h"
#include <libgen.h>
#include <cstring>
#include <iomanip>
#include <regex>
#include <sstream>
#include "string_utils.h"

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
