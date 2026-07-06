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
  const std::size_t i = index(x, y, z);
  if (values_[i] != value) {
    values_[i] = value;
    markDirtyVoxel(x, y, z);
  }
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

void SdfVolume::notifyBeforeEdit(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
  if (observer_ == nullptr) {
    return;
  }
  VoxelBounds bounds{};
  bounds.valid = true;
  bounds.min = {clampInt(minX, 0, size_.x - 1), clampInt(minY, 0, size_.y - 1), clampInt(minZ, 0, size_.z - 1)};
  bounds.max = {clampInt(maxX, 0, size_.x - 1), clampInt(maxY, 0, size_.y - 1), clampInt(maxZ, 0, size_.z - 1)};
  observer_->onBeforeEdit(*this, bounds);
}

void SdfVolume::fillSphere(Vec3 center, float radius) {
  notifyBeforeEdit(0, 0, 0, size_.x - 1, size_.y - 1, size_.z - 1);
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

  // At full strength the brush is an exact CSG union/difference: the field
  // stays a true distance field, which keeps the surface and its normals
  // smooth. Partial strengths blend within a narrow band (soft feel, slightly
  // less exact field).
  const bool exact = amount >= 0.999f;
  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float dist = length(voxelCenter(x, y, z) - center);
        const float brush = dist - radius;
        const std::size_t i = index(x, y, z);
        if (exact) {
          values_[i] = mode == BrushMode::Add ? std::min(values_[i], brush) : std::max(values_[i], -brush);
          continue;
        }

        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / influence);
        // Clamp the source into a narrow band so partial-falloff blends still
        // cross zero in untouched far-field regions (initial distance is large).
        const float base = clamp(values_[i], -influence, influence);
        if (mode == BrushMode::Add) {
          const float target = std::min(base, brush);
          values_[i] = base + (target - base) * amount * falloff;
        } else if (mode == BrushMode::Subtract) {
          const float target = std::max(base, -brush);
          values_[i] = base + (target - base) * amount * falloff;
        }
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::applyCapsuleBrush(Vec3 start, Vec3 end, float radius, BrushMode mode, float strength) {
  if (radius <= 0.0f) {
    return;
  }

  const float amount = clamp(strength, 0.0f, 1.0f);
  if (amount <= 0.0f) {
    return;
  }

  const Vec3 segment = end - start;
  const float segmentLengthSq = dot(segment, segment);
  const float influence = radius + voxelSize_ * 3.0f;
  const Vec3 minPoint{
      std::min(start.x, end.x) - influence,
      std::min(start.y, end.y) - influence,
      std::min(start.z, end.z) - influence,
  };
  const Vec3 maxPoint{
      std::max(start.x, end.x) + influence,
      std::max(start.y, end.y) + influence,
      std::max(start.z, end.z) + influence,
  };

  const int minX = clampInt(static_cast<int>(std::floor((minPoint.x - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((minPoint.y - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((minPoint.z - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((maxPoint.x - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((maxPoint.y - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((maxPoint.z - origin_.z) / voxelSize_)), 0, size_.z - 1);

  const bool exact = amount >= 0.999f;
  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const Vec3 toPoint = p - start;
        const float h =
            segmentLengthSq > 0.000001f ? clamp(dot(toPoint, segment) / segmentLengthSq, 0.0f, 1.0f) : 0.0f;
        const float dist = length(toPoint - segment * h);
        const float brush = dist - radius;
        const std::size_t i = index(x, y, z);
        if (exact) {
          values_[i] = mode == BrushMode::Add ? std::min(values_[i], brush) : std::max(values_[i], -brush);
          continue;
        }

        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / influence);
        const float base = clamp(values_[i], -influence, influence);
        if (mode == BrushMode::Add) {
          const float target = std::min(base, brush);
          values_[i] = base + (target - base) * amount * falloff;
        } else if (mode == BrushMode::Subtract) {
          const float target = std::max(base, -brush);
          values_[i] = base + (target - base) * amount * falloff;
        }
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::applyBoxBrush(Vec3 start, Vec3 end, float halfExtent, BrushMode mode, float strength) {
  if (halfExtent <= 0.0f) {
    return;
  }
  const float amount = clamp(strength, 0.0f, 1.0f);
  if (amount <= 0.0f) {
    return;
  }

  // Sweeping an axis-aligned box along the stroke widens its half-extents by
  // the stroke's per-axis reach; for the small per-frame segment this is a
  // box at the current position that grows slightly along the movement.
  const Vec3 boxCenter = (start + end) * 0.5f;
  const Vec3 boxHalf{
      halfExtent + std::abs(end.x - start.x) * 0.5f,
      halfExtent + std::abs(end.y - start.y) * 0.5f,
      halfExtent + std::abs(end.z - start.z) * 0.5f,
  };
  const float skirt = voxelSize_ * 3.0f;  // soft band outside the box

  const int minX = clampInt(static_cast<int>(std::floor((boxCenter.x - boxHalf.x - skirt - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((boxCenter.y - boxHalf.y - skirt - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((boxCenter.z - boxHalf.z - skirt - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((boxCenter.x + boxHalf.x + skirt - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((boxCenter.y + boxHalf.y + skirt - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((boxCenter.z + boxHalf.z + skirt - origin_.z) / voxelSize_)), 0, size_.z - 1);

  const bool exact = amount >= 0.999f;
  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        // Signed distance to the axis-aligned box.
        const Vec3 q{
            std::abs(p.x - boxCenter.x) - boxHalf.x,
            std::abs(p.y - boxCenter.y) - boxHalf.y,
            std::abs(p.z - boxCenter.z) - boxHalf.z,
        };
        const Vec3 qOutside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
        const float boxDist = length(qOutside) + std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
        const std::size_t i = index(x, y, z);
        if (exact) {
          values_[i] = mode == BrushMode::Add ? std::min(values_[i], boxDist) : std::max(values_[i], -boxDist);
          continue;
        }

        const float surfDist = std::max(boxDist, 0.0f);
        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, surfDist / skirt);
        const float base = clamp(values_[i], -skirt, skirt);
        if (mode == BrushMode::Add) {
          const float target = std::min(base, boxDist);
          values_[i] = base + (target - base) * amount * falloff;
        } else if (mode == BrushMode::Subtract) {
          const float target = std::max(base, -boxDist);
          values_[i] = base + (target - base) * amount * falloff;
        }
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::applyFlattenBrush(Vec3 center, Vec3 planePoint, Vec3 planeNormal, float radius, float strength) {
  if (radius <= 0.0f || strength <= 0.0f) {
    return;
  }

  const Vec3 n = normalize(planeNormal);
  const float amount = clamp(strength, 0.0f, 1.0f);

  const int minX = clampInt(static_cast<int>(std::floor((center.x - radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((center.y - radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((center.z - radius - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((center.x + radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((center.y + radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((center.z + radius - origin_.z) / voxelSize_)), 0, size_.z - 1);

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const float dist = length(p - center);
        if (dist > radius) {
          continue;
        }

        // Blend the local field toward the locked plane: bumps above the
        // plane are shaved off and dips below it are filled in.
        const float planeDistance = dot(p - planePoint, n);
        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / radius);
        const std::size_t i = index(x, y, z);
        const float base = clamp(values_[i], -radius, radius);
        values_[i] = base + (planeDistance - base) * amount * falloff;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::applyPinchBrush(Vec3 center, float radius, float strength) {
  if (radius <= 0.0f || strength <= 0.0f) {
    return;
  }

  const float amount = clamp(strength, 0.0f, 1.0f);

  const int minX = clampInt(static_cast<int>(std::floor((center.x - radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((center.y - radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((center.z - radius - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((center.x + radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((center.y + radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((center.z + radius - origin_.z) / voxelSize_)), 0, size_.z - 1);

  // Resample the field from positions pushed away from the brush center, so
  // the local geometry contracts toward it: edges under the stroke sharpen
  // into a crease (Medium-style pinch). The source copy is local to the
  // brush region (with a margin covering the outward sampling reach) instead
  // of duplicating the whole volume.
  const int margin = static_cast<int>(std::ceil(radius * 0.5f / voxelSize_)) + 2;
  const int srcMinX = clampInt(minX - margin, 0, size_.x - 1);
  const int srcMinY = clampInt(minY - margin, 0, size_.y - 1);
  const int srcMinZ = clampInt(minZ - margin, 0, size_.z - 1);
  const int srcMaxX = clampInt(maxX + margin, 0, size_.x - 1);
  const int srcMaxY = clampInt(maxY + margin, 0, size_.y - 1);
  const int srcMaxZ = clampInt(maxZ + margin, 0, size_.z - 1);
  const IVec3 sourceSize{srcMaxX - srcMinX + 1, srcMaxY - srcMinY + 1, srcMaxZ - srcMinZ + 1};
  const Vec3 sourceOrigin{
      origin_.x + static_cast<float>(srcMinX) * voxelSize_,
      origin_.y + static_cast<float>(srcMinY) * voxelSize_,
      origin_.z + static_cast<float>(srcMinZ) * voxelSize_,
  };
  SdfVolume source(sourceSize, voxelSize_, sourceOrigin, voxelSize_ * 16.0f);
  for (int z = 0; z < sourceSize.z; ++z) {
    for (int y = 0; y < sourceSize.y; ++y) {
      const std::size_t from = index(srcMinX, srcMinY + y, srcMinZ + z);
      const std::size_t to = source.index(0, y, z);
      std::copy_n(values_.data() + from, static_cast<std::size_t>(sourceSize.x), source.values_.data() + to);
    }
  }

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const float dist = length(p - center);
        if (dist > radius) {
          continue;
        }

        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / radius);
        const float scale = 1.0f + amount * 0.45f * falloff;
        const Vec3 samplePoint = center + (p - center) * scale;
        setValue(x, y, z, source.sample(samplePoint));
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::applySmoothBrush(Vec3 center, float radius, float strength) {
  if (radius <= 0.0f || strength <= 0.0f) {
    return;
  }

  const float amount = clamp(strength, 0.0f, 1.0f);
  if (amount <= 0.0f) {
    return;
  }

  const int minX = clampInt(static_cast<int>(std::floor((center.x - radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((center.y - radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((center.z - radius - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((center.x + radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((center.y + radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((center.z + radius - origin_.z) / voxelSize_)), 0, size_.z - 1);

  // Copy only the brush region (with a one-voxel margin for the neighbor
  // reads) instead of the whole volume.
  const int srcMinX = clampInt(minX - 1, 0, size_.x - 1);
  const int srcMinY = clampInt(minY - 1, 0, size_.y - 1);
  const int srcMinZ = clampInt(minZ - 1, 0, size_.z - 1);
  const int srcMaxX = clampInt(maxX + 1, 0, size_.x - 1);
  const int srcMaxY = clampInt(maxY + 1, 0, size_.y - 1);
  const int srcMaxZ = clampInt(maxZ + 1, 0, size_.z - 1);
  const IVec3 regionSize{srcMaxX - srcMinX + 1, srcMaxY - srcMinY + 1, srcMaxZ - srcMinZ + 1};
  std::vector<float> region(static_cast<std::size_t>(regionSize.x) * static_cast<std::size_t>(regionSize.y) *
                            static_cast<std::size_t>(regionSize.z));
  for (int z = 0; z < regionSize.z; ++z) {
    for (int y = 0; y < regionSize.y; ++y) {
      const std::size_t from = index(srcMinX, srcMinY + y, srcMinZ + z);
      const std::size_t to = (static_cast<std::size_t>(z) * static_cast<std::size_t>(regionSize.y) +
                              static_cast<std::size_t>(y)) *
                             static_cast<std::size_t>(regionSize.x);
      std::copy_n(values_.data() + from, static_cast<std::size_t>(regionSize.x), region.data() + to);
    }
  }
  const auto regionValue = [&](int x, int y, int z, float fallback) {
    if (x < srcMinX || y < srcMinY || z < srcMinZ || x > srcMaxX || y > srcMaxY || z > srcMaxZ) {
      return fallback;
    }
    const std::size_t i = (static_cast<std::size_t>(z - srcMinZ) * static_cast<std::size_t>(regionSize.y) +
                           static_cast<std::size_t>(y - srcMinY)) *
                              static_cast<std::size_t>(regionSize.x) +
                          static_cast<std::size_t>(x - srcMinX);
    return region[i];
  };

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float dist = length(voxelCenter(x, y, z) - center);
        if (dist > radius) {
          continue;
        }

        // Average within a narrow band: untouched far-field values (large
        // magnitudes) would otherwise drag the average and carve the surface
        // instead of smoothing it near the borders of the sculpted region.
        const float band = voxelSize_ * 4.0f;
        const std::size_t i = index(x, y, z);
        const float original = clamp(regionValue(x, y, z, 0.0f), -band, band);
        const float average =
            (clamp(regionValue(x - 1, y, z, original), -band, band) +
             clamp(regionValue(x + 1, y, z, original), -band, band) +
             clamp(regionValue(x, y - 1, z, original), -band, band) +
             clamp(regionValue(x, y + 1, z, original), -band, band) +
             clamp(regionValue(x, y, z - 1, original), -band, band) +
             clamp(regionValue(x, y, z + 1, original), -band, band)) /
            6.0f;
        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / radius);
        values_[i] = original + (average - original) * amount * falloff;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::applyStretchBrush(const SdfVolume& source,
                                  Vec3 anchor,
                                  Vec3 delta,
                                  float radius,
                                  float strength,
                                  bool resetToSource) {
  applyStretchBrush(source,
                    anchor,
                    delta,
                    {1.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f},
                    radius,
                    strength,
                    resetToSource);
}

void SdfVolume::applyStretchBrush(const SdfVolume& source,
                                  Vec3 anchor,
                                  Vec3 delta,
                                  Vec3 rotationX,
                                  Vec3 rotationY,
                                  Vec3 rotationZ,
                                  float radius,
                                  float strength,
                                  bool resetToSource) {
  if (radius <= 0.0f || strength <= 0.0f) {
    return;
  }
  if (source.size_.x != size_.x || source.size_.y != size_.y || source.size_.z != size_.z ||
      source.voxelSize_ != voxelSize_) {
    throw std::invalid_argument("SdfVolume stretch source must match destination volume");
  }

  if (resetToSource) {
    notifyBeforeEdit(0, 0, 0, size_.x - 1, size_.y - 1, size_.z - 1);
    values_ = source.values_;
  }

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

  // Influence follows the whole pull path (capsule from anchor to handle),
  // with an axial ramp: material near the anchor stays attached while the
  // far end follows the hand completely. A sphere around the handle alone
  // would detach the pulled lobe as soon as the pull exceeds the radius.
  const Vec3 segment = handle - anchor;
  const float segmentLengthSq = dot(segment, segment);
  const bool degenerate = segmentLengthSq < voxelSize_ * voxelSize_ * 0.01f;

  const auto warpWeight = [&](Vec3 p) {
    float axial = 1.0f;
    float distToPath;
    if (degenerate) {
      distToPath = length(p - handle);
    } else {
      const float h = clamp(dot(p - anchor, segment) / segmentLengthSq, 0.0f, 1.0f);
      distToPath = length(p - (anchor + segment * h));
      axial = h;
    }
    if (distToPath > radius) {
      return 0.0f;
    }
    const float radial = 1.0f - smoothstep(0.0f, 1.0f, distToPath / radius);
    return radial * axial;
  };

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const float weight = warpWeight(p);
        if (weight <= 0.0f) {
          continue;
        }

        const Vec3 translated = p - pull * weight;
        const Vec3 relative = translated - anchor;
        const Vec3 rotatedBack = inverseRotate(relative, rotationX, rotationY, rotationZ);
        const Vec3 sourcePoint = anchor + relative + (rotatedBack - relative) * weight;
        setValue(x, y, z, source.sample(sourcePoint));
      }
    }
  }

  // The warp does not preserve distances (the field gets compressed on one
  // side and dilated on the other), which shows up as torn or crumpled
  // surfaces. One relaxation pass, weighted by the local warp amount,
  // re-regularizes the field. The volume is rebuilt from the source on every
  // application, so this never accumulates over a stroke.
  const float band = voxelSize_ * 4.0f;
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float weight = warpWeight(voxelCenter(x, y, z));
        if (weight <= 0.0f) {
          continue;
        }

        const std::size_t i = index(x, y, z);
        const float original = clamp(values_[i], -band, band);
        const float average =
            (clamp(value(x - 1, y, z), -band, band) + clamp(value(x + 1, y, z), -band, band) +
             clamp(value(x, y - 1, z), -band, band) + clamp(value(x, y + 1, z), -band, band) +
             clamp(value(x, y, z - 1), -band, band) + clamp(value(x, y, z + 1), -band, band)) /
            6.0f;
        values_[i] = original + (average - original) * 0.5f * weight;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
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
  notifyBeforeEdit(0, 0, 0, size_.x - 1, size_.y - 1, size_.z - 1);
  values_ = std::move(values);
  markAllDirty();
}

void SdfVolume::restoreRegion(const SdfVolume& source, VoxelBounds bounds) {
  if (!bounds.valid) {
    return;
  }
  if (source.size_.x != size_.x || source.size_.y != size_.y || source.size_.z != size_.z) {
    throw std::invalid_argument("SdfVolume restore source must match destination volume");
  }

  const int minX = clampInt(bounds.min.x, 0, size_.x - 1);
  const int minY = clampInt(bounds.min.y, 0, size_.y - 1);
  const int minZ = clampInt(bounds.min.z, 0, size_.z - 1);
  const int maxX = clampInt(bounds.max.x, minX, size_.x - 1);
  const int maxY = clampInt(bounds.max.y, minY, size_.y - 1);
  const int maxZ = clampInt(bounds.max.z, minZ, size_.z - 1);

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  const std::size_t rowLength = static_cast<std::size_t>(maxX - minX + 1);
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      const std::size_t i = index(minX, y, z);
      std::copy_n(source.values_.data() + i, rowLength, values_.data() + i);
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SdfVolume::clearDirtyBounds() {
  dirtyBounds_ = {};
}

void SdfVolume::markAllDirty() {
  markDirtyBounds(0, 0, 0, size_.x - 1, size_.y - 1, size_.z - 1);
}

void SdfVolume::markDirtyVoxel(int x, int y, int z) {
  markDirtyBounds(x, y, z, x, y, z);
}

void SdfVolume::markDirtyBounds(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
  if (size_.x <= 0 || size_.y <= 0 || size_.z <= 0) {
    return;
  }

  VoxelBounds next{};
  next.valid = true;
  next.min = {
      clampInt(std::min(minX, maxX), 0, size_.x - 1),
      clampInt(std::min(minY, maxY), 0, size_.y - 1),
      clampInt(std::min(minZ, maxZ), 0, size_.z - 1),
  };
  next.max = {
      clampInt(std::max(minX, maxX), 0, size_.x - 1),
      clampInt(std::max(minY, maxY), 0, size_.y - 1),
      clampInt(std::max(minZ, maxZ), 0, size_.z - 1),
  };

  if (!dirtyBounds_.valid) {
    dirtyBounds_ = next;
    return;
  }

  dirtyBounds_.min.x = std::min(dirtyBounds_.min.x, next.min.x);
  dirtyBounds_.min.y = std::min(dirtyBounds_.min.y, next.min.y);
  dirtyBounds_.min.z = std::min(dirtyBounds_.min.z, next.min.z);
  dirtyBounds_.max.x = std::max(dirtyBounds_.max.x, next.max.x);
  dirtyBounds_.max.y = std::max(dirtyBounds_.max.y, next.max.y);
  dirtyBounds_.max.z = std::max(dirtyBounds_.max.z, next.max.z);
}

}  // namespace large::sdf
