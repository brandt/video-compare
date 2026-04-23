#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace MetricsCalculator {

// Read a single RGB pixel at (x,y). is_10bpc selects between packed 8-bit
// (3 bytes/pixel) and packed 10-bit stored in 16-bit (6 bytes/pixel, low bits).
std::array<int, 3> get_rgb_pixel(uint8_t* rgb_plane, size_t pitch, int x, int y, bool is_10bpc);

// Convert an RGB sample to YUV via a 1x1 sws_scale round-trip.
std::array<int, 3> convert_rgb_to_yuv(const std::array<int, 3>& rgb, AVPixelFormat rgb_format, AVColorSpace color_space, AVColorRange color_range, bool is_10bpc);

// Format pixel tuple as "(r,g,b#hex)" with 8- or 10-bpc widths.
std::string format_pixel(const std::array<int, 3>& pixel, bool is_10bpc);

// Combined helper: read pixel + convert to YUV + format both.
std::string get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, size_t pitch, const AVFrame* frame, int x, int y, bool is_10bpc);

// RGB -> grayscale float plane. Caller owns the returned buffer (delete[]).
float* rgb_to_grayscale(const uint8_t* plane, size_t pitch, int width, int height, bool is_10bpc);

// SSIM over matched grayscale planes.
float compute_ssim_block(const float* left_plane, const float* right_plane, int width, int x_offset, int y_offset, int block_size);
std::string compute_ssim(const float* left_plane, const float* right_plane, int width, int height);

// PSNR over matched grayscale planes. Returns "inf" when identical.
std::string compute_psnr(const float* left_plane, const float* right_plane, int width, int height);

// Convenience: convert two RGB frames to grayscale and compute SSIM as float.
// Returns -max on invalid input; +max on identical frames.
float compute_frame_ssim(const AVFrame* left_frame, const AVFrame* right_frame, bool is_10bpc);

// ---- Structural fingerprints (used by auto-align) ----

// Side length of the square fingerprint; total element count is kFingerprintSize * kFingerprintSize.
constexpr int kFingerprintSize = 64;

// Downscale src to kFingerprintSize x kFingerprintSize GRAY8 via cached_ctx, then
// normalize to zero mean / unit stddev. src_data/src_linesize follow the AVFrame
// convention; cached_ctx must be configured for the actual src format and dims
// and emit AV_PIX_FMT_GRAY8 at kFingerprintSize x kFingerprintSize.
//
// Flat (zero-variance) sources yield an all-zeros output, which correlates to 0
// against anything — the natural "uninformative frame" behavior.
void compute_structural_fingerprint(const uint8_t* const src_data[4], const int src_linesize[4], int src_slice_height, SwsContext* cached_ctx, std::vector<float>& out);

// Pearson correlation of two normalized fingerprints. Range [-1, +1]; returns 0
// on size mismatch or empty inputs.
float structural_correlation(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace MetricsCalculator
