#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "core/MathTypes.h"

namespace large::sdf {

enum class BrushMode {
  Add,
  Subtract,
  Smooth,
  Stretch,
};

struct IVec3 {
  int x = 0;
  int y = 0;
  int z = 0;
};

struct VoxelBounds {
  bool valid = false;
  IVec3 min{};
  IVec3 max{};
};

class SdfVolume {
 public:
  SdfVolume(IVec3 size, float voxelSize, Vec3 origin, float initialDistance);

  IVec3 size() const { return size_; }
  float voxelSize() const { return voxelSize_; }
  Vec3 origin() const { return origin_; }

  bool contains(int x, int y, int z) const;
  std::size_t index(int x, int y, int z) const;

  float value(int x, int y, int z) const;
  void setValue(int x, int y, int z, float value);
  float sample(Vec3 point) const;

  Vec3 voxelCenter(int x, int y, int z) const;
  Vec3 boundsCenter() const;
  float worldExtent() const;

  void fillSphere(Vec3 center, float radius);
  void applySphereBrush(Vec3 center, float radius, BrushMode mode);
  void applySphereBrush(Vec3 center, float radius, BrushMode mode, float strength);
  void applySmoothBrush(Vec3 center, float radius, float strength);
  void applyStretchBrush(const SdfVolume& source, Vec3 anchor, Vec3 delta, float radius, float strength);
  void applyStretchBrush(const SdfVolume& source,
                         Vec3 anchor,
                         Vec3 delta,
                         Vec3 rotationX,
                         Vec3 rotationY,
                         Vec3 rotationZ,
                         float radius,
                         float strength);
  SdfVolume resampled(int resolution) const;

  bool isSolid(int x, int y, int z) const;
  int countSolidVoxels() const;
  int countSurfaceVoxels() const;
  VoxelBounds dirtyBounds() const { return dirtyBounds_; }
  void clearDirtyBounds();
  void markAllDirty();

  const std::vector<float>& values() const { return values_; }
  void restoreValues(std::vector<float> values);

 private:
  void markDirtyVoxel(int x, int y, int z);
  void markDirtyBounds(int minX, int minY, int minZ, int maxX, int maxY, int maxZ);

  IVec3 size_;
  float voxelSize_ = 1.0f;
  Vec3 origin_;
  std::vector<float> values_;
  VoxelBounds dirtyBounds_{};
};

}  // namespace large::sdf
