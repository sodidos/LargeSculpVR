#include "core/SparseSdfHistory.h"

#include <algorithm>
#include <utility>

namespace large::sdf {

namespace {

VoxelBounds mergeBounds(VoxelBounds a, VoxelBounds b) {
  if (!b.valid) {
    return a;
  }
  if (!a.valid) {
    return b;
  }
  a.min.x = std::min(a.min.x, b.min.x);
  a.min.y = std::min(a.min.y, b.min.y);
  a.min.z = std::min(a.min.z, b.min.z);
  a.max.x = std::max(a.max.x, b.max.x);
  a.max.y = std::max(a.max.y, b.max.y);
  a.max.z = std::max(a.max.z, b.max.z);
  return a;
}

constexpr int kBrick = SparseSdfVolume::kBrickSize;

}  // namespace

SparseSdfHistory::SparseSdfHistory(SparseSdfVolume& volume, std::size_t maxBytes)
    : volume_(volume), maxBytes_(std::max<std::size_t>(maxBytes, 1u << 20)) {
  volume_.setEditObserver(this);
}

SparseSdfHistory::~SparseSdfHistory() {
  volume_.setEditObserver(nullptr);
}

std::uint64_t SparseSdfHistory::brickKey(IVec3 index) {
  return (static_cast<std::uint64_t>(index.z) << 42) | (static_cast<std::uint64_t>(index.y) << 21) |
         static_cast<std::uint64_t>(index.x);
}

SparseSdfHistory::BrickSnapshot SparseSdfHistory::captureBrick(IVec3 index) const {
  BrickSnapshot snapshot;
  snapshot.index = index;
  const float* data = volume_.brickData(index);
  if (data != nullptr) {
    snapshot.present = true;
    snapshot.values.assign(data, data + static_cast<std::size_t>(kBrick) * kBrick * kBrick);
  }
  return snapshot;
}

void SparseSdfHistory::onBeforeSparseEdit(const SparseSdfVolume&, VoxelBounds bounds) {
  if (!bounds.valid || undoStack_.empty()) {
    return;
  }

  Entry& entry = undoStack_.back();
  IVec3 minBrick{};
  const IVec3 count = volume_.brickCountForBounds(bounds, minBrick);
  for (int bz = 0; bz < count.z; ++bz) {
    for (int by = 0; by < count.y; ++by) {
      for (int bx = 0; bx < count.x; ++bx) {
        const IVec3 index{minBrick.x + bx, minBrick.y + by, minBrick.z + bz};
        const std::uint64_t key = brickKey(index);
        if (entry.keys.find(key) != entry.keys.end()) {
          continue;
        }
        BrickSnapshot snapshot = captureBrick(index);
        const std::size_t bytes = snapshot.values.size() * sizeof(float) + sizeof(BrickSnapshot);
        entry.bytes += bytes;
        totalBytes_ += bytes;
        entry.keys.insert(key);
        entry.bricks.push_back(std::move(snapshot));
      }
    }
  }
  enforceBudget();
}

void SparseSdfHistory::capture() {
  for (Entry& entry : redoStack_) {
    totalBytes_ -= entry.bytes;
  }
  redoStack_.clear();
  undoStack_.emplace_back();
  enforceBudget();
}

void SparseSdfHistory::enforceBudget() {
  while (totalBytes_ > maxBytes_ && undoStack_.size() > 1) {
    totalBytes_ -= undoStack_.front().bytes;
    undoStack_.erase(undoStack_.begin());
  }
}

void SparseSdfHistory::swapEntryWithLive(Entry& entry) {
  lastChangedBounds_ = {};
  for (BrickSnapshot& snapshot : entry.bricks) {
    // Read the current state for the opposite-direction entry, then restore.
    BrickSnapshot current = captureBrick(snapshot.index);
    volume_.restoreBrick(snapshot.index, snapshot.present ? snapshot.values.data() : nullptr);
    snapshot = std::move(current);

    VoxelBounds brickBounds{};
    brickBounds.valid = true;
    brickBounds.min = {snapshot.index.x * kBrick, snapshot.index.y * kBrick, snapshot.index.z * kBrick};
    brickBounds.max = {brickBounds.min.x + kBrick - 1, brickBounds.min.y + kBrick - 1,
                       brickBounds.min.z + kBrick - 1};
    lastChangedBounds_ = mergeBounds(lastChangedBounds_, brickBounds);
  }
}

bool SparseSdfHistory::undo() {
  while (!undoStack_.empty() && undoStack_.back().bricks.empty()) {
    undoStack_.pop_back();
  }
  if (undoStack_.empty()) {
    return false;
  }

  Entry entry = std::move(undoStack_.back());
  undoStack_.pop_back();
  swapEntryWithLive(entry);  // entry now holds the pre-undo (redo) state
  redoStack_.push_back(std::move(entry));
  return true;
}

bool SparseSdfHistory::redo() {
  if (redoStack_.empty()) {
    return false;
  }

  Entry entry = std::move(redoStack_.back());
  redoStack_.pop_back();
  swapEntryWithLive(entry);
  undoStack_.push_back(std::move(entry));
  return true;
}

void SparseSdfHistory::clear() {
  undoStack_.clear();
  redoStack_.clear();
  totalBytes_ = 0;
  lastChangedBounds_ = {};
}

}  // namespace large::sdf
