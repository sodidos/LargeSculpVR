#include "core/SdfHistory.h"

#include <algorithm>
#include <utility>

namespace large::sdf {

SdfHistory::SdfHistory(SdfVolume& volume, std::size_t maxSnapshots)
    : volume_(volume), maxSnapshots_(std::max<std::size_t>(1, maxSnapshots)) {}

void SdfHistory::capture() {
  undoStack_.push_back(volume_.values());
  if (undoStack_.size() > maxSnapshots_) {
    undoStack_.erase(undoStack_.begin());
  }
  redoStack_.clear();
}

bool SdfHistory::undo() {
  if (undoStack_.empty()) {
    return false;
  }

  redoStack_.push_back(volume_.values());
  auto previous = std::move(undoStack_.back());
  undoStack_.pop_back();
  volume_.restoreValues(std::move(previous));
  return true;
}

bool SdfHistory::redo() {
  if (redoStack_.empty()) {
    return false;
  }

  undoStack_.push_back(volume_.values());
  auto next = std::move(redoStack_.back());
  redoStack_.pop_back();
  volume_.restoreValues(std::move(next));
  return true;
}

void SdfHistory::clear() {
  undoStack_.clear();
  redoStack_.clear();
}

}  // namespace large::sdf
