#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/ObjExporter.h"
#include "core/SdfHistory.h"
#include "core/SdfVolume.h"

using large::sdf::BrushMode;
using large::sdf::IVec3;
using large::sdf::SdfHistory;
using large::sdf::SdfVolume;
using large::sdf::Vec3;
using large::sdf::VoxelBounds;
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
  volume.clearDirtyBounds();

  SdfHistory history(volume);
  history.capture();
  volume.applySphereBrush({0.32f, 0.0f, 0.0f}, 0.20f, BrushMode::Add);
  const large::sdf::VoxelBounds addDirty = volume.dirtyBounds();
  expect(addDirty.valid, "add brush should report dirty voxel bounds");
  expect(addDirty.max.x - addDirty.min.x + 1 < size.x, "add brush dirty bounds should be partial");
  const int afterAdd = volume.countSolidVoxels();
  expect(afterAdd > baseSolid, "add brush should increase solid voxel count");
  expect(history.undo(), "undo should restore captured state");
  expect(volume.dirtyBounds().valid, "undo should mark the restored volume dirty");
  expect(volume.countSolidVoxels() == baseSolid, "undo should restore base solid voxel count");
  volume.clearDirtyBounds();
  expect(history.redo(), "redo should restore add brush state");
  expect(volume.dirtyBounds().valid, "redo should mark the restored volume dirty");
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

  SdfVolume capsuleProbe({32, 32, 32}, 0.05f, origin, 10.0f);
  capsuleProbe.applyCapsuleBrush({-0.40f, 0.0f, 0.0f}, {0.40f, 0.0f, 0.0f}, 0.12f, BrushMode::Add, 1.0f);
  expect(capsuleProbe.sample({0.0f, 0.0f, 0.0f}) < 0.0f, "capsule add should fill the segment middle");
  expect(capsuleProbe.sample({-0.35f, 0.0f, 0.0f}) < 0.0f, "capsule add should fill near the segment start");
  expect(capsuleProbe.sample({0.0f, 0.40f, 0.0f}) > 0.0f, "capsule add should not fill far off-axis");
  const int capsuleSolid = capsuleProbe.countSolidVoxels();
  capsuleProbe.applyCapsuleBrush({-0.40f, 0.0f, 0.0f}, {0.40f, 0.0f, 0.0f}, 0.05f, BrushMode::Subtract, 1.0f);
  expect(capsuleProbe.countSolidVoxels() < capsuleSolid, "capsule subtract should carve a groove");

  SdfVolume flattenProbe({32, 32, 32}, 0.05f, origin, 10.0f);
  flattenProbe.fillSphere({0.0f, 0.0f, 0.0f}, 0.35f);
  const float bumpBefore = flattenProbe.sample({0.0f, 0.30f, 0.0f});
  expect(bumpBefore < 0.0f, "sphere top should be solid before flatten");
  flattenProbe.applyFlattenBrush({0.0f, 0.35f, 0.0f},
                                 {0.0f, 0.20f, 0.0f},
                                 {0.0f, 1.0f, 0.0f},
                                 0.30f,
                                 1.0f);
  expect(flattenProbe.sample({0.0f, 0.30f, 0.0f}) > 0.0f, "flatten should shave the bump above the plane");
  expect(flattenProbe.sample({0.0f, 0.10f, 0.0f}) < 0.0f, "flatten should keep material below the plane");

  SdfVolume pinchProbe({32, 32, 32}, 0.05f, origin, 10.0f);
  pinchProbe.fillSphere({0.0f, 0.0f, 0.0f}, 0.35f);
  const int pinchSolidBefore = pinchProbe.countSolidVoxels();
  const float pinchFlankBefore = pinchProbe.sample({0.18f, 0.28f, 0.0f});
  pinchProbe.applyPinchBrush({0.0f, 0.35f, 0.0f}, 0.28f, 1.0f);
  expect(pinchProbe.countSolidVoxels() < pinchSolidBefore,
         "pinch should contract material toward the brush center");
  expect(pinchProbe.sample({0.18f, 0.28f, 0.0f}) > pinchFlankBefore,
         "pinch should pull the flanks inward to sharpen the shape");

  // Page-journal history: multiple strokes in different regions undo in
  // order, and the memory footprint stays proportional to the touched pages.
  {
    SdfVolume journalProbe({64, 64, 64}, 0.05f, {-1.6f, -1.6f, -1.6f}, 10.0f);
    SdfHistory journal(journalProbe);
    journal.capture();
    journalProbe.applySphereBrush({-1.0f, -1.0f, -1.0f}, 0.20f, BrushMode::Add);
    const int afterFirst = journalProbe.countSolidVoxels();
    journal.capture();
    journalProbe.applySphereBrush({1.0f, 1.0f, 1.0f}, 0.20f, BrushMode::Add);
    expect(journal.totalBytes() < journalProbe.values().size() * sizeof(float),
           "page journal should store far less than full snapshots");
    expect(journal.undo(), "journal undo second stroke");
    expect(journalProbe.countSolidVoxels() == afterFirst, "second stroke undone");
    expect(journal.lastChangedBounds().valid, "undo should report changed bounds");
    expect(journal.undo(), "journal undo first stroke");
    expect(journalProbe.countSolidVoxels() == 0, "first stroke undone");
    expect(journal.redo() && journal.redo(), "journal redo both strokes");
    expect(journalProbe.countSolidVoxels() > afterFirst, "both strokes redone");
  }

  // restoreRegion rewinds only the given box from a snapshot.
  {
    SdfVolume regionProbe({32, 32, 32}, 0.05f, origin, 10.0f);
    regionProbe.fillSphere({0.0f, 0.0f, 0.0f}, 0.30f);
    const SdfVolume snapshot = regionProbe;
    regionProbe.applySphereBrush({0.0f, 0.0f, 0.0f}, 0.45f, BrushMode::Subtract);  // carve everything
    expect(regionProbe.countSolidVoxels() < snapshot.countSolidVoxels(), "carve should remove material");
    VoxelBounds all{};
    all.valid = true;
    all.min = {0, 0, 0};
    all.max = {31, 31, 31};
    regionProbe.restoreRegion(snapshot, all);
    expect(regionProbe.countSolidVoxels() == snapshot.countSolidVoxels(),
           "restoreRegion should bring the snapshot region back");
  }

  SdfVolume colorProbe({8, 8, 8}, 0.1f, {-0.4f, -0.4f, -0.4f}, 10.0f);
  std::vector<std::uint8_t> colors(colorProbe.values().size() * 4, 10);
  SdfHistory colorHistory(colorProbe, 4);
  colorHistory.attachColors(&colors);
  colorHistory.capture();
  colorProbe.fillSphere({0.0f, 0.0f, 0.0f}, 0.25f);
  colors[0] = 200;
  expect(colorHistory.undo(), "color history undo should succeed");
  expect(colors[0] == 10, "undo should restore the paint colors");
  expect(colorProbe.countSolidVoxels() == 0, "undo should restore the empty volume");
  expect(colorHistory.redo(), "color history redo should succeed");
  expect(colors[0] == 200, "redo should restore the painted colors");

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
  expect(stretched.sample({0.38f, 0.0f, 0.0f}) < 0.0f,
         "stretch should keep the pulled lobe connected through a neck");

  // Mirrored stretch: a second, layered application (no source reset) pulls
  // the opposite side symmetrically without undoing the first pull.
  stretched.applyStretchBrush(stretchSource, {-0.28f, 0.0f, 0.0f}, {-0.22f, 0.0f, 0.0f}, 0.32f, 1.0f, false);
  expect(stretched.sample({0.48f, 0.0f, 0.0f}) < stretchSource.sample({0.48f, 0.0f, 0.0f}),
         "layered stretch should keep the first pull");
  expect(stretched.sample({-0.48f, 0.0f, 0.0f}) < stretchSource.sample({-0.48f, 0.0f, 0.0f}),
         "layered stretch should pull the mirrored side too");

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

  // Material touching the volume limits must be capped, not left open.
  SdfVolume clippedProbe({16, 16, 16}, 0.1f, {-0.8f, -0.8f, -0.8f}, 10.0f);
  clippedProbe.fillSphere({0.0f, 0.0f, 0.0f}, 1.2f);  // sticks out on all sides
  const auto cappedPath = std::filesystem::path("out") / "sdf_test_capped.obj";
  const auto cappedStats = exportSdfSurfaceAsObj(clippedProbe, cappedPath);
  expect(cappedStats.faces > 0, "boundary-clipped volume should still export capped faces");

  std::vector<std::uint8_t> exportColors(volume.values().size() * 4, 128);
  const auto coloredPath = std::filesystem::path("out") / "sdf_test_surface_colored.obj";
  const auto coloredStats = exportSdfSurfaceAsObj(volume, coloredPath, exportColors.data());
  expect(coloredStats.vertices == surfaceStats.vertices, "colored export should match vertex count");
  {
    std::ifstream coloredIn(coloredPath);
    std::string line;
    bool foundColoredVertex = false;
    while (std::getline(coloredIn, line)) {
      if (line.rfind("v ", 0) == 0) {
        int spaces = 0;
        for (char c : line) {
          spaces += c == ' ' ? 1 : 0;
        }
        foundColoredVertex = spaces >= 6;
        break;
      }
    }
    expect(foundColoredVertex, "colored export should write r g b after each vertex");
  }

  std::cout << "SDF tests passed\n";
  return 0;
}
