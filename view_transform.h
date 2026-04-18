#pragma once
#include <SDL3/SDL.h>
#include <functional>
#include "display_types.h"

// Read-only view of the window layout state that ViewTransform depends on.
// Implemented by a small adapter over Display for now; will be replaced by a
// WindowLayout instance in phase 9 without changing ViewTransform.
class ViewTransformLayout {
 public:
  virtual ~ViewTransformLayout() = default;
  virtual SDL_Rect content_window() const = 0;
  virtual float video_to_window_width_factor() const = 0;
  virtual float video_to_window_height_factor() const = 0;
  virtual int video_width() const = 0;
  virtual int video_height() const = 0;
  virtual DisplayMode mode() const = 0;
};

class ViewTransform {
 public:
  struct ZoomRect {
    Vector2D start;
    Vector2D end;
    Vector2D size;
    float zoom_factor;
  };

  explicit ViewTransform(const ViewTransformLayout& layout) : layout_(layout) {}

  // --- State reads ---
  float global_zoom_level() const { return global_zoom_level_; }
  float global_zoom_factor() const { return global_zoom_factor_; }
  const Vector2D& move_offset() const { return move_offset_; }
  const Vector2D& global_center() const { return global_center_; }

  bool zoom_left() const { return zoom_left_; }
  bool zoom_right() const { return zoom_right_; }
  void set_zoom_left(bool v) { zoom_left_ = v; }
  void set_zoom_right(bool v) { zoom_right_ = v; }

  // --- Core transform methods ---
  float compute_zoom_factor(float zoom_level) const;
  Vector2D compute_relative_move_offset(const Vector2D& zoom_point, float zoom_factor) const;
  void update_zoom_factor_and_move_offset(float zoom_factor);
  void update_zoom_factor(float zoom_factor);
  void update_move_offset(const Vector2D& move_offset);

  ZoomRect compute_zoom_rect() const;
  Vector2D window_to_video_position(int window_x, int window_y, const ZoomRect& zoom_rect, bool floor_result = true) const;
  SDL_FRect video_to_zoom_space(const SDL_Rect& video_rect, const ZoomRect& zoom_rect) const;

  // Reset pan (move_offset + global_center) to "centered" without changing zoom.
  void reset_pan();

  // Recompute move_offset from global_center_ using current layout's
  // video_width/height. Called after reinitialize_video_dimensions so the
  // view stays locked to the same relative point after a size change.
  void sync_move_offset_to_center();

  // Callback invoked whenever the transform is mutated via
  // update_zoom_factor/update_move_offset (used by Display to refresh
  // mouse-driven selection state).
  void set_on_change(std::function<void()> cb) { on_change_ = std::move(cb); }

 private:
  const ViewTransformLayout& layout_;
  std::function<void()> on_change_;

  float global_zoom_level_{0.0F};
  float global_zoom_factor_{1.0F};
  Vector2D move_offset_{0.0F, 0.0F};
  Vector2D global_center_{0.5F, 0.5F};
  bool zoom_left_{false};
  bool zoom_right_{false};
};
