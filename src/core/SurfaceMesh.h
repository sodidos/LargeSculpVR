#pragma once

#include <vector>

#include "core/SdfVolume.h"

namespace large::sdf {

struct SurfaceTriangle {
  Vec3 a;
  Vec3 b;
  Vec3 c;
  Vec3 normal;
};

struct SurfaceMesh {
  std::vector<SurfaceTriangle> triangles;
};

SurfaceMesh buildSurfaceMesh(const SdfVolume& volume);

}  // namespace large::sdf
