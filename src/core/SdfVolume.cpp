#include "core/SdfVolume.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace large::sdf {

namespace {

float smoothstep(float edge0, float edge1, float x) {
  const float t = clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float sourceValue(const std::vector<float>& values, IVec3 size, float fallback, int x, int y, int z) {
  if (x < 0 || y < 0 || z < 0 || x >= size.x || y >= size.y || z >= size.z) {
    return fallback;
  }
  const std::size_t i = static_cast<std::size_t>(x) +
                        static_cast<std::size_t>(size.x) *
                            (static_cast<std::size_t>(y) + static_cast<std::size_t>(size.y) * static_cast<std::size_t>(z));
  return values[i];
}

Vec3 inverseRotate(Vec3 v, Vec3 rotationX, Vec3 rotationY, Vec3 rotationZ) {
  return {
      dot(v, rotationX),
      dot(v, rotationY),
      dot(v, rotationZ),
  };
}

}  // namespace

SdfVolume::SdfVolume(IVec3 size, float voxelSize, Vec3 origin, float initialDistance)
    : size_(size), voxelSize_(voxelSize), origin_(origin) {
  if (size.x <= 0 || size.y <= 0 || size.z <= 0) {
    throw std::invalid_argument("SdfVolume size must be positive");
  }
  if (voxelSize <= 0.0f) {
    throw std::invalid_argument("SdfVolume voxel size must be positive");
  }

  const auto count = static_cast<std::size_t>(size.x) *
                     static_cast<std::size_t>(size.y) *
                     static_cast<std::size_t>(size.z);
  values_.assign(count, initialDistance);
}

bool SdfVolume::contains(int x, int y, int z) const {
  return x >= 0 && y >= 0 && z >= 0 && x < size_.x && y < size_.y && z < size_.z;
}

std::size_t SdfVolume::index(int x, int y, int z) const {
  return static_cast<std::size_t>(x) +
         static_cast<std::size_t>(size_.x) *
             (static_cast<std::size_t>(y) + static_cast<std::size_t>(size_.y) * static_cast<std::size_t>(z));
}

float SdfVolume::value(int x, int y, int z) const {
  if (!contains(x, y, z)) {
    return voxelSize_ * 16.0f;
  }
  return values_[index(x, y, z)];
}

void SdfVolume::setValue(int x, int y, int z, float value) {
  if (!contains(x, y, z)) {
    return;
  }
  values_[index(x, y, z)] = value;
}

float SdfVolume::sample(Vec3 point) const {
  const float fx = (point.x - origin_.x) / voxelSize_ - 0.5f;
  const float fy = (point.y - origin_.y) / voxelSize_ - 0.5f;
  const float fz = (point.z - origin_.z) / voxelSize_ - 0.5f;

  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const int z0 = static_cast<int>(std::floor(fz));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const float tz = fz - static_cast<float>(z0);

  const float c000 = value(x0, y0, z0);
  const float c100 = value(x0 + 1, y0, z0);
  const float c010 = value(x0, y0 + 1, z0);
  const float c110 = value(x0 + 1, y0 + 1, z0);
  const float c001 = value(x0, y0, z0 + 1);
  const float c101 = value(x0 + 1, y0, z0 + 1);
  const float c011 = value(x0, y0 + 1, z0 + 1);
  const float c111 = value(x0 + 1, y0 + 1, z0 + 1);

  const float c00 = c000 + (c100 - c000) * tx;
  const float c10 = c010 + (c110 - c010) * tx;
  const float c01 = c001 + (c101 - c001) * tx;
  const float c11 = c011 + (c111 - c011) * tx;
  const float c0 = c00 + (c10 - c00) * ty;
  const float c1 = c01 + (c11 - c01) * ty;
  return c0 + (c1 - c0) * tz;
}

Vec3 SdfVolume::voxelCenter(int x, int y, int z) const {
  return {
      origin_.x + (static_cast<float>(x) + 0.5f) * voxelSize_,
      origin_.y + (static_cast<float>(y) + 0.5f) * voxelSize_,
      origin_.z + (static_cast<float>(z) + 0.5f) * voxelSize_,
  };
}

Vec3 SdfVolume::boundsCenter() const {
  return {
      origin_.x + static_cast<float>(size_.x) * voxelSize_ * 0.5f,
      origin_.y + static_cast<float>(size_.y) * voxelSize_ * 0.5f,
      origin_.z + static_cast<float>(size_.z) * voxelSize_ * 0.5f,
  };
}

float SdfVolume::worldExtent() const {
  return static_cast<float>(size_.x) * voxelSize_;
}

void SdfVolume::fillSphere(Vec3 center, float radius) {
  for (int z = 0; z < size_.z; ++z) {
    for (int y = 0; y < size_.y; ++y) {
      for (int x = 0; x < size_.x; ++x) {
        const float d = length(voxelCenter(x, y, z) - center) - radius;
        setValue(x, y, z, d);
      }
    }
  }
}

void SdfVolume::applySphereBrush(Vec3 center, float radius, BrushMode mode) {
  applySphereBrush(center, radius, mode, 1.0f);
}

void SdfVolume::applySphereBrush(Vec3 center, float radius, BrushMode mode, float strength) {
  if (radius <= 0.0f) {
    return;
  }

  const float amount = clamp(strength, 0.0f, 1.0f);
  if (amount <= 0.0f) {
    return;
  }

  // Keep a narrow signed-distance band around the brush surface.
  const float influence = radius + voxelSize_ * 3.0f;
  const int minX = clampInt(static_cast<int>(std::floor((center.x - influence - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((center.y - influence - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((center.z - influence - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((center.x + influence - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((center.y + influence - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((center.z + influence - origin_.z) / voxelSize_)), 0, size_.z - 1);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float dist = length(voxelCenter(x, y, z) - center);
        const float brush = dist - radius;
        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / influence);
        const std::size_t i = index(x, y, z);
        if (mode == BrushMode::Add) {
          const float target = std::min(values_[i], brush);
          values_[i] = values_[i] + (target - values_[i]) * amount * falloff;
        } else if (mode == BrushMode::Subtract) {
          const float target = std::max(values_[i], -brush);
          values_[i] = values_[i] + (target - values_[i]) * amount * falloff;
        }
      }
    }
  }
}

void SdfVolume::applySmoothBrush(Vec3 center, float radius, float strength) {
  if (radius <= 0.0f || strength <= 0.0f) {
    return;
  }

  const float amount = clamp(strength, 0.0f, 1.0f);
  const auto source = values_;

  const int minX = clampInt(static_cast<int>(std::floor((center.x - radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((center.y - radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((center.z - radius - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((center.x + radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((center.y + radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((center.z + radius - origin_.z) / voxelSize_)), 0, size_.z - 1);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float dist = length(voxelCenter(x, y, z) - center);
        if (dist > radius) {
          continue;
        }

        const std::size_t i = index(x, y, z);
        const float original = source[i];
        const float average =
            (sourceValue(source, size_, original, x - 1, y, z) +
             sourceValue(source, size_, original, x + 1, y, z) +
             sourceValue(source, size_, original, x, y - 1, z) +
             sourceValue(source, size_, original, x, y + 1, z) +
             sourceValue(source, size_, original, x, y, z - 1) +
             sourceValue(source, size_, original, x, y, z + 1)) /
            6.0f;
        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / radius);
        values_[i] = original + (average - original) * amount * falloff;
      }
    }
  }
}

void SdfVolume::applyStretchBrush(const SdfVolume& source, Vec3 anchor, Vec3 delta, float radius, float strength) {
  applyStretchBrush(source,
                    anchor,
                    delta,
                    {1.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                    radius,
                    strength);
}

void SdfVolume::applyStretchBrush(const SdfVolume& source,
                                  Vec3 anchor,
                                  Vec3 delta,
                                  Vec3 rotationX,
                                  Vec3 rotationY,
                                  Vec3 rotationZ,
                                  float radius,
                                  float strength) {
  if (radius <= 0.0f || strength <= 0.0f) {
    return;
  }
  if (source.size_.x != size_.x || source.size_.y != size_.y || source.size_.z != size_.z ||
      source.voxelSize_ != voxelSize_) {
    throw std::invalid_argument("SdfVolume stretch source must match destination volume");
  }

  values_ = source.values_;

  const float amount = clamp(strength, 0.0f, 1.0f);
  const Vec3 pull = delta * amount;
  const Vec3 handle = anchor + pull;
  const Vec3 minPoint{
      std::min(anchor.x, handle.x) - radius,
      std::min(anchor.y, handle.y) - radius,
      std::min(anchor.z, handle.z) - radius,
  };
  const Vec3 maxPoint{
      std::max(anchor.x, handle.x) + radius,
      std::max(anchor.y, handle.y) + radius,
      std::max(anchor.z, handle.z) + radius,
  };

  const int minX = clampInt(static_cast<int>(std::floor((minPoint.x - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((minPoint.y - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((minPoint.z - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((maxPoint.x - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((maxPoint.y - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((maxPoint.z - origin_.z) / voxelSize_)), 0, size_.z - 1);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const float dist = length(p - handle);
        if (dist > radius) {
          continue;
        }

        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / radius);
        const Vec3 translated = p - pull * falloff;
        const Vec3 relative = translated - anchor;
        const Vec3 rotatedBack = inverseRotate(relative, rotationX, rotationY, rotationZ);
        const Vec3 sourcePoint = anchor + relative + (rotatedBack - relative) * falloff;
        setValue(x, y, z, source.sample(sourcePoint));
      }
    }
  }
}

SdfVolume SdfVolume::resampled(int resolution) const {
  if (resolution <= 3) {
    throw std::invalid_argument("SdfVolume resample resolution must be greater than 3");
  }

  const IVec3 newSize{resolution, resolution, resolution};
  const float extent = worldExtent();
  const float newVoxelSize = extent / static_cast<float>(resolution);
  SdfVolume next(newSize, newVoxelSize, origin_, 10.0f);
  for (int z = 0; z < newSize.z; ++z) {
    for (int y = 0; y < newSize.y; ++y) {
      for (int x = 0; x < newSize.x; ++x) {
        next.setValue(x, y, z, sample(next.voxelCenter(x, y, z)));
      }
    }
  }
  return next;
}

bool SdfVolume::isSolid(int x, int y, int z) const {
  return contains(x, y, z) && value(x, y, z) < 0.0f;
}

int SdfVolume::countSolidVoxels() const {
  int count = 0;
  for (float v : values_) {
    if (v < 0.0f) {
      ++count;
    }
  }
  return count;
}

int SdfVolume::countSurfaceVoxels() const {
  int count = 0;
  for (int z = 0; z < size_.z; ++z) {
    for (int y = 0; y < size_.y; ++y) {
      for (int x = 0; x < size_.x; ++x) {
        if (!isSolid(x, y, z)) {
          continue;
        }
        const bool hasAirNeighbor =
            !isSolid(x - 1, y, z) || !isSolid(x + 1, y, z) ||
            !isSolid(x, y - 1, z) || !isSolid(x, y + 1, z) ||
            !isSolid(x, y, z - 1) || !isSolid(x, y, z + 1);
        if (hasAirNeighbor) {
          ++count;
        }
      }
    }
  }
  return count;
}

void SdfVolume::restoreValues(std::vector<float> values) {
  if (values.size() != values_.size()) {
    throw std::invalid_argument("SdfVolume snapshot size mismatch");
  }
  values_ = std::move(values);
}

}  // namespace large::sdf
