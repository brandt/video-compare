#pragma once
#include <SDL3/SDL.h>
#include <functional>
#include <string>
extern "C" {
#include <libavutil/frame.h>
}

// Owns the "save image frames" request flag, per-save numbering, and the
// thread-based JXL save pipeline for both the full-frame + OSD save and
// the rectangular cutout save. Does not own the SDL / GPU OSD capture —
// Display supplies the composed OSD frame to save_frames_with_osd.
class ImageSaver {
 public:
  ImageSaver() = default;

  // "F key" request flag — set by event handler, consumed in possibly_refresh.
  bool save_frames_requested() const { return save_frames_requested_; }
  void request_save_frames() { save_frames_requested_ = true; }
  void clear_save_frames_request() { save_frames_requested_ = false; }

  // Save left / right / OSD frames as numbered JXLs (3-threaded, joins
  // before returning). Filenames use `{left_stem,right_stem}` with swap /
  // suffix rules matching the prior Display behavior.
  void save_frames_with_osd(const AVFrame* left, const AVFrame* right, const AVFrame* osd,
                             const std::string& left_stem, const std::string& right_stem);

  // Save a cutout of both sides plus a concatenated side-by-side file.
  // selection_rect is in left-frame coordinates.
  void save_selected_area(const AVFrame* left, const AVFrame* right, const SDL_Rect& selection_rect,
                           const std::string& left_stem, const std::string& right_stem);

  // Optional user notifier for success messages (wraps notify_user / toast).
  void set_notifier(std::function<void(const std::string&)> notify) { notify_ = std::move(notify); }

 private:
  bool save_frames_requested_{false};
  int saved_image_number_{1};
  int saved_selected_image_number_{1};
  std::function<void(const std::string&)> notify_;
};
