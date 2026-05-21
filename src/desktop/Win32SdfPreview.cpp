#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/ObjExporter.h"
#include "core/SdfHistory.h"
#include "core/SdfVolume.h"
#include "core/SurfaceMesh.h"

using large::sdf::BrushMode;
using large::sdf::IVec3;
using large::sdf::ObjExportStats;
using large::sdf::SdfHistory;
using large::sdf::SdfVolume;
using large::sdf::SurfaceMesh;
using large::sdf::SurfaceTriangle;
using large::sdf::Vec3;
using large::sdf::buildSurfaceMesh;
using large::sdf::clamp;
using large::sdf::clampInt;
using large::sdf::dot;
using large::sdf::exportSdfSurfaceAsObj;
using large::sdf::normalize;

namespace {

constexpr int kWindowWidth = 1100;
constexpr int kWindowHeight = 850;
constexpr int kCanvasMargin = 24;
constexpr int kPanelWidth = 310;
constexpr int kViewportGap = 16;

enum class PaintTarget {
  None,
  Slice2d,
  Surface3d,
};

struct AppState {
  AppState()
      : volume({72, 72, 72}, 0.04f, {-1.44f, -1.44f, -1.44f}, 10.0f),
        history(volume, 48) {
    reset(false);
  }

  void reset(bool capture) {
    if (capture) {
      history.capture();
    }
    volume.fillSphere({0.0f, 0.0f, 0.0f}, 0.58f);
    sliceZ = volume.size().z / 2;
    status = L"Sphere SDF initialisee";
    markSurfaceDirty();
  }

  void markSurfaceDirty() {
    surfaceDirty = true;
  }

  void ensureSurface(bool allowRebuild) {
    if (!surfaceDirty) {
      return;
    }
    if (!allowRebuild && !surface.triangles.empty()) {
      return;
    }
    surface = buildSurfaceMesh(volume);
    surfaceDirty = false;
  }

  void changeResolution(int delta) {
    const int nextIndex = clampInt(resolutionIndex + delta, 0, static_cast<int>(resolutionPresets.size()) - 1);
    if (nextIndex == resolutionIndex) {
      return;
    }
    resolutionIndex = nextIndex;
    volume = volume.resampled(resolutionPresets[resolutionIndex]);
    history.clear();
    sliceZ = volume.size().z / 2;
    markSurfaceDirty();

    std::wstringstream ss;
    ss << L"Precision voxel: " << volume.size().x << L"^3";
    status = ss.str();
  }

  SdfVolume volume;
  SdfHistory history;
  SurfaceMesh surface;
  bool surfaceDirty = true;
  std::vector<int> resolutionPresets{48, 72, 96};
  int resolutionIndex = 1;
  BrushMode mode = BrushMode::Add;
  float brushRadius = 0.18f;
  float smoothStrength = 0.35f;
  int sliceZ = 36;
  bool painting = false;
  PaintTarget paintTarget = PaintTarget::None;
  bool rotatingView = false;
  POINT mouse{0, 0};
  POINT lastMouse{0, 0};
  bool mouseInside = false;
  bool brushHitValid = false;
  Vec3 brushHit{};
  float viewYaw = 0.70f;
  float viewPitch = -0.35f;
  float viewZoom = 1.0f;
  bool showWire = false;
  std::wstring status;
};

std::unique_ptr<AppState> gApp;

int leftAreaWidth(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int clientWidth = static_cast<int>(rc.right - rc.left);
  return std::max(240, clientWidth - kPanelWidth - kCanvasMargin * 3);
}

int panelX(HWND hwnd) {
  return kCanvasMargin + leftAreaWidth(hwnd) + kCanvasMargin;
}

int slicePanelSize(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int clientHeight = static_cast<int>(rc.bottom - rc.top);
  return std::max(120, std::min(180, clientHeight / 4));
}

int canvasSize(HWND hwnd) {
  return std::min(leftAreaWidth(hwnd), slicePanelSize(hwnd));
}

RECT view3dRect(HWND hwnd);

RECT canvasRect(HWND hwnd) {
  const int size = canvasSize(hwnd);
  const RECT view = view3dRect(hwnd);
  const int top = view.bottom + kViewportGap;
  return {kCanvasMargin, top, kCanvasMargin + size, top + size};
}

RECT view3dRect(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int clientBottom = static_cast<int>(rc.bottom);
  const int left = kCanvasMargin;
  const int right = kCanvasMargin + leftAreaWidth(hwnd);
  const int top = kCanvasMargin;
  const int bottom = std::max(top + 220, clientBottom - kCanvasMargin - slicePanelSize(hwnd) - kViewportGap);
  return {left, top, right, bottom};
}

COLORREF voxelColor(float value) {
  if (value < 0.0f) {
    const float shade = clamp((-value) / 0.35f, 0.0f, 1.0f);
    const int c = static_cast<int>(200.0f - shade * 90.0f);
    return RGB(c, c + 6, std::min(255, c + 22));
  }

  const float nearSurface = clamp(1.0f - value / 0.10f, 0.0f, 1.0f);
  const int base = static_cast<int>(24.0f + nearSurface * 46.0f);
  return RGB(base, base, base + 4);
}

std::wstring modeName(BrushMode mode) {
  if (mode == BrushMode::Add) {
    return L"Add";
  }
  if (mode == BrushMode::Subtract) {
    return L"Subtract";
  }
  return L"Smooth";
}

bool screenToWorld(HWND hwnd, int px, int py, Vec3& out) {
  const RECT canvas = canvasRect(hwnd);
  if (px < canvas.left || px >= canvas.right || py < canvas.top || py >= canvas.bottom) {
    return false;
  }

  const IVec3 size = gApp->volume.size();
  const Vec3 origin = gApp->volume.origin();
  const float worldW = static_cast<float>(size.x) * gApp->volume.voxelSize();
  const float worldH = static_cast<float>(size.y) * gApp->volume.voxelSize();
  const float u = static_cast<float>(px - canvas.left) / static_cast<float>(canvas.right - canvas.left);
  const float v = static_cast<float>(py - canvas.top) / static_cast<float>(canvas.bottom - canvas.top);

  out.x = origin.x + u * worldW;
  out.y = origin.y + (1.0f - v) * worldH;
  out.z = origin.z + (static_cast<float>(gApp->sliceZ) + 0.5f) * gApp->volume.voxelSize();
  return true;
}

void applyBrushAtWorld(HWND hwnd, Vec3 p);

void applyBrushAt(HWND hwnd, int px, int py) {
  Vec3 p{};
  if (!screenToWorld(hwnd, px, py, p)) {
    return;
  }
  applyBrushAtWorld(hwnd, p);
  gApp->status = modeName(gApp->mode) + L" applique sur coupe";
}

void drawTextLine(HDC dc, int x, int& y, const std::wstring& text) {
  TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
  y += 22;
}

void drawPanel(HWND hwnd, HDC dc) {
  int x = panelX(hwnd);
  int y = kCanvasMargin + 2;

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(230, 232, 238));

  HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  SelectObject(dc, font);

  drawTextLine(dc, x, y, L"Large SDF Preview");
  y += 10;

  std::wstringstream mode;
  mode << L"Outil: " << modeName(gApp->mode);
  drawTextLine(dc, x, y, mode.str());

  std::wstringstream radius;
  radius << L"Taille pinceau: " << static_cast<int>(gApp->brushRadius * 100.0f) << L" cm";
  drawTextLine(dc, x, y, radius.str());

  std::wstringstream strength;
  strength << L"Force smooth: " << static_cast<int>(gApp->smoothStrength * 100.0f) << L"%";
  drawTextLine(dc, x, y, strength.str());

  std::wstringstream slice;
  slice << L"Profondeur slice Z: " << gApp->sliceZ << L" / " << (gApp->volume.size().z - 1);
  drawTextLine(dc, x, y, slice.str());

  std::wstringstream precision;
  precision << L"Precision voxel: " << gApp->volume.size().x << L"^3, "
            << static_cast<int>(gApp->volume.voxelSize() * 1000.0f) << L" mm";
  drawTextLine(dc, x, y, precision.str());

  std::wstringstream view;
  view << L"Zoom vue: " << static_cast<int>(gApp->viewZoom * 100.0f) << L"%";
  drawTextLine(dc, x, y, view.str());

  std::wstringstream solid;
  solid << L"Voxels solides: " << gApp->volume.countSolidVoxels();
  drawTextLine(dc, x, y, solid.str());

  std::wstringstream history;
  history << L"Undo/Redo: " << gApp->history.undoCount() << L" / " << gApp->history.redoCount();
  drawTextLine(dc, x, y, history.str());

  y += 14;
  drawTextLine(dc, x, y, L"Commandes");
  y += 6;
  drawTextLine(dc, x, y, L"Clic gauche vue 3D: sculpter surface");
  drawTextLine(dc, x, y, L"Clic gauche coupe: sculpter slice");
  drawTextLine(dc, x, y, L"Clic droit + glisser: tourner vue 3D");
  drawTextLine(dc, x, y, L"Ctrl + clic droit: zoom vue 3D");
  drawTextLine(dc, x, y, L"Molette: taille pinceau");
  drawTextLine(dc, x, y, L"PgUp/PgDn: profondeur");
  drawTextLine(dc, x, y, L"1: Add");
  drawTextLine(dc, x, y, L"2: Subtract");
  drawTextLine(dc, x, y, L"3: Smooth");
  drawTextLine(dc, x, y, L"4/5: precision voxel -/+");
  drawTextLine(dc, x, y, L"+/- pave num: force smooth");
  drawTextLine(dc, x, y, L"Z: Undo");
  drawTextLine(dc, x, y, L"Y: Redo");
  drawTextLine(dc, x, y, L"W: filaire surface");
  drawTextLine(dc, x, y, L"P: exporter OBJ lisse");
  drawTextLine(dc, x, y, L"R: reset sphere");
  drawTextLine(dc, x, y, L"Echap: quitter");

  y += 14;
  drawTextLine(dc, x, y, L"VR mapping");
  y += 6;
  drawTextLine(dc, x, y, L"Gachette: sculpter");
  drawTextLine(dc, x, y, L"Joystick Y: taille");
  drawTextLine(dc, x, y, L"A/B: outil");
  drawTextLine(dc, x, y, L"Joystick main gauche: undo/redo");

  y += 14;
  drawTextLine(dc, x, y, gApp->status);
}

bool isSurfaceVoxel(const SdfVolume& volume, int x, int y, int z) {
  if (!volume.isSolid(x, y, z)) {
    return false;
  }
  return !volume.isSolid(x - 1, y, z) || !volume.isSolid(x + 1, y, z) ||
         !volume.isSolid(x, y - 1, z) || !volume.isSolid(x, y + 1, z) ||
         !volume.isSolid(x, y, z - 1) || !volume.isSolid(x, y, z + 1);
}

Vec3 rotateForView(Vec3 v, float yaw, float pitch) {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);

  const float x1 = cy * v.x + sy * v.z;
  const float z1 = -sy * v.x + cy * v.z;
  const float y1 = v.y;

  return {x1, cp * y1 - sp * z1, sp * y1 + cp * z1};
}

Vec3 unrotateFromView(Vec3 v, float yaw, float pitch) {
  const float cy = std::cos(yaw);
  const float sy = std::sin(yaw);
  const float cp = std::cos(pitch);
  const float sp = std::sin(pitch);

  const float x1 = v.x;
  const float y1 = cp * v.y + sp * v.z;
  const float z1 = -sp * v.y + cp * v.z;

  return {cy * x1 - sy * z1, y1, sy * x1 + cy * z1};
}

float viewScale(HWND hwnd) {
  const RECT rect = view3dRect(hwnd);
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  return static_cast<float>(std::min(w, h)) / (gApp->volume.worldExtent() * 1.28f) * gApp->viewZoom;
}

POINT projectWorldTo3d(HWND hwnd, Vec3 world) {
  const RECT rect = view3dRect(hwnd);
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  const int cx = rect.left + w / 2;
  const int cy = rect.top + h / 2;
  const Vec3 view = rotateForView(world - gApp->volume.boundsCenter(), gApp->viewYaw, gApp->viewPitch);
  const float scale = viewScale(hwnd);
  return {cx + static_cast<int>(std::round(view.x * scale)),
          cy - static_cast<int>(std::round(view.y * scale))};
}

bool raycastSurface(HWND hwnd, int px, int py, Vec3& hit) {
  const RECT rect = view3dRect(hwnd);
  if (px < rect.left || px >= rect.right || py < rect.top || py >= rect.bottom) {
    return false;
  }

  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  const int cx = rect.left + w / 2;
  const int cy = rect.top + h / 2;
  const float scale = viewScale(hwnd);
  const float viewX = static_cast<float>(px - cx) / scale;
  const float viewY = static_cast<float>(cy - py) / scale;
  const float rayExtent = gApp->volume.worldExtent() * 0.75f;
  const Vec3 center = gApp->volume.boundsCenter();

  Vec3 previous = center + unrotateFromView({viewX, viewY, rayExtent}, gApp->viewYaw, gApp->viewPitch);
  float previousValue = gApp->volume.sample(previous);
  constexpr int steps = 160;
  for (int i = 1; i <= steps; ++i) {
    const float z = rayExtent - (2.0f * rayExtent) * static_cast<float>(i) / static_cast<float>(steps);
    const Vec3 current = center + unrotateFromView({viewX, viewY, z}, gApp->viewYaw, gApp->viewPitch);
    const float currentValue = gApp->volume.sample(current);
    if (previousValue > 0.0f && currentValue <= 0.0f) {
      const float t = previousValue / (previousValue - currentValue);
      hit = previous + (current - previous) * t;
      return true;
    }
    previous = current;
    previousValue = currentValue;
  }

  return false;
}

void applyBrushAtWorld(HWND hwnd, Vec3 p) {
  if (gApp->mode == BrushMode::Smooth) {
    gApp->volume.applySmoothBrush(p, gApp->brushRadius, gApp->smoothStrength);
  } else {
    gApp->volume.applySphereBrush(p, gApp->brushRadius, gApp->mode);
  }
  gApp->brushHit = p;
  gApp->brushHitValid = true;
  gApp->markSurfaceDirty();
  gApp->status = modeName(gApp->mode) + L" applique en 3D";
  InvalidateRect(hwnd, nullptr, FALSE);
}

bool applyBrushAt3d(HWND hwnd, int px, int py) {
  Vec3 hit{};
  if (!raycastSurface(hwnd, px, py, hit)) {
    gApp->brushHitValid = false;
    return false;
  }
  applyBrushAtWorld(hwnd, hit);
  return true;
}

void draw3dView(HWND hwnd, HDC dc) {
  const RECT rect = view3dRect(hwnd);
  HBRUSH bg = CreateSolidBrush(RGB(13, 15, 20));
  FillRect(dc, &rect, bg);
  DeleteObject(bg);

  HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(82, 92, 108));
  HGDIOBJ oldPen = SelectObject(dc, borderPen);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
  Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(borderPen);

  gApp->ensureSurface(!gApp->painting && !gApp->rotatingView);
  const Vec3 center = gApp->volume.boundsCenter();

  struct RenderPoint {
    POINT p[3];
    float depth;
    int shade;
  };
  std::vector<RenderPoint> triangles;
  triangles.reserve(gApp->surface.triangles.size());

  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  const int cx = rect.left + w / 2;
  const int cy = rect.top + h / 2;
  const float scale = viewScale(hwnd);
  const Vec3 light = normalize({-0.35f, 0.55f, 0.75f});

  for (const SurfaceTriangle& tri : gApp->surface.triangles) {
    const Vec3 av = rotateForView(tri.a - center, gApp->viewYaw, gApp->viewPitch);
    const Vec3 bv = rotateForView(tri.b - center, gApp->viewYaw, gApp->viewPitch);
    const Vec3 cv = rotateForView(tri.c - center, gApp->viewYaw, gApp->viewPitch);
    const Vec3 normalView = normalize(rotateForView(tri.normal, gApp->viewYaw, gApp->viewPitch));

    POINT p[3]{
        {cx + static_cast<int>(std::round(av.x * scale)), cy - static_cast<int>(std::round(av.y * scale))},
        {cx + static_cast<int>(std::round(bv.x * scale)), cy - static_cast<int>(std::round(bv.y * scale))},
        {cx + static_cast<int>(std::round(cv.x * scale)), cy - static_cast<int>(std::round(cv.y * scale))},
    };

    const int minX = std::min(p[0].x, std::min(p[1].x, p[2].x));
    const int maxX = std::max(p[0].x, std::max(p[1].x, p[2].x));
    const int minY = std::min(p[0].y, std::min(p[1].y, p[2].y));
    const int maxY = std::max(p[0].y, std::max(p[1].y, p[2].y));
    if (maxX < rect.left || minX > rect.right || maxY < rect.top || minY > rect.bottom) {
      continue;
    }

    const float lighting = clamp(dot(normalView, light) * 0.65f + normalView.z * 0.25f + 0.45f, 0.0f, 1.0f);
    const int shade = static_cast<int>(lighting * 7.0f);
    triangles.push_back({{p[0], p[1], p[2]}, (av.z + bv.z + cv.z) / 3.0f, shade});
  }

  std::sort(triangles.begin(), triangles.end(), [](const RenderPoint& a, const RenderPoint& b) {
    return a.depth < b.depth;
  });

  HBRUSH brushes[8] = {
      CreateSolidBrush(RGB(56, 62, 75)),
      CreateSolidBrush(RGB(72, 80, 96)),
      CreateSolidBrush(RGB(92, 104, 124)),
      CreateSolidBrush(RGB(116, 132, 156)),
      CreateSolidBrush(RGB(142, 160, 186)),
      CreateSolidBrush(RGB(168, 186, 210)),
      CreateSolidBrush(RGB(195, 212, 232)),
      CreateSolidBrush(RGB(222, 234, 248)),
  };
  HPEN meshPen = CreatePen(PS_SOLID, 1, RGB(22, 24, 30));
  oldPen = SelectObject(dc, gApp->showWire ? static_cast<HGDIOBJ>(meshPen) : GetStockObject(NULL_PEN));

  for (const RenderPoint& tri : triangles) {
    HGDIOBJ oldTriangleBrush = SelectObject(dc, brushes[clampInt(tri.shade, 0, 7)]);
    Polygon(dc, const_cast<POINT*>(tri.p), 3);
    SelectObject(dc, oldTriangleBrush);
  }

  SelectObject(dc, oldPen);
  DeleteObject(meshPen);

  for (HBRUSH brush : brushes) {
    DeleteObject(brush);
  }

  if (gApp->brushHitValid) {
    const POINT brush = projectWorldTo3d(hwnd, gApp->brushHit);
    const int radiusPx = static_cast<int>(std::round(gApp->brushRadius * scale));
    COLORREF color = RGB(245, 204, 92);
    if (gApp->mode == BrushMode::Subtract) {
      color = RGB(118, 186, 255);
    } else if (gApp->mode == BrushMode::Smooth) {
      color = RGB(116, 230, 157);
    }
    HPEN brushPen = CreatePen(PS_SOLID, 2, color);
    oldPen = SelectObject(dc, brushPen);
    oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Ellipse(dc, brush.x - radiusPx, brush.y - radiusPx, brush.x + radiusPx, brush.y + radiusPx);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brushPen);
  }

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(190, 198, 214));
  int labelY = rect.top + 8;
  std::wstringstream label;
  label << L"Vue 3D surface - clic gauche sculpte, clic droit tourne - " << gApp->surface.triangles.size()
        << L" tris" << (gApp->surfaceDirty ? L" (maj au relachement)" : L"");
  drawTextLine(dc, rect.left + 10, labelY, label.str());
}

void drawCanvas(HWND hwnd, HDC dc) {
  const RECT canvas = canvasRect(hwnd);
  HBRUSH bg = CreateSolidBrush(RGB(14, 16, 20));
  FillRect(dc, &canvas, bg);
  DeleteObject(bg);

  const IVec3 size = gApp->volume.size();
  const float cell = static_cast<float>(canvas.right - canvas.left) / static_cast<float>(size.x);
  const int z = gApp->sliceZ;

  for (int y = 0; y < size.y; ++y) {
    for (int x = 0; x < size.x; ++x) {
      RECT r{
          canvas.left + static_cast<int>(std::floor(static_cast<float>(x) * cell)),
          canvas.top + static_cast<int>(std::floor(static_cast<float>(size.y - 1 - y) * cell)),
          canvas.left + static_cast<int>(std::ceil(static_cast<float>(x + 1) * cell)),
          canvas.top + static_cast<int>(std::ceil(static_cast<float>(size.y - y) * cell)),
      };
      HBRUSH b = CreateSolidBrush(voxelColor(gApp->volume.value(x, y, z)));
      FillRect(dc, &r, b);
      DeleteObject(b);
    }
  }

  HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(44, 48, 56));
  HGDIOBJ oldPen = SelectObject(dc, gridPen);
  for (int i = 0; i <= size.x; i += 8) {
    const int p = canvas.left + static_cast<int>(std::round(static_cast<float>(i) * cell));
    MoveToEx(dc, p, canvas.top, nullptr);
    LineTo(dc, p, canvas.bottom);
  }
  for (int i = 0; i <= size.y; i += 8) {
    const int p = canvas.top + static_cast<int>(std::round(static_cast<float>(i) * cell));
    MoveToEx(dc, canvas.left, p, nullptr);
    LineTo(dc, canvas.right, p);
  }
  SelectObject(dc, oldPen);
  DeleteObject(gridPen);

  HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(102, 112, 132));
  oldPen = SelectObject(dc, borderPen);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
  Rectangle(dc, canvas.left, canvas.top, canvas.right, canvas.bottom);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(borderPen);

  if (gApp->mouseInside) {
    const int radiusPx = static_cast<int>(std::round(gApp->brushRadius / gApp->volume.voxelSize() * cell));
    COLORREF color = RGB(245, 204, 92);
    if (gApp->mode == BrushMode::Subtract) {
      color = RGB(118, 186, 255);
    } else if (gApp->mode == BrushMode::Smooth) {
      color = RGB(116, 230, 157);
    }
    HPEN brushPen = CreatePen(PS_SOLID, 2, color);
    oldPen = SelectObject(dc, brushPen);
    oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Ellipse(dc,
            gApp->mouse.x - radiusPx,
            gApp->mouse.y - radiusPx,
            gApp->mouse.x + radiusPx,
            gApp->mouse.y + radiusPx);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brushPen);
  }
}

void paint(HWND hwnd) {
  PAINTSTRUCT ps{};
  HDC windowDc = BeginPaint(hwnd, &ps);

  RECT rc{};
  GetClientRect(hwnd, &rc);
  HDC memDc = CreateCompatibleDC(windowDc);
  HBITMAP bitmap = CreateCompatibleBitmap(windowDc, rc.right - rc.left, rc.bottom - rc.top);
  HGDIOBJ oldBitmap = SelectObject(memDc, bitmap);

  HBRUSH bg = CreateSolidBrush(RGB(10, 12, 16));
  FillRect(memDc, &rc, bg);
  DeleteObject(bg);

  drawCanvas(hwnd, memDc);
  draw3dView(hwnd, memDc);
  drawPanel(hwnd, memDc);

  BitBlt(windowDc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, memDc, 0, 0, SRCCOPY);

  SelectObject(memDc, oldBitmap);
  DeleteObject(bitmap);
  DeleteDC(memDc);
  EndPaint(hwnd, &ps);
}

void exportPreview(HWND hwnd) {
  try {
    const auto path = std::filesystem::path("out") / "interactive_sculpt.obj";
    const ObjExportStats stats = exportSdfSurfaceAsObj(gApp->volume, path);
    std::wstringstream ss;
    ss << L"Export OBJ: " << stats.faces << L" faces";
    gApp->status = ss.str();
  } catch (...) {
    gApp->status = L"Export OBJ echoue";
  }
  InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_CREATE:
      gApp = std::make_unique<AppState>();
      return 0;

    case WM_MOUSEMOVE: {
      gApp->mouse.x = GET_X_LPARAM(lParam);
      gApp->mouse.y = GET_Y_LPARAM(lParam);
      if (gApp->rotatingView) {
        const int dx = gApp->mouse.x - gApp->lastMouse.x;
        const int dy = gApp->mouse.y - gApp->lastMouse.y;
        const bool zoomGesture = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (zoomGesture) {
          gApp->viewZoom = clamp(gApp->viewZoom * std::exp(static_cast<float>(-dy) * 0.01f), 0.35f, 3.0f);
          gApp->status = L"Zoom vue 3D";
        } else {
          gApp->viewYaw += static_cast<float>(dx) * 0.01f;
          gApp->viewPitch = clamp(gApp->viewPitch + static_cast<float>(dy) * 0.01f, -1.35f, 1.35f);
        }
        gApp->lastMouse = gApp->mouse;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (gApp->painting) {
        if (gApp->paintTarget == PaintTarget::Surface3d) {
          applyBrushAt3d(hwnd, gApp->mouse.x, gApp->mouse.y);
        } else if (gApp->paintTarget == PaintTarget::Slice2d) {
          applyBrushAt(hwnd, gApp->mouse.x, gApp->mouse.y);
        }
      } else {
        Vec3 hit{};
        gApp->brushHitValid = raycastSurface(hwnd, gApp->mouse.x, gApp->mouse.y, hit);
        if (gApp->brushHitValid) {
          gApp->brushHit = hit;
        }
        Vec3 p{};
        gApp->mouseInside = screenToWorld(hwnd, gApp->mouse.x, gApp->mouse.y, p);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_RBUTTONDOWN:
      SetCapture(hwnd);
      gApp->rotatingView = true;
      gApp->lastMouse.x = GET_X_LPARAM(lParam);
      gApp->lastMouse.y = GET_Y_LPARAM(lParam);
      gApp->status = (GetKeyState(VK_CONTROL) & 0x8000) != 0 ? L"Zoom vue 3D" : L"Rotation vue 3D";
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_RBUTTONUP:
      gApp->rotatingView = false;
      if (!gApp->painting) {
        ReleaseCapture();
      }
      return 0;

    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      if (raycastSurface(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), gApp->brushHit)) {
        gApp->history.capture();
        gApp->painting = true;
        gApp->paintTarget = PaintTarget::Surface3d;
        applyBrushAtWorld(hwnd, gApp->brushHit);
      } else {
        Vec3 p{};
        if (screenToWorld(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), p)) {
          gApp->history.capture();
          gApp->painting = true;
          gApp->paintTarget = PaintTarget::Slice2d;
          applyBrushAtWorld(hwnd, p);
          gApp->status = modeName(gApp->mode) + L" applique sur coupe";
        } else {
          gApp->painting = false;
          gApp->paintTarget = PaintTarget::None;
          ReleaseCapture();
        }
      }
      return 0;

    case WM_LBUTTONUP:
      gApp->painting = false;
      gApp->paintTarget = PaintTarget::None;
      if (!gApp->rotatingView) {
        ReleaseCapture();
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
      const float scale = delta > 0 ? 1.08f : 0.92f;
      gApp->brushRadius = clamp(gApp->brushRadius * scale, 0.04f, 0.60f);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        DestroyWindow(hwnd);
      } else if (wParam == '1') {
        gApp->mode = BrushMode::Add;
        gApp->status = L"Outil Add";
      } else if (wParam == '2') {
        gApp->mode = BrushMode::Subtract;
        gApp->status = L"Outil Subtract";
      } else if (wParam == '3') {
        gApp->mode = BrushMode::Smooth;
        gApp->status = L"Outil Smooth";
      } else if (wParam == '4' || wParam == VK_OEM_4) {
        gApp->changeResolution(-1);
      } else if (wParam == '5' || wParam == VK_OEM_6) {
        gApp->changeResolution(1);
      } else if (wParam == VK_ADD || wParam == VK_OEM_PLUS) {
        gApp->smoothStrength = clamp(gApp->smoothStrength + 0.05f, 0.05f, 1.0f);
      } else if (wParam == VK_SUBTRACT || wParam == VK_OEM_MINUS) {
        gApp->smoothStrength = clamp(gApp->smoothStrength - 0.05f, 0.05f, 1.0f);
      } else if (wParam == VK_NEXT) {
        gApp->sliceZ = std::max(0, gApp->sliceZ - 1);
      } else if (wParam == VK_PRIOR) {
        gApp->sliceZ = std::min(gApp->volume.size().z - 1, gApp->sliceZ + 1);
      } else if (wParam == 'Z') {
        if (gApp->history.undo()) {
          gApp->markSurfaceDirty();
          gApp->status = L"Undo";
        } else {
          gApp->status = L"Rien a annuler";
        }
      } else if (wParam == 'Y') {
        if (gApp->history.redo()) {
          gApp->markSurfaceDirty();
          gApp->status = L"Redo";
        } else {
          gApp->status = L"Rien a retablir";
        }
      } else if (wParam == 'W') {
        gApp->showWire = !gApp->showWire;
        gApp->status = gApp->showWire ? L"Filaire surface actif" : L"Filaire surface masque";
      } else if (wParam == 'P') {
        exportPreview(hwnd);
        return 0;
      } else if (wParam == 'R') {
        gApp->reset(true);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;

    case WM_PAINT:
      paint(hwnd);
      return 0;

    case WM_DESTROY:
      gApp.reset();
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  const wchar_t* className = L"LargeSdfPreviewWindow";

  WNDCLASSW wc{};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = instance;
  wc.lpszClassName = className;
  wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

  RegisterClassW(&wc);

  HWND hwnd = CreateWindowExW(0,
                              className,
                              L"Large SDF - Native Sculpt Preview",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT,
                              CW_USEDEFAULT,
                              kWindowWidth,
                              kWindowHeight,
                              nullptr,
                              nullptr,
                              instance,
                              nullptr);
  if (!hwnd) {
    return 1;
  }

  ShowWindow(hwnd, showCmd);
  UpdateWindow(hwnd);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  return 0;
}
