#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

#include "core/SdfVolume.h"

namespace large::sdf {

// Page-journal undo: instead of snapshotting the whole volume, each undo
// entry stores only the 32x32x32 pages a stroke touched (SDF values and,
// when a color buffer is attached, the matching RGBA8 colors). The history
// registers itself as the volume's edit observer, so brushes preserve their
// pre-images automatically. Undo/redo swap the saved pages with the live
// data, which turns an undo entry into its own redo entry.
class SdfHistory : public SdfEditObserver {
 public:
  explicit SdfHistory(SdfVolume& volume, std::size_t maxBytes = 256ull << 20);
  ~SdfHistory() override;

  // Optional external RGBA8 color buffer (4 bytes per voxel) preserved and
  // restored alongside the SDF pages. The buffer must outlive the history.
  void attachColors(std::vector<std::uint8_t>* colors);

  // Begins a new undo entry (typically at stroke start) and clears redo.
  void capture();

  // Preserves pages for a color-only edit (paint strokes do not go through
  // an SdfVolume brush). Must be called before the colors are modified.
  void preserveColorRegion(VoxelBounds bounds);

  bool undo();
  bool redo();
  void clear();

  // Merged voxel bounds of the pages changed by the last undo()/redo(),
  // so the caller can re-upload only that region.
  VoxelBounds lastChangedBounds() const { return lastChangedBounds_; }

  std::size_t undoCount() const { return undoStack_.size(); }
  std::size_t redoCount() const { return redoStack_.size(); }
  std::size_t totalBytes() const { return totalBytes_; }

  void onBeforeEdit(const SdfVolume& volume, VoxelBounds bounds) override;

 private:
  static constexpr int kPageSize = 32;

  struct Page {
    IVec3 pageIndex{};
    VoxelBounds bounds{};  // clipped voxel bounds covered by this page
    std::vector<float> sdf;
    std::vector<std::uint8_t> color;
  };

  struct Entry {
    std::vector<Page> pages;
    std::set<std::uint64_t> keys;
    std::size_t bytes = 0;
  };

  std::uint64_t pageKey(IVec3 pageIndex) const;
  VoxelBounds pageBounds(IVec3 pageIndex) const;
  void preserveRegion(VoxelBounds bounds);
  Page copyPage(IVec3 pageIndex) const;
  void swapEntryWithLive(Entry& entry);
  void enforceBudget();

  SdfVolume& volume_;
  std::vector<std::uint8_t>* colors_ = nullptr;
  std::size_t maxBytes_;
  std::size_t totalBytes_ = 0;
  std::vector<Entry> undoStack_;
  std::vector<Entry> redoStack_;
  VoxelBounds lastChangedBounds_{};
};

}  // namespace large::sdf
