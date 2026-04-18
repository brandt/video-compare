#pragma once
#include <cstdint>

inline uint32_t clamp_u32(int v, uint32_t hi) {
  return (v < 0) ? 0u : (v > (int)hi ? hi : (uint32_t)v);
}

inline int clamp_int_to_byte_range(int value) {
  return (value < 0) ? 0 : (value > 255) ? 255 : value;
}

inline int clamp_int_to_10_bpc_range(int value) {
  return (value < 0) ? 0 : (value > 1023) ? 1023 : value;
}

inline uint8_t clamp_int_to_byte(int value) {
  return static_cast<uint8_t>(clamp_int_to_byte_range(value));
}

inline uint16_t clamp_int_to_10_bpc(int value) {
  return static_cast<uint16_t>(clamp_int_to_10_bpc_range(value));
}

template <int Bpc>
struct BitDepthTraits;

template <>
struct BitDepthTraits<8> {
  using P = uint8_t;
  static constexpr uint32_t MaxCode = 255u;
  static constexpr int PackShift = 0;          // stored as 8b
  static inline int to10(int v) { return v; }  // already 8-bit working domain
  static inline P from10(uint32_t v) { return (P)clamp_u32((int)v, MaxCode); }
};

template <>
struct BitDepthTraits<10> {
  using P = uint16_t;
  static constexpr uint32_t MaxCode = 1023u;
  static constexpr int PackShift = 6;          // stored as 16b with <<6
  static inline int to10(int v) { return v; }  // values in working domain are 10b
  static inline P from10(uint32_t v) { return (P)(clamp_u32((int)v, MaxCode) << PackShift); }
};
