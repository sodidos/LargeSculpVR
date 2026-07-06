#pragma once

// Sparse brick SDF volume (adaptive-resolution step 1, see
// docs/resolution_adaptative.md): the workspace is a virtual grid of 32^3
// bricks allocated on demand. Empty space and deep interior cost nothing:
// absent bricks read as a constant "far outside" distance, which the sphere
// tracer naturally crosses in large steps. Brushes only allocate the bricks
// their narrow band actually touches.
//
// The API mirrors SdfVolume so the brush math and its callers stay
// identical; parity between the two implementations is covered by tests.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/MathTypes.h"
#include "core/SdfVolume.h"

namespace large::sdf {

class SparseSdfVolume {
 public:
  static constexpr int kBrickSize = 32;

  SparseSdfVolume(IVec3 brickCount, float voxelSize, Vec3 origin);

  IVec3 size() const { return size_; }
  IVec3 brickCount() const { return brickCount_; }
  float voxelSize() const { return voxelSize_; }
  Vec3 origin() const { return origin_; }
  float emptyValue() const { return emptyValue_; }

  bool contains(int x, int y, int z) const;
  Vec3 voxelCenter(int x, int y, int z) const;

  float value(int x, int y, int z) const;
  void setValue(int x, int y, int z, float value);
  float sample(Vec3 point) const;

  void applySphereBrush(Vec3 center, float radius, BrushMode mode, float strength);
  void applyCapsuleBrush(Vec3 start, Vec3 end, float radius, BrushMode mode, float strength);
  void applyFlattenBrush(Vec3 center, Vec3 planePoint, Vec3 planeNormal, float radius, float strength);
  void applyPinchBrush(Vec3 center, float radius, float strength);
  void applySmoothBrush(Vec3 center, float radius, float strength);
  void applyStretchBrush(const SparseSdfVolume& source,
                         Vec3 anchor,
                         Vec3 delta,
                         Vec3 rotationX,
                         Vec3 rotationY,
                         Vec3 rotationZ,
                         float radius,
                         float strength);
  void restoreRegion(const SparseSdfVolume& source, VoxelBounds bounds);

  int countSolidVoxels() const;
  VoxelBounds dirtyBounds() const { return dirtyBounds_; }
  void clearDirtyBounds() { dirtyBounds_ = {}; }

  std::size_t activeBrickCount() const { return bricks_.size(); }
  std::size_t memoryBytes() const;

  // Iterates the active bricks (for GPU atlas uploads and serialization).
  // f(IVec3 brickIndex, const float* values) with kBrickSize^3 values.
  template <typename F>
  void forEachActiveBrick(F&& f) const {
    for (const auto& entry : bricks_) {
      f(brickIndexFromKey(entry.first), entry.second.values.data());
    }
  }

 private:
  struct Brick {
    std::vector<float> values;
  };

  // Consecutive voxel accesses along X stay inside one brick 32 times out of
  // 32: the cursor caches the last brick to amortize the hash lookups.
  struct Cursor {
    std::uint64_t key = ~0ull;
    Brick* brick = nullptr;
  };
  struct ConstCursor {
    std::uint64_t key = ~0ull;
    const Brick* brick = nullptr;
  };

  static std::uint64_t brickKey(int bx, int by, int bz);
  static IVec3 brickIndexFromKey(std::uint64_t key);
  float readValue(int x, int y, int z, ConstCursor& cursor) const;
  float& writeValue(int x, int y, int z, Cursor& cursor);
  Brick& getOrCreateBrick(int bx, int by, int bz);
  const Brick* findBrick(int bx, int by, int bz) const;
  void markDirtyBounds(int minX, int minY, int minZ, int maxX, int maxY, int maxZ);

  IVec3 brickCount_{};
  IVec3 size_{};
  float voxelSize_ = 1.0f;
  Vec3 origin_{};
  float emptyValue_ = 1.0f;
  std::unordered_map<std::uint64_t, Brick> bricks_;
  VoxelBounds dirtyBounds_{};
};

}  // namespace large::sdf
