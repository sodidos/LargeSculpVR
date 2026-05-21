#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "core/ObjExporter.h"
#include "core/SdfHistory.h"
#include "core/SdfVolume.h"

using large::sdf::BrushMode;
using large::sdf::IVec3;
using large::sdf::SdfHistory;
using large::sdf::SdfVolume;
using large::sdf::Vec3;
using large::sdf::exportSdfSurfaceAsObj;
using large::sdf::exportSolidVoxelsAsObj;

namespace {

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  const IVec3 size{32, 32, 32};
  const float voxelSize = 0.05f;
  const Vec3 origin{-0.8f, -0.8f, -0.8f};

  SdfVolume volume(size, voxelSize, origin, 10.0f);
  volume.fillSphere({0.0f, 0.0f, 0.0f}, 0.35f);

  expect(volume.value(16, 16, 16) < 0.0f, "sphere center should be solid");
  expect(volume.value(0, 0, 0) > 0.0f, "far corner should be empty");

  const int baseSolid = volume.countSolidVoxels();
  expect(baseSolid > 0, "sphere should create solid voxels");

  SdfHistory history(volume);
  history.capture();
  volume.applySphereBrush({0.32f, 0.0f, 0.0f}, 0.20f, BrushMode::Add);
  const int afterAdd = volume.countSolidVoxels();
  expect(afterAdd > baseSolid, "add brush should increase solid voxel count");
  expect(history.undo(), "undo should restore captured state");
  expect(volume.countSolidVoxels() == baseSolid, "undo should restore base solid voxel count");
  expect(history.redo(), "redo should restore add brush state");
  expect(volume.countSolidVoxels() == afterAdd, "redo should restore add brush solid voxel count");

  history.capture();
  volume.applySphereBrush({0.0f, 0.0f, 0.0f}, 0.18f, BrushMode::Subtract);
  const int afterSubtract = volume.countSolidVoxels();
  expect(afterSubtract < afterAdd, "subtract brush should reduce solid voxel count");
  expect(volume.countSurfaceVoxels() > 0, "edited volume should have surface voxels");

  SdfVolume smoothProbe({7, 7, 7}, 1.0f, {-3.5f, -3.5f, -3.5f}, 1.0f);
  smoothProbe.setValue(3, 3, 3, -1.0f);
  smoothProbe.applySmoothBrush({0.0f, 0.0f, 0.0f}, 2.0f, 1.0f);
  expect(smoothProbe.value(3, 3, 3) > -1.0f, "smooth brush should relax isolated sharp values");

  const float sampledLobe = volume.sample({0.32f, 0.0f, 0.0f});
  expect(sampledLobe < 0.0f, "trilinear sampling should read the remaining sculpted lobe");
  SdfVolume resampled = volume.resampled(24);
  expect(resampled.size().x == 24, "resample should change X resolution");
  expect(resampled.size().y == 24, "resample should change Y resolution");
  expect(resampled.size().z == 24, "resample should change Z resolution");
  expect(resampled.sample({0.32f, 0.0f, 0.0f}) < 0.0f, "resample should preserve the sculpted volume");

  SdfVolume stretchSource({32, 32, 32}, 0.05f, origin, 10.0f);
  stretchSource.fillSphere({0.0f, 0.0f, 0.0f}, 0.30f);
  SdfVolume stretched = stretchSource;
  stretched.applyStretchBrush(stretchSource, {0.28f, 0.0f, 0.0f}, {0.22f, 0.0f, 0.0f}, 0.32f, 1.0f);
  expect(stretched.sample({0.48f, 0.0f, 0.0f}) < stretchSource.sample({0.48f, 0.0f, 0.0f}),
         "stretch brush should pull material toward the drag direction");

  const auto path = std::filesystem::path("out") / "sdf_test.obj";
  const auto stats = exportSolidVoxelsAsObj(volume, path);
  expect(stats.vertices > 0, "OBJ export should write vertices");
  expect(stats.faces > 0, "OBJ export should write faces");
  expect(std::filesystem::exists(path), "OBJ file should exist");

  const auto surfacePath = std::filesystem::path("out") / "sdf_test_surface.obj";
  const auto surfaceStats = exportSdfSurfaceAsObj(volume, surfacePath);
  expect(surfaceStats.vertices > 0, "surface OBJ export should write vertices");
  expect(surfaceStats.faces > 0, "surface OBJ export should write faces");
  expect(std::filesystem::exists(surfacePath), "surface OBJ file should exist");

  std::cout << "SDF tests passed\n";
  return 0;
}
