#include "core/SparseSdfVolume.h"

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
  return {dot(v, rotationX), dot(v, rotationY), dot(v, rotationZ)};
}

constexpr std::uint64_t kAxisMask = (1ull << 21) - 1ull;

}  // namespace

SparseSdfVolume::SparseSdfVolume(IVec3 brickCount, float voxelSize, Vec3 origin)
    : brickCount_(brickCount), voxelSize_(voxelSize), origin_(origin) {
  if (brickCount.x <= 0 || brickCount.y <= 0 || brickCount.z <= 0) {
    throw std::invalid_argument("SparseSdfVolume brick count must be positive");
  }
  if (voxelSize <= 0.0f) {
    throw std::invalid_argument("SparseSdfVolume voxel size must be positive");
  }
  size_ = {brickCount.x * kBrickSize, brickCount.y * kBrickSize, brickCount.z * kBrickSize};
  // Brushes write a narrow band of ~3 voxels beyond the surface, so a voxel
  // in an absent brick is always at least ~3 voxels away from any surface:
  // this constant is a safe (under)estimate for the sphere tracer.
  emptyValue_ = voxelSize_ * 3.0f;
}

std::uint64_t SparseSdfVolume::brickKey(int bx, int by, int bz) {
  return (static_cast<std::uint64_t>(bz) << 42) | (static_cast<std::uint64_t>(by) << 21) |
         static_cast<std::uint64_t>(bx);
}

IVec3 SparseSdfVolume::brickIndexFromKey(std::uint64_t key) {
  return {static_cast<int>(key & kAxisMask), static_cast<int>((key >> 21) & kAxisMask),
          static_cast<int>((key >> 42) & kAxisMask)};
}

bool SparseSdfVolume::contains(int x, int y, int z) const {
  return x >= 0 && y >= 0 && z >= 0 && x < size_.x && y < size_.y && z < size_.z;
}

Vec3 SparseSdfVolume::voxelCenter(int x, int y, int z) const {
  return {
      origin_.x + (static_cast<float>(x) + 0.5f) * voxelSize_,
      origin_.y + (static_cast<float>(y) + 0.5f) * voxelSize_,
      origin_.z + (static_cast<float>(z) + 0.5f) * voxelSize_,
  };
}

SparseSdfVolume::Brick& SparseSdfVolume::getOrCreateBrick(int bx, int by, int bz) {
  Brick& brick = bricks_[brickKey(bx, by, bz)];
  if (brick.values.empty()) {
    brick.values.assign(static_cast<std::size_t>(kBrickSize) * kBrickSize * kBrickSize, emptyValue_);
  }
  return brick;
}

const SparseSdfVolume::Brick* SparseSdfVolume::findBrick(int bx, int by, int bz) const {
  const auto it = bricks_.find(brickKey(bx, by, bz));
  return it == bricks_.end() ? nullptr : &it->second;
}

float SparseSdfVolume::readValue(int x, int y, int z, ConstCursor& cursor) const {
  if (!contains(x, y, z)) {
    return emptyValue_;
  }
  const std::uint64_t key = brickKey(x / kBrickSize, y / kBrickSize, z / kBrickSize);
  if (key != cursor.key) {
    cursor.key = key;
    const auto it = bricks_.find(key);
    cursor.brick = it == bricks_.end() ? nullptr : &it->second;
  }
  if (cursor.brick == nullptr) {
    return emptyValue_;
  }
  const int lx = x % kBrickSize;
  const int ly = y % kBrickSize;
  const int lz = z % kBrickSize;
  return cursor.brick->values[(static_cast<std::size_t>(lz) * kBrickSize + ly) * kBrickSize + lx];
}

float& SparseSdfVolume::writeValue(int x, int y, int z, Cursor& cursor) {
  const std::uint64_t key = brickKey(x / kBrickSize, y / kBrickSize, z / kBrickSize);
  if (key != cursor.key || cursor.brick == nullptr) {
    cursor.key = key;
    cursor.brick = &getOrCreateBrick(x / kBrickSize, y / kBrickSize, z / kBrickSize);
  }
  const int lx = x % kBrickSize;
  const int ly = y % kBrickSize;
  const int lz = z % kBrickSize;
  return cursor.brick->values[(static_cast<std::size_t>(lz) * kBrickSize + ly) * kBrickSize + lx];
}

float SparseSdfVolume::value(int x, int y, int z) const {
  ConstCursor cursor;
  return readValue(x, y, z, cursor);
}

void SparseSdfVolume::setValue(int x, int y, int z, float value) {
  if (!contains(x, y, z)) {
    return;
  }
  Cursor cursor;
  writeValue(x, y, z, cursor) = value;
  markDirtyBounds(x, y, z, x, y, z);
}

float SparseSdfVolume::sample(Vec3 point) const {
  const float fx = (point.x - origin_.x) / voxelSize_ - 0.5f;
  const float fy = (point.y - origin_.y) / voxelSize_ - 0.5f;
  const float fz = (point.z - origin_.z) / voxelSize_ - 0.5f;

  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const int z0 = static_cast<int>(std::floor(fz));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  const float tz = fz - static_cast<float>(z0);

  ConstCursor cursor;
  const float c000 = readValue(x0, y0, z0, cursor);
  const float c100 = readValue(x0 + 1, y0, z0, cursor);
  const float c010 = readValue(x0, y0 + 1, z0, cursor);
  const float c110 = readValue(x0 + 1, y0 + 1, z0, cursor);
  const float c001 = readValue(x0, y0, z0 + 1, cursor);
  const float c101 = readValue(x0 + 1, y0, z0 + 1, cursor);
  const float c011 = readValue(x0, y0 + 1, z0 + 1, cursor);
  const float c111 = readValue(x0 + 1, y0 + 1, z0 + 1, cursor);

  const float c00 = c000 + (c100 - c000) * tx;
  const float c10 = c010 + (c110 - c010) * tx;
  const float c01 = c001 + (c101 - c001) * tx;
  const float c11 = c011 + (c111 - c011) * tx;
  const float c0 = c00 + (c10 - c00) * ty;
  const float c1 = c01 + (c11 - c01) * ty;
  return c0 + (c1 - c0) * tz;
}

void SparseSdfVolume::applySphereBrush(Vec3 center, float radius, BrushMode mode, float strength) {
  applyCapsuleBrush(center, center, radius, mode, strength);
}

void SparseSdfVolume::applyCapsuleBrush(Vec3 start, Vec3 end, float radius, BrushMode mode, float strength) {
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
  const int minX = clampInt(static_cast<int>(std::floor((std::min(start.x, end.x) - influence - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((std::min(start.y, end.y) - influence - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((std::min(start.z, end.z) - influence - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((std::max(start.x, end.x) + influence - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((std::max(start.y, end.y) + influence - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((std::max(start.z, end.z) + influence - origin_.z) / voxelSize_)), 0, size_.z - 1);

  const bool exact = amount >= 0.999f;
  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  ConstCursor readCursor;
  Cursor writeCursor;

  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const Vec3 toPoint = p - start;
        const float h =
            segmentLengthSq > 0.000001f ? clamp(dot(toPoint, segment) / segmentLengthSq, 0.0f, 1.0f) : 0.0f;
        const float dist = length(toPoint - segment * h);
        const float brush = dist - radius;
        const float base = readValue(x, y, z, readCursor);

        float next;
        if (exact) {
          next = mode == BrushMode::Add ? std::min(base, brush) : std::max(base, -brush);
        } else {
          const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / influence);
          const float clamped = clamp(base, -influence, influence);
          const float target = mode == BrushMode::Add ? std::min(clamped, brush) : std::max(clamped, -brush);
          next = clamped + (target - clamped) * amount * falloff;
        }

        // Bricks stay absent when the write would just re-store the empty
        // constant (far corners of the bounding box).
        if (next == base && std::abs(base - emptyValue_) < 0.000001f) {
          continue;
        }
        writeValue(x, y, z, writeCursor) = next;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SparseSdfVolume::applyFlattenBrush(Vec3 center, Vec3 planePoint, Vec3 planeNormal, float radius,
                                        float strength) {
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
  ConstCursor readCursor;
  Cursor writeCursor;
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const Vec3 p = voxelCenter(x, y, z);
        const float dist = length(p - center);
        if (dist > radius) {
          continue;
        }
        const float planeDistance = dot(p - planePoint, n);
        const float falloff = 1.0f - smoothstep(0.0f, 1.0f, dist / radius);
        const float base = readValue(x, y, z, readCursor);
        const float clamped = clamp(base, -radius, radius);
        const float next = clamped + (planeDistance - clamped) * amount * falloff;
        if (next == base && std::abs(base - emptyValue_) < 0.000001f) {
          continue;
        }
        writeValue(x, y, z, writeCursor) = next;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SparseSdfVolume::applyPinchBrush(Vec3 center, float radius, float strength) {
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

  // Dense local snapshot with the outward-sampling margin (same approach as
  // the dense implementation).
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
  SdfVolume source(sourceSize, voxelSize_, sourceOrigin, emptyValue_);
  {
    ConstCursor readCursor;
    for (int z = 0; z < sourceSize.z; ++z) {
      for (int y = 0; y < sourceSize.y; ++y) {
        for (int x = 0; x < sourceSize.x; ++x) {
          source.setValue(x, y, z, readValue(srcMinX + x, srcMinY + y, srcMinZ + z, readCursor));
        }
      }
    }
  }

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  ConstCursor readCursor;
  Cursor writeCursor;
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
        const float next = source.sample(samplePoint);
        const float base = readValue(x, y, z, readCursor);
        if (next == base && std::abs(base - emptyValue_) < 0.000001f) {
          continue;
        }
        writeValue(x, y, z, writeCursor) = next;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SparseSdfVolume::applySmoothBrush(Vec3 center, float radius, float strength) {
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

  const int srcMinX = clampInt(minX - 1, 0, size_.x - 1);
  const int srcMinY = clampInt(minY - 1, 0, size_.y - 1);
  const int srcMinZ = clampInt(minZ - 1, 0, size_.z - 1);
  const int srcMaxX = clampInt(maxX + 1, 0, size_.x - 1);
  const int srcMaxY = clampInt(maxY + 1, 0, size_.y - 1);
  const int srcMaxZ = clampInt(maxZ + 1, 0, size_.z - 1);
  const IVec3 regionSize{srcMaxX - srcMinX + 1, srcMaxY - srcMinY + 1, srcMaxZ - srcMinZ + 1};
  std::vector<float> region(static_cast<std::size_t>(regionSize.x) * regionSize.y * regionSize.z);
  {
    ConstCursor readCursor;
    std::size_t i = 0;
    for (int z = srcMinZ; z <= srcMaxZ; ++z) {
      for (int y = srcMinY; y <= srcMaxY; ++y) {
        for (int x = srcMinX; x <= srcMaxX; ++x) {
          region[i++] = readValue(x, y, z, readCursor);
        }
      }
    }
  }
  const auto regionValue = [&](int x, int y, int z, float fallback) {
    if (x < srcMinX || y < srcMinY || z < srcMinZ || x > srcMaxX || y > srcMaxY || z > srcMaxZ) {
      return fallback;
    }
    const std::size_t i = (static_cast<std::size_t>(z - srcMinZ) * regionSize.y +
                           static_cast<std::size_t>(y - srcMinY)) *
                              static_cast<std::size_t>(regionSize.x) +
                          static_cast<std::size_t>(x - srcMinX);
    return region[i];
  };

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  Cursor writeCursor;
  ConstCursor readCursor;
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float dist = length(voxelCenter(x, y, z) - center);
        if (dist > radius) {
          continue;
        }
        const float band = voxelSize_ * 4.0f;
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
        const float next = original + (average - original) * amount * falloff;
        const float base = readValue(x, y, z, readCursor);
        if (next == base && std::abs(base - emptyValue_) < 0.000001f) {
          continue;
        }
        writeValue(x, y, z, writeCursor) = next;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SparseSdfVolume::applyStretchBrush(const SparseSdfVolume& source,
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
    throw std::invalid_argument("SparseSdfVolume stretch source must match destination volume");
  }

  const float amount = clamp(strength, 0.0f, 1.0f);
  const Vec3 pull = delta * amount;
  const Vec3 handle = anchor + pull;
  const int minX = clampInt(static_cast<int>(std::floor((std::min(anchor.x, handle.x) - radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int minY = clampInt(static_cast<int>(std::floor((std::min(anchor.y, handle.y) - radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int minZ = clampInt(static_cast<int>(std::floor((std::min(anchor.z, handle.z) - radius - origin_.z) / voxelSize_)), 0, size_.z - 1);
  const int maxX = clampInt(static_cast<int>(std::ceil((std::max(anchor.x, handle.x) + radius - origin_.x) / voxelSize_)), 0, size_.x - 1);
  const int maxY = clampInt(static_cast<int>(std::ceil((std::max(anchor.y, handle.y) + radius - origin_.y) / voxelSize_)), 0, size_.y - 1);
  const int maxZ = clampInt(static_cast<int>(std::ceil((std::max(anchor.z, handle.z) + radius - origin_.z) / voxelSize_)), 0, size_.z - 1);

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
    return (1.0f - smoothstep(0.0f, 1.0f, distToPath / radius)) * axial;
  };

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  ConstCursor readCursor;
  Cursor writeCursor;
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
        const Vec3 samplePoint = anchor + relative + (rotatedBack - relative) * weight;
        const float next = source.sample(samplePoint);
        const float base = readValue(x, y, z, readCursor);
        if (next == base && std::abs(base - emptyValue_) < 0.000001f) {
          continue;
        }
        writeValue(x, y, z, writeCursor) = next;
      }
    }
  }

  // Relaxation weighted by the warp amount (same rationale as the dense
  // implementation: the warp does not preserve distances).
  const float band = voxelSize_ * 4.0f;
  ConstCursor relaxCursor;
  Cursor relaxWrite;
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float weight = warpWeight(voxelCenter(x, y, z));
        if (weight <= 0.0f) {
          continue;
        }
        const float original = clamp(readValue(x, y, z, relaxCursor), -band, band);
        const float average = (clamp(readValue(x - 1, y, z, relaxCursor), -band, band) +
                               clamp(readValue(x + 1, y, z, relaxCursor), -band, band) +
                               clamp(readValue(x, y - 1, z, relaxCursor), -band, band) +
                               clamp(readValue(x, y + 1, z, relaxCursor), -band, band) +
                               clamp(readValue(x, y, z - 1, relaxCursor), -band, band) +
                               clamp(readValue(x, y, z + 1, relaxCursor), -band, band)) /
                              6.0f;
        const float next = original + (average - original) * 0.5f * weight;
        const float base = readValue(x, y, z, relaxCursor);
        if (next == base && std::abs(base - emptyValue_) < 0.000001f) {
          continue;
        }
        writeValue(x, y, z, relaxWrite) = next;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

void SparseSdfVolume::restoreRegion(const SparseSdfVolume& source, VoxelBounds bounds) {
  if (!bounds.valid) {
    return;
  }
  if (source.size_.x != size_.x || source.size_.y != size_.y || source.size_.z != size_.z) {
    throw std::invalid_argument("SparseSdfVolume restore source must match destination volume");
  }

  const int minX = clampInt(bounds.min.x, 0, size_.x - 1);
  const int minY = clampInt(bounds.min.y, 0, size_.y - 1);
  const int minZ = clampInt(bounds.min.z, 0, size_.z - 1);
  const int maxX = clampInt(bounds.max.x, minX, size_.x - 1);
  const int maxY = clampInt(bounds.max.y, minY, size_.y - 1);
  const int maxZ = clampInt(bounds.max.z, minZ, size_.z - 1);

  notifyBeforeEdit(minX, minY, minZ, maxX, maxY, maxZ);
  ConstCursor sourceCursor;
  ConstCursor readCursor;
  Cursor writeCursor;
  for (int z = minZ; z <= maxZ; ++z) {
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const float next = source.readValue(x, y, z, sourceCursor);
        const float base = readValue(x, y, z, readCursor);
        if (next == base) {
          continue;
        }
        writeValue(x, y, z, writeCursor) = next;
      }
    }
  }
  markDirtyBounds(minX, minY, minZ, maxX, maxY, maxZ);
}

int SparseSdfVolume::countSolidVoxels() const {
  int count = 0;
  for (const auto& entry : bricks_) {
    for (float value : entry.second.values) {
      if (value < 0.0f) {
        ++count;
      }
    }
  }
  return count;
}

std::size_t SparseSdfVolume::memoryBytes() const {
  return bricks_.size() * static_cast<std::size_t>(kBrickSize) * kBrickSize * kBrickSize * sizeof(float);
}

void SparseSdfVolume::markDirtyBounds(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
  VoxelBounds next{};
  next.valid = true;
  next.min = {clampInt(std::min(minX, maxX), 0, size_.x - 1), clampInt(std::min(minY, maxY), 0, size_.y - 1),
              clampInt(std::min(minZ, maxZ), 0, size_.z - 1)};
  next.max = {clampInt(std::max(minX, maxX), 0, size_.x - 1), clampInt(std::max(minY, maxY), 0, size_.y - 1),
              clampInt(std::max(minZ, maxZ), 0, size_.z - 1)};

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

void SparseSdfVolume::notifyBeforeEdit(int minX, int minY, int minZ, int maxX, int maxY, int maxZ) {
  if (observer_ == nullptr) {
    return;
  }
  VoxelBounds bounds{};
  bounds.valid = true;
  bounds.min = {clampInt(minX, 0, size_.x - 1), clampInt(minY, 0, size_.y - 1), clampInt(minZ, 0, size_.z - 1)};
  bounds.max = {clampInt(maxX, 0, size_.x - 1), clampInt(maxY, 0, size_.y - 1), clampInt(maxZ, 0, size_.z - 1)};
  observer_->onBeforeSparseEdit(*this, bounds);
}

IVec3 SparseSdfVolume::brickCountForBounds(VoxelBounds bounds, IVec3& minBrick) const {
  if (!bounds.valid) {
    minBrick = {0, 0, 0};
    return {0, 0, 0};
  }
  const IVec3 lo{clampInt(bounds.min.x, 0, size_.x - 1) / kBrickSize,
                 clampInt(bounds.min.y, 0, size_.y - 1) / kBrickSize,
                 clampInt(bounds.min.z, 0, size_.z - 1) / kBrickSize};
  const IVec3 hi{clampInt(bounds.max.x, 0, size_.x - 1) / kBrickSize,
                 clampInt(bounds.max.y, 0, size_.y - 1) / kBrickSize,
                 clampInt(bounds.max.z, 0, size_.z - 1) / kBrickSize};
  minBrick = lo;
  return {hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
}

const float* SparseSdfVolume::brickData(IVec3 brickIndex) const {
  const Brick* brick = findBrick(brickIndex.x, brickIndex.y, brickIndex.z);
  return brick == nullptr ? nullptr : brick->values.data();
}

void SparseSdfVolume::restoreBrick(IVec3 brickIndex, const float* values) {
  const std::uint64_t key = brickKey(brickIndex.x, brickIndex.y, brickIndex.z);
  if (values == nullptr) {
    bricks_.erase(key);  // the brick was absent in the snapshot
  } else {
    Brick& brick = getOrCreateBrick(brickIndex.x, brickIndex.y, brickIndex.z);
    std::copy_n(values, static_cast<std::size_t>(kBrickSize) * kBrickSize * kBrickSize, brick.values.data());
  }
  const int minX = brickIndex.x * kBrickSize;
  const int minY = brickIndex.y * kBrickSize;
  const int minZ = brickIndex.z * kBrickSize;
  markDirtyBounds(minX, minY, minZ, minX + kBrickSize - 1, minY + kBrickSize - 1, minZ + kBrickSize - 1);
}

}  // namespace large::sdf
