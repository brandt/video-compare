#pragma once
#include "../display_types.h"

// Per-session playback state: play/pause, loop mode, seek requests, frame
// navigation offsets, playback speed. Most fields are transient — set by
// event handlers and consumed each frame by the video pipeline.
class PlaybackController {
 public:
  using Loop = DisplayLoop;

  PlaybackController() = default;

  // --- Play / pause / loop / direction ---
  bool play() const { return play_; }
  void set_play(bool on) { play_ = on; }
  // Flip play_, clear loop mode, and mirror tick_playback_ to the new play_.
  void toggle_play();

  Loop loop_mode() const { return buffer_play_loop_mode_; }
  // Set loop mode and side-effects: pause, arm tick_playback_, snap forward
  // when mode is ForwardOnly.
  void set_loop_mode(Loop mode);

  bool forward() const { return buffer_play_forward_; }
  void toggle_direction() { buffer_play_forward_ = !buffer_play_forward_; }

  bool tick_playback() const { return tick_playback_; }
  void set_tick_playback(bool v) { tick_playback_ = v; }

  bool possibly_tick_playback() const { return possibly_tick_playback_; }
  void set_possibly_tick_playback(bool v) { possibly_tick_playback_ = v; }

  // --- Seek / frame navigation (transient per-event) ---
  float seek_relative() const { return seek_relative_; }
  void set_seek_relative(float v) { seek_relative_ = v; }
  void add_seek_relative(float delta) { seek_relative_ += delta; }

  bool seek_from_start() const { return seek_from_start_; }
  void set_seek_from_start(bool v) { seek_from_start_ = v; }

  int frame_buffer_offset_delta() const { return frame_buffer_offset_delta_; }
  void adjust_frame_buffer_offset_delta(int delta) { frame_buffer_offset_delta_ += delta; }

  int frame_navigation_delta() const { return frame_navigation_delta_; }
  void adjust_frame_navigation_delta(int delta) { frame_navigation_delta_ += delta; }

  int shift_right_frames() const { return shift_right_frames_; }
  void adjust_shift_right_frames(int delta) { shift_right_frames_ += delta; }

  bool auto_align_requested() const { return auto_align_requested_; }
  void request_auto_align() { auto_align_requested_ = true; }

  // --- Playback speed ---
  float playback_speed_factor() const { return playback_speed_factor_; }
  float playback_speed_level() const { return playback_speed_level_; }
  bool playback_speed_modified() const { return playback_speed_level_ != 0; }
  void update_playback_speed(float playback_speed_level_delta);

  // Reset per-event transient state at the start of each input frame. Does
  // NOT touch play_, loop mode, direction, or speed — those persist.
  void clear_transient_state();

 private:
  bool play_{true};
  Loop buffer_play_loop_mode_{Loop::Off};
  bool buffer_play_forward_{true};
  float seek_relative_{0.0F};
  int frame_buffer_offset_delta_{0};
  int frame_navigation_delta_{0};
  int shift_right_frames_{0};
  bool auto_align_requested_{false};
  bool seek_from_start_{false};
  float playback_speed_level_{0.0F};
  float playback_speed_factor_{1.0F};
  bool tick_playback_{false};
  bool possibly_tick_playback_{false};
};
