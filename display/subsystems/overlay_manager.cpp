#include "overlay_manager.h"
#include <algorithm>
#include "controls.h"
#include "../display_utils.h"
#include "string_utils.h"

OverlayManager::~OverlayManager() {
  destroy_help_resources();
  if (message_texture_ != nullptr) {
    SDL_DestroyTexture(message_texture_);
  }
}

void OverlayManager::destroy_help_resources() {
  for (auto* t : help_textures_) SDL_DestroyTexture(t);
  help_textures_.clear();
  for (auto* s : help_surfaces_) SDL_DestroySurface(s);
  help_surfaces_.clear();
  help_total_height_ = 0;
}

void OverlayManager::rebuild_help(TTF_Font* small_font, TTF_Font* big_font,
                                   SDL_Renderer* renderer, const bool gpu_active, const int drawable_width) {
  destroy_help_resources();

  bool primary_color = true;

  auto add_help_texture = [&](TTF_Font* font, const std::string& text) {
    SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0,
        primary_color ? HELP_TEXT_PRIMARY_COLOR : HELP_TEXT_ALTERNATE_COLOR,
        drawable_width - HELP_TEXT_HORIZONTAL_MARGIN * 2);
    if (!surface) return;

    if (gpu_active) {
      SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(surface);
      if (!rgba) return;
      help_total_height_ += rgba->h;
      help_surfaces_.push_back(rgba);
    } else {
      SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_DestroySurface(surface);
      float tw, th; SDL_GetTextureSize(texture, &tw, &th);
      help_total_height_ += static_cast<int>(th);
      help_textures_.push_back(texture);
    }
  };

  add_help_texture(small_font, " ");

  const auto& sections = get_control_sections();
  for (size_t i = 0; i < sections.size(); ++i) {
    const auto& section = sections[i];

    primary_color = true;

    TTF_SetFontStyle(big_font, TTF_STYLE_BOLD | TTF_STYLE_UNDERLINE);
    add_help_texture(big_font, to_upper_case(section.title));
    TTF_SetFontStyle(big_font, TTF_STYLE_NORMAL);

    primary_color = true;

    for (size_t j = 0; j < section.entries.size(); ++j) {
      const auto& entry = section.entries[j];
      primary_color = !primary_color;

      if (entry.key.empty()) {
        add_help_texture(small_font, entry.description);
        add_help_texture(small_font, " ");
      } else {
        add_help_texture(small_font, string_sprintf(" %-16s %s", entry.key.c_str(), entry.description.c_str()));
      }
    }

    if (i + 1 < sections.size()) {
      add_help_texture(small_font, " ");
    }
  }
}

void OverlayManager::render_help_sdl(SDL_Renderer* renderer) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer, nullptr);

  int y = help_y_offset_;

  for (size_t i = 0; i < help_textures_.size(); i++) {
    float fw, fh;
    SDL_GetTextureSize(help_textures_[i], &fw, &fh);

    SDL_FRect screen_area = {static_cast<float>(HELP_TEXT_HORIZONTAL_MARGIN), static_cast<float>(y), fw, fh};
    SDL_RenderTexture(renderer, help_textures_[i], nullptr, &screen_area);

    y += static_cast<int>(fh) + HELP_TEXT_LINE_SPACING;
  }
}

void OverlayManager::clamp_help_scroll(const int drawable_height, const bool gpu_active, const int line_spacing) {
  const size_t count = help_item_count(gpu_active);
  const int min_offset = drawable_height - help_total_height_ - static_cast<int>(count) * line_spacing;
  help_y_offset_ = std::max(help_y_offset_, min_offset);
  help_y_offset_ = std::min(help_y_offset_, 0);
}

void OverlayManager::set_pending_message(const std::string& message) {
  pending_message_ = message;
}
