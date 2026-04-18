#pragma once
#include <SDL3/SDL.h>
#include "../display_types.h"

// Owns rectangle-selection and crop-mode state shared by the save-selected-
// area feature and the crop-mode UI. State transitions are driven by mouse
// and keyboard events; Display still composes renderer primitives (drawing
// rects, clipping via ViewTransform) by reading this state.
class SelectionManager {
 public:
  SelectionManager() = default;

  // --- State reads ---
  SelectionState state() const { return state_; }
  const Vector2D& start() const { return start_; }
  const Vector2D& end() const { return end_; }
  bool wrap() const { return wrap_; }

  bool save_selected_area_requested() const { return save_selected_area_; }
  bool crop_mode() const { return crop_mode_; }
  CropTargetSide crop_target_side() const { return crop_target_side_; }

  bool has_active_cursor_mode() const { return save_selected_area_ || crop_mode_; }

  // --- Selection lifecycle ---
  // Begin a selection at the given video-space point. Sets wrap_ and, when
  // wrapping, rewrites start_ back into left-frame coordinates.
  void begin_selection(const Vector2D& start_video_pos, DisplayMode mode, int video_width, int video_height);
  void set_end(const Vector2D& end_video_pos) { end_ = end_video_pos; }
  void complete_selection() { state_ = SelectionState::Completed; }
  void reset_selection();  // state_ = None + save_selected = false

  // Returns the selection rect in left-frame coordinates, clipped to the video.
  SDL_Rect get_left_selection_rect(int video_width, int video_height) const;

  // Wrap a video-space point back into the left frame (accounts for HStack /
  // VStack doubled layouts). Used both when starting a selection and when
  // updating the end point during drag.
  static Vector2D wrap_to_left_frame(const Vector2D& video_position, DisplayMode mode, int video_width, int video_height);

  // --- Save-selected-area request flag ---
  void request_save_selected_area() { save_selected_area_ = true; }
  void cancel_save_selected_area();  // clears flag + state_

  // --- Crop mode ---
  void start_crop_for_side(CropTargetSide side);
  void reset_crop_mode();
  void toggle_crop_for_side(CropTargetSide side);

  // --- Pending crop request (consumed by video pipeline) ---
  const PendingCropRequest& pending_crop_request() const { return pending_crop_request_; }
  PendingCropRequest get_and_clear_pending_crop_request();
  // Set the pending request fields in one shot (used when committing a crop).
  void set_pending_crop_request(const PendingCropRequest& req) { pending_crop_request_ = req; }
  // User-initiated "backspace" — signal the pipeline to drop any active crop.
  void request_clear_crop();

 private:
  SelectionState state_{SelectionState::None};
  Vector2D start_{0.0F, 0.0F};
  Vector2D end_{0.0F, 0.0F};
  bool wrap_{false};
  bool save_selected_area_{false};
  bool crop_mode_{false};
  CropTargetSide crop_target_side_{CropTargetSide::Undefined};
  PendingCropRequest pending_crop_request_;
};
