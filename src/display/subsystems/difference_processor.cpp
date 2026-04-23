#include "difference_processor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>
#include "../display_utils.h"
#include "core/concurrency/row_workers.h"
extern "C" {
#include <libavutil/frame.h>
}

namespace {

template <int Bpc>
inline void process_difference_scanline(const typename BitDepthTraits<Bpc>::P* plane_left,
                                        const typename BitDepthTraits<Bpc>::P* plane_right,
                                        typename BitDepthTraits<Bpc>::P* plane_difference,
                                        const int pixels,
                                        const DisplayDiffMode mode,
                                        const bool luma_only,
                                        const std::vector<uint32_t>& mag_u,
                                        const std::vector<uint32_t>& mag_s) {
  using T = BitDepthTraits<Bpc>;
  constexpr uint32_t MAX = T::MaxCode;
  constexpr uint32_t MID = MAX >> 1;

  auto load = [](typename T::P v) -> int { return (int)(v >> T::PackShift); };

  for (int i = 0; i < pixels; i++) {
    const int idx = i * 3;
    const int rl = load(plane_left[idx]), gl = load(plane_left[idx + 1]), bl = load(plane_left[idx + 2]);
    const int rr = load(plane_right[idx]), gr = load(plane_right[idx + 1]), br = load(plane_right[idx + 2]);

    if (mode == DisplayDiffMode::LegacyAbs) {
      // Original: per-channel abs * AMPLIFICATION, clamped to bit depth
      constexpr int AMPLIFICATION = 2;

      if (luma_only) {
        const int dl = luma709(rl, gl, bl) - luma709(rr, gr, br);
        const uint32_t Y = clamp_u32(std::abs(dl) * AMPLIFICATION, MAX);
        auto y_p = T::from10(Y);

        plane_difference[idx] = y_p;
        plane_difference[idx + 1] = y_p;
        plane_difference[idx + 2] = y_p;
      } else {
        const uint32_t R = clamp_u32(std::abs(rl - rr) * AMPLIFICATION, MAX);
        const uint32_t G = clamp_u32(std::abs(gl - gr) * AMPLIFICATION, MAX);
        const uint32_t B = clamp_u32(std::abs(bl - br) * AMPLIFICATION, MAX);

        plane_difference[idx + 0] = T::from10(R);
        plane_difference[idx + 1] = T::from10(G);
        plane_difference[idx + 2] = T::from10(B);
      }
      continue;
    }

    // Adaptive mapping with optional sign and luma-only
    if (luma_only) {
      const int dl = luma709(rl, gl, bl) - luma709(rr, gr, br);
      const uint32_t a = (uint32_t)std::min<int>(MAX, std::abs(dl));

      if (mode == DisplayDiffMode::SignedDiverging) {
        const uint32_t m = mag_s[a];
        const uint32_t Y = (dl >= 0) ? (MID + m) : (MID - m);
        auto y_p = T::from10(Y);
        plane_difference[idx + 0] = plane_difference[idx + 1] = plane_difference[idx + 2] = y_p;
      } else {
        const uint32_t Y = mag_u[a];
        auto y_p = T::from10(Y);
        plane_difference[idx + 0] = plane_difference[idx + 1] = plane_difference[idx + 2] = y_p;
      }
    } else {
      const int dr = rl - rr, dg = gl - gr, db = bl - br;

      if (mode == DisplayDiffMode::SignedDiverging) {
        const uint32_t ar = (uint32_t)std::min<int>(MAX, std::abs(dr));
        const uint32_t ag = (uint32_t)std::min<int>(MAX, std::abs(dg));
        const uint32_t ab = (uint32_t)std::min<int>(MAX, std::abs(db));

        plane_difference[idx + 0] = T::from10(dr >= 0 ? (MID + mag_s[ar]) : (MID - mag_s[ar]));
        plane_difference[idx + 1] = T::from10(dg >= 0 ? (MID + mag_s[ag]) : (MID - mag_s[ag]));
        plane_difference[idx + 2] = T::from10(db >= 0 ? (MID + mag_s[ab]) : (MID - mag_s[ab]));
      } else {
        const uint32_t ar = (uint32_t)std::min<int>(MAX, std::abs(dr));
        const uint32_t ag = (uint32_t)std::min<int>(MAX, std::abs(dg));
        const uint32_t ab = (uint32_t)std::min<int>(MAX, std::abs(db));

        plane_difference[idx + 0] = T::from10(mag_u[ar]);
        plane_difference[idx + 1] = T::from10(mag_u[ag]);
        plane_difference[idx + 2] = T::from10(mag_u[ab]);
      }
    }
  }
}

std::pair<std::vector<uint32_t>, std::vector<uint32_t>> make_diff_lut(uint32_t max_code, DisplayDiffMode mode, uint32_t scale_max) {
  std::vector<uint32_t> mag_u(max_code + 1);
  std::vector<uint32_t> mag_s(max_code + 1);

  if (mode != DisplayDiffMode::LegacyAbs) {
    if (scale_max == 0) {
      scale_max = 1;
    }

    const uint32_t MID = max_code >> 1;
    const uint32_t Q = 16;
    const uint32_t ONE_Q = 1u << Q;
    const uint64_t HALF = uint64_t(1) << (Q - 1);

    for (uint32_t a = 0; a <= max_code; a++) {
      // x_q = clamp(a/scale, 0..1) in Q16
      uint32_t x_q = (uint32_t)std::min<uint64_t>(ONE_Q, ((uint64_t)a << Q) / scale_max);

      // map_unit(x): Linear / Sqrt (SignedDiverging uses sqrt magnitude)
      uint32_t y_q;
      switch (mode) {
        case DisplayDiffMode::AbsLinear:
          y_q = x_q;
          break;
        case DisplayDiffMode::AbsSqrt:
        case DisplayDiffMode::SignedDiverging: {
          const double x = double(x_q) / double(ONE_Q);
          const double y = std::sqrt(x);
          y_q = (uint32_t)std::llround(y * double(ONE_Q));
          break;
        }
        default:  // LegacyAbs not expected here; fall back to linear
          y_q = x_q;
          break;
      }

      // Scale back to code domain with Q16 rounding
      mag_u[a] = (uint32_t)(((uint64_t)y_q * max_code + HALF) >> Q);  // [0..MAX]
      mag_s[a] = (uint32_t)(((uint64_t)y_q * MID + HALF) >> Q);       // [0..MID]
    }
  }

  return std::pair<std::vector<uint32_t>, std::vector<uint32_t>>(std::move(mag_u), std::move(mag_s));
}

}  // namespace

DifferenceProcessor::DifferenceProcessor(RowWorkers& row_workers, bool start_in_subtraction_mode)
    : row_workers_(row_workers), subtraction_mode_(start_in_subtraction_mode) {}

DifferenceProcessor::~DifferenceProcessor() {
  delete[] diff_buffer_;
  delete[] left_buffer_;
  delete[] right_buffer_;
  if (diff_upload_frame_ != nullptr) {
    // diff_upload_frame_ aliases diff_buffer_ which is freed above — clear
    // data[0] first so av_frame_free doesn't walk into memory it doesn't own.
    diff_upload_frame_->data[0] = nullptr;
    av_frame_free(&diff_upload_frame_);
  }
}

void DifferenceProcessor::resize(const int video_width, const int video_height, const bool is_10bpc) {
  video_width_ = video_width;
  video_height_ = video_height;
  is_10bpc_ = is_10bpc;

  delete[] diff_buffer_;
  const size_t pixel_size = is_10bpc ? sizeof(uint16_t) : sizeof(uint8_t);
  diff_buffer_ = new uint8_t[video_width * video_height * 3 * pixel_size];
  diff_planes_ = {diff_buffer_, nullptr, nullptr};
  diff_pitches_ = {static_cast<size_t>(video_width) * 3 * pixel_size, 0, 0};

  // Force reallocation of packed-pixel buffers on next frame (pitch depends
  // on runtime values we don't know here).
  delete[] left_buffer_;
  left_buffer_ = nullptr;
  delete[] right_buffer_;
  right_buffer_ = nullptr;
  left_planes_ = {nullptr, nullptr, nullptr};
  right_planes_ = {nullptr, nullptr, nullptr};
}

const std::array<uint32_t*, 3>& DifferenceProcessor::ensure_left_planes(const size_t pitch_bytes) {
  if (left_buffer_ == nullptr) {
    left_buffer_ = new uint32_t[pitch_bytes * video_height_ / 4];
    left_planes_ = {left_buffer_, nullptr, nullptr};
  }
  return left_planes_;
}

const std::array<uint32_t*, 3>& DifferenceProcessor::ensure_right_planes(const size_t pitch_bytes) {
  if (right_buffer_ == nullptr) {
    right_buffer_ = new uint32_t[pitch_bytes * video_height_ / 4];
    right_planes_ = {right_buffer_, nullptr, nullptr};
  }
  return right_planes_;
}

AVFrame* DifferenceProcessor::ensure_diff_upload_frame() {
  if (diff_upload_frame_ == nullptr) {
    diff_upload_frame_ = av_frame_alloc();
  }
  return diff_upload_frame_;
}

void DifferenceProcessor::convert_to_packed_10_bpc(std::array<uint8_t*, 3> in_planes, std::array<size_t, 3> in_pitches,
                                                    std::array<uint32_t*, 3> out_planes, std::array<size_t, 3> out_pitches,
                                                    const SDL_Rect& roi) {
  row_workers_.run_dynamic(
      roi.h,
      [=](const int start_row, const int end_row) {
        uint16_t* p_in = reinterpret_cast<uint16_t*>(in_planes[0] + roi.x * 6 + in_pitches[0] * (roi.y + start_row));
        uint32_t* p_out = out_planes[0] + roi.x + out_pitches[0] * (roi.y + start_row) / sizeof(uint32_t);

        for (int y = start_row; y < end_row; y++) {
          for (int in_x = 0, out_x = 0; out_x < roi.w; in_x += 3, out_x++) {
            const uint32_t r = p_in[in_x] >> 6;
            const uint32_t g = p_in[in_x + 1] >> 6;
            const uint32_t b = p_in[in_x + 2] >> 6;

            p_out[out_x] = (r << 20) | (g << 10) | (b);
          }

          p_in += in_pitches[0] / sizeof(uint16_t);
          p_out += out_pitches[0] / sizeof(uint32_t);
        }
      },
      suggest_block_rows_by_bytes(roi.w, roi.h, sizeof(uint16_t), 3));
}

template <int Bpc>
float DifferenceProcessor::calculate_frame_p99(const typename BitDepthTraits<Bpc>::P* plane_left,
                                                const typename BitDepthTraits<Bpc>::P* plane_right,
                                                const size_t pitch_left, const size_t pitch_right, const int width_right) const {
  using T = BitDepthTraits<Bpc>;
  static_assert(Bpc == 8 || Bpc == 10, "Bpc must be 8 or 10");
  constexpr int CHANNELS = 3;

  const size_t stride_l = pitch_left / sizeof(typename T::P);
  const size_t stride_r = pitch_right / sizeof(typename T::P);

  const int bins = static_cast<int>(T::MaxCode) + 1;
  const int num_threads = row_workers_.size();

  std::vector<std::vector<uint32_t>> thread_histograms(num_threads, std::vector<uint32_t>(bins, 0u));

  // Use RowWorkers to compute histograms for different row ranges
  auto histograms_ptr = std::make_shared<std::vector<std::vector<uint32_t>>>(std::move(thread_histograms));

  const bool luma_only = diff_luma_only_;
  row_workers_.run_dynamic_indexed(
      video_height_,
      [=](const int start_row, const int end_row, const int worker_index) {
        auto& hist = (*histograms_ptr)[worker_index];

        for (int y = start_row; y < end_row; y++) {
          const typename T::P* row_l = plane_left + y * stride_l;
          const typename T::P* row_r = plane_right + y * stride_r;

          for (int x = 0; x < width_right; x++) {
            const int idx = x * CHANNELS;

            const int rl = row_l[idx + 0] >> T::PackShift;
            const int gl = row_l[idx + 1] >> T::PackShift;
            const int bl = row_l[idx + 2] >> T::PackShift;

            const int rr = row_r[idx + 0] >> T::PackShift;
            const int gr = row_r[idx + 1] >> T::PackShift;
            const int br = row_r[idx + 2] >> T::PackShift;

            int d;
            if (luma_only) {
              const int yl = luma709(rl, gl, bl);
              const int yr = luma709(rr, gr, br);
              d = std::abs(yl - yr);
            } else {
              const int dr = std::abs(rl - rr);
              const int dg = std::abs(gl - gr);
              const int db = std::abs(bl - br);
              d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
            }

            const int bin = clamp_range(d, 0, bins - 1);
            hist[static_cast<size_t>(bin)]++;
          }
        }
      },
      suggest_block_rows_by_bytes(video_width_, video_height_, sizeof(typename BitDepthTraits<Bpc>::P), 3));

  // Merge histograms
  std::vector<uint32_t> hist(bins, 0u);
  for (const auto& thread_hist : *histograms_ptr) {
    for (size_t i = 0; i < bins; ++i) {
      hist[i] += thread_hist[i];
    }
  }

  // Sum of histogram counts
  uint64_t total = std::accumulate(hist.begin(), hist.end(), 0);

  if (total == 0) {
    return 1.f;
  }

  // Linear-interpolated 99th percentile
  const double target_f = 0.99 * (double)(total - 1);
  const uint64_t r0 = (uint64_t)std::floor(target_f);
  const uint64_t r1 = (uint64_t)std::ceil(target_f);
  const double frac = target_f - (double)r0;

  int v0 = bins - 1, v1 = bins - 1;
  uint64_t acc = 0;

  // Find the values at ranks r0 and r1
  for (int k = 0; k < bins; k++) {
    const uint64_t next = acc + hist[static_cast<size_t>(k)];
    if (acc <= r0 && r0 < next) {
      v0 = k;
    }
    if (acc <= r1 && r1 < next) {
      v1 = k;
      break;
    }
    acc = next;
  }

  const float p = (float)v0 + frac * (float)(v1 - v0);
  return p;
}

template <int Bpc>
void DifferenceProcessor::process_difference_planes(const typename BitDepthTraits<Bpc>::P* plane_left0,
                                                     const typename BitDepthTraits<Bpc>::P* plane_right0,
                                                     typename BitDepthTraits<Bpc>::P* plane_difference0,
                                                     const size_t pitch_left,
                                                     const size_t pitch_right,
                                                     const size_t pitch_difference,
                                                     const int width_right,
                                                     const float diff_max) const {
  using T = BitDepthTraits<Bpc>;
  constexpr uint32_t MAX = T::MaxCode;

  const float scale_max = (diff_mode_ == DiffMode::LegacyAbs) ? -1.f : clamp_range(diff_max, 4.f, (float)MAX);

  // Integerize/clip scale once
  const uint32_t scale_max_i = (uint32_t)std::max<double>(1.0, std::min<double>(double(MAX), std::round(std::fabs(scale_max))));

  // Build LUTs (only for adaptive mapping)
  auto luts = make_diff_lut(MAX, diff_mode_, scale_max_i);
  const std::vector<uint32_t> mag_u = std::move(luts.first);
  const std::vector<uint32_t> mag_s = std::move(luts.second);

  const DiffMode mode = diff_mode_;
  const bool luma_only = diff_luma_only_;
  row_workers_.run_dynamic(
      video_height_,
      [=](const int start_row, const int end_row) {
        auto plane_left = plane_left0 + start_row * (pitch_left / sizeof(typename T::P));
        auto plane_right = plane_right0 + start_row * (pitch_right / sizeof(typename T::P));
        auto plane_difference = plane_difference0 + start_row * (pitch_difference / sizeof(typename T::P));

        for (int y = start_row; y < end_row; y++) {
          process_difference_scanline<Bpc>(plane_left, plane_right, plane_difference, width_right, mode, luma_only, mag_u, mag_s);
          plane_left += pitch_left / sizeof(typename T::P);
          plane_right += pitch_right / sizeof(typename T::P);
          plane_difference += pitch_difference / sizeof(typename T::P);
        }
      },
      suggest_block_rows_by_bytes(video_width_, video_height_, sizeof(typename BitDepthTraits<Bpc>::P), 3));
}

void DifferenceProcessor::update_difference(std::array<uint8_t*, 3> planes_left, std::array<size_t, 3> pitches_left,
                                             std::array<uint8_t*, 3> planes_right, std::array<size_t, 3> pitches_right,
                                             const int split_x) {
  constexpr int CHANNELS = 3;

  const int width_right = (video_width_ - split_x);
  if (width_right <= 0) {
    return;
  }

  const bool update_frame_max = diff_mode_ != DiffMode::LegacyAbs;
  float frame_max = 1.f;

  // row starts after split_x pixels, i.e., split_x * 3 samples
  if (is_10bpc_) {
    auto plane_left0 = reinterpret_cast<uint16_t*>(planes_left[0]) + split_x * CHANNELS;
    auto plane_right0 = reinterpret_cast<uint16_t*>(planes_right[0]) + split_x * CHANNELS;
    auto plane_difference0 = reinterpret_cast<uint16_t*>(diff_planes_[0]) + split_x * CHANNELS;

    if (update_frame_max) {
      frame_max = calculate_frame_p99<10>(plane_left0, plane_right0, pitches_left[0], pitches_right[0], width_right);
    }

    process_difference_planes<10>(plane_left0, plane_right0, plane_difference0, pitches_left[0], pitches_right[0], diff_pitches_[0], width_right, frame_max);
  } else {
    auto plane_left0 = planes_left[0] + split_x * CHANNELS;
    auto plane_right0 = planes_right[0] + split_x * CHANNELS;
    auto plane_difference0 = diff_planes_[0] + split_x * CHANNELS;

    if (update_frame_max) {
      frame_max = calculate_frame_p99<8>(plane_left0, plane_right0, pitches_left[0], pitches_right[0], width_right);
    }

    process_difference_planes<8>(plane_left0, plane_right0, plane_difference0, pitches_left[0], pitches_right[0], diff_pitches_[0], width_right, frame_max);
  }
}
