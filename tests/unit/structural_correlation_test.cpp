#include <doctest/doctest.h>
#include <cmath>
#include <vector>
#include "analysis/metrics/metrics_calculator.h"

namespace {

// Produce a zero-mean, unit-variance vector from a linear ramp [0..N-1].
// Mirrors what compute_structural_fingerprint would produce for a gradient.
std::vector<float> make_normalized_ramp(size_t n) {
  std::vector<float> v(n);
  const double mean = (n - 1) / 2.0;
  double var_sum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(i) - mean;
    var_sum += d * d;
  }
  const double stddev = std::sqrt(var_sum / static_cast<double>(n));
  for (size_t i = 0; i < n; ++i) {
    v[i] = static_cast<float>((static_cast<double>(i) - mean) / stddev);
  }
  return v;
}

}  // namespace

TEST_CASE("structural_correlation: identical normalized vectors correlate at 1.0") {
  const auto ramp = make_normalized_ramp(4096);
  const float corr = MetricsCalculator::structural_correlation(ramp, ramp);
  CHECK(std::abs(corr - 1.0f) < 1e-4f);
}

TEST_CASE("structural_correlation: negated vector correlates at -1.0") {
  const auto ramp = make_normalized_ramp(4096);
  std::vector<float> neg = ramp;
  for (auto& x : neg) x = -x;
  const float corr = MetricsCalculator::structural_correlation(ramp, neg);
  CHECK(std::abs(corr + 1.0f) < 1e-4f);
}

TEST_CASE("structural_correlation: symmetric in arguments") {
  const auto a = make_normalized_ramp(4096);
  std::vector<float> b = a;
  // Shuffle b a bit by reversing halves, which changes the dot product.
  for (size_t i = 0; i < b.size() / 2; ++i) {
    std::swap(b[i], b[b.size() - 1 - i]);
  }
  const float ab = MetricsCalculator::structural_correlation(a, b);
  const float ba = MetricsCalculator::structural_correlation(b, a);
  CHECK(std::abs(ab - ba) < 1e-6f);
}

TEST_CASE("structural_correlation: size mismatch returns 0") {
  const std::vector<float> a(4096, 1.0f);
  const std::vector<float> b(100, 1.0f);
  CHECK(MetricsCalculator::structural_correlation(a, b) == 0.0f);
}

TEST_CASE("structural_correlation: empty vectors return 0") {
  const std::vector<float> empty;
  CHECK(MetricsCalculator::structural_correlation(empty, empty) == 0.0f);
}

TEST_CASE("structural_correlation: orthogonal-ish vectors near 0") {
  // Sine and cosine at matching frequency are orthogonal over a full period.
  constexpr size_t N = 4096;
  std::vector<float> s(N);
  std::vector<float> c(N);
  double s_sum = 0.0;
  double c_sum = 0.0;
  for (size_t i = 0; i < N; ++i) {
    const double t = 2.0 * M_PI * static_cast<double>(i) / static_cast<double>(N);
    s[i] = static_cast<float>(std::sin(t));
    c[i] = static_cast<float>(std::cos(t));
    s_sum += s[i];
    c_sum += c[i];
  }
  // Zero-mean (already) and scale to unit-variance for fair correlation.
  // Both sin and cos have var = 1/2 over full period; scale by sqrt(2).
  const float scale = std::sqrt(2.0f);
  for (size_t i = 0; i < N; ++i) {
    s[i] *= scale;
    c[i] *= scale;
  }
  const float corr = MetricsCalculator::structural_correlation(s, c);
  CHECK(std::abs(corr) < 1e-3f);
}
