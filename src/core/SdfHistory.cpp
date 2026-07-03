#include "core/SdfHistory.h"

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

}  // namespace

SdfHistory::SdfHistory(SdfVolume& volume, std::size_t maxBytes)
    : volume_(volume), maxBytes_(std::max<std::size_t>(maxBytes, 1u << 20)) {
  volume_.setEditObserver(this);
}

SdfHistory::~SdfHistory() {
  volume_.setEditObserver(nullptr);
}

void SdfHistory::attachColors(std::vector<std::uint8_t>* colors) {
  colors_ = colors;
}

std::uint64_t SdfHistory::pageKey(IVec3 pageIndex) const {
  return (static_cast<std::uint64_t>(pageIndex.z) << 42) | (static_cast<std::uint64_t>(pageIndex.y) << 21) |
         static_cast<std::uint64_t>(pageIndex.x);
}

VoxelBounds SdfHistory::pageBounds(IVec3 pageIndex) const {
  const IVec3 size = volume_.size();
  VoxelBounds bounds{};
  bounds.valid = true;
  bounds.min = {pageIndex.x * kPageSize, pageIndex.y * kPageSize, pageIndex.z * kPageSize};
  bounds.max = {
      std::min(bounds.min.x + kPageSize - 1, size.x - 1),
      std::min(bounds.min.y + kPageSize - 1, size.y - 1),
      std::min(bounds.min.z + kPageSize - 1, size.z - 1),
  };
  return bounds;
}

SdfHistory::Page SdfHistory::copyPage(IVec3 pageIndex) const {
  Page page;
  page.pageIndex = pageIndex;
  page.bounds = pageBounds(pageIndex);
  const int width = page.bounds.max.x - page.bounds.min.x + 1;
  const int height = page.bounds.max.y - page.bounds.min.y + 1;
  const int depth = page.bounds.max.z - page.bounds.min.z + 1;

  page.sdf.resize(static_cast<std::size_t>(width) * height * depth);
  const float* values = volume_.values().data();
  for (int z = 0; z < depth; ++z) {
    for (int y = 0; y < height; ++y) {
      const std::size_t from = volume_.index(page.bounds.min.x, page.bounds.min.y + y, page.bounds.min.z + z);
      const std::size_t to =
          (static_cast<std::size_t>(z) * height + static_cast<std::size_t>(y)) * static_cast<std::size_t>(width);
      std::copy_n(values + from, static_cast<std::size_t>(width), page.sdf.data() + to);
    }
  }

  if (colors_ != nullptr && colors_->size() == volume_.values().size() * 4) {
    page.color.resize(page.sdf.size() * 4);
    const std::uint8_t* colors = colors_->data();
    for (int z = 0; z < depth; ++z) {
      for (int y = 0; y < height; ++y) {
        const std::size_t from =
            volume_.index(page.bounds.min.x, page.bounds.min.y + y, page.bounds.min.z + z) * 4;
        const std::size_t to =
            (static_cast<std::size_t>(z) * height + static_cast<std::size_t>(y)) * static_cast<std::size_t>(width) *
            4;
        std::copy_n(colors + from, static_cast<std::size_t>(width) * 4, page.color.data() + to);
      }
    }
  }
  return page;
}

void SdfHistory::onBeforeEdit(const SdfVolume&, VoxelBounds bounds) {
  preserveRegion(bounds);
}

void SdfHistory::preserveColorRegion(VoxelBounds bounds) {
  preserveRegion(bounds);
}

void SdfHistory::preserveRegion(VoxelBounds bounds) {
  if (!bounds.valid || undoStack_.empty()) {
    return;
  }

  Entry& entry = undoStack_.back();
  const IVec3 minPage{bounds.min.x / kPageSize, bounds.min.y / kPageSize, bounds.min.z / kPageSize};
  const IVec3 maxPage{bounds.max.x / kPageSize, bounds.max.y / kPageSize, bounds.max.z / kPageSize};
  for (int pz = minPage.z; pz <= maxPage.z; ++pz) {
    for (int py = minPage.y; py <= maxPage.y; ++py) {
      for (int px = minPage.x; px <= maxPage.x; ++px) {
        const IVec3 pageIndex{px, py, pz};
        const std::uint64_t key = pageKey(pageIndex);
        if (entry.keys.find(key) != entry.keys.end()) {
          continue;
        }
        Page page = copyPage(pageIndex);
        const std::size_t bytes = page.sdf.size() * sizeof(float) + page.color.size();
        entry.bytes += bytes;
        totalBytes_ += bytes;
        entry.keys.insert(key);
        entry.pages.push_back(std::move(page));
      }
    }
  }
  enforceBudget();
}

void SdfHistory::capture() {
  for (Entry& entry : redoStack_) {
    totalBytes_ -= entry.bytes;
  }
  redoStack_.clear();
  undoStack_.emplace_back();
  enforceBudget();
}

void SdfHistory::enforceBudget() {
  // Drop the oldest undo entries first; the entry currently being written
  // (back of the stack) is always kept, even if it alone exceeds the budget.
  while (totalBytes_ > maxBytes_ && undoStack_.size() > 1) {
    totalBytes_ -= undoStack_.front().bytes;
    undoStack_.erase(undoStack_.begin());
  }
}

void SdfHistory::swapEntryWithLive(Entry& entry) {
  lastChangedBounds_ = {};
  float* values = volume_.values_.data();  // friend access
  for (Page& page : entry.pages) {
    const int width = page.bounds.max.x - page.bounds.min.x + 1;
    const int height = page.bounds.max.y - page.bounds.min.y + 1;
    const int depth = page.bounds.max.z - page.bounds.min.z + 1;
    for (int z = 0; z < depth; ++z) {
      for (int y = 0; y < height; ++y) {
        const std::size_t live = volume_.index(page.bounds.min.x, page.bounds.min.y + y, page.bounds.min.z + z);
        const std::size_t saved =
            (static_cast<std::size_t>(z) * height + static_cast<std::size_t>(y)) * static_cast<std::size_t>(width);
        std::swap_ranges(page.sdf.data() + saved, page.sdf.data() + saved + width, values + live);
        if (!page.color.empty() && colors_ != nullptr) {
          std::swap_ranges(page.color.data() + saved * 4, page.color.data() + (saved + width) * 4,
                           colors_->data() + live * 4);
        }
      }
    }
    volume_.markDirtyBounds(page.bounds.min.x, page.bounds.min.y, page.bounds.min.z, page.bounds.max.x,
                            page.bounds.max.y, page.bounds.max.z);
    lastChangedBounds_ = mergeBounds(lastChangedBounds_, page.bounds);
  }
}

bool SdfHistory::undo() {
  // Skip entries that never received an edit (capture without stroke).
  while (!undoStack_.empty() && undoStack_.back().pages.empty()) {
    undoStack_.pop_back();
  }
  if (undoStack_.empty()) {
    return false;
  }

  Entry entry = std::move(undoStack_.back());
  undoStack_.pop_back();
  swapEntryWithLive(entry);  // the entry now holds the pre-undo (redo) data
  redoStack_.push_back(std::move(entry));
  return true;
}

bool SdfHistory::redo() {
  if (redoStack_.empty()) {
    return false;
  }

  Entry entry = std::move(redoStack_.back());
  redoStack_.pop_back();
  swapEntryWithLive(entry);
  undoStack_.push_back(std::move(entry));
  return true;
}

void SdfHistory::clear() {
  undoStack_.clear();
  redoStack_.clear();
  totalBytes_ = 0;
  lastChangedBounds_ = {};
}

}  // namespace large::sdf
