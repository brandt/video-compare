#pragma once
#include <SDL3/SDL.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include "../display_types.h"
#include "../pixel_format_utils.h"

class RowWorkers;
struct AVFrame;

// Owns per-frame subtraction-mode state and the pixel-math workspaces used by
// both the GPU and SDL rendering paths:
//   - `diff_buffer_`  : output of subtraction (RGB24 or RGB48LE)
//   - `left/right_buffer_` : SDL-path packed-10bpc upload buffers (lazy)
//   - `diff_upload_frame_` : AVFrame shell used to hand diff_buffer_ to GPU
class DifferenceProcessor {
 public:
  using DiffMode = DisplayDiffMode;

  DifferenceProcessor(RowWorkers& row_workers, bool start_in_subtraction_mode);
  ~DifferenceProcessor();

  DifferenceProcessor(const DifferenceProcessor&) = delete;
  DifferenceProcessor& operator=(const DifferenceProcessor&) = delete;

  // (Re)allocate diff_buffer_ for the given video dimensions / bit-depth and
  // invalidate the SDL-path packing buffers (they re-allocate lazily on next
  // use). Safe to call repeatedly.
  void resize(int video_width, int video_height, bool is_10bpc);

  // Pack RGB48LE planes into ARGB2101010 output (SDL 10-bpc upload path).
  void convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches,
                                std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches,
                                const SDL_Rect& roi);

  // Compute left - right starting at x >= split_x into diff_buffer_. Uses
  // current diff_mode_ and diff_luma_only_ state.
  void update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left,
                         std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right,
                         int split_x);

  // Lazy-allocate the SDL-path per-side packed-10-bpc upload buffer. Pitch is
  // only known at frame time so buffers are created on first use. Returns the
  // planes array (with [0] populated).
  const std::array<uint32_t*, 3>& ensure_left_planes(size_t pitch_bytes);
  const std::array<uint32_t*, 3>& ensure_right_planes(size_t pitch_bytes);

  // Lazy-allocate the AVFrame shell aliasing diff_buffer_ for GPU upload. The
  // caller is responsible for setting format/width/height/colorspace on the
  // returned frame every call; DifferenceProcessor only owns lifetime.
  AVFrame* ensure_diff_upload_frame();

  uint8_t* diff_buffer() const { return diff_buffer_; }
  const std::array<uint8_t*, 3>& diff_planes() const { return diff_planes_; }
  const std::array<size_t, 3>& diff_pitches() const { return diff_pitches_; }

  bool subtraction_mode() const { return subtraction_mode_; }
  void set_subtraction_mode(bool on) { subtraction_mode_ = on; }
  void toggle_subtraction_mode() { subtraction_mode_ = !subtraction_mode_; }

  DiffMode diff_mode() const { return diff_mode_; }
  void set_diff_mode(DiffMode mode) { diff_mode_ = mode; }

  bool diff_luma_only() const { return diff_luma_only_; }
  void set_diff_luma_only(bool on) { diff_luma_only_ = on; }
  void toggle_diff_luma_only() { diff_luma_only_ = !diff_luma_only_; }

 private:
  template <int Bpc>
  float calculate_frame_p99(const typename BitDepthTraits<Bpc>::P* plane_left,
                            const typename BitDepthTraits<Bpc>::P* plane_right,
                            size_t pitch_left, size_t pitch_right, int width_right) const;

  template <int Bpc>
  void process_difference_planes(const typename BitDepthTraits<Bpc>::P* plane_left0,
                                 const typename BitDepthTraits<Bpc>::P* plane_right0,
                                 typename BitDepthTraits<Bpc>::P* plane_difference0,
                                 size_t pitch_left, size_t pitch_right, size_t pitch_difference,
                                 int width_right, float diff_max) const;

  RowWorkers& row_workers_;

  bool subtraction_mode_;
  DiffMode diff_mode_{DiffMode::AbsLinear};
  bool diff_luma_only_{false};

  int video_width_{0};
  int video_height_{0};
  bool is_10bpc_{false};

  uint8_t* diff_buffer_{nullptr};
  std::array<uint8_t*, 3> diff_planes_{};
  std::array<size_t, 3> diff_pitches_{};

  uint32_t* left_buffer_{nullptr};
  uint32_t* right_buffer_{nullptr};
  std::array<uint32_t*, 3> left_planes_{};
  std::array<uint32_t*, 3> right_planes_{};

  AVFrame* diff_upload_frame_{nullptr};
};
