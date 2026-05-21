#pragma once

#include <cstddef>
#include <vector>

#include "core/SdfVolume.h"

namespace large::sdf {

class SdfHistory {
 public:
  explicit SdfHistory(SdfVolume& volume, std::size_t maxSnapshots = 32);

  void capture();
  bool undo();
  bool redo();
  void clear();

  std::size_t undoCount() const { return undoStack_.size(); }
  std::size_t redoCount() const { return redoStack_.size(); }

 private:
  SdfVolume& volume_;
  std::size_t maxSnapshots_ = 32;
  std::vector<std::vector<float>> undoStack_;
  std::vector<std::vector<float>> redoStack_;
};

}  // namespace large::sdf
