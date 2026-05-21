#pragma once

#include <filesystem>

#include "core/SdfVolume.h"

namespace large::sdf {

struct ObjExportStats {
  int vertices = 0;
  int faces = 0;
};

ObjExportStats exportSolidVoxelsAsObj(const SdfVolume& volume, const std::filesystem::path& path);
ObjExportStats exportSdfSurfaceAsObj(const SdfVolume& volume, const std::filesystem::path& path);

}  // namespace large::sdf
