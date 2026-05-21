#include "core/SurfaceMesh.h"

#include <array>
#include <cmath>
#include <vector>

namespace large::sdf {

namespace {

Vec3 lerp(Vec3 a, Vec3 b, float t) {
  return a + (b - a) * t;
}

Vec3 interpolateIso(Vec3 a, Vec3 b, float va, float vb) {
  const float denom = va - vb;
  if (std::abs(denom) < 0.000001f) {
    return (a + b) * 0.5f;
  }
  const float t = clamp(va / denom, 0.0f, 1.0f);
  return lerp(a, b, t);
}

Vec3 estimateGradient(const SdfVolume& volume, Vec3 p) {
  const float s = volume.voxelSize();
  return {
      volume.sample({p.x + s, p.y, p.z}) - volume.sample({p.x - s, p.y, p.z}),
      volume.sample({p.x, p.y + s, p.z}) - volume.sample({p.x, p.y - s, p.z}),
      volume.sample({p.x, p.y, p.z + s}) - volume.sample({p.x, p.y, p.z - s}),
  };
}

void addTriangle(SurfaceMesh& mesh, const SdfVolume& volume, Vec3 a, Vec3 b, Vec3 c) {
  const Vec3 center = (a + b + c) / 3.0f;
  Vec3 normal = normalize(cross(b - a, c - a));
  const Vec3 gradient = estimateGradient(volume, center);
  if (dot(normal, gradient) < 0.0f) {
    std::swap(b, c);
    normal = normal * -1.0f;
  }
  mesh.triangles.push_back({a, b, c, normal});
}

void polygoniseTetra(SurfaceMesh& mesh,
                     const SdfVolume& volume,
                     const std::array<Vec3, 4>& p,
                     const std::array<float, 4>& v) {
  constexpr std::array<std::array<int, 2>, 6> edges = {{
      {{0, 1}},
      {{0, 2}},
      {{0, 3}},
      {{1, 2}},
      {{1, 3}},
      {{2, 3}},
  }};

  std::vector<Vec3> points;
  points.reserve(4);
  for (const auto& edge : edges) {
    const int a = edge[0];
    const int b = edge[1];
    if ((v[a] < 0.0f) == (v[b] < 0.0f)) {
      continue;
    }
    points.push_back(interpolateIso(p[a], p[b], v[a], v[b]));
  }

  if (points.size() == 3) {
    addTriangle(mesh, volume, points[0], points[1], points[2]);
  } else if (points.size() == 4) {
    addTriangle(mesh, volume, points[0], points[1], points[2]);
    addTriangle(mesh, volume, points[0], points[2], points[3]);
  }
}

}  // namespace

SurfaceMesh buildSurfaceMesh(const SdfVolume& volume) {
  SurfaceMesh mesh;
  const IVec3 size = volume.size();
  mesh.triangles.reserve(static_cast<std::size_t>(volume.countSurfaceVoxels()) * 4);

  constexpr std::array<std::array<int, 4>, 6> tetrahedra = {{
      {{0, 5, 1, 6}},
      {{0, 1, 2, 6}},
      {{0, 2, 3, 6}},
      {{0, 3, 7, 6}},
      {{0, 7, 4, 6}},
      {{0, 4, 5, 6}},
  }};

  for (int z = 0; z < size.z - 1; ++z) {
    for (int y = 0; y < size.y - 1; ++y) {
      for (int x = 0; x < size.x - 1; ++x) {
        const std::array<Vec3, 8> p = {{
            volume.voxelCenter(x, y, z),
            volume.voxelCenter(x + 1, y, z),
            volume.voxelCenter(x + 1, y + 1, z),
            volume.voxelCenter(x, y + 1, z),
            volume.voxelCenter(x, y, z + 1),
            volume.voxelCenter(x + 1, y, z + 1),
            volume.voxelCenter(x + 1, y + 1, z + 1),
            volume.voxelCenter(x, y + 1, z + 1),
        }};
        const std::array<float, 8> v = {{
            volume.value(x, y, z),
            volume.value(x + 1, y, z),
            volume.value(x + 1, y + 1, z),
            volume.value(x, y + 1, z),
            volume.value(x, y, z + 1),
            volume.value(x + 1, y, z + 1),
            volume.value(x + 1, y + 1, z + 1),
            volume.value(x, y + 1, z + 1),
        }};

        for (const auto& tetra : tetrahedra) {
          polygoniseTetra(mesh,
                          volume,
                          {p[tetra[0]], p[tetra[1]], p[tetra[2]], p[tetra[3]]},
                          {v[tetra[0]], v[tetra[1]], v[tetra[2]], v[tetra[3]]});
        }
      }
    }
  }

  return mesh;
}

}  // namespace large::sdf
