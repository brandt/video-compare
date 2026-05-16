#include "overlay_manager.h"

#include <algorithm>
#include <limits>

#include "../display_utils.h"
#include "core/strings/string_utils.h"
#include "display/controls.h"

namespace {

// Layout knobs for the single-page, multi-column help overlay.
constexpr int kHelpOuterMargin = 24;     // gap from window edge to first box
constexpr int kHelpColumnGutter = 16;    // horizontal gap between columns
constexpr int kHelpBoxPadding = 10;      // inner padding inside each section box
constexpr int kHelpBoxSpacing = 10;      // vertical gap between sections in the same column
constexpr int kHelpLineSpacing = 2;      // extra px between text rows
constexpr int kHelpTitleGap = 4;         // extra px below the section title

// Format a key + description as one row. The key column is right-padded to a
// monospace alignment so descriptions line up. Keep the key column narrow —
// the help overlay needs to fit on a single page across all sections.
std::string format_entry_row(const std::string& key, const std::string& description) {
  if (key.empty()) {
    return description;
  }
  return string_sprintf("%-13s %s", key.c_str(), description.c_str());
}

// Choose a column count that lets each column hold ~30 monospace characters.
int choose_column_count(int drawable_width) {
  if (drawable_width >= 1500) return 4;
  if (drawable_width >= 900) return 3;
  if (drawable_width >= 600) return 2;
  return 1;
}

}  // namespace

OverlayManager::~OverlayManager() {
  destroy_help_resources();
  if (message_texture_ != nullptr) {
    SDL_DestroyTexture(message_texture_);
  }
}

void OverlayManager::destroy_help_resources() {
  for (auto& item : help_items_) {
    if (item.texture != nullptr) {
      SDL_DestroyTexture(item.texture);
    }
    if (item.surface != nullptr) {
      SDL_DestroySurface(item.surface);
    }
  }
  help_items_.clear();
  help_boxes_.clear();
}

void OverlayManager::rebuild_help(TTF_Font* small_font, TTF_Font* big_font,
                                   SDL_Renderer* renderer, const bool gpu_active,
                                   const int drawable_width, const int drawable_height) {
  destroy_help_resources();

  const int columns = choose_column_count(drawable_width);
  const int avail_width = drawable_width - 2 * kHelpOuterMargin - (columns - 1) * kHelpColumnGutter;
  const int column_w = std::max(120, avail_width / columns);
  const int text_wrap_w = column_w - 2 * kHelpBoxPadding;
  const int avail_h = drawable_height - 2 * kHelpOuterMargin;

  // Render one text fragment to either a texture (SDL path) or an RGBA
  // surface (GPU path). The caller-supplied wrap_w of 0 disables wrapping;
  // positive values invoke TTF's wrapping renderer.
  auto render_fragment = [&](TTF_Font* font, const std::string& text,
                             const SDL_Color& color, int wrap_w,
                             HelpItem& out_item) -> bool {
    SDL_Surface* surface = TTF_RenderText_Blended_Wrapped(font, text.c_str(), 0, color,
                                                         wrap_w);
    if (surface == nullptr) {
      return false;
    }
    out_item.w = surface->w;
    out_item.h = surface->h;
    if (gpu_active) {
      SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
      SDL_DestroySurface(surface);
      if (rgba == nullptr) {
        return false;
      }
      out_item.surface = rgba;
    } else {
      SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_DestroySurface(surface);
      if (texture == nullptr) {
        return false;
      }
      out_item.texture = texture;
    }
    return true;
  };

  // First pass: render every section's text fragments and compute its total
  // height. Each entry produces a single rendered fragment. The section's
  // box is sized to wrap them (plus padding + title).
  struct RenderedSection {
    HelpItem title;
    std::vector<HelpItem> entries;
    int height{0};  // total box height including padding
  };
  std::vector<RenderedSection> rendered;

  const auto& sections = get_control_sections();
  rendered.reserve(sections.size());

  for (const auto& section : sections) {
    RenderedSection rs;

    TTF_SetFontStyle(big_font, TTF_STYLE_BOLD);
    render_fragment(big_font, to_upper_case(section.title), HELP_TEXT_PRIMARY_COLOR,
                    text_wrap_w, rs.title);
    TTF_SetFontStyle(big_font, TTF_STYLE_NORMAL);

    bool alternate = false;
    for (const auto& entry : section.entries) {
      HelpItem item;
      const SDL_Color color = alternate ? HELP_TEXT_ALTERNATE_COLOR : HELP_TEXT_PRIMARY_COLOR;
      alternate = !alternate;

      const std::string row = format_entry_row(entry.key, entry.description);
      if (!render_fragment(small_font, row, color, text_wrap_w, item)) {
        continue;
      }
      rs.entries.push_back(item);
    }

    int h = kHelpBoxPadding;
    h += rs.title.h + kHelpTitleGap;
    for (const auto& e : rs.entries) {
      h += e.h + kHelpLineSpacing;
    }
    if (!rs.entries.empty()) {
      h -= kHelpLineSpacing;  // no trailing spacing after last row
    }
    h += kHelpBoxPadding;
    rs.height = h;

    rendered.push_back(std::move(rs));
  }

  // Greedy bin-pack: place each section into the shortest column whose
  // remaining space can still fit it; if none fits, fall back to the
  // shortest column (the section will overflow, but in practice the layout
  // has been sized to fit). Ties broken by column index for stable ordering.
  std::vector<int> column_x(columns, 0);
  std::vector<int> column_y(columns, kHelpOuterMargin);
  for (int c = 0; c < columns; ++c) {
    column_x[c] = kHelpOuterMargin + c * (column_w + kHelpColumnGutter);
  }

  for (const auto& rs : rendered) {
    int chosen = 0;
    bool found_fit = false;
    int best_y = std::numeric_limits<int>::max();
    for (int c = 0; c < columns; ++c) {
      const int after = column_y[c] + rs.height;
      const bool fits = after <= kHelpOuterMargin + avail_h;
      if (fits && column_y[c] < best_y) {
        chosen = c;
        best_y = column_y[c];
        found_fit = true;
      }
    }
    if (!found_fit) {
      // Pick the shortest column to minimize overflow.
      chosen = 0;
      for (int c = 1; c < columns; ++c) {
        if (column_y[c] < column_y[chosen]) chosen = c;
      }
    }

    const int box_x = column_x[chosen];
    const int box_y = column_y[chosen];
    help_boxes_.push_back({box_x, box_y, column_w, rs.height});

    int y = box_y + kHelpBoxPadding;

    HelpItem title = rs.title;
    title.x = box_x + kHelpBoxPadding;
    title.y = y;
    y += title.h + kHelpTitleGap;
    help_items_.push_back(title);

    for (const auto& entry_template : rs.entries) {
      HelpItem entry = entry_template;
      entry.x = box_x + kHelpBoxPadding;
      entry.y = y;
      y += entry.h + kHelpLineSpacing;
      help_items_.push_back(entry);
    }

    column_y[chosen] = box_y + rs.height + kHelpBoxSpacing;
  }
}

void OverlayManager::render_help_sdl(SDL_Renderer* renderer) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, BACKGROUND_ALPHA * 3 / 2);
  SDL_RenderFillRect(renderer, nullptr);

  // Thin white outlines around each section.
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
  for (const auto& box : help_boxes_) {
    SDL_FRect r{static_cast<float>(box.x), static_cast<float>(box.y),
                static_cast<float>(box.w), static_cast<float>(box.h)};
    SDL_RenderRect(renderer, &r);
  }

  for (const auto& item : help_items_) {
    if (item.texture == nullptr) continue;
    SDL_FRect dst{static_cast<float>(item.x), static_cast<float>(item.y),
                  static_cast<float>(item.w), static_cast<float>(item.h)};
    SDL_RenderTexture(renderer, item.texture, nullptr, &dst);
  }
}

void OverlayManager::set_pending_message(const std::string& message) {
  pending_message_ = message;
}
