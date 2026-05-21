#pragma once

#include <algorithm>
#include <cmath>

namespace large::sdf {

struct Vec3 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 v, float s) {
  return {v.x * s, v.y * s, v.z * s};
}

inline Vec3 operator/(Vec3 v, float s) {
  return {v.x / s, v.y / s, v.z / s};
}

inline float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}

inline float length(Vec3 v) {
  return std::sqrt(dot(v, v));
}

inline Vec3 normalize(Vec3 v) {
  const float l = length(v);
  if (l <= 0.000001f) {
    return {0.0f, 1.0f, 0.0f};
  }
  return v / l;
}

inline float clamp(float v, float low, float high) {
  return std::max(low, std::min(high, v));
}

inline int clampInt(int v, int low, int high) {
  return std::max(low, std::min(high, v));
}

}  // namespace large::sdf
