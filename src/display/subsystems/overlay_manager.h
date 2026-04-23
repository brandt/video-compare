#pragma once
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <chrono>
#include <string>
#include <vector>

// Owns the help-panel contents + the message-toast lifecycle shared by both
// the SDL and GPU renderer paths. Visibility flags for both the help and
// metadata panels live here too — they're cross-cutting toggles that belong
// with the overlay system.
class OverlayManager {
 public:
  OverlayManager() = default;
  ~OverlayManager();

  OverlayManager(const OverlayManager&) = delete;
  OverlayManager& operator=(const OverlayManager&) = delete;

  // --- Visibility flags ---
  bool show_help() const { return show_help_; }
  void toggle_help() { show_help_ = !show_help_; }

  bool show_metadata() const { return show_metadata_; }
  void toggle_metadata() { show_metadata_ = !show_metadata_; }

  // --- Help panel ---
  void rebuild_help(TTF_Font* small_font, TTF_Font* big_font,
                    SDL_Renderer* renderer, bool gpu_active, int drawable_width);
  void render_help_sdl(SDL_Renderer* renderer);

  int help_total_height() const { return help_total_height_; }
  int help_scroll_offset() const { return help_y_offset_; }
  void set_help_scroll_offset(int y) { help_y_offset_ = y; }
  void adjust_help_scroll_offset(int delta) { help_y_offset_ += delta; }
  void clamp_help_scroll(int drawable_height, bool gpu_active, int line_spacing);
  size_t help_item_count(bool gpu_active) const { return gpu_active ? help_surfaces_.size() : help_textures_.size(); }

  const std::vector<SDL_Surface*>& help_surfaces() const { return help_surfaces_; }

  // --- Message toast ---
  // Display's public API calls land here. Fullscreen path stores the message
  // so the GPU/SDL renderer can display it; non-fullscreen path routes the
  // string to stdout (caller's responsibility).
  void set_pending_message(const std::string& message);
  const std::string& pending_message() const { return pending_message_; }
  void clear_pending_message() { pending_message_.clear(); }
  std::chrono::milliseconds message_shown_at() const { return message_shown_at_; }
  void set_message_shown_at(std::chrono::milliseconds t) { message_shown_at_ = t; }

  // SDL-path message texture (rebuilt each refresh while pending_message_
  // is non-empty).
  SDL_Texture*& message_texture() { return message_texture_; }
  int& message_width() { return message_width_; }
  int& message_height() { return message_height_; }

  // GPU-path holds the message beyond pending_message_.clear() so the fade-
  // out can still render.
  const std::string& gpu_active_message() const { return gpu_active_message_; }
  void set_gpu_active_message(const std::string& s) { gpu_active_message_ = s; }
  void clear_gpu_active_message() { gpu_active_message_.clear(); }

 private:
  void destroy_help_resources();

  bool show_help_{false};
  bool show_metadata_{false};

  std::vector<SDL_Texture*> help_textures_;
  std::vector<SDL_Surface*> help_surfaces_;
  int help_total_height_{0};
  int help_y_offset_{0};

  std::string pending_message_;
  std::chrono::milliseconds message_shown_at_{0};
  SDL_Texture* message_texture_{nullptr};
  int message_width_{0};
  int message_height_{0};

  std::string gpu_active_message_;
};
