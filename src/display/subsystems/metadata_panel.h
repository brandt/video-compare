#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <vector>
#include "../display_types.h"

// Metadata table shown on Ctrl+I. Holds the two VideoMetadata sources plus
// the pre-rendered textures (SDL path) or RGBA surfaces (GPU path).
// Display composes the backgrounds / positions on its renderer; this class
// owns the text resources and scroll state.
class MetadataPanel {
 public:
  MetadataPanel() = default;
  ~MetadataPanel();

  MetadataPanel(const MetadataPanel&) = delete;
  MetadataPanel& operator=(const MetadataPanel&) = delete;

  // Replace both metadata sources; marks dirty so ensure_current() rebuilds.
  void update(const VideoMetadata& left, const VideoMetadata& right);
  void mark_dirty() { dirty_ = true; }

  // Rebuild textures / surfaces if dirty or swap changed. `displayed_left_is_left`
  // and `displayed_right_is_right` drive which stored metadata appears on each
  // side (swap-aware).
  void ensure_current(bool swap,
                      bool displayed_left_is_left, bool displayed_right_is_right,
                      TTF_Font* small_font, TTF_Font* big_font,
                      SDL_Renderer* renderer, bool gpu_active,
                      int drawable_width);

  // SDL path: full-screen dim background + centered table.
  void render_sdl(SDL_Renderer* renderer, int drawable_width, int drawable_height, DisplayMode mode);

  int total_height() const { return total_height_; }
  int scroll_offset() const { return y_offset_; }
  void set_scroll_offset(int y) { y_offset_ = y; }
  void adjust_scroll_offset(int delta) { y_offset_ += delta; }
  void clamp_scroll(int drawable_height, bool gpu_active, int line_spacing);

  size_t item_count(bool gpu_active) const { return gpu_active ? surfaces_.size() : textures_.size(); }

  // GPU-path composition needs direct surface access.
  const std::vector<SDL_Surface*>& surfaces() const { return surfaces_; }

  const VideoMetadata& left() const { return left_metadata_; }
  const VideoMetadata& right() const { return right_metadata_; }

 private:
  void build(const VideoMetadata& left, const VideoMetadata& right,
             TTF_Font* small_font, TTF_Font* big_font,
             SDL_Renderer* renderer, bool gpu_active, int drawable_width);
  void destroy_resources();

  std::vector<SDL_Texture*> textures_;
  std::vector<SDL_Surface*> surfaces_;
  int total_height_{0};
  int y_offset_{0};
  VideoMetadata left_metadata_;
  VideoMetadata right_metadata_;
  bool dirty_{true};
  bool last_swap_{false};
};
