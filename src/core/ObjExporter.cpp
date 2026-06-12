#include "core/ObjExporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace large::sdf {

namespace {

struct Face {
  int dx;
  int dy;
  int dz;
  std::array<Vec3, 4> corners;
};

Vec3 corner(Vec3 origin, float s, int x, int y, int z) {
  return {origin.x + static_cast<float>(x) * s,
          origin.y + static_cast<float>(y) * s,
          origin.z + static_cast<float>(z) * s};
}

void writeVertex(std::ofstream& out, Vec3 v) {
  out << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
}

Vec3 nearestVoxelColor(const SdfVolume& volume, const std::uint8_t* colorsRgba, Vec3 p) {
  const IVec3 size = volume.size();
  const Vec3 origin = volume.origin();
  const float s = volume.voxelSize();
  const int x = clampInt(static_cast<int>(std::round((p.x - origin.x) / s - 0.5f)), 0, size.x - 1);
  const int y = clampInt(static_cast<int>(std::round((p.y - origin.y) / s - 0.5f)), 0, size.y - 1);
  const int z = clampInt(static_cast<int>(std::round((p.z - origin.z) / s - 0.5f)), 0, size.z - 1);
  const std::size_t i = volume.index(x, y, z) * 4;
  return {static_cast<float>(colorsRgba[i]) / 255.0f,
          static_cast<float>(colorsRgba[i + 1]) / 255.0f,
          static_cast<float>(colorsRgba[i + 2]) / 255.0f};
}

void writeVertexWithColor(std::ofstream& out, const SdfVolume& volume, const std::uint8_t* colorsRgba, Vec3 v) {
  if (colorsRgba == nullptr) {
    writeVertex(out, v);
    return;
  }
  const Vec3 c = nearestVoxelColor(volume, colorsRgba, v);
  out << "v " << v.x << ' ' << v.y << ' ' << v.z << ' ' << c.x << ' ' << c.y << ' ' << c.z << '\n';
}

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
  const IVec3 size = volume.size();
  const Vec3 origin = volume.origin();
  const float s = volume.voxelSize();
  const int x = clampInt(static_cast<int>(std::round((p.x - origin.x) / s - 0.5f)), 1, size.x - 2);
  const int y = clampInt(static_cast<int>(std::round((p.y - origin.y) / s - 0.5f)), 1, size.y - 2);
  const int z = clampInt(static_cast<int>(std::round((p.z - origin.z) / s - 0.5f)), 1, size.z - 2);
  return {
      volume.value(x + 1, y, z) - volume.value(x - 1, y, z),
      volume.value(x, y + 1, z) - volume.value(x, y - 1, z),
      volume.value(x, y, z + 1) - volume.value(x, y, z - 1),
  };
}

void writeTriangle(std::ofstream& out,
                   ObjExportStats& stats,
                   const SdfVolume& volume,
                   const std::uint8_t* colorsRgba,
                   Vec3 a,
                   Vec3 b,
                   Vec3 c) {
  const Vec3 center = (a + b + c) / 3.0f;
  const Vec3 normal = cross(b - a, c - a);
  const Vec3 gradient = estimateGradient(volume, center);
  if (dot(normal, gradient) < 0.0f) {
    std::swap(b, c);
  }

  const int firstVertex = stats.vertices + 1;
  writeVertexWithColor(out, volume, colorsRgba, a);
  writeVertexWithColor(out, volume, colorsRgba, b);
  writeVertexWithColor(out, volume, colorsRgba, c);
  stats.vertices += 3;
  out << "f " << firstVertex << ' ' << firstVertex + 1 << ' ' << firstVertex + 2 << '\n';
  ++stats.faces;
}

void polygoniseTetra(std::ofstream& out,
                     ObjExportStats& stats,
                     const SdfVolume& volume,
                     const std::uint8_t* colorsRgba,
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
    writeTriangle(out, stats, volume, colorsRgba, points[0], points[1], points[2]);
  } else if (points.size() == 4) {
    writeTriangle(out, stats, volume, colorsRgba, points[0], points[1], points[2]);
    writeTriangle(out, stats, volume, colorsRgba, points[0], points[2], points[3]);
  }
}

}  // namespace

ObjExportStats exportSolidVoxelsAsObj(const SdfVolume& volume, const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());

  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("Unable to open OBJ export path");
  }

  ObjExportStats stats;
  const IVec3 size = volume.size();
  const float s = volume.voxelSize();
  const Vec3 origin = volume.origin();

  out << "# LargeSculpVR blocky mesh\n";

  for (int z = 0; z < size.z; ++z) {
    for (int y = 0; y < size.y; ++y) {
      for (int x = 0; x < size.x; ++x) {
        if (!volume.isSolid(x, y, z)) {
          continue;
        }

        const std::array<Face, 6> faces = {{
            {-1, 0, 0, {corner(origin, s, x, y, z), corner(origin, s, x, y, z + 1),
                        corner(origin, s, x, y + 1, z + 1), corner(origin, s, x, y + 1, z)}},
            {1, 0, 0, {corner(origin, s, x + 1, y, z), corner(origin, s, x + 1, y + 1, z),
                       corner(origin, s, x + 1, y + 1, z + 1), corner(origin, s, x + 1, y, z + 1)}},
            {0, -1, 0, {corner(origin, s, x, y, z), corner(origin, s, x + 1, y, z),
                        corner(origin, s, x + 1, y, z + 1), corner(origin, s, x, y, z + 1)}},
            {0, 1, 0, {corner(origin, s, x, y + 1, z), corner(origin, s, x, y + 1, z + 1),
                       corner(origin, s, x + 1, y + 1, z + 1), corner(origin, s, x + 1, y + 1, z)}},
            {0, 0, -1, {corner(origin, s, x, y, z), corner(origin, s, x, y + 1, z),
                        corner(origin, s, x + 1, y + 1, z), corner(origin, s, x + 1, y, z)}},
            {0, 0, 1, {corner(origin, s, x, y, z + 1), corner(origin, s, x + 1, y, z + 1),
                       corner(origin, s, x + 1, y + 1, z + 1), corner(origin, s, x, y + 1, z + 1)}},
        }};

        for (const Face& face : faces) {
          if (volume.isSolid(x + face.dx, y + face.dy, z + face.dz)) {
            continue;
          }

          const int firstVertex = stats.vertices + 1;
          for (Vec3 v : face.corners) {
            writeVertex(out, v);
            ++stats.vertices;
          }
          out << "f " << firstVertex << ' ' << firstVertex + 1 << ' ' << firstVertex + 2 << ' ' << firstVertex + 3
              << '\n';
          ++stats.faces;
        }
      }
    }
  }

  return stats;
}

ObjExportStats exportSdfSurfaceAsObj(const SdfVolume& volume, const std::filesystem::path& path) {
  return exportSdfSurfaceAsObj(volume, path, nullptr);
}

ObjExportStats exportSdfSurfaceAsObj(const SdfVolume& volume,
                                     const std::filesystem::path& path,
                                     const std::uint8_t* colorsRgba) {
  std::filesystem::create_directories(path.parent_path());

  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("Unable to open OBJ export path");
  }

  ObjExportStats stats;
  const IVec3 size = volume.size();

  out << "# LargeSculpVR triangulated surface mesh\n";

  constexpr std::array<std::array<int, 4>, 6> tetrahedra = {{
      {{0, 5, 1, 6}},
      {{0, 1, 2, 6}},
      {{0, 2, 3, 6}},
      {{0, 3, 7, 6}},
      {{0, 7, 4, 6}},
      {{0, 4, 5, 6}},
  }};

  // The grid extends one cell beyond the volume on every side: out-of-bounds
  // values read as positive (empty), so material touching the volume limits
  // is capped by triangles instead of leaving the mesh open.
  for (int z = -1; z < size.z; ++z) {
    for (int y = -1; y < size.y; ++y) {
      for (int x = -1; x < size.x; ++x) {
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
          polygoniseTetra(out,
                          stats,
                          volume,
                          colorsRgba,
                          {p[tetra[0]], p[tetra[1]], p[tetra[2]], p[tetra[3]]},
                          {v[tetra[0]], v[tetra[1]], v[tetra[2]], v[tetra[3]]});
        }
      }
    }
  }

  return stats;
}

}  // namespace large::sdf
