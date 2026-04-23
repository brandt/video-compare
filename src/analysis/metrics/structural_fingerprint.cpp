#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "analysis/metrics/metrics_calculator.h"
extern "C" {
#include <libswscale/swscale.h>
}

// Auto-align fingerprint primitives. Separated from metrics_calculator.cpp to
// keep this TU free of FormatConverter / display / filterer / string_utils
// dependencies — the unit tests for these primitives (tests/unit/
// structural_correlation_test.cpp) then link only against libswscale and
// libavutil, not the whole pipeline.

namespace MetricsCalculator {

void compute_structural_fingerprint(const uint8_t* const src_data[4], const int src_linesize[4], const int src_slice_height, SwsContext* cached_ctx, std::vector<float>& out) {
  constexpr int N = kFingerprintSize;
  constexpr int count = N * N;

  out.assign(count, 0.0f);
  if (cached_ctx == nullptr || src_data == nullptr || src_linesize == nullptr || src_slice_height <= 0) {
    return;
  }

  std::array<uint8_t, count> gray{};
  uint8_t* dst_data[4] = {gray.data(), nullptr, nullptr, nullptr};
  int dst_linesize[4] = {N, 0, 0, 0};

  const int ret = sws_scale(cached_ctx, src_data, src_linesize, 0, src_slice_height, dst_data, dst_linesize);
  if (ret <= 0) {
    return;
  }

  double sum = 0.0;
  for (int i = 0; i < count; ++i) {
    sum += gray[i];
  }
  const float mean = static_cast<float>(sum / static_cast<double>(count));

  double var_sum = 0.0;
  for (int i = 0; i < count; ++i) {
    const float d = static_cast<float>(gray[i]) - mean;
    var_sum += static_cast<double>(d) * d;
  }
  const float stddev = std::sqrt(static_cast<float>(var_sum / count));

  if (stddev <= 1e-6f) {
    return;  // flat image; leave zero-filled
  }

  const float inv_stddev = 1.0f / stddev;
  for (int i = 0; i < count; ++i) {
    out[i] = (static_cast<float>(gray[i]) - mean) * inv_stddev;
  }
}

float structural_correlation(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size() || a.empty()) {
    return 0.0f;
  }
  double sum = 0.0;
  const size_t n = a.size();
  for (size_t i = 0; i < n; ++i) {
    sum += static_cast<double>(a[i]) * static_cast<double>(b[i]);
  }
  return static_cast<float>(sum / static_cast<double>(n));
}

}  // namespace MetricsCalculator
