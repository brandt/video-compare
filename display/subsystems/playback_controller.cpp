#include "playback_controller.h"
#include <cmath>
#include "../display_utils.h"

void PlaybackController::toggle_play() {
  play_ = !play_;
  buffer_play_loop_mode_ = Loop::Off;
  tick_playback_ = play_;
}

void PlaybackController::set_loop_mode(const Loop mode) {
  buffer_play_loop_mode_ = mode;
  play_ = false;
  tick_playback_ = true;

  if (mode == Loop::ForwardOnly) {
    buffer_play_forward_ = true;
  }
}

void PlaybackController::update_playback_speed(const float playback_speed_level_delta) {
  const float playback_speed_level = playback_speed_level_ + playback_speed_level_delta;

  // allow 128x change of playback speed
  if (std::abs(playback_speed_level) <= static_cast<float>(PLAYBACK_SPEED_KEY_PRESSES_TO_DOUBLE * 7)) {
    playback_speed_level_ = playback_speed_level;
    playback_speed_factor_ = std::pow(PLAYBACK_SPEED_STEP_SIZE, playback_speed_level);
  }
}

void PlaybackController::clear_transient_state() {
  seek_relative_ = 0.0F;
  seek_from_start_ = false;
  frame_buffer_offset_delta_ = 0;
  frame_navigation_delta_ = 0;
  shift_right_frames_ = 0;
  auto_align_requested_ = false;
  right_only_seek_ = false;
  tick_playback_ = false;
  possibly_tick_playback_ = false;
}
