#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>
#include "analysis/metrics/metrics_calculator.h"
extern "C" {
#include <libswscale/swscale.h>
}

namespace {

// RAII wrapper so tests don't leak SwsContext on assertion failure.
struct SwsCtxGuard {
  SwsContext* ctx{nullptr};
  ~SwsCtxGuard() { if (ctx) sws_freeContext(ctx); }
};

// Build an identity-ish SwsContext: GRAY8 in, GRAY8 out, same dimensions.
SwsCtxGuard make_gray_identity_ctx(int w, int h) {
  SwsCtxGuard g;
  g.ctx = sws_getContext(w, h, AV_PIX_FMT_GRAY8, w, h, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
  return g;
}

SwsCtxGuard make_gray_downscale_ctx(int src_w, int src_h, int dst_w, int dst_h) {
  SwsCtxGuard g;
  g.ctx = sws_getContext(src_w, src_h, AV_PIX_FMT_GRAY8, dst_w, dst_h, AV_PIX_FMT_GRAY8, SWS_BILINEAR, nullptr, nullptr, nullptr);
  return g;
}

// Fill a GRAY8 buffer with a horizontal gradient 0..255 then repeating.
std::vector<uint8_t> make_horizontal_gradient(int w, int h) {
  std::vector<uint8_t> buf(w * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      buf[y * w + x] = static_cast<uint8_t>((x * 255) / std::max(1, w - 1));
    }
  }
  return buf;
}

std::vector<uint8_t> make_flat(int w, int h, uint8_t value) {
  return std::vector<uint8_t>(w * h, value);
}

}  // namespace

TEST_CASE("compute_structural_fingerprint: size is kFingerprintSize^2") {
  constexpr int N = MetricsCalculator::kFingerprintSize;
  auto ctx = make_gray_identity_ctx(N, N);
  REQUIRE(ctx.ctx != nullptr);

  auto src = make_horizontal_gradient(N, N);
  const uint8_t* src_data[4] = {src.data(), nullptr, nullptr, nullptr};
  int src_linesize[4] = {N, 0, 0, 0};
  std::vector<float> fp;

  MetricsCalculator::compute_structural_fingerprint(src_data, src_linesize, N, ctx.ctx, fp);
  CHECK(fp.size() == static_cast<size_t>(N * N));
}

TEST_CASE("compute_structural_fingerprint: output has zero mean") {
  constexpr int N = MetricsCalculator::kFingerprintSize;
  auto ctx = make_gray_identity_ctx(N, N);
  auto src = make_horizontal_gradient(N, N);
  const uint8_t* src_data[4] = {src.data(), nullptr, nullptr, nullptr};
  int src_linesize[4] = {N, 0, 0, 0};
  std::vector<float> fp;

  MetricsCalculator::compute_structural_fingerprint(src_data, src_linesize, N, ctx.ctx, fp);

  const double sum = std::accumulate(fp.begin(), fp.end(), 0.0);
  const double mean = sum / fp.size();
  CHECK(std::abs(mean) < 1e-4);
}

TEST_CASE("compute_structural_fingerprint: output has unit variance") {
  constexpr int N = MetricsCalculator::kFingerprintSize;
  auto ctx = make_gray_identity_ctx(N, N);
  auto src = make_horizontal_gradient(N, N);
  const uint8_t* src_data[4] = {src.data(), nullptr, nullptr, nullptr};
  int src_linesize[4] = {N, 0, 0, 0};
  std::vector<float> fp;

  MetricsCalculator::compute_structural_fingerprint(src_data, src_linesize, N, ctx.ctx, fp);

  double var_sum = 0.0;
  for (float x : fp) var_sum += x * x;
  const double variance = var_sum / fp.size();
  CHECK(std::abs(variance - 1.0) < 1e-3);
}

TEST_CASE("compute_structural_fingerprint: flat source produces all-zeros") {
  constexpr int N = MetricsCalculator::kFingerprintSize;
  auto ctx = make_gray_identity_ctx(N, N);
  auto src = make_flat(N, N, /*value=*/128);
  const uint8_t* src_data[4] = {src.data(), nullptr, nullptr, nullptr};
  int src_linesize[4] = {N, 0, 0, 0};
  std::vector<float> fp;

  MetricsCalculator::compute_structural_fingerprint(src_data, src_linesize, N, ctx.ctx, fp);
  // Every element should be exactly 0 — flat source means stddev == 0 path fires.
  for (float x : fp) CHECK(x == 0.0f);
}

TEST_CASE("compute_structural_fingerprint: null context yields empty-but-sized zeros") {
  std::vector<float> fp;
  MetricsCalculator::compute_structural_fingerprint(nullptr, nullptr, 0, nullptr, fp);
  // Contract (from impl): out.assign(count, 0.0f) runs before the null guards,
  // so the vector is sized to N*N and zero-initialized.
  CHECK(fp.size() == static_cast<size_t>(MetricsCalculator::kFingerprintSize * MetricsCalculator::kFingerprintSize));
  for (float x : fp) CHECK(x == 0.0f);
}

TEST_CASE("compute_structural_fingerprint + correlation: identical sources correlate at 1.0") {
  constexpr int N = MetricsCalculator::kFingerprintSize;
  auto ctx = make_gray_identity_ctx(N, N);
  auto src = make_horizontal_gradient(N, N);
  const uint8_t* src_data[4] = {src.data(), nullptr, nullptr, nullptr};
  int src_linesize[4] = {N, 0, 0, 0};

  std::vector<float> fp_a;
  std::vector<float> fp_b;
  MetricsCalculator::compute_structural_fingerprint(src_data, src_linesize, N, ctx.ctx, fp_a);
  MetricsCalculator::compute_structural_fingerprint(src_data, src_linesize, N, ctx.ctx, fp_b);

  const float corr = MetricsCalculator::structural_correlation(fp_a, fp_b);
  CHECK(std::abs(corr - 1.0f) < 1e-4f);
}

TEST_CASE("compute_structural_fingerprint: downscale preserves gradient structure") {
  // 256x256 gradient downscaled to 64x64 should still correlate ~1.0 with
  // the 64x64 identity fingerprint. Guards against swscale/normalization
  // interacting badly across resolutions.
  constexpr int N = MetricsCalculator::kFingerprintSize;
  constexpr int SRC = 256;
  auto ctx_down = make_gray_downscale_ctx(SRC, SRC, N, N);
  auto ctx_id = make_gray_identity_ctx(N, N);

  auto src_hi = make_horizontal_gradient(SRC, SRC);
  auto src_lo = make_horizontal_gradient(N, N);
  const uint8_t* src_hi_data[4] = {src_hi.data(), nullptr, nullptr, nullptr};
  int src_hi_linesize[4] = {SRC, 0, 0, 0};
  const uint8_t* src_lo_data[4] = {src_lo.data(), nullptr, nullptr, nullptr};
  int src_lo_linesize[4] = {N, 0, 0, 0};

  std::vector<float> fp_hi;
  std::vector<float> fp_lo;
  MetricsCalculator::compute_structural_fingerprint(src_hi_data, src_hi_linesize, SRC, ctx_down.ctx, fp_hi);
  MetricsCalculator::compute_structural_fingerprint(src_lo_data, src_lo_linesize, N, ctx_id.ctx, fp_lo);

  const float corr = MetricsCalculator::structural_correlation(fp_hi, fp_lo);
  CHECK(corr > 0.99f);
}

TEST_CASE("compute_structural_fingerprint: inverted gradient correlates at -1.0") {
  constexpr int N = MetricsCalculator::kFingerprintSize;
  auto ctx = make_gray_identity_ctx(N, N);

  auto grad = make_horizontal_gradient(N, N);
  std::vector<uint8_t> inv(grad.size());
  for (size_t i = 0; i < grad.size(); ++i) inv[i] = 255 - grad[i];

  const uint8_t* grad_data[4] = {grad.data(), nullptr, nullptr, nullptr};
  const uint8_t* inv_data[4] = {inv.data(), nullptr, nullptr, nullptr};
  int linesize[4] = {N, 0, 0, 0};

  std::vector<float> fp_grad;
  std::vector<float> fp_inv;
  MetricsCalculator::compute_structural_fingerprint(grad_data, linesize, N, ctx.ctx, fp_grad);
  MetricsCalculator::compute_structural_fingerprint(inv_data, linesize, N, ctx.ctx, fp_inv);

  const float corr = MetricsCalculator::structural_correlation(fp_grad, fp_inv);
  CHECK(corr < -0.99f);
}
