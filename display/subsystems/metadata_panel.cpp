#include "metadata_panel.h"
#include <algorithm>
#include <vector>
#include "../display_utils.h"
#include "string_utils.h"

MetadataPanel::~MetadataPanel() {
  destroy_resources();
}

void MetadataPanel::destroy_resources() {
  for (auto* texture : textures_) SDL_DestroyTexture(texture);
  textures_.clear();
  for (auto* s : surfaces_) SDL_DestroySurface(s);
  surfaces_.clear();
  total_height_ = 0;
}

void MetadataPanel::update(const VideoMetadata& left, const VideoMetadata& right) {
  left_metadata_ = left;
  right_metadata_ = right;
  dirty_ = true;
}

void MetadataPanel::ensure_current(const bool swap,
                                   const bool displayed_left_is_left, const bool displayed_right_is_right,
                                   TTF_Font* small_font, TTF_Font* big_font,
                                   SDL_Renderer* renderer, const bool gpu_active,
                                   const int drawable_width) {
  if (!dirty_ && swap == last_swap_) {
    return;
  }
  last_swap_ = swap;

  const VideoMetadata& left_meta = displayed_left_is_left ? left_metadata_ : right_metadata_;
  const VideoMetadata& right_meta = displayed_right_is_right ? right_metadata_ : left_metadata_;
  build(left_meta, right_meta, small_font, big_font, renderer, gpu_active, drawable_width);

  dirty_ = false;
}

void MetadataPanel::build(const VideoMetadata& left_metadata, const VideoMetadata& right_metadata,
                          TTF_Font* small_font, TTF_Font* big_font,
                          SDL_Renderer* renderer, const bool gpu_active, const int drawable_width) {
  constexpr char TOKENIZER = ',';

  destroy_resources();

  auto add_metadata_texture = [&](TTF_Font* font, const std::string& text, bool primary_color, bool is_header) {
    SDL_Color text_color = is_header ? HELP_TEXT_PRIMARY_COLOR : (primary_color ? HELP_TEXT_PRIMARY_COLOR : HELP_TEXT_ALTERNATE_COLOR);

    SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, text_color, drawable_width - HELP_TEXT_HORIZONTAL_MARGIN * 2);
    if (!surface) return;

    if (gpu_active) {
      SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(surface);
      if (!rgba) return;
      total_height_ += rgba->h + HELP_TEXT_LINE_SPACING;
      surfaces_.push_back(rgba);
    } else {
      SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_DestroySurface(surface);
      float tw, th; SDL_GetTextureSize(texture, &tw, &th);
      total_height_ += static_cast<int>(th) + HELP_TEXT_LINE_SPACING;
      textures_.push_back(texture);
    }
  };

  auto calculate_max_length = [](const VideoMetadata& metadata) -> size_t {
    size_t max_length = 0;
    for (const auto& kv : metadata.properties) {
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

  const int available_width = drawable_width - HELP_TEXT_HORIZONTAL_MARGIN * 2;

  constexpr int spacing = 2;

  int prop_cols = MetadataProperties::LONGEST + spacing;
  int left_cols = left_max_length + spacing;
  int right_cols = right_max_length + spacing;
  int total_cols = prop_cols + left_cols + right_cols;

  const std::string test_text = "FOR COMPUTING THE AVERAGE CHARACTER WIDTHS, WE NEED TO TEST THE WIDTH OF A STRING";

  int char_width_small = 10;
  int char_width_big = 14;

  int text_width, text_height;

  if (TTF_GetStringSize(small_font, test_text.c_str(), 0, &text_width, &text_height)) {
    char_width_small = text_width / test_text.length() + 1;
  }
  if (TTF_GetStringSize(big_font, test_text.c_str(), 0, &text_width, &text_height)) {
    char_width_big = text_width / test_text.length() + 1;
  }

  const int max_cols_per_line_big = available_width / char_width_big;
  const int max_cols_per_line_small = available_width / char_width_small;

  const int char_width = max_cols_per_line_big >= total_cols ? char_width_big : char_width_small;
  auto font = max_cols_per_line_big >= total_cols ? big_font : small_font;

  const int max_cols_per_line = available_width / char_width;

  if (total_cols > max_cols_per_line) {
    const int overshoot = total_cols - max_cols_per_line;

    const int prop_cols_overshoot = std::min(prop_cols, overshoot * prop_cols / total_cols * 2);
    const int left_cols_overshoot = std::max(0, overshoot - prop_cols_overshoot) * left_cols / (left_cols + right_cols);
    const int right_cols_overshoot = overshoot - prop_cols_overshoot - left_cols_overshoot;

    prop_cols -= prop_cols_overshoot;
    left_cols -= left_cols_overshoot;
    right_cols -= right_cols_overshoot;
  }

  TTF_SetFontStyle(font, TTF_STYLE_ITALIC | TTF_STYLE_UNDERLINE);
  add_metadata_texture(font, string_sprintf("%-*s%-*s%-*s", prop_cols, "", left_cols, "LEFT", right_cols, "RIGHT"), true, false);
  TTF_SetFontStyle(font, TTF_STYLE_NORMAL);

  bool primary_color = false;

  for (const auto& prop : properties) {
    std::string prop_value = to_upper_case(prop);

    std::string left_value = left_metadata.get(prop);
    std::string right_value = right_metadata.get(prop);

    std::vector<std::string> left_tokens = string_split(left_value, TOKENIZER);
    std::vector<std::string> right_tokens = string_split(right_value, TOKENIZER);

    size_t max_tokens = std::max(left_tokens.size(), right_tokens.size());

    for (size_t i = 0; i < max_tokens; i++) {
      std::string current_prop_value = (i == 0) ? prop_value : "";
      std::string current_left_value = (i < left_tokens.size()) ? left_tokens[i] : "";
      std::string current_right_value = (i < right_tokens.size()) ? right_tokens[i] : "";

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

void MetadataPanel::render_sdl(SDL_Renderer* renderer, const int drawable_width, const int drawable_height, const DisplayMode mode) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer, nullptr);

  const int table_width = drawable_width - HELP_TEXT_HORIZONTAL_MARGIN * 2;
  const int table_x = HELP_TEXT_HORIZONTAL_MARGIN;

  int y;
  if (mode == DisplayMode::VStack && total_height_ < drawable_height / 2) {
    y = (drawable_height / 2 - total_height_) / 2;
  } else if (mode != DisplayMode::VStack && total_height_ < drawable_height) {
    y = (drawable_height - total_height_) / 2;
  } else {
    y = y_offset_ + 10;
  }

  for (size_t i = 0; i < textures_.size(); i++) {
    float fw, fh;
    SDL_GetTextureSize(textures_[i], &fw, &fh);
    int w = static_cast<int>(fw), h = static_cast<int>(fh);

    int x_offset = (table_width - w) / 2;

    SDL_FRect screen_area = {static_cast<float>(table_x + x_offset), static_cast<float>(y), fw, fh};
    SDL_RenderTexture(renderer, textures_[i], nullptr, &screen_area);

    y += h + HELP_TEXT_LINE_SPACING;
  }
}

void MetadataPanel::clamp_scroll(const int drawable_height, const bool gpu_active, const int line_spacing) {
  const size_t count = item_count(gpu_active);
  const int min_offset = drawable_height - total_height_ - static_cast<int>(count) * line_spacing;
  y_offset_ = std::max(y_offset_, min_offset);
  y_offset_ = std::min(y_offset_, 0);
}
