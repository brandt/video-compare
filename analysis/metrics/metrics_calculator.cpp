#include "analysis/metrics/metrics_calculator.h"
#include <cmath>
#include <limits>
#include "display/display_utils.h"
#include "core/ffmpeg/ffmpeg.h"
#include "media/format_converter.h"
#include "core/strings/string_utils.h"
extern "C" {
#include <libavutil/imgutils.h>
}

namespace MetricsCalculator {

std::array<int, 3> get_rgb_pixel(uint8_t* rgb_plane, const size_t pitch, const int x, const int y, const bool is_10bpc) {
  int r, g, b;

  if (is_10bpc) {
    uint16_t* rgb_pixel = reinterpret_cast<uint16_t*>(rgb_plane + x * 6 + y * pitch);

    r = *(rgb_pixel) >> 6;
    g = *(rgb_pixel + 1) >> 6;
    b = *(rgb_pixel + 2) >> 6;

  } else {
    uint8_t* rgb_pixel = rgb_plane + x * 3 + y * pitch;

    r = *(rgb_pixel);
    g = *(rgb_pixel + 1);
    b = *(rgb_pixel + 2);
  }

  return {r, g, b};
}

std::array<int, 3> convert_rgb_to_yuv(const std::array<int, 3>& rgb, const AVPixelFormat rgb_format, const AVColorSpace color_space, const AVColorRange color_range, const bool is_10bpc) {
  auto allocate_frame = [&](const AVPixelFormat format) -> AVFramePtr {
    AVFrame* raw_frame = av_frame_alloc();

    if (raw_frame == nullptr) {
      throw ffmpeg::Error("Couldn't allocate frame");
    }

    raw_frame->format = format;
    raw_frame->width = 1;
    raw_frame->height = 1;
    raw_frame->colorspace = color_space;
    raw_frame->color_range = color_range;

    ffmpeg::check(av_image_alloc(raw_frame->data, raw_frame->linesize, raw_frame->width, raw_frame->height, format, 64));

    return AVFramePtr(raw_frame);
  };

  const AVPixelFormat yuv_format = is_10bpc ? AV_PIX_FMT_YUV444P10 : AV_PIX_FMT_YUV444P;

  auto rgb_pixel_frame = allocate_frame(rgb_format);
  auto yuv_pixel_frame = allocate_frame(yuv_format);

  if (is_10bpc) {
    uint16_t* rgb_data = reinterpret_cast<uint16_t*>(rgb_pixel_frame->data[0]);

    auto extend_10_to_16_bit = [](const int value) {
      return (value * 1025) >> 4;  // 1023->65535
    };

    rgb_data[0] = extend_10_to_16_bit(rgb[0]);
    rgb_data[1] = extend_10_to_16_bit(rgb[1]);
    rgb_data[2] = extend_10_to_16_bit(rgb[2]);
  } else {
    uint8_t* rgb_data = reinterpret_cast<uint8_t*>(rgb_pixel_frame->data[0]);

    rgb_data[0] = rgb[0];
    rgb_data[1] = rgb[1];
    rgb_data[2] = rgb[2];
  }

  FormatConverter rgb_to_yuv_converter(1, 1, 1, 1, rgb_format, yuv_format, color_space, color_range);
  rgb_to_yuv_converter(rgb_pixel_frame.get(), yuv_pixel_frame.get());

  if (is_10bpc) {
    auto y_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[0]);
    auto u_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[1]);
    auto v_data = reinterpret_cast<const uint16_t*>(yuv_pixel_frame->data[2]);

    return {y_data[0], u_data[0], v_data[0]};
  } else {
    return {yuv_pixel_frame->data[0][0], yuv_pixel_frame->data[1][0], yuv_pixel_frame->data[2][0]};
  }
}

std::string format_pixel(const std::array<int, 3>& pixel, const bool is_10bpc) {
  std::string hex_pixel = is_10bpc ? to_hex((pixel[0] << 20) | (pixel[1] << 10) | pixel[2], 8) : to_hex((pixel[0] << 16) | (pixel[1] << 8) | pixel[2], 6);

  return is_10bpc ? string_sprintf("(%4d,%4d,%4d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str()) : string_sprintf("(%3d,%3d,%3d#%s)", pixel[0], pixel[1], pixel[2], hex_pixel.c_str());
}

std::string get_and_format_rgb_yuv_pixel(uint8_t* rgb_plane, const size_t pitch, const AVFrame* frame, const int x, const int y, const bool is_10bpc) {
  auto rgb_format = static_cast<AVPixelFormat>(frame->format);

  const std::array<int, 3> rgb = get_rgb_pixel(rgb_plane, pitch, x, y, is_10bpc);
  const std::array<int, 3> yuv = convert_rgb_to_yuv(rgb, rgb_format, frame->colorspace, frame->color_range, is_10bpc);

  return "RGB" + format_pixel(rgb, is_10bpc) + ", YUV" + format_pixel(yuv, is_10bpc);
}

float* rgb_to_grayscale(const uint8_t* plane, const size_t pitch, const int width, const int height, const bool is_10bpc) {
  float* grayscale_image = new float[width * height];
  float* p_out = grayscale_image;

  auto to_grayscale = [](const float r, const float g, const float b, const float normalization_factor) -> float { return (r * 0.299f + g * 0.587f + b * 0.114f) * normalization_factor; };

  if (is_10bpc) {
    for (int y = 0; y < height; y++) {
      const uint16_t* row = reinterpret_cast<const uint16_t*>(plane + y * pitch);
      for (int x = 0; x < (width * 3); x += 3) {
        const float r = row[x] >> 6;
        const float g = row[x + 1] >> 6;
        const float b = row[x + 2] >> 6;
        *(p_out++) = to_grayscale(r, g, b, 1.f / 1023.f);
      }
    }
  } else {
    for (int y = 0; y < height; y++) {
      const uint8_t* row = plane + y * pitch;
      for (int x = 0; x < (width * 3); x += 3) {
        const float r = row[x];
        const float g = row[x + 1];
        const float b = row[x + 2];
        *(p_out++) = to_grayscale(r, g, b, 1.f / 255.f);
      }
    }
  }

  return grayscale_image;
}

float compute_ssim_block(const float* left_plane, const float* right_plane, const int width, const int x_offset, const int y_offset, const int block_size) {
  const int block_elements = block_size * block_size;

  auto compute_mean = [&](const float* plane) {
    float sum = 0;

    for (int y = y_offset; y < (y_offset + block_size); y++) {
      const float* row = plane + y * width + x_offset;

      for (int x = 0; x < block_size; x++) {
        sum += *(row++);
      }
    }

    return sum / block_elements;
  };

  float mean1 = compute_mean(left_plane);
  float mean2 = compute_mean(right_plane);

  // compute variance and convariance
  float sum_var1 = 0, sum_var2 = 0, sum_covar = 0;

  for (int y = y_offset; y < (y_offset + block_size); y++) {
    const float* row1 = left_plane + y * width + x_offset;
    const float* row2 = right_plane + y * width + x_offset;

    for (int x = 0; x < block_size; x++) {
      float diff1 = *(row1++) - mean1;
      float diff2 = *(row2++) - mean2;

      sum_var1 += diff1 * diff1;
      sum_var2 += diff2 * diff2;
      sum_covar += diff1 * diff2;
    }
  }

  float variance1 = sum_var1 / block_elements;
  float variance2 = sum_var2 / block_elements;
  float covariance = sum_covar / block_elements;

  float geomtric_mean_variance12 = sqrtf(variance1 * variance2);

  // compute SSIM metrics
  static constexpr float k1 = 0.01f;
  static constexpr float k2 = 0.03f;
  static constexpr float c1 = k1 * k1;
  static constexpr float c2 = k2 * k2;
  static constexpr float c3 = c2 / 2.f;

  float luminance = (2.f * mean1 * mean2 + c1) / (mean1 * mean1 + mean2 * mean2 + c1);
  float contrast = (2.f * geomtric_mean_variance12 + c2) / (variance1 + variance2 + c2);
  float structure = (covariance + c3) / (geomtric_mean_variance12 + c3);

  return luminance * contrast * structure;
}

std::string compute_ssim(const float* left_plane, const float* right_plane, const int width, const int height) {
  static constexpr int overlap = 4;
  static constexpr int block_size = 8;

  float ssim_sum = 0.0;
  int count = 0;

  for (int y = 0; y < height - (block_size - 1); y += block_size - overlap) {
    for (int x = 0; x < width - (block_size - 1); count++, x += block_size - overlap) {
      ssim_sum += compute_ssim_block(left_plane, right_plane, width, x, y, block_size);
    }
  }

  if (count == 0) {
    return "n/a";
  }

  const float ssim = ssim_sum / static_cast<float>(count);
  return string_sprintf("%.5f", ssim);
}

std::string compute_psnr(const float* left_plane, const float* right_plane, const int width, const int height) {
  // compute MSE
  double mse = 0.0;

  for (int i = 0; i < (width * height); i++) {
    const float diff = *(left_plane++) - *(right_plane++);
    mse += static_cast<double>(diff) * static_cast<double>(diff);
  }

  mse /= static_cast<double>(width) * static_cast<double>(height);

  if (mse == 0) {
    return "inf";
  }

  // compute PSNR
  return string_sprintf("%.3f", -10.f * log10f(static_cast<float>(mse)));
}

float compute_frame_psnr(const AVFrame* left_frame, const AVFrame* right_frame, const bool is_10bpc) {
  if (left_frame == nullptr || right_frame == nullptr) {
    return -std::numeric_limits<float>::max();
  }
  if (left_frame->width != right_frame->width || left_frame->height != right_frame->height || left_frame->width <= 0 || left_frame->height <= 0) {
    return -std::numeric_limits<float>::max();
  }

  const int width = left_frame->width;
  const int height = left_frame->height;

  float* left_gray = rgb_to_grayscale(left_frame->data[0], left_frame->linesize[0], width, height, is_10bpc);
  float* right_gray = rgb_to_grayscale(right_frame->data[0], right_frame->linesize[0], width, height, is_10bpc);

  double mse = 0.0;
  const float* lp = left_gray;
  const float* rp = right_gray;
  for (int i = 0; i < width * height; i++) {
    const float diff = *(lp++) - *(rp++);
    mse += static_cast<double>(diff) * static_cast<double>(diff);
  }
  mse /= static_cast<double>(width) * static_cast<double>(height);

  delete[] left_gray;
  delete[] right_gray;

  if (mse == 0.0) {
    return std::numeric_limits<float>::max();
  }
  return -10.f * log10f(static_cast<float>(mse));
}

}  // namespace MetricsCalculator
