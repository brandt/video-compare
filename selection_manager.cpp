#include "selection_manager.h"
#include <algorithm>
#include <cmath>

void SelectionManager::begin_selection(const Vector2D& start_video_pos, const DisplayMode mode, const int video_width, const int video_height) {
  state_ = SelectionState::Started;
  start_ = start_video_pos;

  // Check if the selection started outside the left video frame (stacked mode).
  wrap_ = (mode == DisplayMode::HStack && start_.x() >= video_width) ||
          (mode == DisplayMode::VStack && start_.y() >= video_height);

  if (wrap_) {
    start_ = wrap_to_left_frame(start_, mode, video_width, video_height);
  }

  end_ = start_;
}

void SelectionManager::reset_selection() {
  state_ = SelectionState::None;
  save_selected_area_ = false;
}

SDL_Rect SelectionManager::get_left_selection_rect(const int video_width, const int video_height) const {
  const int x = std::min(start_.x(), end_.x());
  const int y = std::min(start_.y(), end_.y());
  const int w = std::abs(end_.x() - start_.x());
  const int h = std::abs(end_.y() - start_.y());

  const int clipped_x = std::max(0, x);
  const int clipped_y = std::max(0, y);
  const int clipped_w = std::min(w - (clipped_x - x), video_width - clipped_x);
  const int clipped_h = std::min(h - (clipped_y - y), video_height - clipped_y);

  return {clipped_x, clipped_y, clipped_w, clipped_h};
}

Vector2D SelectionManager::wrap_to_left_frame(const Vector2D& video_position, const DisplayMode mode, const int video_width, const int video_height) {
  switch (mode) {
    case DisplayMode::HStack:
      return video_position - Vector2D(video_width, 0);
    case DisplayMode::VStack:
      return video_position - Vector2D(0, video_height);
    default:
      break;
  }
  return video_position;
}

void SelectionManager::cancel_save_selected_area() {
  save_selected_area_ = false;
  state_ = SelectionState::None;
}

void SelectionManager::start_crop_for_side(const CropTargetSide side) {
  save_selected_area_ = false;
  crop_target_side_ = side;
  crop_mode_ = true;
  state_ = SelectionState::None;
}

void SelectionManager::reset_crop_mode() {
  crop_mode_ = false;
  crop_target_side_ = CropTargetSide::Undefined;
  state_ = SelectionState::None;
}

void SelectionManager::toggle_crop_for_side(const CropTargetSide side) {
  if (crop_mode_) {
    reset_crop_mode();
  } else {
    start_crop_for_side(side);
  }
}

PendingCropRequest SelectionManager::get_and_clear_pending_crop_request() {
  const PendingCropRequest request = pending_crop_request_;
  pending_crop_request_ = PendingCropRequest{};
  return request;
}

void SelectionManager::request_clear_crop() {
  pending_crop_request_ = PendingCropRequest{};
  pending_crop_request_.clear_requested = true;
  reset_crop_mode();
}
