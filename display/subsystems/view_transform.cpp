#include "view_transform.h"
#include <cmath>
#include "../display_utils.h"

float ViewTransform::compute_zoom_factor(const float zoom_level) const {
  return std::pow(ZOOM_STEP_SIZE, zoom_level);
}

Vector2D ViewTransform::compute_relative_move_offset(const Vector2D& zoom_point, const float zoom_factor) const {
  const float zoom_factor_change = zoom_factor / global_zoom_factor_;

  const SDL_Rect content = layout_.content_window();
  const Vector2D view_center((static_cast<float>(content.x) + static_cast<float>(content.w) / 2.0F) * layout_.video_to_window_width_factor(),
                             (static_cast<float>(content.y) + static_cast<float>(content.h) / 2.0F) * layout_.video_to_window_height_factor());

  // the center point has to be moved relative to the zoom point
  const Vector2D new_move_offset = move_offset_ - (view_center + move_offset_ - zoom_point) * (1.0F - zoom_factor_change);

  return new_move_offset;
}

void ViewTransform::update_zoom_factor_and_move_offset(const float zoom_factor) {
  const DisplayMode mode = layout_.mode();
  const Vector2D zoom_point(static_cast<float>(layout_.video_width()) * (mode == DisplayMode::HStack ? 1.0F : 0.5F),
                            static_cast<float>(layout_.video_height()) * (mode == DisplayMode::VStack ? 1.0F : 0.5F));
  update_move_offset(compute_relative_move_offset(zoom_point, zoom_factor));
  update_zoom_factor(zoom_factor);
}

void ViewTransform::update_zoom_factor(const float zoom_factor) {
  global_zoom_factor_ = zoom_factor;
  global_zoom_level_ = std::log(zoom_factor) / std::log(ZOOM_STEP_SIZE);
  if (on_change_) on_change_();
}

void ViewTransform::update_move_offset(const Vector2D& move_offset) {
  move_offset_ = move_offset;
  global_center_ = Vector2D(move_offset_.x() / layout_.video_width() + 0.5F, move_offset_.y() / layout_.video_height() + 0.5F);
  if (on_change_) on_change_();
}

ViewTransform::ZoomRect ViewTransform::compute_zoom_rect() const {
  const Vector2D video_extent(layout_.video_width(), layout_.video_height());
  const Vector2D zoom_rect_start((global_center_ - global_zoom_factor_ * 0.5F) * video_extent);
  const Vector2D zoom_rect_end((global_center_ + global_zoom_factor_ * 0.5F) * video_extent);
  const Vector2D zoom_rect_size(zoom_rect_end - zoom_rect_start);
  return {zoom_rect_start, zoom_rect_end, zoom_rect_size, global_zoom_factor_};
}

Vector2D ViewTransform::window_to_video_position(const int window_x_position, const int window_y_position, const ZoomRect& zoom_rect, const bool floor_result) const {
  auto floor_or_ceil = [&](const float value) -> int { return floor_result ? std::floor(value) : std::ceil(value); };

  const SDL_Rect content = layout_.content_window();
  const float window_x_in_content = static_cast<float>(window_x_position - content.x);
  const float window_y_in_content = static_cast<float>(window_y_position - content.y);
  const int video_x = floor_or_ceil((window_x_in_content * layout_.video_to_window_width_factor() - zoom_rect.start.x()) * static_cast<float>(layout_.video_width()) / zoom_rect.size.x());
  const int video_y = floor_or_ceil((window_y_in_content * layout_.video_to_window_height_factor() - zoom_rect.start.y()) * static_cast<float>(layout_.video_height()) / zoom_rect.size.y());

  return Vector2D(video_x, video_y);
}

SDL_FRect ViewTransform::video_to_zoom_space(const SDL_Rect& video_rect, const ZoomRect& zoom_rect) const {
  // transform video coordinates to the currently zoomed area space
  return SDL_FRect({zoom_rect.start.x() + float(video_rect.x) * zoom_rect.zoom_factor,
                   zoom_rect.start.y() + float(video_rect.y) * zoom_rect.zoom_factor,
                   std::min(float(video_rect.w) * zoom_rect.zoom_factor, zoom_rect.size.x()),
                   std::min(float(video_rect.h) * zoom_rect.zoom_factor, zoom_rect.size.y())});
}

void ViewTransform::reset_pan() {
  move_offset_ = Vector2D(0.0F, 0.0F);
  global_center_ = Vector2D(0.5F, 0.5F);
}

void ViewTransform::sync_move_offset_to_center() {
  move_offset_ = Vector2D((global_center_.x() - 0.5F) * static_cast<float>(layout_.video_width()),
                          (global_center_.y() - 0.5F) * static_cast<float>(layout_.video_height()));
}
