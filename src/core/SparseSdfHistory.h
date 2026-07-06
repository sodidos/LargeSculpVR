#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "core/SparseSdfVolume.h"

namespace large::sdf {

// Undo/redo for the sparse volume. Bricks are the natural undo unit: an entry
// stores the pre-edit state of every brick a stroke touched, including bricks
// that were absent (undo then removes a brick the stroke created). The history
// registers itself as the volume's edit observer, so brushes preserve their
// pre-images automatically. Undo/redo swap saved bricks with the live data.
class SparseSdfHistory : public SparseEditObserver {
 public:
  explicit SparseSdfHistory(SparseSdfVolume& volume, std::size_t maxBytes = 256ull << 20);
  ~SparseSdfHistory() override;

  void capture();
  bool undo();
  bool redo();
  void clear();

  VoxelBounds lastChangedBounds() const { return lastChangedBounds_; }
  std::size_t undoCount() const { return undoStack_.size(); }
  std::size_t redoCount() const { return redoStack_.size(); }
  std::size_t totalBytes() const { return totalBytes_; }

  void onBeforeSparseEdit(const SparseSdfVolume& volume, VoxelBounds bounds) override;

 private:
  struct BrickSnapshot {
    IVec3 index{};
    bool present = false;
    std::vector<float> values;  // empty when the brick was absent
  };

  struct Entry {
    std::vector<BrickSnapshot> bricks;
    std::set<std::uint64_t> keys;
    std::size_t bytes = 0;
  };

  static std::uint64_t brickKey(IVec3 index);
  BrickSnapshot captureBrick(IVec3 index) const;
  void swapEntryWithLive(Entry& entry);
  void enforceBudget();

  SparseSdfVolume& volume_;
  std::size_t maxBytes_;
  std::size_t totalBytes_ = 0;
  std::vector<Entry> undoStack_;
  std::vector<Entry> redoStack_;
  VoxelBounds lastChangedBounds_{};
};

}  // namespace large::sdf
