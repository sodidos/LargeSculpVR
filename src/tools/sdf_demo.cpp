#include <filesystem>
#include <iostream>

#include "core/ObjExporter.h"
#include "core/SdfVolume.h"

using large::sdf::BrushMode;
using large::sdf::IVec3;
using large::sdf::SdfVolume;
using large::sdf::Vec3;
using large::sdf::exportSdfSurfaceAsObj;
using large::sdf::exportSolidVoxelsAsObj;

int main() {
  const IVec3 size{72, 72, 72};
  const float voxelSize = 0.04f;
  const Vec3 origin{
      -static_cast<float>(size.x) * voxelSize * 0.5f,
      -static_cast<float>(size.y) * voxelSize * 0.5f,
      -static_cast<float>(size.z) * voxelSize * 0.5f,
  };

  SdfVolume volume(size, voxelSize, origin, 10.0f);

  volume.fillSphere({0.0f, 0.0f, 0.0f}, 0.62f);
  const int baseSolid = volume.countSolidVoxels();

  volume.applySphereBrush({0.50f, 0.02f, 0.0f}, 0.26f, BrushMode::Add);
  const int afterAdd = volume.countSolidVoxels();

  volume.applySphereBrush({0.05f, 0.35f, 0.0f}, 0.22f, BrushMode::Subtract);
  const int afterSubtract = volume.countSolidVoxels();

  volume.applySmoothBrush({0.05f, 0.35f, 0.0f}, 0.34f, 0.35f);
  const int afterSmooth = volume.countSolidVoxels();

  const auto blockyPath = std::filesystem::path("out") / "sdf_demo_blocky.obj";
  const auto smoothPath = std::filesystem::path("out") / "sdf_demo_smooth.obj";
  const auto blockyStats = exportSolidVoxelsAsObj(volume, blockyPath);
  const auto smoothStats = exportSdfSurfaceAsObj(volume, smoothPath);

  std::cout << "Large SDF demo\n";
  std::cout << "Volume: " << size.x << "x" << size.y << "x" << size.z << " voxels\n";
  std::cout << "Voxel size: " << voxelSize << " m\n";
  std::cout << "Solid voxels, base sphere: " << baseSolid << '\n';
  std::cout << "Solid voxels, after add: " << afterAdd << '\n';
  std::cout << "Solid voxels, after subtract: " << afterSubtract << '\n';
  std::cout << "Solid voxels, after smooth: " << afterSmooth << '\n';
  std::cout << "Surface voxels: " << volume.countSurfaceVoxels() << '\n';
  std::cout << "Blocky OBJ vertices: " << blockyStats.vertices << '\n';
  std::cout << "Blocky OBJ faces: " << blockyStats.faces << '\n';
  std::cout << "Smooth OBJ vertices: " << smoothStats.vertices << '\n';
  std::cout << "Smooth OBJ faces: " << smoothStats.faces << '\n';
  std::cout << "Wrote: " << blockyPath.string() << '\n';
  std::cout << "Wrote: " << smoothPath.string() << '\n';

  return 0;
}
