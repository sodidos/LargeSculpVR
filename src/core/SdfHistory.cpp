#include "core/SdfHistory.h"

#include <algorithm>
#include <utility>

namespace large::sdf {

SdfHistory::SdfHistory(SdfVolume& volume, std::size_t maxSnapshots)
    : volume_(volume), maxSnapshots_(std::max<std::size_t>(1, maxSnapshots)) {}

void SdfHistory::attachColors(std::vector<std::uint8_t>* colors) {
  colors_ = colors;
}

SdfHistory::Snapshot SdfHistory::makeSnapshot() const {
  Snapshot snapshot;
  snapshot.values = volume_.values();
  if (colors_ != nullptr && !colors_->empty()) {
    snapshot.colors = *colors_;
  }
  return snapshot;
}

void SdfHistory::restoreSnapshot(Snapshot snapshot) {
  volume_.restoreValues(std::move(snapshot.values));
  // Snapshots taken before a color buffer existed carry no colors; in that
  // case the current paint is left untouched.
  if (colors_ != nullptr && snapshot.colors.size() == colors_->size()) {
    *colors_ = std::move(snapshot.colors);
  }
}

void SdfHistory::capture() {
  undoStack_.push_back(makeSnapshot());
  if (undoStack_.size() > maxSnapshots_) {
    undoStack_.erase(undoStack_.begin());
  }
  redoStack_.clear();
}

bool SdfHistory::undo() {
  if (undoStack_.empty()) {
    return false;
  }

  redoStack_.push_back(makeSnapshot());
  Snapshot previous = std::move(undoStack_.back());
  undoStack_.pop_back();
  restoreSnapshot(std::move(previous));
  return true;
}

bool SdfHistory::redo() {
  if (redoStack_.empty()) {
    return false;
  }

  undoStack_.push_back(makeSnapshot());
  Snapshot next = std::move(redoStack_.back());
  redoStack_.pop_back();
  restoreSnapshot(std::move(next));
  return true;
}

void SdfHistory::clear() {
  undoStack_.clear();
  redoStack_.clear();
}

}  // namespace large::sdf
