#pragma once

// CPU-side painter for the left-wrist HUD/menu panel.
//
// The panel is painted into an RGBA8 buffer (real text included, via an
// embedded 5x7 bitmap font), uploaded to a 2D texture only when the HUD
// state changes, then sampled by the UI overlay shader on the panel plane.
// This replaces the per-glyph shader branches that made text in the HUD
// nearly impossible to maintain.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace large::hud {

// Panel pixel layout, shared by the painter and the C++ ray hit-testing so
// what is drawn is exactly what is clickable. The menu is paged: a header
// (always visible), a tab bar and one page at a time, all with large
// finger-friendly controls.
constexpr int kContentWidth = 320;
constexpr int kContentHeight = 544;       // full texture height (menu open)
constexpr int kHeaderVisibleHeight = 72;  // visible height when the menu is closed
constexpr float kPanelWidthMeters = 0.38f;
constexpr int kPanelCornerRadius = 14;

// Header: active tool chip + MENU button.
constexpr int kHeaderChipLeft = 8;
constexpr int kHeaderChipTop = 10;
constexpr int kHeaderChipRight = 214;
constexpr int kHeaderChipBottom = 62;
constexpr int kMenuButtonLeft = 222;
constexpr int kMenuButtonTop = 10;
constexpr int kMenuButtonRight = 312;
constexpr int kMenuButtonBottom = 62;

// Tab bar: OUTILS / FICHIERS / COULEUR / PINCEAU.
constexpr int kTabCount = 4;
constexpr int kTabTop = 78;
constexpr int kTabHeight = 46;
constexpr int kTabFirstLeft = 8;
constexpr int kTabPitch = 77;
constexpr int kTabWidth = 73;

// Page TOOLS: 2x4 grid of large buttons.
constexpr int kToolGridLeft = 8;
constexpr int kToolGridTop = 136;
constexpr int kToolButtonWidth = 148;
constexpr int kToolButtonHeight = 94;
constexpr int kToolGridPitchX = 156;
constexpr int kToolGridPitchY = 102;

// Page FILES: full-width rows (SAVE/LOAD/EXPORT/AR/LOCK/EXIT).
constexpr int kFileRowCount = 6;
constexpr int kFileRowLeft = 8;
constexpr int kFileRowRight = 312;
constexpr int kFileRowTop = 140;
constexpr int kFileRowHeight = 56;
constexpr int kFileRowPitch = 66;

// Page COLOR: preview bar, saturation/value square, hue bar.
constexpr int kColorPreviewLeft = 8;
constexpr int kColorPreviewTop = 140;
constexpr int kColorPreviewRight = 312;
constexpr int kColorPreviewBottom = 176;
constexpr int kSvLeft = 8;
constexpr int kSvTop = 188;
constexpr int kSvSize = 240;
constexpr int kHueLeft = 262;
constexpr int kHueTop = 188;
constexpr int kHueWidth = 50;
constexpr int kHueHeight = 240;

// Page BRUSH: large sliders + mirror toggle + brush preview.
constexpr int kSliderLeft = 8;
constexpr int kSliderRight = 312;
constexpr int kSizeSliderTop = 168;
constexpr int kPowerSliderTop = 256;
constexpr int kSliderHeight = 44;
constexpr int kMirrorButtonTop = 330;
constexpr int kMirrorButtonBottom = 386;
constexpr int kBrushPreviewCenterY = 466;

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = 6;

struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 255;
};

inline std::array<std::uint8_t, kGlyphHeight> glyphRows(char c) {
  switch (c) {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case 'J': return {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x0A, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E};
    case '6': return {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C};
    case '%': return {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C};
    case ':': return {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00};
    case '-': return {0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00};
    case '/': return {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10};
    default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  }
}

class Painter {
 public:
  Painter(int width, int height) : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4, 0) {}

  int width() const { return width_; }
  int height() const { return height_; }
  const std::uint8_t* pixels() const { return pixels_.data(); }

  void clear() { std::memset(pixels_.data(), 0, pixels_.size()); }

  void blendPixel(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || c.a == 0) {
      return;
    }
    std::uint8_t* p = pixels_.data() + (static_cast<std::size_t>(y) * width_ + x) * 4;
    const int srcA = c.a;
    const int dstA = p[3];
    const int outA = srcA + dstA * (255 - srcA) / 255;
    if (outA <= 0) {
      p[0] = p[1] = p[2] = p[3] = 0;
      return;
    }
    p[0] = static_cast<std::uint8_t>((c.r * srcA + p[0] * dstA * (255 - srcA) / 255) / outA);
    p[1] = static_cast<std::uint8_t>((c.g * srcA + p[1] * dstA * (255 - srcA) / 255) / outA);
    p[2] = static_cast<std::uint8_t>((c.b * srcA + p[2] * dstA * (255 - srcA) / 255) / outA);
    p[3] = static_cast<std::uint8_t>(outA);
  }

  void fillRect(int x0, int y0, int x1, int y1, Color c) {
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        blendPixel(x, y, c);
      }
    }
  }

  void outlineRect(int x0, int y0, int x1, int y1, int border, Color c) {
    fillRect(x0, y0, x1, y0 + border, c);
    fillRect(x0, y1 - border, x1, y1, c);
    fillRect(x0, y0 + border, x0 + border, y1 - border, c);
    fillRect(x1 - border, y0 + border, x1, y1 - border, c);
  }

  void fillCircle(int cx, int cy, int radius, Color c) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
      for (int x = cx - radius; x <= cx + radius; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= radius * radius) {
          blendPixel(x, y, c);
        }
      }
    }
  }

  void fillRoundedRect(int x0, int y0, int x1, int y1, int radius, Color c) {
    const float halfW = (x1 - x0) * 0.5f - radius;
    const float halfH = (y1 - y0) * 0.5f - radius;
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        const float dx = std::max(std::abs(x + 0.5f - cx) - halfW, 0.0f);
        const float dy = std::max(std::abs(y + 0.5f - cy) - halfH, 0.0f);
        if (dx * dx + dy * dy <= static_cast<float>(radius) * static_cast<float>(radius)) {
          blendPixel(x, y, c);
        }
      }
    }
  }

  void outlineRoundedRect(int x0, int y0, int x1, int y1, int radius, int border, Color c) {
    const float halfW = (x1 - x0) * 0.5f - radius;
    const float halfH = (y1 - y0) * 0.5f - radius;
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        const float dx = std::max(std::abs(x + 0.5f - cx) - halfW, 0.0f);
        const float dy = std::max(std::abs(y + 0.5f - cy) - halfH, 0.0f);
        const float d = std::sqrt(dx * dx + dy * dy) - static_cast<float>(radius);
        if (d <= 0.0f && d >= -static_cast<float>(border)) {
          blendPixel(x, y, c);
        }
      }
    }
  }

  void ring(int cx, int cy, int outerRadius, int thickness, Color c) {
    const int innerRadius = outerRadius - thickness;
    for (int y = cy - outerRadius; y <= cy + outerRadius; ++y) {
      for (int x = cx - outerRadius; x <= cx + outerRadius; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        const int d2 = dx * dx + dy * dy;
        if (d2 <= outerRadius * outerRadius && d2 >= innerRadius * innerRadius) {
          blendPixel(x, y, c);
        }
      }
    }
  }

  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, Color c) {
    const int minX = std::min(x0, std::min(x1, x2));
    const int maxX = std::max(x0, std::max(x1, x2));
    const int minY = std::min(y0, std::min(y1, y2));
    const int maxY = std::max(y0, std::max(y1, y2));
    const auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
      return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };
    for (int y = minY; y <= maxY; ++y) {
      for (int x = minX; x <= maxX; ++x) {
        const int e0 = edge(x0, y0, x1, y1, x, y);
        const int e1 = edge(x1, y1, x2, y2, x, y);
        const int e2 = edge(x2, y2, x0, y0, x, y);
        if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
          blendPixel(x, y, c);
        }
      }
    }
  }

  static int textWidth(const char* text, int scale) {
    int count = 0;
    while (text[count] != '\0') {
      ++count;
    }
    return count > 0 ? (count * kGlyphAdvance - 1) * scale : 0;
  }

  void drawText(int x, int y, const char* text, int scale, Color c) {
    int penX = x;
    for (const char* s = text; *s != '\0'; ++s) {
      const std::array<std::uint8_t, kGlyphHeight> rows = glyphRows(*s);
      for (int row = 0; row < kGlyphHeight; ++row) {
        for (int col = 0; col < kGlyphWidth; ++col) {
          if ((rows[static_cast<std::size_t>(row)] & (1 << (kGlyphWidth - 1 - col))) == 0) {
            continue;
          }
          fillRect(penX + col * scale, y + row * scale, penX + (col + 1) * scale, y + (row + 1) * scale, c);
        }
      }
      penX += kGlyphAdvance * scale;
    }
  }

  void drawTextCentered(int centerX, int y, const char* text, int scale, Color c) {
    drawText(centerX - textWidth(text, scale) / 2, y, text, scale, c);
  }

 private:
  int width_ = 0;
  int height_ = 0;
  std::vector<std::uint8_t> pixels_;
};

}  // namespace large::hud
