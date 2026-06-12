#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/SdfVolume.h"

namespace large::sdf {

class SdfHistory {
 public:
  explicit SdfHistory(SdfVolume& volume, std::size_t maxSnapshots = 32);

  // Optional external RGBA8 color buffer captured and restored alongside the
  // SDF values (one entry of 4 bytes per voxel). The buffer must outlive the
  // history.
  void attachColors(std::vector<std::uint8_t>* colors);

  void capture();
  bool undo();
  bool redo();
  void clear();

  std::size_t undoCount() const { return undoStack_.size(); }
  std::size_t redoCount() const { return redoStack_.size(); }

 private:
  struct Snapshot {
    std::vector<float> values;
    std::vector<std::uint8_t> colors;
  };

  Snapshot makeSnapshot() const;
  void restoreSnapshot(Snapshot snapshot);

  SdfVolume& volume_;
  std::vector<std::uint8_t>* colors_ = nullptr;
  std::size_t maxSnapshots_ = 32;
  std::vector<Snapshot> undoStack_;
  std::vector<Snapshot> redoStack_;
};

}  // namespace large::sdf
