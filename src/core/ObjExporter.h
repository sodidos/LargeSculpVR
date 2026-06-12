#pragma once

#include <cstdint>
#include <filesystem>

#include "core/SdfVolume.h"

namespace large::sdf {

struct ObjExportStats {
  int vertices = 0;
  int faces = 0;
};

ObjExportStats exportSolidVoxelsAsObj(const SdfVolume& volume, const std::filesystem::path& path);
ObjExportStats exportSdfSurfaceAsObj(const SdfVolume& volume, const std::filesystem::path& path);

// Same surface export, with per-vertex colors ("v x y z r g b", understood by
// Blender/MeshLab). colorsRgba is an RGBA8 buffer of 4 bytes per voxel in
// volume order; pass nullptr to export positions only.
ObjExportStats exportSdfSurfaceAsObj(const SdfVolume& volume,
                                     const std::filesystem::path& path,
                                     const std::uint8_t* colorsRgba);

}  // namespace large::sdf
