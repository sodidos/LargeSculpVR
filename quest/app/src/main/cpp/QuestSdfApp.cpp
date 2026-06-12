#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <ctime>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "core/SdfHistory.h"
#include "core/SdfVolume.h"
#include "core/ObjExporter.h"
#include "core/SurfaceMesh.h"

#include "HudPainter.h"

#if LARGE_USE_OPENXR
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#endif

namespace {

constexpr char kLogTag[] = "LargeSdfQuest";
constexpr int kEyeCount = 2;
constexpr float kVrRenderScale = 0.60f;
constexpr large::sdf::Vec3 kInitialObjectPosition{0.0f, -0.10f, -2.05f};
constexpr float kDefaultBrushRadius = 0.095f;
constexpr float kMinimumBrushRadius = 0.015f;
constexpr float kMaximumBrushRadius = 0.60f;
constexpr float kDefaultBrushStrength = 1.0f;
constexpr float kMinimumBrushStrength = 0.02f;
constexpr float kMaximumBrushStrength = 1.0f;
constexpr float kTriggerThreshold = 0.55f;
constexpr float kGripThreshold = 0.55f;
constexpr float kMinimumObjectScale = 0.05f;
constexpr float kMaximumObjectScale = 10.00f;
constexpr float kHandPinchDistance = 0.035f;
constexpr float kHandPinchReleaseDistance = 0.052f;
constexpr float kHandFistTipDistance = 0.090f;
constexpr float kHandFistReleaseTipDistance = 0.112f;
constexpr float kHandPoseSmoothing = 0.45f;  // fraction of the previous pose kept each frame
constexpr float kHandStretchSurfaceSnapDistance = 0.080f;
constexpr float kHandSculptSurfaceSnapDistance = 0.095f;
constexpr float kHandOpenFingerDistance = 0.125f;
constexpr float kHandOpenLittleDistance = 0.105f;
constexpr float kHandOpenThumbDistance = 0.095f;
constexpr float kHandOpenPinchDistance = 0.065f;
constexpr float kHandClapDistance = 0.115f;
constexpr float kHandClapReleaseDistance = 0.195f;
constexpr float kHandPinchZoomSpeed = 3.0f;
constexpr float kControllerToolLength = 0.20f;
constexpr float kHandSmoothStrength = 1.0f;
constexpr float kHandEraseStrength = 1.0f;
constexpr int kMenuToolCount = 8;
constexpr int kMenuFileActionCount = 4;
constexpr int kMenuArChoice = kMenuToolCount + kMenuFileActionCount;  // AR toggle row
constexpr int kMenuMirrorChoice = kMenuArChoice + 1;                  // symmetry toggle row
constexpr int kMenuLockChoice = kMenuMirrorChoice + 1;                // freeze object pose/scale
constexpr int kMenuPaletteFirstChoice = kMenuLockChoice + 1;          // 8 paint color swatches
constexpr int kMenuToggleChoice = 99;                                 // MENU header button
static_assert(kMenuToolCount == large::hud::kMenuToolRowCount, "tool rows must match HUD layout");
static_assert(kMenuFileActionCount + 3 == large::hud::kMenuActionRowCount,
              "action rows must match HUD layout");

// Paint palette (linear RGB); entry 0 is the default clay used to clear the color volume.
constexpr std::array<std::array<float, 3>, large::hud::kPaletteCount> kPaintPalette{{
    {0.86f, 0.68f, 0.54f},  // clay
    {0.93f, 0.93f, 0.93f},  // white
    {0.25f, 0.25f, 0.28f},  // graphite
    {0.84f, 0.19f, 0.19f},  // red
    {1.00f, 0.62f, 0.10f},  // orange
    {1.00f, 0.87f, 0.35f},  // yellow
    {0.18f, 0.80f, 0.44f},  // green
    {0.20f, 0.60f, 0.86f},  // blue
}};

enum class VrTool {
  Add,
  Subtract,
  Smooth,
  Stretch,
  Flatten,
  Groove,
  Crease,
  Paint,
};

constexpr int kVrToolCount = 8;

const char* toolName(VrTool tool) {
  switch (tool) {
    case VrTool::Add:
      return "Add";
    case VrTool::Subtract:
      return "Subtract";
    case VrTool::Smooth:
      return "Smooth";
    case VrTool::Stretch:
      return "Stretch";
    case VrTool::Flatten:
      return "Flatten";
    case VrTool::Groove:
      return "Groove";
    case VrTool::Crease:
      return "Crease";
    case VrTool::Paint:
      return "Paint";
  }
  return "Unknown";
}

int toolIndex(VrTool tool) {
  switch (tool) {
    case VrTool::Add:
      return 0;
    case VrTool::Subtract:
      return 1;
    case VrTool::Smooth:
      return 2;
    case VrTool::Stretch:
      return 3;
    case VrTool::Flatten:
      return 4;
    case VrTool::Groove:
      return 5;
    case VrTool::Crease:
      return 6;
    case VrTool::Paint:
      return 7;
  }
  return 0;
}

VrTool toolFromIndex(int index) {
  constexpr std::array<VrTool, kVrToolCount> tools{
      VrTool::Add,
      VrTool::Subtract,
      VrTool::Smooth,
      VrTool::Stretch,
      VrTool::Flatten,
      VrTool::Groove,
      VrTool::Crease,
      VrTool::Paint,
  };
  const int wrapped = (index % kVrToolCount + kVrToolCount) % kVrToolCount;
  return tools[static_cast<std::size_t>(wrapped)];
}

VrTool nextHandTool(VrTool tool) {
  return toolFromIndex(toolIndex(tool) + 1);
}

void logInfo(const char* format, ...) {
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_INFO, kLogTag, format, args);
  va_end(args);
}

void logError(const char* format, ...) {
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_ERROR, kLogTag, format, args);
  va_end(args);
}

large::sdf::SdfVolume makeInitialVolume() {
  constexpr int resolution = 128;
  constexpr float extent = 4.8f;
  const float voxelSize = extent / static_cast<float>(resolution);
  const large::sdf::Vec3 origin{-extent * 0.5f, -extent * 0.5f, -extent * 0.5f};

  // The scene starts empty: the user creates the first material with the Add
  // tool (trigger or hand pinch) inside the workspace bounds cube.
  return large::sdf::SdfVolume({resolution, resolution, resolution}, voxelSize, origin, 10.0f);
}

#if LARGE_USE_OPENXR

const char* xrResultName(XrInstance instance, XrResult result) {
  static char buffer[XR_MAX_RESULT_STRING_SIZE];
  if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, buffer))) {
    return buffer;
  }
  std::snprintf(buffer, sizeof(buffer), "%d", result);
  return buffer;
}

bool checkXr(XrInstance instance, XrResult result, const char* call) {
  if (XR_SUCCEEDED(result)) {
    return true;
  }
  logError("%s failed: %s", call, xrResultName(instance, result));
  return false;
}

struct EglState {
  EGLDisplay display = EGL_NO_DISPLAY;
  EGLConfig config = nullptr;
  EGLContext context = EGL_NO_CONTEXT;
  EGLSurface tinySurface = EGL_NO_SURFACE;

  bool create() {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
      logError("eglGetDisplay failed");
      return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(display, &major, &minor)) {
      logError("eglInitialize failed: 0x%x", eglGetError());
      return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      logError("eglBindAPI failed: 0x%x", eglGetError());
      return false;
    }

    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE,
    };

    EGLint configCount = 0;
    if (!eglChooseConfig(display, configAttributes, &config, 1, &configCount) || configCount < 1) {
      logError("eglChooseConfig failed: 0x%x", eglGetError());
      return false;
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT) {
      logError("eglCreateContext failed: 0x%x", eglGetError());
      return false;
    }

    const EGLint surfaceAttributes[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE,
    };
    tinySurface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    if (tinySurface == EGL_NO_SURFACE) {
      logError("eglCreatePbufferSurface failed: 0x%x", eglGetError());
      return false;
    }

    if (!eglMakeCurrent(display, tinySurface, tinySurface, context)) {
      logError("eglMakeCurrent failed: 0x%x", eglGetError());
      return false;
    }

    logInfo("EGL ready: %d.%d, GL=%s", major, minor, glGetString(GL_VERSION));
    return true;
  }

  void destroy() {
    if (display != EGL_NO_DISPLAY) {
      eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      if (context != EGL_NO_CONTEXT) {
        eglDestroyContext(display, context);
      }
      if (tinySurface != EGL_NO_SURFACE) {
        eglDestroySurface(display, tinySurface);
      }
      eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    config = nullptr;
    context = EGL_NO_CONTEXT;
    tinySurface = EGL_NO_SURFACE;
  }
};

struct EyeSwapchain {
  XrSwapchain handle = XR_NULL_HANDLE;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<XrSwapchainImageOpenGLESKHR> images;
};

struct MeshVertex {
  large::sdf::Vec3 position;
  large::sdf::Vec3 normal;
};

struct Mat4 {
  std::array<float, 16> m{};

  float& at(int row, int col) { return m[static_cast<std::size_t>(col * 4 + row)]; }
  float at(int row, int col) const { return m[static_cast<std::size_t>(col * 4 + row)]; }
};

Mat4 identityMatrix() {
  Mat4 result{};
  result.at(0, 0) = 1.0f;
  result.at(1, 1) = 1.0f;
  result.at(2, 2) = 1.0f;
  result.at(3, 3) = 1.0f;
  return result;
}

Mat4 multiply(Mat4 a, Mat4 b) {
  Mat4 result{};
  for (int row = 0; row < 4; ++row) {
    for (int col = 0; col < 4; ++col) {
      for (int k = 0; k < 4; ++k) {
        result.at(row, col) += a.at(row, k) * b.at(k, col);
      }
    }
  }
  return result;
}

Mat4 makeTranslation(float x, float y, float z) {
  Mat4 result = identityMatrix();
  result.at(0, 3) = x;
  result.at(1, 3) = y;
  result.at(2, 3) = z;
  return result;
}

Mat4 makeScale(float scale) {
  Mat4 result = identityMatrix();
  result.at(0, 0) = scale;
  result.at(1, 1) = scale;
  result.at(2, 2) = scale;
  return result;
}

Mat4 makeRotationY(float radians) {
  Mat4 result = identityMatrix();
  const float c = std::cos(radians);
  const float s = std::sin(radians);
  result.at(0, 0) = c;
  result.at(0, 2) = s;
  result.at(2, 0) = -s;
  result.at(2, 2) = c;
  return result;
}

Mat4 makeProjection(const XrFovf& fov, float nearZ, float farZ) {
  const float tanLeft = std::tan(fov.angleLeft);
  const float tanRight = std::tan(fov.angleRight);
  const float tanDown = std::tan(fov.angleDown);
  const float tanUp = std::tan(fov.angleUp);
  const float tanWidth = tanRight - tanLeft;
  const float tanHeight = tanUp - tanDown;

  Mat4 result{};
  result.at(0, 0) = 2.0f / tanWidth;
  result.at(0, 2) = (tanRight + tanLeft) / tanWidth;
  result.at(1, 1) = 2.0f / tanHeight;
  result.at(1, 2) = (tanUp + tanDown) / tanHeight;
  result.at(2, 2) = -(farZ + nearZ) / (farZ - nearZ);
  result.at(2, 3) = -(2.0f * farZ * nearZ) / (farZ - nearZ);
  result.at(3, 2) = -1.0f;
  return result;
}

Mat4 makePoseMatrix(const XrPosef& pose) {
  const XrQuaternionf& q = pose.orientation;
  const float x2 = q.x + q.x;
  const float y2 = q.y + q.y;
  const float z2 = q.z + q.z;
  const float xx = q.x * x2;
  const float yy = q.y * y2;
  const float zz = q.z * z2;
  const float xy = q.x * y2;
  const float xz = q.x * z2;
  const float yz = q.y * z2;
  const float wx = q.w * x2;
  const float wy = q.w * y2;
  const float wz = q.w * z2;

  Mat4 result = identityMatrix();
  result.at(0, 0) = 1.0f - yy - zz;
  result.at(0, 1) = xy - wz;
  result.at(0, 2) = xz + wy;
  result.at(1, 0) = xy + wz;
  result.at(1, 1) = 1.0f - xx - zz;
  result.at(1, 2) = yz - wx;
  result.at(2, 0) = xz - wy;
  result.at(2, 1) = yz + wx;
  result.at(2, 2) = 1.0f - xx - yy;
  result.at(0, 3) = pose.position.x;
  result.at(1, 3) = pose.position.y;
  result.at(2, 3) = pose.position.z;
  return result;
}

Mat4 makeViewMatrix(const XrPosef& pose) {
  const Mat4 poseMatrix = makePoseMatrix(pose);
  Mat4 result = identityMatrix();
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      result.at(row, col) = poseMatrix.at(col, row);
    }
  }

  const float px = pose.position.x;
  const float py = pose.position.y;
  const float pz = pose.position.z;
  result.at(0, 3) = -(result.at(0, 0) * px + result.at(0, 1) * py + result.at(0, 2) * pz);
  result.at(1, 3) = -(result.at(1, 0) * px + result.at(1, 1) * py + result.at(1, 2) * pz);
  result.at(2, 3) = -(result.at(2, 0) * px + result.at(2, 1) * py + result.at(2, 2) * pz);
  return result;
}

GLuint compileShader(GLenum type, const char* source) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (compiled == GL_TRUE) {
    return shader;
  }

  std::array<char, 1024> log{};
  GLsizei length = 0;
  glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &length, log.data());
  logError("Shader compile failed: %s", log.data());
  glDeleteShader(shader);
  return 0;
}

GLuint linkProgram(const char* vertexSource, const char* fragmentSource) {
  const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
  const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
  if (vertexShader == 0 || fragmentShader == 0) {
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return 0;
  }

  const GLuint program = glCreateProgram();
  glAttachShader(program, vertexShader);
  glAttachShader(program, fragmentShader);
  glBindAttribLocation(program, 0, "aPosition");
  glBindAttribLocation(program, 1, "aNormal");
  glLinkProgram(program);
  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked == GL_TRUE) {
    return program;
  }

  std::array<char, 1024> log{};
  GLsizei length = 0;
  glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), &length, log.data());
  logError("Program link failed: %s", log.data());
  glDeleteProgram(program);
  return 0;
}

large::sdf::Vec3 rotateByQuaternion(const XrQuaternionf& q, large::sdf::Vec3 v) {
  const large::sdf::Vec3 qv{q.x, q.y, q.z};
  const large::sdf::Vec3 t = large::sdf::cross(qv, v) * 2.0f;
  return v + t * q.w + large::sdf::cross(qv, t);
}

XrQuaternionf normalizeQuaternion(XrQuaternionf q) {
  const float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (length <= 0.000001f) {
    return {0.0f, 0.0f, 0.0f, 1.0f};
  }
  const float invLength = 1.0f / length;
  return {q.x * invLength, q.y * invLength, q.z * invLength, q.w * invLength};
}

XrQuaternionf conjugateQuaternion(XrQuaternionf q) {
  return {-q.x, -q.y, -q.z, q.w};
}

XrQuaternionf multiplyQuaternions(XrQuaternionf a, XrQuaternionf b) {
  return normalizeQuaternion({
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  });
}

XrQuaternionf quaternionFromAxisAngle(large::sdf::Vec3 axis, float angle) {
  axis = large::sdf::normalize(axis);
  const float halfAngle = angle * 0.5f;
  const float s = std::sin(halfAngle);
  return normalizeQuaternion({axis.x * s, axis.y * s, axis.z * s, std::cos(halfAngle)});
}

XrQuaternionf quaternionFromTo(large::sdf::Vec3 from, large::sdf::Vec3 to) {
  from = large::sdf::normalize(from);
  to = large::sdf::normalize(to);
  const float d = large::sdf::clamp(large::sdf::dot(from, to), -1.0f, 1.0f);
  if (d > 0.9995f) {
    return {0.0f, 0.0f, 0.0f, 1.0f};
  }
  if (d < -0.9995f) {
    large::sdf::Vec3 axis = large::sdf::cross({1.0f, 0.0f, 0.0f}, from);
    if (large::sdf::length(axis) < 0.0001f) {
      axis = large::sdf::cross({0.0f, 1.0f, 0.0f}, from);
    }
    return quaternionFromAxisAngle(axis, 3.1415926535f);
  }

  const large::sdf::Vec3 axis = large::sdf::cross(from, to);
  return normalizeQuaternion({axis.x, axis.y, axis.z, 1.0f + d});
}

std::array<float, 9> makeRotationMatrix3(XrQuaternionf q) {
  q = normalizeQuaternion(q);
  const float x2 = q.x + q.x;
  const float y2 = q.y + q.y;
  const float z2 = q.z + q.z;
  const float xx = q.x * x2;
  const float yy = q.y * y2;
  const float zz = q.z * z2;
  const float xy = q.x * y2;
  const float xz = q.x * z2;
  const float yz = q.y * z2;
  const float wx = q.w * x2;
  const float wy = q.w * y2;
  const float wz = q.w * z2;

  return {
      1.0f - yy - zz, xy + wz, xz - wy,
      xy - wz, 1.0f - xx - zz, yz + wx,
      xz + wy, yz - wx, 1.0f - xx - yy,
  };
}

std::array<float, 9> transposeMatrix3(const std::array<float, 9>& m) {
  return {
      m[0], m[3], m[6],
      m[1], m[4], m[7],
      m[2], m[5], m[8],
  };
}

large::sdf::Vec3 makeVec3(const XrVector3f& v) {
  return {v.x, v.y, v.z};
}

bool intersectVolumeBounds(const large::sdf::SdfVolume& volume,
                           large::sdf::Vec3 rayOrigin,
                           large::sdf::Vec3 rayDirection,
                           float& nearT,
                           float& farT) {
  const large::sdf::IVec3 size = volume.size();
  const large::sdf::Vec3 boxMin = volume.origin();
  const large::sdf::Vec3 boxMax{
      boxMin.x + static_cast<float>(size.x) * volume.voxelSize(),
      boxMin.y + static_cast<float>(size.y) * volume.voxelSize(),
      boxMin.z + static_cast<float>(size.z) * volume.voxelSize(),
  };

  nearT = 0.0f;
  farT = 1000.0f;

  const std::array<float, 3> origin{rayOrigin.x, rayOrigin.y, rayOrigin.z};
  const std::array<float, 3> direction{rayDirection.x, rayDirection.y, rayDirection.z};
  const std::array<float, 3> minValue{boxMin.x, boxMin.y, boxMin.z};
  const std::array<float, 3> maxValue{boxMax.x, boxMax.y, boxMax.z};

  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(direction[axis]) < 0.00001f) {
      if (origin[axis] < minValue[axis] || origin[axis] > maxValue[axis]) {
        return false;
      }
      continue;
    }

    float t0 = (minValue[axis] - origin[axis]) / direction[axis];
    float t1 = (maxValue[axis] - origin[axis]) / direction[axis];
    if (t0 > t1) {
      std::swap(t0, t1);
    }
    nearT = std::max(nearT, t0);
    farT = std::min(farT, t1);
    if (farT <= nearT) {
      return false;
    }
  }

  return true;
}

#endif

class QuestSdfApp {
 public:
  explicit QuestSdfApp(android_app* app)
      : app_(app), volume_(makeInitialVolume()), stretchSourceVolume_(volume_), history_(volume_, 12) {}

  void run() {
    app_->userData = this;
    app_->onAppCmd = &QuestSdfApp::handleAppCmd;
    app_->onInputEvent = &QuestSdfApp::handleInputEvent;

    logInfo("Large SDF Quest starting: %dx%dx%d volume",
            volume_.size().x,
            volume_.size().y,
            volume_.size().z);
    stretchSourceVolume_ = volume_;

#if LARGE_USE_OPENXR
    openXrReady_ = initializeOpenXr();
    if (!openXrReady_) {
      logError("OpenXR initialization failed; app loop remains alive for logcat diagnostics.");
    }
#else
    logInfo("Built without OpenXR. Enable LARGE_USE_OPENXR for Quest VR.");
#endif

    while (!app_->destroyRequested) {
      pollAndroidEvents();

#if LARGE_USE_OPENXR
      if (openXrReady_) {
        pollOpenXrEvents();
        renderOpenXrFrame();
      } else {
        tickDiagnostics();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
#else
      tickDiagnostics();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
    }

#if LARGE_USE_OPENXR
    shutdownOpenXr();
#endif
  }

 private:
  static void handleAppCmd(android_app* app, int32_t command) {
    auto* self = static_cast<QuestSdfApp*>(app->userData);
    if (self != nullptr) {
      self->onAppCmd(command);
    }
  }

  static int32_t handleInputEvent(android_app* app, AInputEvent* event) {
    auto* self = static_cast<QuestSdfApp*>(app->userData);
    if (self == nullptr) {
      return 0;
    }
    return self->onInputEvent(event);
  }

  void pollAndroidEvents() {
    int events = 0;
    android_poll_source* source = nullptr;
    while (ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
      if (source != nullptr) {
        source->process(app_, source);
      }
      if (app_->destroyRequested) {
        break;
      }
    }
  }

  void onAppCmd(int32_t command) {
    switch (command) {
      case APP_CMD_GAINED_FOCUS:
        isFocused_ = true;
        logInfo("NativeActivity gained focus");
        break;
      case APP_CMD_LOST_FOCUS:
        isFocused_ = false;
        logInfo("NativeActivity lost focus");
        break;
      case APP_CMD_INIT_WINDOW:
        hasWindow_ = true;
        logInfo("Native window ready");
        break;
      case APP_CMD_TERM_WINDOW:
        hasWindow_ = false;
        logInfo("Native window released");
        break;
      default:
        break;
    }
  }

  int32_t onInputEvent(AInputEvent*) { return 0; }

  void tickDiagnostics() {
    const auto now = std::chrono::steady_clock::now();
    if (now < nextStatsLog_) {
      return;
    }

    nextStatsLog_ = now + std::chrono::seconds(3);
    logInfo("SDF loaded: %d solid voxels, window=%d, focus=%d",
            volume_.countSolidVoxels(),
            hasWindow_ ? 1 : 0,
            isFocused_ ? 1 : 0);
  }

#if LARGE_USE_OPENXR
  bool isOpenXrExtensionSupported(const char* extensionName) const {
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
      logError("xrEnumerateInstanceExtensionProperties(count) failed: %d", result);
      return false;
    }

    std::vector<XrExtensionProperties> properties(count);
    for (XrExtensionProperties& property : properties) {
      property.type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
    if (XR_FAILED(result)) {
      logError("xrEnumerateInstanceExtensionProperties(list) failed: %d", result);
      return false;
    }

    for (const XrExtensionProperties& property : properties) {
      if (std::strcmp(property.extensionName, extensionName) == 0) {
        return true;
      }
    }
    return false;
  }

  bool initializeOpenXr() {
    if (!initializeOpenXrLoader()) {
      return false;
    }

    std::vector<const char*> extensions{
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    handTrackingExtensionEnabled_ = isOpenXrExtensionSupported(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
    if (handTrackingExtensionEnabled_) {
      extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
      logInfo("OpenXR hand tracking extension enabled");
    } else {
      logInfo("OpenXR hand tracking extension unavailable");
    }
    passthroughExtensionEnabled_ = isOpenXrExtensionSupported(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (passthroughExtensionEnabled_) {
      extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
      logInfo("OpenXR passthrough extension enabled");
    } else {
      logInfo("OpenXR passthrough extension unavailable");
    }

    XrInstanceCreateInfoAndroidKHR androidInfo{};
    androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidInfo.applicationVM = app_->activity->vm;
    androidInfo.applicationActivity = app_->activity->clazz;

    XrInstanceCreateInfo createInfo{};
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = &androidInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    std::strncpy(createInfo.applicationInfo.applicationName, "Large SDF Quest", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(createInfo.applicationInfo.engineName, "large_sdf_core", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    if (!checkXr(xrInstance_, xrCreateInstance(&createInfo, &xrInstance_), "xrCreateInstance")) {
      return false;
    }

    XrSystemGetInfo systemInfo{};
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!checkXr(xrInstance_, xrGetSystem(xrInstance_, &systemInfo, &xrSystemId_), "xrGetSystem")) {
      return false;
    }
    queryEnvironmentBlendModes();

    if (handTrackingExtensionEnabled_) {
      XrSystemHandTrackingPropertiesEXT handProperties{};
      handProperties.type = XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT;
      XrSystemProperties systemProperties{};
      systemProperties.type = XR_TYPE_SYSTEM_PROPERTIES;
      systemProperties.next = &handProperties;
      if (checkXr(xrInstance_,
                  xrGetSystemProperties(xrInstance_, xrSystemId_, &systemProperties),
                  "xrGetSystemProperties(hand tracking)")) {
        handTrackingSupported_ = handProperties.supportsHandTracking == XR_TRUE;
      }
      logInfo("OpenXR hand tracking support: %d", handTrackingSupported_ ? 1 : 0);
    }

    if (passthroughExtensionEnabled_) {
      XrSystemPassthroughPropertiesFB passthroughProperties{};
      passthroughProperties.type = XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES_FB;
      XrSystemProperties systemProperties{};
      systemProperties.type = XR_TYPE_SYSTEM_PROPERTIES;
      systemProperties.next = &passthroughProperties;
      if (checkXr(xrInstance_,
                  xrGetSystemProperties(xrInstance_, xrSystemId_, &systemProperties),
                  "xrGetSystemProperties(passthrough)")) {
        passthroughSupported_ = passthroughProperties.supportsPassthrough == XR_TRUE;
      }
      logInfo("OpenXR passthrough support: %d", passthroughSupported_ ? 1 : 0);
    }

    if (!initializeActions()) {
      return false;
    }

    if (!egl_.create()) {
      return false;
    }

    if (!checkGraphicsRequirements()) {
      return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{};
    graphicsBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    graphicsBinding.display = egl_.display;
    graphicsBinding.config = egl_.config;
    graphicsBinding.context = egl_.context;

    XrSessionCreateInfo sessionInfo{};
    sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = xrSystemId_;
    if (!checkXr(xrInstance_, xrCreateSession(xrInstance_, &sessionInfo, &xrSession_), "xrCreateSession")) {
      return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!checkXr(xrInstance_, xrCreateReferenceSpace(xrSession_, &spaceInfo, &xrSpace_), "xrCreateReferenceSpace")) {
      return false;
    }

    initializePassthrough();

    if (!initializeActionSpacesAndAttachSet()) {
      return false;
    }

    if (!createSwapchains()) {
      return false;
    }

    glGenFramebuffers(1, &framebuffer_);
    if (!initializeSdfRaymarchRenderer()) {
      return false;
    }
    if (!initializeUiOverlayRenderer()) {
      logError("UI overlay renderer disabled; SDF renderer remains active.");
    }

    logInfo("OpenXR renderer ready, system id=%llu", static_cast<unsigned long long>(xrSystemId_));
    return true;
  }

  bool loadPassthroughFunctions() {
    if (!passthroughExtensionEnabled_ || !passthroughSupported_) {
      return false;
    }

    const auto loadFunction = [&](const char* name, PFN_xrVoidFunction* function) {
      const XrResult result = xrGetInstanceProcAddr(xrInstance_, name, function);
      if (XR_FAILED(result) || *function == nullptr) {
        logError("Passthrough function unavailable: %s (%s)", name, xrResultName(xrInstance_, result));
        return false;
      }
      return true;
    };

    return loadFunction("xrCreatePassthroughFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrCreatePassthroughFB_)) &&
           loadFunction("xrDestroyPassthroughFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyPassthroughFB_)) &&
           loadFunction("xrPassthroughStartFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughStartFB_)) &&
           loadFunction("xrPassthroughPauseFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughPauseFB_)) &&
           loadFunction("xrCreatePassthroughLayerFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrCreatePassthroughLayerFB_)) &&
           loadFunction("xrDestroyPassthroughLayerFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyPassthroughLayerFB_)) &&
           loadFunction("xrPassthroughLayerPauseFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerPauseFB_)) &&
           loadFunction("xrPassthroughLayerResumeFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerResumeFB_)) &&
           loadFunction("xrPassthroughLayerSetStyleFB",
                        reinterpret_cast<PFN_xrVoidFunction*>(&xrPassthroughLayerSetStyleFB_));
  }

  void queryEnvironmentBlendModes() {
    alphaBlendEnvironmentSupported_ = false;
    uint32_t modeCount = 0;
    XrResult result = xrEnumerateEnvironmentBlendModes(xrInstance_,
                                                       xrSystemId_,
                                                       XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                       0,
                                                       &modeCount,
                                                       nullptr);
    if (XR_FAILED(result) || modeCount == 0) {
      logError("xrEnumerateEnvironmentBlendModes(count) failed: %s", xrResultName(xrInstance_, result));
      return;
    }

    std::vector<XrEnvironmentBlendMode> modes(modeCount);
    result = xrEnumerateEnvironmentBlendModes(xrInstance_,
                                              xrSystemId_,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              modeCount,
                                              &modeCount,
                                              modes.data());
    if (XR_FAILED(result)) {
      logError("xrEnumerateEnvironmentBlendModes(list) failed: %s", xrResultName(xrInstance_, result));
      return;
    }

    bool opaqueSupported = false;
    bool additiveSupported = false;
    for (XrEnvironmentBlendMode mode : modes) {
      opaqueSupported = opaqueSupported || mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
      additiveSupported = additiveSupported || mode == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
      alphaBlendEnvironmentSupported_ = alphaBlendEnvironmentSupported_ ||
                                        mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    }
    logInfo("OpenXR blend modes: opaque=%d additive=%d alpha=%d",
            opaqueSupported ? 1 : 0,
            additiveSupported ? 1 : 0,
            alphaBlendEnvironmentSupported_ ? 1 : 0);
  }

  void initializePassthrough() {
    if (!loadPassthroughFunctions()) {
      passthroughReady_ = false;
      return;
    }

    // Created paused; xrPassthroughStartFB / LayerResumeFB run on toggle so
    // the runtime never sees a "running but never submitted" layer state.
    XrPassthroughCreateInfoFB passthroughInfo{};
    passthroughInfo.type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB;
    passthroughInfo.flags = 0;
    XrResult result = xrCreatePassthroughFB_(xrSession_, &passthroughInfo, &passthrough_);
    if (XR_FAILED(result) || passthrough_ == XR_NULL_HANDLE) {
      logError("xrCreatePassthroughFB failed: %s", xrResultName(xrInstance_, result));
      passthroughReady_ = false;
      return;
    }

    XrPassthroughLayerCreateInfoFB layerInfo{};
    layerInfo.type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB;
    layerInfo.passthrough = passthrough_;
    layerInfo.flags = 0;
    layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    result = xrCreatePassthroughLayerFB_(xrSession_, &layerInfo, &passthroughLayer_);
    if (XR_FAILED(result) || passthroughLayer_ == XR_NULL_HANDLE) {
      logError("xrCreatePassthroughLayerFB failed: %s", xrResultName(xrInstance_, result));
      if (xrDestroyPassthroughFB_ != nullptr && passthrough_ != XR_NULL_HANDLE) {
        xrDestroyPassthroughFB_(passthrough_);
      }
      passthrough_ = XR_NULL_HANDLE;
      passthroughReady_ = false;
      return;
    }

    passthroughReady_ = true;
    logInfo("OpenXR passthrough ready (paused until AR toggle)");
  }

  void setArModeEnabled(bool enabled) {
    if (enabled && !passthroughReady_) {
      arModeEnabled_ = false;
      logInfo("AR passthrough unavailable on this runtime");
      return;
    }

    if (enabled == arModeEnabled_) {
      return;
    }

    if (enabled) {
      XrResult result = xrPassthroughStartFB_(passthrough_);
      logInfo("xrPassthroughStartFB: %s", xrResultName(xrInstance_, result));
      if (XR_FAILED(result)) {
        arModeEnabled_ = false;
        return;
      }
      result = xrPassthroughLayerResumeFB_(passthroughLayer_);
      logInfo("xrPassthroughLayerResumeFB: %s", xrResultName(xrInstance_, result));
      if (XR_FAILED(result)) {
        xrPassthroughPauseFB_(passthrough_);
        arModeEnabled_ = false;
        return;
      }
      XrPassthroughStyleFB style{};
      style.type = XR_TYPE_PASSTHROUGH_STYLE_FB;
      style.textureOpacityFactor = 1.0f;
      style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
      result = xrPassthroughLayerSetStyleFB_(passthroughLayer_, &style);
      logInfo("xrPassthroughLayerSetStyleFB: %s", xrResultName(xrInstance_, result));
      arModeEnabled_ = true;
      logInfo("AR passthrough enabled");
      return;
    }

    if (passthroughReady_) {
      xrPassthroughLayerPauseFB_(passthroughLayer_);
      xrPassthroughPauseFB_(passthrough_);
    }
    arModeEnabled_ = false;
    logInfo("AR passthrough disabled");
  }

  bool initializeOpenXrLoader() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    XrResult result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
    if (XR_FAILED(result) || initializeLoader == nullptr) {
      logError("xrInitializeLoaderKHR unavailable: %d", result);
      return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInfo{};
    loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    loaderInfo.applicationVM = app_->activity->vm;
    loaderInfo.applicationContext = app_->activity->clazz;

    result = initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo));
    if (XR_FAILED(result)) {
      logError("xrInitializeLoaderKHR failed: %d", result);
      return false;
    }

    return true;
  }

  bool checkGraphicsRequirements() {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!checkXr(xrInstance_,
                 xrGetInstanceProcAddr(xrInstance_,
                                       "xrGetOpenGLESGraphicsRequirementsKHR",
                                       reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
                 "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") ||
        getRequirements == nullptr) {
      return false;
    }

    XrGraphicsRequirementsOpenGLESKHR requirements{};
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    return checkXr(xrInstance_,
                   getRequirements(xrInstance_, xrSystemId_, &requirements),
                   "xrGetOpenGLESGraphicsRequirementsKHR");
  }

  void initializeHandTracking() {
    if (!handTrackingSupported_) {
      return;
    }

    XrResult result = xrGetInstanceProcAddr(
        xrInstance_,
        "xrCreateHandTrackerEXT",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateHandTrackerEXT_));
    result = XR_SUCCEEDED(result)
                 ? xrGetInstanceProcAddr(xrInstance_,
                                         "xrDestroyHandTrackerEXT",
                                         reinterpret_cast<PFN_xrVoidFunction*>(&xrDestroyHandTrackerEXT_))
                 : result;
    result = XR_SUCCEEDED(result)
                 ? xrGetInstanceProcAddr(xrInstance_,
                                         "xrLocateHandJointsEXT",
                                         reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateHandJointsEXT_))
                 : result;
    if (XR_FAILED(result) || xrCreateHandTrackerEXT_ == nullptr || xrDestroyHandTrackerEXT_ == nullptr ||
        xrLocateHandJointsEXT_ == nullptr) {
      logError("OpenXR hand tracking functions unavailable: %s", xrResultName(xrInstance_, result));
      handTrackingSupported_ = false;
      return;
    }

    XrHandTrackerCreateInfoEXT createInfo{};
    createInfo.type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT;
    createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

    createInfo.hand = XR_HAND_LEFT_EXT;
    result = xrCreateHandTrackerEXT_(xrSession_, &createInfo, &leftHandTracker_);
    if (XR_FAILED(result)) {
      logError("xrCreateHandTrackerEXT(left) failed: %s", xrResultName(xrInstance_, result));
      handTrackingSupported_ = false;
      return;
    }

    createInfo.hand = XR_HAND_RIGHT_EXT;
    result = xrCreateHandTrackerEXT_(xrSession_, &createInfo, &rightHandTracker_);
    if (XR_FAILED(result)) {
      logError("xrCreateHandTrackerEXT(right) failed: %s", xrResultName(xrInstance_, result));
      if (leftHandTracker_ != XR_NULL_HANDLE) {
        xrDestroyHandTrackerEXT_(leftHandTracker_);
        leftHandTracker_ = XR_NULL_HANDLE;
      }
      handTrackingSupported_ = false;
      return;
    }

    handTrackingReady_ = true;
    logInfo("OpenXR hand tracking ready");
  }

  bool initializeActions() {
    if (!checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left", &leftHandPath_),
                 "xrStringToPath(/user/hand/left)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right", &rightHandPath_),
                 "xrStringToPath(/user/hand/right)")) {
      return false;
    }

    const std::array<XrPath, 2> handPaths{leftHandPath_, rightHandPath_};

    XrActionSetCreateInfo actionSetInfo{};
    actionSetInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::strncpy(actionSetInfo.actionSetName, "large_sdf_actions", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(actionSetInfo.localizedActionSetName, "Large SDF Actions", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    actionSetInfo.priority = 0;
    if (!checkXr(xrInstance_,
                 xrCreateActionSet(xrInstance_, &actionSetInfo, &actionSet_),
                 "xrCreateActionSet")) {
      return false;
    }

    XrActionCreateInfo aimInfo{};
    aimInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    aimInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(aimInfo.actionName, "right_aim_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(aimInfo.localizedActionName, "Right Aim Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    aimInfo.countSubactionPaths = 1;
    aimInfo.subactionPaths = &rightHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &aimInfo, &rightAimPoseAction_),
                 "xrCreateAction(right aim pose)")) {
      return false;
    }

    XrActionCreateInfo triggerInfo{};
    triggerInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    triggerInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strncpy(triggerInfo.actionName, "right_trigger", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(triggerInfo.localizedActionName, "Right Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    triggerInfo.countSubactionPaths = 1;
    triggerInfo.subactionPaths = &rightHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &triggerInfo, &rightTriggerAction_),
                 "xrCreateAction(right trigger)")) {
      return false;
    }

    XrActionCreateInfo gripPoseInfo{};
    gripPoseInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    gripPoseInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strncpy(gripPoseInfo.actionName, "object_grip_pose", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(gripPoseInfo.localizedActionName, "Object Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    gripPoseInfo.countSubactionPaths = static_cast<uint32_t>(handPaths.size());
    gripPoseInfo.subactionPaths = handPaths.data();
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &gripPoseInfo, &gripPoseAction_),
                 "xrCreateAction(object grip pose)")) {
      return false;
    }

    XrActionCreateInfo gripValueInfo{};
    gripValueInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    gripValueInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    std::strncpy(gripValueInfo.actionName, "object_grip_value", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(gripValueInfo.localizedActionName, "Object Grip Value", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    gripValueInfo.countSubactionPaths = static_cast<uint32_t>(handPaths.size());
    gripValueInfo.subactionPaths = handPaths.data();
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &gripValueInfo, &gripValueAction_),
                 "xrCreateAction(object grip value)")) {
      return false;
    }

    XrActionCreateInfo nextToolInfo{};
    nextToolInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    nextToolInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(nextToolInfo.actionName, "next_tool", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(nextToolInfo.localizedActionName, "Next Tool", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    nextToolInfo.countSubactionPaths = 1;
    nextToolInfo.subactionPaths = &rightHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &nextToolInfo, &nextToolAction_),
                 "xrCreateAction(next tool)")) {
      return false;
    }

    XrActionCreateInfo previousToolInfo{};
    previousToolInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    previousToolInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(previousToolInfo.actionName, "previous_tool", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(previousToolInfo.localizedActionName, "Previous Tool", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    previousToolInfo.countSubactionPaths = 1;
    previousToolInfo.subactionPaths = &rightHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &previousToolInfo, &previousToolAction_),
                 "xrCreateAction(previous tool)")) {
      return false;
    }

    XrActionCreateInfo leftStickInfo{};
    leftStickInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    leftStickInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(leftStickInfo.actionName, "brush_adjust", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(leftStickInfo.localizedActionName, "Brush Adjust", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    leftStickInfo.countSubactionPaths = 1;
    leftStickInfo.subactionPaths = &leftHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &leftStickInfo, &brushAdjustAction_),
                 "xrCreateAction(brush adjust)")) {
      return false;
    }

    XrActionCreateInfo rightStickInfo{};
    rightStickInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    rightStickInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strncpy(rightStickInfo.actionName, "locomotion", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(rightStickInfo.localizedActionName, "Locomotion", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    rightStickInfo.countSubactionPaths = 1;
    rightStickInfo.subactionPaths = &rightHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &rightStickInfo, &locomotionAction_),
                 "xrCreateAction(locomotion)")) {
      return false;
    }

    XrActionCreateInfo undoInfo{};
    undoInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    undoInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(undoInfo.actionName, "undo_sculpt", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(undoInfo.localizedActionName, "Undo Sculpt", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    undoInfo.countSubactionPaths = 1;
    undoInfo.subactionPaths = &leftHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &undoInfo, &undoAction_),
                 "xrCreateAction(undo sculpt)")) {
      return false;
    }

    XrActionCreateInfo redoInfo{};
    redoInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    redoInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(redoInfo.actionName, "redo_sculpt", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(redoInfo.localizedActionName, "Redo Sculpt", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    redoInfo.countSubactionPaths = 1;
    redoInfo.subactionPaths = &leftHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &redoInfo, &redoAction_),
                 "xrCreateAction(redo sculpt)")) {
      return false;
    }

    XrActionCreateInfo menuInfo{};
    menuInfo.type = XR_TYPE_ACTION_CREATE_INFO;
    menuInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strncpy(menuInfo.actionName, "open_file_menu", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(menuInfo.localizedActionName, "Open File Menu", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    menuInfo.countSubactionPaths = 1;
    menuInfo.subactionPaths = &leftHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateAction(actionSet_, &menuInfo, &menuAction_),
                 "xrCreateAction(open file menu)")) {
      return false;
    }

    XrPath interactionProfile = XR_NULL_PATH;
    XrPath leftGripPosePath = XR_NULL_PATH;
    XrPath leftGripValuePath = XR_NULL_PATH;
    XrPath leftStickPath = XR_NULL_PATH;
    XrPath rightStickPath = XR_NULL_PATH;
    XrPath leftXPath = XR_NULL_PATH;
    XrPath leftYPath = XR_NULL_PATH;
    XrPath leftMenuPath = XR_NULL_PATH;
    XrPath rightAimPath = XR_NULL_PATH;
    XrPath rightTriggerPath = XR_NULL_PATH;
    XrPath rightGripPosePath = XR_NULL_PATH;
    XrPath rightGripValuePath = XR_NULL_PATH;
    XrPath rightAPath = XR_NULL_PATH;
    XrPath rightBPath = XR_NULL_PATH;
    if (!checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/interaction_profiles/oculus/touch_controller", &interactionProfile),
                 "xrStringToPath(oculus touch profile)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left/input/grip/pose", &leftGripPosePath),
                 "xrStringToPath(left grip pose path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left/input/squeeze/value", &leftGripValuePath),
                 "xrStringToPath(left squeeze path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left/input/thumbstick", &leftStickPath),
                 "xrStringToPath(left thumbstick path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/thumbstick", &rightStickPath),
                 "xrStringToPath(right thumbstick path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left/input/x/click", &leftXPath),
                 "xrStringToPath(left X path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left/input/y/click", &leftYPath),
                 "xrStringToPath(left Y path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/left/input/menu/click", &leftMenuPath),
                 "xrStringToPath(left menu path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/aim/pose", &rightAimPath),
                 "xrStringToPath(right aim path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/trigger/value", &rightTriggerPath),
                 "xrStringToPath(right trigger path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/grip/pose", &rightGripPosePath),
                 "xrStringToPath(right grip pose path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/squeeze/value", &rightGripValuePath),
                 "xrStringToPath(right squeeze path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/a/click", &rightAPath),
                 "xrStringToPath(right A path)") ||
        !checkXr(xrInstance_,
                 xrStringToPath(xrInstance_, "/user/hand/right/input/b/click", &rightBPath),
                 "xrStringToPath(right B path)")) {
      return false;
    }

    std::array<XrActionSuggestedBinding, 13> bindings{{
        {gripPoseAction_, leftGripPosePath},
        {gripValueAction_, leftGripValuePath},
        {brushAdjustAction_, leftStickPath},
        {locomotionAction_, rightStickPath},
        {undoAction_, leftXPath},
        {redoAction_, leftYPath},
        {menuAction_, leftMenuPath},
        {rightAimPoseAction_, rightAimPath},
        {rightTriggerAction_, rightTriggerPath},
        {gripPoseAction_, rightGripPosePath},
        {gripValueAction_, rightGripValuePath},
        {nextToolAction_, rightAPath},
        {previousToolAction_, rightBPath},
    }};

    XrInteractionProfileSuggestedBinding suggestedBinding{};
    suggestedBinding.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggestedBinding.interactionProfile = interactionProfile;
    suggestedBinding.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggestedBinding.suggestedBindings = bindings.data();

    if (!checkXr(xrInstance_,
                 xrSuggestInteractionProfileBindings(xrInstance_, &suggestedBinding),
                 "xrSuggestInteractionProfileBindings")) {
      return false;
    }

    logInfo("OpenXR actions ready: aim/trigger/buttons/sticks + both grip controls");
    return true;
  }

  bool initializeActionSpacesAndAttachSet() {
    XrActionSpaceCreateInfo actionSpaceInfo{};
    actionSpaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
    actionSpaceInfo.action = rightAimPoseAction_;
    actionSpaceInfo.subactionPath = rightHandPath_;
    actionSpaceInfo.poseInActionSpace.orientation.w = 1.0f;
    if (!checkXr(xrInstance_,
                 xrCreateActionSpace(xrSession_, &actionSpaceInfo, &rightAimSpace_),
                 "xrCreateActionSpace(right aim)")) {
      return false;
    }

    actionSpaceInfo.action = gripPoseAction_;
    actionSpaceInfo.subactionPath = leftHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateActionSpace(xrSession_, &actionSpaceInfo, &leftGripSpace_),
                 "xrCreateActionSpace(left grip)")) {
      return false;
    }

    actionSpaceInfo.action = gripPoseAction_;
    actionSpaceInfo.subactionPath = rightHandPath_;
    if (!checkXr(xrInstance_,
                 xrCreateActionSpace(xrSession_, &actionSpaceInfo, &rightGripSpace_),
                 "xrCreateActionSpace(right grip)")) {
      return false;
    }

    XrSessionActionSetsAttachInfo attachInfo{};
    attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet_;
    return checkXr(xrInstance_,
                   xrAttachSessionActionSets(xrSession_, &attachInfo),
                   "xrAttachSessionActionSets");
  }

  bool createSwapchains() {
    uint32_t viewCount = 0;
    if (!checkXr(xrInstance_,
                 xrEnumerateViewConfigurationViews(
                     xrInstance_, xrSystemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr),
                 "xrEnumerateViewConfigurationViews(count)") ||
        viewCount < kEyeCount) {
      logError("Primary stereo view configuration is unavailable");
      return false;
    }

    viewConfigs_.resize(viewCount);
    for (auto& viewConfig : viewConfigs_) {
      viewConfig.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }

    if (!checkXr(xrInstance_,
                 xrEnumerateViewConfigurationViews(xrInstance_,
                                                   xrSystemId_,
                                                   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                   viewCount,
                                                   &viewCount,
                                                   viewConfigs_.data()),
                 "xrEnumerateViewConfigurationViews(data)")) {
      return false;
    }

    uint32_t formatCount = 0;
    if (!checkXr(xrInstance_,
                 xrEnumerateSwapchainFormats(xrSession_, 0, &formatCount, nullptr),
                 "xrEnumerateSwapchainFormats(count)") ||
        formatCount == 0) {
      return false;
    }

    std::vector<int64_t> formats(formatCount);
    if (!checkXr(xrInstance_,
                 xrEnumerateSwapchainFormats(xrSession_, formatCount, &formatCount, formats.data()),
                 "xrEnumerateSwapchainFormats(data)")) {
      return false;
    }

    const int64_t colorFormat = chooseColorFormat(formats);
    for (int eye = 0; eye < kEyeCount; ++eye) {
      EyeSwapchain& swapchain = eyeSwapchains_[eye];
      const int32_t recommendedWidth = static_cast<int32_t>(viewConfigs_[eye].recommendedImageRectWidth);
      const int32_t recommendedHeight = static_cast<int32_t>(viewConfigs_[eye].recommendedImageRectHeight);
      swapchain.width = std::max(1, static_cast<int32_t>(static_cast<float>(recommendedWidth) * kVrRenderScale));
      swapchain.height = std::max(1, static_cast<int32_t>(static_cast<float>(recommendedHeight) * kVrRenderScale));

      XrSwapchainCreateInfo createInfo{};
      createInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
      createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
      createInfo.format = colorFormat;
      createInfo.sampleCount = viewConfigs_[eye].recommendedSwapchainSampleCount;
      createInfo.width = static_cast<uint32_t>(swapchain.width);
      createInfo.height = static_cast<uint32_t>(swapchain.height);
      createInfo.faceCount = 1;
      createInfo.arraySize = 1;
      createInfo.mipCount = 1;

      if (!checkXr(xrInstance_,
                   xrCreateSwapchain(xrSession_, &createInfo, &swapchain.handle),
                   "xrCreateSwapchain")) {
        return false;
      }

      uint32_t imageCount = 0;
      if (!checkXr(xrInstance_,
                   xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr),
                   "xrEnumerateSwapchainImages(count)") ||
          imageCount == 0) {
        return false;
      }

      swapchain.images.resize(imageCount);
      for (auto& image : swapchain.images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
      }

      if (!checkXr(xrInstance_,
                   xrEnumerateSwapchainImages(swapchain.handle,
                                              imageCount,
                                              &imageCount,
                                              reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
                   "xrEnumerateSwapchainImages(data)")) {
        return false;
      }

      logInfo("Eye %d swapchain: %dx%d at %.2fx render scale, images=%u",
              eye,
              swapchain.width,
              swapchain.height,
              kVrRenderScale,
              imageCount);
    }

    views_.resize(kEyeCount);
    for (auto& view : views_) {
      view.type = XR_TYPE_VIEW;
    }
    return true;
  }

  int64_t chooseColorFormat(const std::vector<int64_t>& formats) const {
    const std::array<int64_t, 2> preferred{
        GL_SRGB8_ALPHA8,
        GL_RGBA8,
    };

    for (int64_t candidate : preferred) {
      if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
        return candidate;
      }
    }
    return formats.front();
  }

  void pollOpenXrEvents() {
    while (true) {
      XrEventDataBuffer event{};
      event.type = XR_TYPE_EVENT_DATA_BUFFER;
      const XrResult result = xrPollEvent(xrInstance_, &event);
      if (result == XR_EVENT_UNAVAILABLE) {
        return;
      }
      if (!checkXr(xrInstance_, result, "xrPollEvent")) {
        return;
      }

      switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
          handleSessionState(*reinterpret_cast<const XrEventDataSessionStateChanged*>(&event));
          break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
          ANativeActivity_finish(app_->activity);
          break;
        default:
          break;
      }
    }
  }

  void handleSessionState(const XrEventDataSessionStateChanged& event) {
    xrSessionState_ = event.state;
    logInfo("OpenXR session state changed: %d", xrSessionState_);

    if (xrSessionState_ == XR_SESSION_STATE_READY) {
      XrSessionBeginInfo beginInfo{};
      beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
      beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
      if (checkXr(xrInstance_, xrBeginSession(xrSession_, &beginInfo), "xrBeginSession")) {
        xrSessionRunning_ = true;
      }
    } else if (xrSessionState_ == XR_SESSION_STATE_STOPPING) {
      xrSessionRunning_ = false;
      checkXr(xrInstance_, xrEndSession(xrSession_), "xrEndSession");
    } else if (xrSessionState_ == XR_SESSION_STATE_EXITING ||
               xrSessionState_ == XR_SESSION_STATE_LOSS_PENDING) {
      // Finish the Android activity; the glue then delivers APP_CMD_DESTROY
      // which sets destroyRequested through the normal lifecycle.
      ANativeActivity_finish(app_->activity);
    }
  }

  void renderOpenXrFrame() {
    if (!xrSessionRunning_) {
      tickDiagnostics();
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      return;
    }

    XrFrameWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    if (!checkXr(xrInstance_, xrWaitFrame(xrSession_, &waitInfo, &frameState), "xrWaitFrame")) {
      return;
    }

    XrFrameBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (!checkXr(xrInstance_, xrBeginFrame(xrSession_, &beginInfo), "xrBeginFrame")) {
      return;
    }

    std::array<XrCompositionLayerProjectionView, kEyeCount> projectionViews{};
    std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
    uint32_t layerCount = 0;
    XrCompositionLayerPassthroughFB passthroughCompositionLayer{};
    XrCompositionLayerProjection projectionLayer{};

    if (frameState.shouldRender && locateViews(frameState.predictedDisplayTime)) {
      updateControllerAndSculpt(frameState.predictedDisplayTime);
      updateHudTexture();
      for (int eye = 0; eye < kEyeCount; ++eye) {
        renderEye(eye);

        projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projectionViews[eye].pose = views_[eye].pose;
        projectionViews[eye].fov = views_[eye].fov;
        projectionViews[eye].subImage.swapchain = eyeSwapchains_[eye].handle;
        projectionViews[eye].subImage.imageRect.offset = {0, 0};
        projectionViews[eye].subImage.imageRect.extent = {eyeSwapchains_[eye].width, eyeSwapchains_[eye].height};
      }

      // In AR the passthrough layer must sit *under* the projection layer,
      // and the projection layer must alpha-blend over it (the SDF shader
      // outputs alpha 0 on the background in AR mode).
      const bool arActive = arModeEnabled_ && passthroughReady_;
      if (arActive) {
        passthroughCompositionLayer.type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
        passthroughCompositionLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        passthroughCompositionLayer.space = XR_NULL_HANDLE;
        passthroughCompositionLayer.layerHandle = passthroughLayer_;
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughCompositionLayer);
      }

      projectionLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
      projectionLayer.layerFlags = arActive
                                       ? (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                          XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT)
                                       : 0;
      projectionLayer.space = xrSpace_;
      projectionLayer.viewCount = kEyeCount;
      projectionLayer.views = projectionViews.data();
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);
    }

    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layerCount > 0 ? layers.data() : nullptr;
    checkXr(xrInstance_, xrEndFrame(xrSession_, &endInfo), "xrEndFrame");
    ++frameCounter_;
  }

  bool locateViews(XrTime predictedDisplayTime) {
    XrViewLocateInfo locateInfo{};
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = xrSpace_;

    XrViewState viewState{};
    viewState.type = XR_TYPE_VIEW_STATE;
    uint32_t viewCount = 0;
    if (!checkXr(xrInstance_,
                 xrLocateViews(xrSession_,
                               &locateInfo,
                               &viewState,
                               static_cast<uint32_t>(views_.size()),
                               &viewCount,
                               views_.data()),
                 "xrLocateViews")) {
      return false;
    }

    return viewCount == kEyeCount;
  }

  struct ControllerPose {
    bool active = false;
    large::sdf::Vec3 position{};
    XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
  };

  struct ToolSettings {
    float radius = kDefaultBrushRadius;
    float strength = kDefaultBrushStrength;
  };

  struct HandState {
    bool active = false;
    bool fist = false;
    bool open = false;
    bool pinch = false;
    ControllerPose pose{};
    large::sdf::Vec3 pinchPosition{};
    large::sdf::Vec3 openToolPosition{};
    large::sdf::Vec3 fistToolPosition{};
    float thumbIndexDistance = 1.0f;
    large::sdf::Vec3 thumbTip{};
    large::sdf::Vec3 thumbDirection{0.0f, 1.0f, 0.0f};
    large::sdf::Vec3 indexTip{};
    large::sdf::Vec3 indexDirection{0.0f, 0.0f, -1.0f};
    bool indexPointing = false;
    std::array<large::sdf::Vec3, XR_HAND_JOINT_COUNT_EXT> joints{};
  };

  float readFloatAction(XrAction action, XrPath subactionPath, const char* label) {
    if (action == XR_NULL_HANDLE) {
      return 0.0f;
    }

    XrActionStateGetInfo info{};
    info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    info.action = action;
    info.subactionPath = subactionPath;

    XrActionStateFloat state{};
    state.type = XR_TYPE_ACTION_STATE_FLOAT;
    const XrResult result = xrGetActionStateFloat(xrSession_, &info, &state);
    if (!XR_SUCCEEDED(result)) {
      logError("xrGetActionStateFloat(%s) failed: %s", label, xrResultName(xrInstance_, result));
      return 0.0f;
    }
    return state.isActive ? state.currentState : 0.0f;
  }

  XrVector2f readVector2Action(XrAction action, XrPath subactionPath, const char* label) {
    if (action == XR_NULL_HANDLE) {
      return {0.0f, 0.0f};
    }

    XrActionStateGetInfo info{};
    info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    info.action = action;
    info.subactionPath = subactionPath;

    XrActionStateVector2f state{};
    state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    const XrResult result = xrGetActionStateVector2f(xrSession_, &info, &state);
    if (!XR_SUCCEEDED(result)) {
      logError("xrGetActionStateVector2f(%s) failed: %s", label, xrResultName(xrInstance_, result));
      return {0.0f, 0.0f};
    }
    return state.isActive ? state.currentState : XrVector2f{0.0f, 0.0f};
  }

  bool readBoolAction(XrAction action, XrPath subactionPath, const char* label) {
    if (action == XR_NULL_HANDLE) {
      return false;
    }

    XrActionStateGetInfo info{};
    info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    info.action = action;
    info.subactionPath = subactionPath;

    XrActionStateBoolean state{};
    state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    const XrResult result = xrGetActionStateBoolean(xrSession_, &info, &state);
    if (!XR_SUCCEEDED(result)) {
      logError("xrGetActionStateBoolean(%s) failed: %s", label, xrResultName(xrInstance_, result));
      return false;
    }
    return state.isActive && state.currentState;
  }

  void storeActiveToolSettings() {
    toolSettings_[static_cast<std::size_t>(activeToolIndex())] = {brushRadius_, brushStrength_};
  }

  void loadActiveToolSettings() {
    const ToolSettings& settings = toolSettings_[static_cast<std::size_t>(activeToolIndex())];
    brushRadius_ = settings.radius;
    brushStrength_ = settings.strength;
  }

  void clearStretchInteraction() {
    stretchAnchorSet_ = false;
    stretchPullActive_ = false;
    stretchPullStartToolOrientationLocal_ = {0.0f, 0.0f, 0.0f, 1.0f};
    stretchUploadBounds_ = {};
    rightHandPinchStretchActive_ = false;
    rightHandPinchWasActive_ = false;
    rightHandPinchToolActive_ = false;
    rightHandPinchTool_ = -1;
    rightHandShapeBrushActive_ = false;
    rightHandShapeBrushTool_ = -1;
    strokeHasLast_ = false;
  }

  void setActiveTool(VrTool tool, const char* source) {
    storeActiveToolSettings();
    activeTool_ = tool;
    loadActiveToolSettings();
    clearStretchInteraction();
    logInfo("%s tool: %s radius=%.3f strength=%.2f",
            source,
            toolName(activeTool_),
            brushRadius_,
            brushStrength_);
  }

  void updateBrushAdjustments() {
    leftStickValue_ = readVector2Action(brushAdjustAction_, leftHandPath_, "brush adjust");
    constexpr float deadzone = 0.18f;
    bool changed = false;

    if (std::abs(leftStickValue_.x) > deadzone) {
      brushStrength_ = large::sdf::clamp(brushStrength_ + leftStickValue_.x * 0.008f,
                                         kMinimumBrushStrength,
                                         kMaximumBrushStrength);
      changed = true;
    }

    if (std::abs(leftStickValue_.y) > deadzone) {
      brushRadius_ = large::sdf::clamp(brushRadius_ * std::exp(leftStickValue_.y * 0.020f),
                                       kMinimumBrushRadius,
                                       kMaximumBrushRadius);
      changed = true;
    }

    if (changed && frameCounter_ >= nextBrushAdjustLogFrame_) {
      logInfo("Brush adjusted: radius=%.3f strength=%.2f", brushRadius_, brushStrength_);
      nextBrushAdjustLogFrame_ = frameCounter_ + 30;
    }
    if (changed) {
      storeActiveToolSettings();
    }
  }

  void updateEditButtons() {
    const bool undoDown = readBoolAction(undoAction_, leftHandPath_, "undo");
    const bool redoDown = readBoolAction(redoAction_, leftHandPath_, "redo");
    const bool menuDown = readBoolAction(menuAction_, leftHandPath_, "menu");

    if (undoDown && !undoWasDown_) {
      if (history_.undo()) {
        uploadSdfTexture();
        markAllColorDirty();
        uploadColorTexture();
        stretchSourceVolume_ = volume_;
        clearStretchInteraction();
        logInfo("Undo sculpt");
      } else {
        logInfo("Undo sculpt: no snapshot");
      }
    }

    if (redoDown && !redoWasDown_) {
      if (history_.redo()) {
        uploadSdfTexture();
        markAllColorDirty();
        uploadColorTexture();
        stretchSourceVolume_ = volume_;
        clearStretchInteraction();
        logInfo("Redo sculpt");
      } else {
        logInfo("Redo sculpt: no snapshot");
      }
    }

    if (menuDown && !menuWasDown_) {
      menuVisible_ = !menuVisible_;
      logInfo("Menu %s: tools/AR/save/load/export/quit actions are queued for the next UI pass",
              menuVisible_ ? "opened" : "closed");
    }

    undoWasDown_ = undoDown;
    redoWasDown_ = redoDown;
    menuWasDown_ = menuDown;
  }

  std::filesystem::path appDataPath() const {
    const char* basePath = app_->activity->externalDataPath;
    if (basePath == nullptr || basePath[0] == '\0') {
      basePath = app_->activity->internalDataPath;
    }
    if (basePath == nullptr || basePath[0] == '\0') {
      return "/sdcard/Android/data/com.large.sdfquest/files";
    }
    return basePath;
  }

  std::filesystem::path savedVolumePath() const { return appDataPath() / "large_sdf_volume.bin"; }

  void saveSdfVolume() {
    try {
      const std::filesystem::path path = savedVolumePath();
      std::filesystem::create_directories(path.parent_path());

      std::ofstream out(path, std::ios::binary);
      if (!out) {
        throw std::runtime_error("open failed");
      }

      // v2 appends the RGBA color volume after the SDF values; v1 files
      // (SDF only) remain loadable.
      constexpr char magic[8] = {'L', 'S', 'D', 'F', 'V', 'R', '2', '\0'};
      const large::sdf::IVec3 size = volume_.size();
      const std::int32_t dims[3] = {size.x, size.y, size.z};
      const float voxelSize = volume_.voxelSize();
      const large::sdf::Vec3 origin = volume_.origin();
      const float originValues[3] = {origin.x, origin.y, origin.z};
      const std::uint64_t valueCount = static_cast<std::uint64_t>(volume_.values().size());

      out.write(magic, sizeof(magic));
      out.write(reinterpret_cast<const char*>(dims), sizeof(dims));
      out.write(reinterpret_cast<const char*>(&voxelSize), sizeof(voxelSize));
      out.write(reinterpret_cast<const char*>(originValues), sizeof(originValues));
      out.write(reinterpret_cast<const char*>(&valueCount), sizeof(valueCount));
      out.write(reinterpret_cast<const char*>(volume_.values().data()),
                static_cast<std::streamsize>(volume_.values().size() * sizeof(float)));
      if (colorVoxels_.size() == valueCount * 4) {
        out.write(reinterpret_cast<const char*>(colorVoxels_.data()),
                  static_cast<std::streamsize>(colorVoxels_.size()));
      }
      if (!out) {
        throw std::runtime_error("write failed");
      }
      logInfo("Menu SAVE: %s", path.string().c_str());
    } catch (const std::exception& e) {
      logError("Menu SAVE failed: %s", e.what());
    }
  }

  void loadSdfVolume() {
    try {
      const std::filesystem::path path = savedVolumePath();
      std::ifstream in(path, std::ios::binary);
      if (!in) {
        throw std::runtime_error("open failed");
      }

      char magic[8]{};
      std::int32_t dims[3]{};
      float voxelSize = 0.0f;
      float originValues[3]{};
      std::uint64_t valueCount = 0;

      in.read(magic, sizeof(magic));
      in.read(reinterpret_cast<char*>(dims), sizeof(dims));
      in.read(reinterpret_cast<char*>(&voxelSize), sizeof(voxelSize));
      in.read(reinterpret_cast<char*>(originValues), sizeof(originValues));
      in.read(reinterpret_cast<char*>(&valueCount), sizeof(valueCount));

      constexpr char magicV1[8] = {'L', 'S', 'D', 'F', 'V', 'R', '1', '\0'};
      constexpr char magicV2[8] = {'L', 'S', 'D', 'F', 'V', 'R', '2', '\0'};
      const bool isV1 = std::memcmp(magic, magicV1, sizeof(magicV1)) == 0;
      const bool isV2 = std::memcmp(magic, magicV2, sizeof(magicV2)) == 0;
      const large::sdf::IVec3 size = volume_.size();
      if ((!isV1 && !isV2) || dims[0] != size.x || dims[1] != size.y ||
          dims[2] != size.z || valueCount != volume_.values().size()) {
        throw std::runtime_error("file does not match current volume");
      }
      if (std::abs(voxelSize - volume_.voxelSize()) > 0.000001f) {
        throw std::runtime_error("voxel size mismatch");
      }

      std::vector<float> values(static_cast<std::size_t>(valueCount));
      in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(values.size() * sizeof(float)));
      if (!in) {
        throw std::runtime_error("read failed");
      }

      std::vector<std::uint8_t> colors;
      if (isV2) {
        colors.resize(static_cast<std::size_t>(valueCount) * 4);
        in.read(reinterpret_cast<char*>(colors.data()), static_cast<std::streamsize>(colors.size()));
        if (!in) {
          throw std::runtime_error("color read failed");
        }
      }

      history_.capture();
      volume_.restoreValues(std::move(values));
      if (isV2) {
        colorVoxels_ = std::move(colors);
        markAllColorDirty();
      } else if (!colorVoxels_.empty()) {
        // v1 files carry no paint: reset the color volume to clay.
        initializeColorVoxels();
      }
      stretchSourceVolume_ = volume_;
      clearStretchInteraction();
      uploadSdfTexture();
      uploadColorTexture();
      logInfo("Menu LOAD: %s (%s)", path.string().c_str(), isV2 ? "v2" : "v1");
    } catch (const std::exception& e) {
      logError("Menu LOAD failed: %s", e.what());
    }
  }

  // Copies a local file into the shared "Documents/LargeSculpVR" folder via
  // MediaStore, so exports are visible in the Quest file manager and over USB
  // without any storage permission.
  bool publishFileToDocuments(const std::filesystem::path& source, const char* displayName) {
    JavaVM* vm = app_->activity->vm;
    JNIEnv* env = nullptr;
    bool attachedHere = false;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
      if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
        logError("EXPORT publish: cannot attach JNI thread");
        return false;
      }
      attachedHere = true;
    }

    const auto clearException = [&]() {
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return true;
      }
      return false;
    };

    bool ok = false;
    do {
      const jobject activity = app_->activity->clazz;
      const jclass activityClass = env->GetObjectClass(activity);
      const jmethodID getResolver =
          env->GetMethodID(activityClass, "getContentResolver", "()Landroid/content/ContentResolver;");
      const jobject resolver = getResolver != nullptr ? env->CallObjectMethod(activity, getResolver) : nullptr;
      if (clearException() || resolver == nullptr) {
        break;
      }

      const jclass valuesClass = env->FindClass("android/content/ContentValues");
      const jmethodID valuesCtor = env->GetMethodID(valuesClass, "<init>", "()V");
      const jobject values = env->NewObject(valuesClass, valuesCtor);
      const jmethodID putString =
          env->GetMethodID(valuesClass, "put", "(Ljava/lang/String;Ljava/lang/String;)V");
      if (clearException() || values == nullptr || putString == nullptr) {
        break;
      }
      const auto putValue = [&](const char* key, const char* value) {
        const jstring jKey = env->NewStringUTF(key);
        const jstring jValue = env->NewStringUTF(value);
        env->CallVoidMethod(values, putString, jKey, jValue);
        env->DeleteLocalRef(jKey);
        env->DeleteLocalRef(jValue);
      };
      putValue("_display_name", displayName);
      putValue("mime_type", "application/octet-stream");
      putValue("relative_path", "Documents/LargeSculpVR/");
      if (clearException()) {
        break;
      }

      const jclass filesClass = env->FindClass("android/provider/MediaStore$Files");
      const jmethodID getContentUri =
          env->GetStaticMethodID(filesClass, "getContentUri", "(Ljava/lang/String;)Landroid/net/Uri;");
      const jstring volumeName = env->NewStringUTF("external");
      const jobject collection = env->CallStaticObjectMethod(filesClass, getContentUri, volumeName);
      if (clearException() || collection == nullptr) {
        break;
      }

      const jclass resolverClass = env->GetObjectClass(resolver);
      const jmethodID insert = env->GetMethodID(
          resolverClass, "insert", "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;");
      const jobject uri = env->CallObjectMethod(resolver, insert, collection, values);
      if (clearException() || uri == nullptr) {
        logError("EXPORT publish: MediaStore insert failed");
        break;
      }

      const jmethodID openOutputStream = env->GetMethodID(
          resolverClass, "openOutputStream", "(Landroid/net/Uri;)Ljava/io/OutputStream;");
      const jobject stream = env->CallObjectMethod(resolver, openOutputStream, uri);
      if (clearException() || stream == nullptr) {
        logError("EXPORT publish: openOutputStream failed");
        break;
      }

      const jclass streamClass = env->GetObjectClass(stream);
      const jmethodID writeMethod = env->GetMethodID(streamClass, "write", "([BII)V");
      const jmethodID closeMethod = env->GetMethodID(streamClass, "close", "()V");

      std::ifstream in(source, std::ios::binary);
      std::vector<char> buffer(static_cast<std::size_t>(64) * 1024);
      const jbyteArray jBuffer = env->NewByteArray(static_cast<jsize>(buffer.size()));
      bool writeOk = in.good() && jBuffer != nullptr;
      while (writeOk && in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        if (count <= 0) {
          break;
        }
        env->SetByteArrayRegion(jBuffer, 0, static_cast<jsize>(count),
                                reinterpret_cast<const jbyte*>(buffer.data()));
        env->CallVoidMethod(stream, writeMethod, jBuffer, 0, static_cast<jint>(count));
        if (clearException()) {
          writeOk = false;
        }
      }
      env->CallVoidMethod(stream, closeMethod);
      clearException();
      ok = writeOk;
    } while (false);

    if (attachedHere) {
      vm->DetachCurrentThread();
    }
    return ok;
  }

  void exportSdfVolume() {
    try {
      char fileName[64];
      std::time_t now = std::time(nullptr);
      std::tm timeInfo{};
      localtime_r(&now, &timeInfo);
      std::strftime(fileName, sizeof(fileName), "sculpt_%Y%m%d_%H%M%S.obj", &timeInfo);

      const std::filesystem::path path = appDataPath() / fileName;
      const large::sdf::ObjExportStats stats = large::sdf::exportSdfSurfaceAsObj(
          volume_, path, colorVoxels_.empty() ? nullptr : colorVoxels_.data());
      if (publishFileToDocuments(path, fileName)) {
        logInfo("Menu EXPORT: Documents/LargeSculpVR/%s, vertices=%d faces=%d (vertex colors)",
                fileName,
                stats.vertices,
                stats.faces);
      } else {
        logInfo("Menu EXPORT: %s (Documents copy failed), vertices=%d faces=%d",
                path.string().c_str(),
                stats.vertices,
                stats.faces);
      }
    } catch (const std::exception& e) {
      logError("Menu EXPORT failed: %s", e.what());
    }
  }

  void activateMenuChoice(int choice) {
    if (choice == kMenuToggleChoice) {
      menuVisible_ = !menuVisible_;
      logInfo("Menu %s via panel button", menuVisible_ ? "opened" : "closed");
      return;
    }

    if (choice == kMenuArChoice) {
      setArModeEnabled(!arModeEnabled_);
      return;
    }

    if (choice == kMenuMirrorChoice) {
      mirrorEnabled_ = !mirrorEnabled_;
      logInfo("Mirror mode %s", mirrorEnabled_ ? "enabled" : "disabled");
      return;
    }

    if (choice == kMenuLockChoice) {
      objectLocked_ = !objectLocked_;
      logInfo("Object lock %s", objectLocked_ ? "enabled" : "disabled");
      return;
    }

    if (choice >= kMenuPaletteFirstChoice && choice < kMenuPaletteFirstChoice + large::hud::kPaletteCount) {
      paintColorIndex_ = choice - kMenuPaletteFirstChoice;
      setActiveTool(VrTool::Paint, "Menu palette");
      return;
    }

    if (choice >= 0 && choice < kMenuToolCount) {
      setActiveTool(toolFromIndex(choice), "Menu");
      return;
    }

    switch (choice) {
      case kMenuToolCount + 0:
        saveSdfVolume();
        break;
      case kMenuToolCount + 1:
        loadSdfVolume();
        break;
      case kMenuToolCount + 2:
        exportSdfVolume();
        break;
      case kMenuToolCount + 3:
        logInfo("Menu QUIT");
        // Ask the runtime to end the session; the state machine then goes
        // STOPPING -> xrEndSession -> EXITING -> activity finish. Forcing
        // destroyRequested here would tear GL/XR down mid-frame and crash.
        if (xrSession_ != XR_NULL_HANDLE && xrSessionRunning_) {
          xrRequestExitSession(xrSession_);
        } else {
          ANativeActivity_finish(app_->activity);
        }
        break;
      default:
        break;
    }
  }

  int activeToolIndex() const { return toolIndex(activeTool_); }

  int displayToolIndex() const {
    return handDisplayToolIndex_ >= 0 ? handDisplayToolIndex_ : activeToolIndex();
  }

  void selectToolOffset(int offset) {
    setActiveTool(toolFromIndex(activeToolIndex() + offset), "Active");
  }

  void updateToolButtons() {
    const bool nextDown = readBoolAction(nextToolAction_, rightHandPath_, "next tool");
    const bool previousDown = readBoolAction(previousToolAction_, rightHandPath_, "previous tool");

    if (nextDown && !nextToolWasDown_) {
      selectToolOffset(1);
    }
    if (previousDown && !previousToolWasDown_) {
      selectToolOffset(-1);
    }

    nextToolWasDown_ = nextDown;
    previousToolWasDown_ = previousDown;
  }

  ControllerPose locateControllerPose(XrAction action,
                                      XrPath subactionPath,
                                      XrSpace actionSpace,
                                      XrTime predictedDisplayTime,
                                      XrSpaceLocationFlags requiredFlags,
                                      const char* label) {
    ControllerPose result{};
    if (action == XR_NULL_HANDLE || actionSpace == XR_NULL_HANDLE) {
      return result;
    }

    XrActionStateGetInfo info{};
    info.type = XR_TYPE_ACTION_STATE_GET_INFO;
    info.action = action;
    info.subactionPath = subactionPath;

    XrActionStatePose state{};
    state.type = XR_TYPE_ACTION_STATE_POSE;
    XrResult xrResult = xrGetActionStatePose(xrSession_, &info, &state);
    if (!XR_SUCCEEDED(xrResult)) {
      logError("xrGetActionStatePose(%s) failed: %s", label, xrResultName(xrInstance_, xrResult));
      return result;
    }
    if (!state.isActive) {
      return result;
    }

    XrSpaceLocation location{};
    location.type = XR_TYPE_SPACE_LOCATION;
    xrResult = xrLocateSpace(actionSpace, xrSpace_, predictedDisplayTime, &location);
    if (!XR_SUCCEEDED(xrResult)) {
      logError("xrLocateSpace(%s) failed: %s", label, xrResultName(xrInstance_, xrResult));
      return result;
    }
    if ((location.locationFlags & requiredFlags) != requiredFlags) {
      return result;
    }

    result.active = true;
    result.position = makeVec3(location.pose.position);
    result.orientation = location.pose.orientation;
    return result;
  }

  bool locateHandState(XrHandTrackerEXT tracker, HandState& state, XrTime predictedDisplayTime, const char* label) {
    const HandState previous = state;
    state = {};
    if (!handTrackingReady_ || tracker == XR_NULL_HANDLE || xrLocateHandJointsEXT_ == nullptr) {
      return false;
    }

    std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
    XrHandJointLocationsEXT locations{};
    locations.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT;
    locations.jointCount = static_cast<uint32_t>(joints.size());
    locations.jointLocations = joints.data();

    XrHandJointsLocateInfoEXT locateInfo{};
    locateInfo.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT;
    locateInfo.baseSpace = xrSpace_;
    locateInfo.time = predictedDisplayTime;

    const XrResult result = xrLocateHandJointsEXT_(tracker, &locateInfo, &locations);
    if (!XR_SUCCEEDED(result)) {
      if (frameCounter_ >= nextHandLogFrame_) {
        logError("xrLocateHandJointsEXT(%s) failed: %s", label, xrResultName(xrInstance_, result));
        nextHandLogFrame_ = frameCounter_ + 180;
      }
      return false;
    }
    if (locations.isActive != XR_TRUE) {
      return false;
    }

    const auto jointValid = [&](XrHandJointEXT joint) {
      const XrSpaceLocationFlags flags = joints[static_cast<std::size_t>(joint)].locationFlags;
      return (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    };
    if (!jointValid(XR_HAND_JOINT_PALM_EXT) || !jointValid(XR_HAND_JOINT_THUMB_TIP_EXT) ||
        !jointValid(XR_HAND_JOINT_INDEX_TIP_EXT) || !jointValid(XR_HAND_JOINT_MIDDLE_TIP_EXT) ||
        !jointValid(XR_HAND_JOINT_RING_TIP_EXT) || !jointValid(XR_HAND_JOINT_LITTLE_TIP_EXT)) {
      return false;
    }

    const large::sdf::Vec3 palm = makeVec3(joints[XR_HAND_JOINT_PALM_EXT].pose.position);
    const large::sdf::Vec3 thumbTip = makeVec3(joints[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position);
    const large::sdf::Vec3 indexTip = makeVec3(joints[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position);
    const large::sdf::Vec3 middleTip = makeVec3(joints[XR_HAND_JOINT_MIDDLE_TIP_EXT].pose.position);
    const large::sdf::Vec3 ringTip = makeVec3(joints[XR_HAND_JOINT_RING_TIP_EXT].pose.position);
    const large::sdf::Vec3 littleTip = makeVec3(joints[XR_HAND_JOINT_LITTLE_TIP_EXT].pose.position);

    const float indexDistance = large::sdf::length(indexTip - palm);
    const float middleDistance = large::sdf::length(middleTip - palm);
    const float ringDistance = large::sdf::length(ringTip - palm);
    const float littleDistance = large::sdf::length(littleTip - palm);
    const float thumbDistance = large::sdf::length(thumbTip - palm);
    const float pinchDistance = large::sdf::length(thumbTip - indexTip);

    state.active = true;
    state.pose.active = true;
    state.pose.position = palm;
    for (std::size_t i = 0; i < joints.size(); ++i) {
      const XrSpaceLocationFlags flags = joints[i].locationFlags;
      state.joints[i] = (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0
                            ? makeVec3(joints[i].pose.position)
                            : palm;
    }
    const XrSpaceLocationFlags palmFlags = joints[XR_HAND_JOINT_PALM_EXT].locationFlags;
    state.pose.orientation = (palmFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0
                                  ? joints[XR_HAND_JOINT_PALM_EXT].pose.orientation
                                  : XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
    state.pinchPosition = (thumbTip + indexTip) * 0.5f;
    state.openToolPosition = (indexTip + middleTip + ringTip + littleTip) * 0.25f;
    if (jointValid(XR_HAND_JOINT_INDEX_PROXIMAL_EXT) && jointValid(XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT) &&
        jointValid(XR_HAND_JOINT_RING_PROXIMAL_EXT) && jointValid(XR_HAND_JOINT_LITTLE_PROXIMAL_EXT)) {
      state.fistToolPosition =
          (makeVec3(joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT].pose.position) +
           makeVec3(joints[XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT].pose.position) +
           makeVec3(joints[XR_HAND_JOINT_RING_PROXIMAL_EXT].pose.position) +
           makeVec3(joints[XR_HAND_JOINT_LITTLE_PROXIMAL_EXT].pose.position)) *
          0.25f;
    } else {
      state.fistToolPosition = palm;
    }
    state.thumbIndexDistance = pinchDistance;
    state.thumbTip = thumbTip;
    if (jointValid(XR_HAND_JOINT_THUMB_PROXIMAL_EXT)) {
      const large::sdf::Vec3 thumbBase = makeVec3(joints[XR_HAND_JOINT_THUMB_PROXIMAL_EXT].pose.position);
      const large::sdf::Vec3 thumbDirection = large::sdf::normalize(thumbTip - thumbBase);
      state.thumbDirection =
          large::sdf::dot(thumbDirection, thumbDirection) > 0.001f ? thumbDirection : state.thumbDirection;
    }
    state.indexTip = indexTip;
    const large::sdf::Vec3 indexBase = makeVec3(joints[XR_HAND_JOINT_INDEX_PROXIMAL_EXT].pose.position);
    const large::sdf::Vec3 indexDirection = large::sdf::normalize(indexTip - indexBase);
    state.indexDirection = large::sdf::dot(indexDirection, indexDirection) > 0.001f ? indexDirection : state.indexDirection;
    state.indexPointing = indexDistance > kHandFistTipDistance * 1.25f;

    // Hysteresis: a gesture engages below its trigger threshold but only
    // releases above a wider one, so tracking jitter cannot make it flicker.
    const float fistThreshold = previous.fist ? kHandFistReleaseTipDistance : kHandFistTipDistance;
    state.fist = indexDistance < fistThreshold && middleDistance < fistThreshold &&
                 ringDistance < fistThreshold && littleDistance < fistThreshold &&
                 thumbDistance < fistThreshold * 1.35f;
    const float pinchThreshold = previous.pinch ? kHandPinchReleaseDistance : kHandPinchDistance;
    state.pinch = !state.fist && pinchDistance < pinchThreshold;
    const float openScale = previous.open ? 0.90f : 1.0f;
    state.open = !state.fist && !state.pinch && pinchDistance > kHandOpenPinchDistance * openScale &&
                 indexDistance > kHandOpenFingerDistance * openScale &&
                 middleDistance > kHandOpenFingerDistance * openScale &&
                 ringDistance > kHandOpenFingerDistance * 0.92f * openScale &&
                 littleDistance > kHandOpenLittleDistance * openScale &&
                 thumbDistance > kHandOpenThumbDistance * openScale;

    // Light exponential smoothing of the tracked positions to remove jitter
    // while sculpting; gestures above already work on the raw values.
    if (previous.active) {
      const auto smooth = [](large::sdf::Vec3 prev, large::sdf::Vec3 next) {
        return prev * kHandPoseSmoothing + next * (1.0f - kHandPoseSmoothing);
      };
      state.pose.position = smooth(previous.pose.position, state.pose.position);
      state.pinchPosition = smooth(previous.pinchPosition, state.pinchPosition);
      state.openToolPosition = smooth(previous.openToolPosition, state.openToolPosition);
      state.fistToolPosition = smooth(previous.fistToolPosition, state.fistToolPosition);
      for (std::size_t i = 0; i < state.joints.size(); ++i) {
        state.joints[i] = smooth(previous.joints[i], state.joints[i]);
      }
    }
    return true;
  }

  void updateHandTracking(XrTime predictedDisplayTime) {
    if (handTrackingSupported_ && !handTrackingCreateAttempted_ && frameCounter_ > 30) {
      handTrackingCreateAttempted_ = true;
      initializeHandTracking();
    }

    if (!handTrackingReady_) {
      leftHandState_ = {};
      rightHandState_ = {};
      return;
    }
    locateHandState(leftHandTracker_, leftHandState_, predictedDisplayTime, "left");
    locateHandState(rightHandTracker_, rightHandState_, predictedDisplayTime, "right");
  }

  large::sdf::Vec3 objectToWorldPoint(large::sdf::Vec3 localPoint) const {
    return objectPosition_ + rotateByQuaternion(objectRotation_, localPoint * objectScale_);
  }

  large::sdf::Vec3 worldToObjectPoint(large::sdf::Vec3 worldPoint) const {
    const XrQuaternionf inverseRotation = conjugateQuaternion(objectRotation_);
    return rotateByQuaternion(inverseRotation, worldPoint - objectPosition_) / std::max(objectScale_, 0.001f);
  }

  large::sdf::Vec3 worldToObjectDirection(large::sdf::Vec3 worldDirection) const {
    const XrQuaternionf inverseRotation = conjugateQuaternion(objectRotation_);
    return large::sdf::normalize(rotateByQuaternion(inverseRotation, worldDirection));
  }

  large::sdf::Vec3 estimateSdfNormalLocal(large::sdf::Vec3 localPoint) const {
    const float e = std::max(volume_.voxelSize() * 1.5f, 0.004f);
    const large::sdf::Vec3 dx{e, 0.0f, 0.0f};
    const large::sdf::Vec3 dy{0.0f, e, 0.0f};
    const large::sdf::Vec3 dz{0.0f, 0.0f, e};
    const large::sdf::Vec3 normal{
        volume_.sample(localPoint + dx) - volume_.sample(localPoint - dx),
        volume_.sample(localPoint + dy) - volume_.sample(localPoint - dy),
        volume_.sample(localPoint + dz) - volume_.sample(localPoint - dz),
    };
    if (large::sdf::dot(normal, normal) < 0.000001f) {
      return {0.0f, 1.0f, 0.0f};
    }
    return large::sdf::normalize(normal);
  }

  bool findToolSurfaceContact(large::sdf::Vec3 toolCenterLocal,
                              float localBrushRadius,
                              large::sdf::Vec3& contactLocal) const {
    const float signedDistance = volume_.sample(toolCenterLocal);
    const float padding = volume_.voxelSize() * 1.5f;
    if (signedDistance > localBrushRadius + padding) {
      return false;
    }

    const large::sdf::Vec3 normal = estimateSdfNormalLocal(toolCenterLocal);
    const float projectionDistance =
        large::sdf::clamp(signedDistance, -localBrushRadius, localBrushRadius);
    contactLocal = toolCenterLocal - normal * projectionDistance;
    return true;
  }

  large::sdf::Vec3 currentHeadPosition() const {
    if (views_.size() >= kEyeCount) {
      return (makeVec3(views_[0].pose.position) + makeVec3(views_[1].pose.position)) * 0.5f;
    }
    return {0.0f, 1.5f, 0.0f};
  }

  large::sdf::Vec3 currentHeadRight() const {
    if (views_.size() >= kEyeCount) {
      return large::sdf::normalize(rotateByQuaternion(views_[0].pose.orientation, {1.0f, 0.0f, 0.0f}));
    }
    return {1.0f, 0.0f, 0.0f};
  }

  ControllerPose leftUiPose() const {
    if (leftGripPose_.active) {
      return leftGripPose_;
    }
    if (leftHandState_.active) {
      return leftHandState_.pose;
    }
    return {};
  }

  bool isLeftUiVisible() const {
    return leftGripPose_.active || leftHandState_.active;
  }

  large::sdf::Vec3 leftUiPosition() const {
    const ControllerPose uiPose = leftUiPose();
    if (!uiPose.active) {
      return {};
    }
    const large::sdf::Vec3 leftForward =
        large::sdf::normalize(rotateByQuaternion(uiPose.orientation, {0.0f, 0.0f, -1.0f}));
    return uiPose.position + leftForward * 0.24f;
  }

  struct UiPointerHit {
    large::sdf::Vec3 point{};
    float signedDistance = 0.0f;
    int menuChoice = -1;
  };

  struct UiPanelFrame {
    large::sdf::Vec3 center{};
    large::sdf::Vec3 forward{0.0f, 0.0f, 1.0f};
    large::sdf::Vec3 right{1.0f, 0.0f, 0.0f};
    large::sdf::Vec3 up{0.0f, 1.0f, 0.0f};
    float height = 0.0f;
    float contentHeight = 0.0f;  // visible pixels (header only, or full menu)
  };

  bool leftUiPanelFrame(UiPanelFrame& frame) const {
    if (!isLeftUiVisible()) {
      return false;
    }

    frame.center = leftUiPosition();
    const large::sdf::Vec3 head = currentHeadPosition();
    frame.forward = large::sdf::normalize(head - frame.center);
    frame.right = large::sdf::cross({0.0f, 1.0f, 0.0f}, frame.forward);
    if (large::sdf::dot(frame.right, frame.right) < 0.05f) {
      frame.right = {1.0f, 0.0f, 0.0f};
    } else {
      frame.right = large::sdf::normalize(frame.right);
    }
    frame.up = large::sdf::normalize(large::sdf::cross(frame.forward, frame.right));
    frame.contentHeight =
        static_cast<float>(menuVisible_ ? large::hud::kContentHeight : large::hud::kHeaderVisibleHeight);
    frame.height = large::hud::kPanelWidthMeters * frame.contentHeight /
                   static_cast<float>(large::hud::kContentWidth);
    return true;
  }

  bool resolveLeftUiHit(const UiPanelFrame& frame, large::sdf::Vec3 point, UiPointerHit& hit) const {
    namespace hud = large::hud;
    const large::sdf::Vec3 offset = point - frame.center;
    const float localX = large::sdf::dot(offset, frame.right);
    const float localY = large::sdf::dot(offset, frame.up);
    if (std::abs(localX) > hud::kPanelWidthMeters * 0.5f || std::abs(localY) > frame.height * 0.5f) {
      return false;
    }

    // Same mapping as the UI overlay shader, so the pointer hits exactly what
    // is drawn on the HUD texture.
    const float fragX = (localX / hud::kPanelWidthMeters + 0.5f) * static_cast<float>(hud::kContentWidth);
    const float fragY = (0.5f - localY / frame.height) * frame.contentHeight;

    hit.point = point;
    hit.menuChoice = -1;

    const bool onMenuButton = fragX >= static_cast<float>(hud::kMenuButtonLeft) &&
                              fragX <= static_cast<float>(hud::kMenuButtonRight) &&
                              fragY >= static_cast<float>(hud::kMenuButtonTop) &&
                              fragY <= static_cast<float>(hud::kMenuButtonBottom);
    if (onMenuButton) {
      hit.menuChoice = kMenuToggleChoice;
      return true;
    }

    // With the menu closed, only the MENU button is interactive: the rest of
    // the header must not block sculpting near the left hand.
    if (!menuVisible_) {
      return false;
    }

    const bool inToolColumn = fragX >= static_cast<float>(hud::kMenuToolColumnLeft) &&
                              fragX <= static_cast<float>(hud::kMenuToolColumnRight);
    const bool inActionColumn = fragX >= static_cast<float>(hud::kMenuActionColumnLeft) &&
                                fragX <= static_cast<float>(hud::kMenuActionColumnRight);
    if (inToolColumn || inActionColumn) {
      const int rowCount = inToolColumn ? hud::kMenuToolRowCount : hud::kMenuActionRowCount;
      for (int i = 0; i < rowCount; ++i) {
        const float rowTop = static_cast<float>(hud::kMenuRowTop + i * hud::kMenuRowPitch);
        const float rowBottom = rowTop + static_cast<float>(hud::kMenuRowHeight);
        if (fragY >= rowTop && fragY <= rowBottom) {
          hit.menuChoice = inToolColumn ? i : kMenuToolCount + i;
          return true;
        }
      }
    }

    const float paletteX = fragX - static_cast<float>(hud::kPaletteLeft);
    const float paletteY = fragY - static_cast<float>(hud::kPaletteTop);
    if (paletteX >= 0.0f && paletteY >= 0.0f) {
      const int column = static_cast<int>(std::floor(paletteX / static_cast<float>(hud::kPalettePitch)));
      const int row = static_cast<int>(std::floor(paletteY / static_cast<float>(hud::kPalettePitch)));
      const bool inColumn = column >= 0 && column < hud::kPaletteColumns &&
                            paletteX - static_cast<float>(column * hud::kPalettePitch) <=
                                static_cast<float>(hud::kPaletteSwatch);
      const bool inRow = row >= 0 && row < hud::kPaletteRows &&
                         paletteY - static_cast<float>(row * hud::kPalettePitch) <=
                             static_cast<float>(hud::kPaletteSwatch);
      if (inColumn && inRow) {
        hit.menuChoice = kMenuPaletteFirstChoice + row * hud::kPaletteColumns + column;
      }
    }
    return true;
  }

  bool raycastLeftUi(large::sdf::Vec3 rayOrigin, large::sdf::Vec3 rayDirection, UiPointerHit& hit) const {
    UiPanelFrame frame{};
    if (!leftUiPanelFrame(frame)) {
      return false;
    }

    const float denom = large::sdf::dot(rayDirection, frame.forward);
    if (std::abs(denom) < 0.001f) {
      return false;
    }

    const float t = large::sdf::dot(frame.center - rayOrigin, frame.forward) / denom;
    if (t <= 0.0f || t > 2.0f) {
      return false;
    }

    const large::sdf::Vec3 point = rayOrigin + rayDirection * t;
    hit.signedDistance = 0.0f;
    return resolveLeftUiHit(frame, point, hit);
  }

  // Direct touch for hand tracking: the right index fingertip hovers and
  // presses the wrist panel (menu rows, palette, MENU button). Returns true
  // while the fingertip is in the panel interaction zone, which also
  // suppresses sculpting so a pinch near the panel cannot carve the object.
  bool updateHandMenuPoke() {
    if (!rightHandState_.active || !isLeftUiVisible()) {
      handPokeWasTouching_ = false;
      return false;
    }

    UiPanelFrame frame{};
    if (!leftUiPanelFrame(frame)) {
      handPokeWasTouching_ = false;
      return false;
    }

    const large::sdf::Vec3 tip = rightHandState_.indexTip;
    const float planeDistance = large::sdf::dot(tip - frame.center, frame.forward);
    if (planeDistance > 0.10f || planeDistance < -0.05f) {
      handPokeWasTouching_ = false;
      return false;
    }

    const large::sdf::Vec3 onPlane = tip - frame.forward * planeDistance;
    UiPointerHit hit{};
    if (!resolveLeftUiHit(frame, onPlane, hit)) {
      handPokeWasTouching_ = false;
      return false;
    }

    menuPointerActive_ = true;
    menuHoverIndex_ = hit.menuChoice;
    menuPointerStart_ = tip;
    menuPointerEnd_ = onPlane;

    // Press on plane contact, with hysteresis so a trembling fingertip does
    // not double-trigger.
    const bool touching = planeDistance < (handPokeWasTouching_ ? 0.030f : 0.012f);
    if (touching && !handPokeWasTouching_ && hit.menuChoice >= 0) {
      activateMenuChoice(hit.menuChoice);
    }
    handPokeWasTouching_ = touching;
    return true;
  }

  bool updateMenuPointer(large::sdf::Vec3 rayOrigin, large::sdf::Vec3 rayDirection, bool selectDown) {
    menuPointerActive_ = false;
    menuHoverIndex_ = -1;

    UiPointerHit hit{};
    if (!raycastLeftUi(rayOrigin, rayDirection, hit)) {
      menuSelectWasDown_ = selectDown;
      return false;
    }

    menuPointerActive_ = true;
    menuHoverIndex_ = hit.menuChoice;
    menuPointerStart_ = rayOrigin + rayDirection * 0.035f;
    menuPointerEnd_ = hit.point;
    if (selectDown && !menuSelectWasDown_ && menuHoverIndex_ >= 0) {
      activateMenuChoice(menuHoverIndex_);
    }
    menuSelectWasDown_ = selectDown;
    return true;
  }

  void updateObjectManipulation(XrTime predictedDisplayTime) {
    leftGripValue_ = readFloatAction(gripValueAction_, leftHandPath_, "left grip");
    rightGripValue_ = readFloatAction(gripValueAction_, rightHandPath_, "right grip");

    const XrSpaceLocationFlags gripFlags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    leftGripPose_ = locateControllerPose(
        gripPoseAction_, leftHandPath_, leftGripSpace_, predictedDisplayTime, gripFlags, "left grip");
    rightGripPose_ = locateControllerPose(
        gripPoseAction_, rightHandPath_, rightGripSpace_, predictedDisplayTime, gripFlags, "right grip");
    updateHandTracking(predictedDisplayTime);

    // LOCK mode: the object's pose and scale are frozen; grips and hand
    // grabs no longer move it (the left grip still drives the wrist UI).
    if (objectLocked_) {
      leftGripActive_ = false;
      rightGripActive_ = false;
      oneHandGrabActive_ = false;
      oneHandGrabHand_ = 0;
      twoHandGrabActive_ = false;
      return;
    }

    const bool leftHandGrab = leftHandState_.active && leftHandState_.fist;
    const bool rightHandGrab = false;
    const ControllerPose leftObjectPose = leftHandGrab ? leftHandState_.pose : leftGripPose_;
    const ControllerPose rightObjectPose = rightHandGrab ? rightHandState_.pose : rightGripPose_;
    const bool leftGrab = (leftGripPose_.active && leftGripValue_ >= kGripThreshold) || leftHandGrab;
    const bool rightGrab = (rightGripPose_.active && rightGripValue_ >= kGripThreshold) || rightHandGrab;
    leftGripActive_ = leftGrab;
    rightGripActive_ = rightGrab;

    if (leftGrab && rightGrab) {
      const large::sdf::Vec3 left = leftObjectPose.position;
      const large::sdf::Vec3 right = rightObjectPose.position;
      const large::sdf::Vec3 midpoint = (left + right) * 0.5f;
      const float distance = std::max(large::sdf::length(right - left), 0.001f);

      if (!twoHandGrabActive_) {
        twoHandGrabActive_ = true;
        oneHandGrabActive_ = false;
        twoHandStartDistance_ = distance;
        twoHandStartScale_ = objectScale_;
        twoHandStartMidpoint_ = midpoint;
        twoHandStartVector_ = right - left;
        twoHandStartObjectPosition_ = objectPosition_;
        twoHandStartObjectRotation_ = objectRotation_;
        logInfo("Two-hand grab started: distance=%.3f scale=%.2f", distance, objectScale_);
      }

      const XrQuaternionf deltaRotation = quaternionFromTo(twoHandStartVector_, right - left);
      objectRotation_ = multiplyQuaternions(deltaRotation, twoHandStartObjectRotation_);
      objectScale_ =
          large::sdf::clamp(twoHandStartScale_ * distance / twoHandStartDistance_,
                            kMinimumObjectScale,
                            kMaximumObjectScale);
      const float scaleRatio = objectScale_ / std::max(twoHandStartScale_, 0.001f);
      objectPosition_ =
          midpoint + rotateByQuaternion(deltaRotation, (twoHandStartObjectPosition_ - twoHandStartMidpoint_) * scaleRatio);
      return;
    }

    twoHandGrabActive_ = false;

    if (leftGrab || rightGrab) {
      const int activeHand = leftGrab ? -1 : 1;
      const large::sdf::Vec3 handPosition = leftGrab ? leftObjectPose.position : rightObjectPose.position;
      if (!oneHandGrabActive_ || oneHandGrabHand_ != activeHand) {
        oneHandGrabActive_ = true;
        oneHandGrabHand_ = activeHand;
        grabStartHandPosition_ = handPosition;
        grabStartObjectPosition_ = objectPosition_;
        grabStartHandOrientation_ = leftGrab ? leftObjectPose.orientation : rightObjectPose.orientation;
        grabStartObjectRotation_ = objectRotation_;
        logInfo("%s %s grab started",
                activeHand < 0 ? "Left" : "Right",
                (leftGrab ? leftHandGrab : rightHandGrab) ? "hand" : "controller");
      }

      const XrQuaternionf handOrientation = leftGrab ? leftObjectPose.orientation : rightObjectPose.orientation;
      const XrQuaternionf deltaRotation =
          multiplyQuaternions(handOrientation, conjugateQuaternion(grabStartHandOrientation_));
      objectRotation_ = multiplyQuaternions(deltaRotation, grabStartObjectRotation_);
      objectPosition_ = handPosition + rotateByQuaternion(deltaRotation, grabStartObjectPosition_ - grabStartHandPosition_);
      return;
    }

    oneHandGrabActive_ = false;
    oneHandGrabHand_ = 0;
  }

  // Right-stick locomotion: the user moves through the scene. In tracking
  // space this shifts the object (and the procedural room, via uWorldOffset)
  // in the opposite direction, which is also the only visible effect in AR.
  void updateLocomotion(XrTime predictedDisplayTime) {
    const XrVector2f stick = readVector2Action(locomotionAction_, rightHandPath_, "locomotion");

    float dt = 1.0f / 72.0f;
    if (lastLocomotionTime_ != 0) {
      dt = large::sdf::clamp(static_cast<float>(predictedDisplayTime - lastLocomotionTime_) * 1e-9f, 0.0f, 0.05f);
    }
    lastLocomotionTime_ = predictedDisplayTime;

    constexpr float deadzone = 0.15f;
    if (std::abs(stick.x) < deadzone && std::abs(stick.y) < deadzone) {
      return;
    }

    large::sdf::Vec3 forward = rotateByQuaternion(views_[0].pose.orientation, {0.0f, 0.0f, -1.0f});
    forward.y = 0.0f;
    if (large::sdf::dot(forward, forward) < 0.001f) {
      forward = {0.0f, 0.0f, -1.0f};
    }
    forward = large::sdf::normalize(forward);
    large::sdf::Vec3 right = rotateByQuaternion(views_[0].pose.orientation, {1.0f, 0.0f, 0.0f});
    right.y = 0.0f;
    if (large::sdf::dot(right, right) < 0.001f) {
      right = {1.0f, 0.0f, 0.0f};
    }
    right = large::sdf::normalize(right);

    constexpr float speed = 1.6f;  // meters per second at full stick
    const large::sdf::Vec3 move = (right * stick.x + forward * stick.y) * (speed * dt);
    worldOffset_ = worldOffset_ + move;
    // While the object is held it stays in the hand; only the room scrolls.
    if (!oneHandGrabActive_ && !twoHandGrabActive_) {
      objectPosition_ = objectPosition_ - move;
    }
  }

  void updateLeftHandPinchZoom() {
    if (objectLocked_) {
      leftHandPinchZoomActive_ = false;
      return;
    }
    if (!leftHandState_.active || !leftHandState_.pinch || leftHandState_.fist) {
      leftHandPinchZoomActive_ = false;
      return;
    }

    const float handX = large::sdf::dot(leftHandState_.pinchPosition, currentHeadRight());
    if (!leftHandPinchZoomActive_) {
      leftHandPinchZoomActive_ = true;
      leftHandPinchZoomStartX_ = handX;
      leftHandPinchZoomStartScale_ = objectScale_;
      logInfo("Left pinch zoom started: scale=%.2f", objectScale_);
    }

    const float deltaX = handX - leftHandPinchZoomStartX_;
    objectScale_ = large::sdf::clamp(leftHandPinchZoomStartScale_ * std::exp(-deltaX * kHandPinchZoomSpeed),
                                     kMinimumObjectScale,
                                     kMaximumObjectScale);
  }

  // Starts a sculpt stroke: resets the capsule chaining and, for Flatten,
  // locks the work plane on the surface point under the brush so the whole
  // stroke flattens toward the same plane (Medium-style flatten).
  void beginSculptStroke(large::sdf::Vec3 centerLocal, bool hasContact, large::sdf::Vec3 contactLocal) {
    strokeHasLast_ = false;
    if (activeTool_ == VrTool::Flatten) {
      const large::sdf::Vec3 planePoint = hasContact ? contactLocal : centerLocal;
      flattenPlanePointLocal_ = planePoint;
      flattenPlaneNormalLocal_ = estimateSdfNormalLocal(planePoint);
    }
  }

  // The symmetry plane is local X=0 (the volume is centered on the origin).
  static large::sdf::Vec3 mirrorLocal(large::sdf::Vec3 p) { return {-p.x, p.y, p.z}; }

  void applySculptStampAt(large::sdf::Vec3 from,
                          large::sdf::Vec3 centerLocal,
                          large::sdf::Vec3 flattenPoint,
                          large::sdf::Vec3 flattenNormal,
                          float localBrushRadius,
                          float strength) {
    switch (activeTool_) {
      case VrTool::Add:
        volume_.applyCapsuleBrush(from, centerLocal, localBrushRadius, large::sdf::BrushMode::Add, strength);
        break;
      case VrTool::Subtract:
        volume_.applyCapsuleBrush(from, centerLocal, localBrushRadius, large::sdf::BrushMode::Subtract, strength);
        break;
      case VrTool::Smooth:
        volume_.applySmoothBrush(centerLocal, localBrushRadius * 1.35f, strength * 0.65f);
        break;
      case VrTool::Flatten:
        volume_.applyFlattenBrush(centerLocal, flattenPoint, flattenNormal, localBrushRadius * 1.25f, strength);
        break;
      case VrTool::Groove:
        volume_.applyCapsuleBrush(from,
                                  centerLocal,
                                  std::max(localBrushRadius * 0.35f, volume_.voxelSize()),
                                  large::sdf::BrushMode::Subtract,
                                  strength);
        break;
      case VrTool::Crease:
        volume_.applyPinchBrush(centerLocal, localBrushRadius, strength);
        break;
      case VrTool::Paint:
        applyPaintStroke(from, centerLocal, localBrushRadius);
        break;
      case VrTool::Stretch:
        break;
    }
  }

  // Applies one stamp of the active tool. Successive stamps are connected by
  // capsules so fast strokes stay continuous instead of leaving sphere gaps.
  // With mirror mode on, every stamp is duplicated across the X=0 plane
  // (Stretch is the only tool left out of the symmetry).
  void applySculptStamp(large::sdf::Vec3 centerLocal, float localBrushRadius, float strength) {
    const large::sdf::Vec3 from = strokeHasLast_ ? strokeLastLocal_ : centerLocal;
    applySculptStampAt(from, centerLocal, flattenPlanePointLocal_, flattenPlaneNormalLocal_, localBrushRadius,
                       strength);
    if (mirrorEnabled_) {
      applySculptStampAt(mirrorLocal(from),
                         mirrorLocal(centerLocal),
                         mirrorLocal(flattenPlanePointLocal_),
                         {-flattenPlaneNormalLocal_.x, flattenPlaneNormalLocal_.y, flattenPlaneNormalLocal_.z},
                         localBrushRadius,
                         strength);
    }
    strokeLastLocal_ = centerLocal;
    strokeHasLast_ = true;
  }

  void initializeColorVoxels() {
    const std::size_t count = volume_.values().size();
    const auto& clay = kPaintPalette[0];
    const std::uint8_t r = static_cast<std::uint8_t>(clay[0] * 255.0f + 0.5f);
    const std::uint8_t g = static_cast<std::uint8_t>(clay[1] * 255.0f + 0.5f);
    const std::uint8_t b = static_cast<std::uint8_t>(clay[2] * 255.0f + 0.5f);
    colorVoxels_.resize(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
      colorVoxels_[i * 4 + 0] = r;
      colorVoxels_[i * 4 + 1] = g;
      colorVoxels_[i * 4 + 2] = b;
      colorVoxels_[i * 4 + 3] = 255;
    }
    markAllColorDirty();
    // From here on, undo/redo snapshots also carry the paint colors.
    history_.attachColors(&colorVoxels_);
  }

  void markAllColorDirty() {
    const large::sdf::IVec3 size = volume_.size();
    colorDirtyBounds_.valid = true;
    colorDirtyBounds_.min = {0, 0, 0};
    colorDirtyBounds_.max = {size.x - 1, size.y - 1, size.z - 1};
  }

  // Blends the paint color into the color volume along a capsule, restricted
  // to a narrow band around the current surface.
  void applyPaintStroke(large::sdf::Vec3 from, large::sdf::Vec3 to, float localRadius) {
    if (localRadius <= 0.0f || colorVoxels_.empty()) {
      return;
    }

    const large::sdf::IVec3 size = volume_.size();
    const float voxelSize = volume_.voxelSize();
    const large::sdf::Vec3 origin = volume_.origin();
    const float band = voxelSize * 4.0f;
    const auto& paint = kPaintPalette[static_cast<std::size_t>(paintColorIndex_)];

    const large::sdf::Vec3 minPoint{
        std::min(from.x, to.x) - localRadius,
        std::min(from.y, to.y) - localRadius,
        std::min(from.z, to.z) - localRadius,
    };
    const large::sdf::Vec3 maxPoint{
        std::max(from.x, to.x) + localRadius,
        std::max(from.y, to.y) + localRadius,
        std::max(from.z, to.z) + localRadius,
    };
    const int minX = large::sdf::clampInt(static_cast<int>(std::floor((minPoint.x - origin.x) / voxelSize)), 0, size.x - 1);
    const int minY = large::sdf::clampInt(static_cast<int>(std::floor((minPoint.y - origin.y) / voxelSize)), 0, size.y - 1);
    const int minZ = large::sdf::clampInt(static_cast<int>(std::floor((minPoint.z - origin.z) / voxelSize)), 0, size.z - 1);
    const int maxX = large::sdf::clampInt(static_cast<int>(std::ceil((maxPoint.x - origin.x) / voxelSize)), 0, size.x - 1);
    const int maxY = large::sdf::clampInt(static_cast<int>(std::ceil((maxPoint.y - origin.y) / voxelSize)), 0, size.y - 1);
    const int maxZ = large::sdf::clampInt(static_cast<int>(std::ceil((maxPoint.z - origin.z) / voxelSize)), 0, size.z - 1);

    const large::sdf::Vec3 segment = to - from;
    const float segmentLengthSq = large::sdf::dot(segment, segment);
    const std::vector<float>& values = volume_.values();
    bool painted = false;

    for (int z = minZ; z <= maxZ; ++z) {
      for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
          const std::size_t i = volume_.index(x, y, z);
          // Paint the interior too (values <= band keeps all solid voxels):
          // a surface-only shell gets diluted by the trilinear color filter
          // and only shows up along edges.
          if (values[i] > band) {
            continue;
          }

          const large::sdf::Vec3 p = volume_.voxelCenter(x, y, z);
          const large::sdf::Vec3 toPoint = p - from;
          const float h = segmentLengthSq > 0.000001f
                              ? large::sdf::clamp(large::sdf::dot(toPoint, segment) / segmentLengthSq, 0.0f, 1.0f)
                              : 0.0f;
          const float dist = large::sdf::length(toPoint - segment * h);
          if (dist > localRadius) {
            continue;
          }

          const float falloff = 1.0f - (dist / localRadius) * (dist / localRadius);
          const float blend = large::sdf::clamp(brushStrength_ * falloff, 0.0f, 1.0f);
          std::uint8_t* voxel = colorVoxels_.data() + i * 4;
          for (int channel = 0; channel < 3; ++channel) {
            const float current = static_cast<float>(voxel[channel]) / 255.0f;
            const float next = current + (paint[static_cast<std::size_t>(channel)] - current) * blend;
            voxel[channel] = static_cast<std::uint8_t>(large::sdf::clamp(next, 0.0f, 1.0f) * 255.0f + 0.5f);
          }
          painted = true;
        }
      }
    }

    if (painted) {
      large::sdf::VoxelBounds stampBounds{};
      stampBounds.valid = true;
      stampBounds.min = {minX, minY, minZ};
      stampBounds.max = {maxX, maxY, maxZ};
      colorDirtyBounds_ = mergeVoxelBounds(colorDirtyBounds_, stampBounds);
    }
  }

  void updateStretchTool(large::sdf::Vec3 hitLocal,
                         large::sdf::Vec3 toolCenterLocal,
                         XrQuaternionf toolOrientationLocal,
                         float localBrushRadius,
                         bool triggerDown) {
    if (triggerDown && !stretchAnchorSet_) {
      history_.capture();
      stretchAnchorLocal_ = hitLocal;
      stretchAnchorSet_ = true;
      stretchSourceVolume_ = volume_;
      stretchPullStartControllerLocal_ = toolCenterLocal;
      stretchPullStartToolOrientationLocal_ = toolOrientationLocal;
      stretchUploadBounds_ = {};
      stretchPullActive_ = true;
      brushHitWorld_ = objectToWorldPoint(stretchAnchorLocal_);
      logInfo("Stretch anchor set: (%.2f %.2f %.2f)",
              stretchAnchorLocal_.x,
              stretchAnchorLocal_.y,
              stretchAnchorLocal_.z);
    }

    if (!triggerDown) {
      clearStretchInteraction();
      return;
    }

    if (!stretchAnchorSet_) {
      return;
    }

    const large::sdf::Vec3 delta = toolCenterLocal - stretchPullStartControllerLocal_;
    const XrQuaternionf rotationDelta =
        multiplyQuaternions(toolOrientationLocal, conjugateQuaternion(stretchPullStartToolOrientationLocal_));
    const large::sdf::Vec3 rotationX = rotateByQuaternion(rotationDelta, {1.0f, 0.0f, 0.0f});
    const large::sdf::Vec3 rotationY = rotateByQuaternion(rotationDelta, {0.0f, 1.0f, 0.0f});
    const large::sdf::Vec3 rotationZ = rotateByQuaternion(rotationDelta, {0.0f, 0.0f, 1.0f});
    brushHitLocal_ = stretchAnchorLocal_ + delta;
    brushHitWorld_ = objectToWorldPoint(brushHitLocal_);
    const float influenceRadius = localBrushRadius * (1.25f + brushStrength_ * 2.25f);
    const float rotationAngle =
        2.0f * std::acos(large::sdf::clamp(std::abs(rotationDelta.w), 0.0f, 1.0f));
    const bool moved = large::sdf::length(delta) > volume_.voxelSize() * 0.35f;
    const bool rotated = rotationAngle * influenceRadius > volume_.voxelSize() * 0.35f;
    if ((moved || rotated) && frameCounter_ >= nextSculptFrame_) {
      const large::sdf::VoxelBounds previousStretchBounds = stretchUploadBounds_;
      volume_.applyStretchBrush(stretchSourceVolume_,
                                stretchAnchorLocal_,
                                delta,
                                rotationX,
                                rotationY,
                                rotationZ,
                                influenceRadius,
                                brushStrength_);
      if (mirrorEnabled_) {
        // Mirrored rotation basis: R' = M R M with M = diag(-1, 1, 1).
        volume_.applyStretchBrush(stretchSourceVolume_,
                                  mirrorLocal(stretchAnchorLocal_),
                                  mirrorLocal(delta),
                                  {rotationX.x, -rotationX.y, -rotationX.z},
                                  {-rotationY.x, rotationY.y, rotationY.z},
                                  {-rotationZ.x, rotationZ.y, rotationZ.z},
                                  influenceRadius,
                                  brushStrength_,
                                  false);
      }
      const large::sdf::VoxelBounds currentStretchBounds = volume_.dirtyBounds();
      uploadSdfTexture(previousStretchBounds);
      stretchUploadBounds_ = currentStretchBounds;
      nextSculptFrame_ = frameCounter_ + 2;
      if (frameCounter_ >= nextSculptLogFrame_) {
        logInfo("VR Stretch brush: delta=(%.2f %.2f %.2f), rot=%.1fdeg, radius=%.2f strength=%.2f",
                delta.x,
                delta.y,
                delta.z,
                rotationAngle * 57.29578f,
                influenceRadius,
                brushStrength_);
        nextSculptLogFrame_ = frameCounter_ + 60;
      }
    }
  }

  float handGestureLocalRadius(float fallbackLocalRadius) {
    return fallbackLocalRadius;
  }

  bool updateHandClapToolSelector() {
    if (!leftHandState_.active || !rightHandState_.active || leftHandState_.pinch || rightHandState_.pinch ||
        leftHandState_.fist || rightHandState_.fist) {
      handClapWasClosed_ = false;
      return false;
    }

    const float palmDistance = large::sdf::length(leftHandState_.pose.position - rightHandState_.pose.position);
    if (palmDistance > kHandClapReleaseDistance) {
      handClapWasClosed_ = false;
      return false;
    }

    if (palmDistance <= kHandClapDistance && !handClapWasClosed_) {
      setActiveTool(nextHandTool(activeTool_), "Hand clap");
      rightHandPinchStretchActive_ = false;
      rightHandPinchWasActive_ = false;
      rightHandPinchToolActive_ = false;
      rightHandPinchTool_ = -1;
      handClapWasClosed_ = true;
      handDisplayToolIndex_ = activeToolIndex();
      return true;
    }

    return false;
  }

  bool updateRightHandPinchStretch(float localBrushRadius) {
    if (!rightHandState_.active || !rightHandState_.pinch) {
      rightHandShapeBrushActive_ = false;
      rightHandShapeBrushTool_ = -1;
      rightHandPinchToolActive_ = false;
      rightHandPinchTool_ = -1;
      if (rightHandPinchStretchActive_) {
        clearStretchInteraction();
      } else {
        rightHandPinchWasActive_ = false;
      }
      return false;
    }

    rightHandShapeBrushActive_ = false;
    rightHandShapeBrushTool_ = -1;
    const float influenceRadius = handGestureLocalRadius(localBrushRadius);
    const int activeIndex = activeToolIndex();
    handDisplayToolIndex_ = activeIndex;
    const large::sdf::Vec3 pinchLocal = worldToObjectPoint(rightHandState_.pinchPosition);
    const float surfaceDistance = std::abs(volume_.sample(pinchLocal));
    const float snapDistance = std::max(kHandSculptSurfaceSnapDistance / std::max(objectScale_, 0.001f),
                                        volume_.voxelSize() * 2.0f);

    if (activeTool_ != VrTool::Stretch) {
      rightHandPinchStretchActive_ = false;
      rightHandPinchWasActive_ = false;
      stretchPullActive_ = false;
      const bool nearSurface = surfaceDistance <= std::max(influenceRadius * 1.15f, snapDistance);
      // Add can create material in empty space (needed for the empty start
      // scene); the other tools need an existing surface to act on.
      brushVisible_ = nearSurface || activeTool_ == VrTool::Add;
      brushHitLocal_ = pinchLocal;
      brushHitWorld_ = objectToWorldPoint(pinchLocal);
      if (!nearSurface && activeTool_ != VrTool::Add) {
        rightHandPinchToolActive_ = false;
        rightHandPinchTool_ = -1;
        strokeHasLast_ = false;
        return true;
      }

      if (frameCounter_ >= nextSculptFrame_) {
        if (!rightHandPinchToolActive_ || rightHandPinchTool_ != activeIndex) {
          history_.capture();
          large::sdf::Vec3 contactLocal = pinchLocal;
          const bool hasContact = findToolSurfaceContact(pinchLocal, influenceRadius, contactLocal);
          beginSculptStroke(pinchLocal, hasContact, contactLocal);
        }

        applySculptStamp(pinchLocal, influenceRadius, 1.0f);

        rightHandPinchToolActive_ = true;
        rightHandPinchTool_ = activeIndex;
        if (activeTool_ == VrTool::Flatten) {
          flattenPreviewCenterWorld_ = objectToWorldPoint(flattenPlanePointLocal_);
          flattenPreviewNormalWorld_ =
              large::sdf::normalize(rotateByQuaternion(objectRotation_, flattenPlaneNormalLocal_));
          flattenPreviewVisible_ = true;
        }
        uploadSdfTexture();
        uploadColorTexture();
        nextSculptFrame_ = frameCounter_ + (activeTool_ == VrTool::Smooth ? 3 : 2);
        if (frameCounter_ >= nextSculptLogFrame_) {
          logInfo("Hand %s pinch: center=(%.2f %.2f %.2f), radius=%.2f",
                  toolName(activeTool_),
                  pinchLocal.x,
                  pinchLocal.y,
                  pinchLocal.z,
                  influenceRadius);
          nextSculptLogFrame_ = frameCounter_ + 60;
        }
      }
      return true;
    }

    rightHandPinchToolActive_ = false;
    rightHandPinchTool_ = -1;
    if (!rightHandPinchStretchActive_) {
      const float stretchSnapDistance = std::max(kHandStretchSurfaceSnapDistance / std::max(objectScale_, 0.001f),
                                                 volume_.voxelSize() * 2.0f);
      if (surfaceDistance > stretchSnapDistance) {
        return false;
      }
      history_.capture();
      stretchAnchorLocal_ = pinchLocal;
      stretchAnchorSet_ = true;
      stretchSourceVolume_ = volume_;
      stretchPullStartControllerLocal_ = pinchLocal;
      stretchUploadBounds_ = {};
      rightHandPinchStretchActive_ = true;
      rightHandPinchWasActive_ = true;
      logInfo("Hand Stretch anchor set: (%.2f %.2f %.2f)",
              stretchAnchorLocal_.x,
              stretchAnchorLocal_.y,
              stretchAnchorLocal_.z);
    }

    brushVisible_ = true;
    stretchPullActive_ = true;

    const large::sdf::Vec3 delta = pinchLocal - stretchPullStartControllerLocal_;
    brushHitLocal_ = stretchAnchorLocal_ + delta;
    brushHitWorld_ = objectToWorldPoint(brushHitLocal_);
    if (large::sdf::length(delta) > volume_.voxelSize() * 0.35f && frameCounter_ >= nextSculptFrame_) {
      const large::sdf::VoxelBounds previousStretchBounds = stretchUploadBounds_;
      volume_.applyStretchBrush(stretchSourceVolume_, stretchAnchorLocal_, delta, influenceRadius, 1.0f);
      if (mirrorEnabled_) {
        volume_.applyStretchBrush(stretchSourceVolume_,
                                  mirrorLocal(stretchAnchorLocal_),
                                  mirrorLocal(delta),
                                  influenceRadius,
                                  1.0f,
                                  false);
      }
      const large::sdf::VoxelBounds currentStretchBounds = volume_.dirtyBounds();
      uploadSdfTexture(previousStretchBounds);
      stretchUploadBounds_ = currentStretchBounds;
      nextSculptFrame_ = frameCounter_ + 2;
      if (frameCounter_ >= nextSculptLogFrame_) {
        logInfo("Hand Stretch direct: delta=(%.2f %.2f %.2f), radius=%.2f",
                delta.x,
                delta.y,
                delta.z,
                influenceRadius);
        nextSculptLogFrame_ = frameCounter_ + 60;
      }
    }
    return true;
  }

  bool updateRightHandShapeBrush(float localBrushRadius) {
    if (!rightHandState_.active || rightHandState_.pinch || leftGripActive_ || rightGripActive_) {
      rightHandShapeBrushActive_ = false;
      rightHandShapeBrushTool_ = -1;
      return false;
    }

    const bool erase = rightHandState_.fist;
    const bool smooth = rightHandState_.open;
    if (!erase && !smooth) {
      rightHandShapeBrushActive_ = false;
      rightHandShapeBrushTool_ = -1;
      return false;
    }

    const int gestureToolIndex = erase ? 1 : 2;
    const large::sdf::Vec3 centerWorld = erase ? rightHandState_.fistToolPosition : rightHandState_.openToolPosition;
    const large::sdf::Vec3 centerLocal = worldToObjectPoint(centerWorld);
    const float localRadius = handGestureLocalRadius(localBrushRadius);
    const float surfaceDistance = std::abs(volume_.sample(centerLocal));
    const float snapDistance =
        std::max(kHandSculptSurfaceSnapDistance / std::max(objectScale_, 0.001f), volume_.voxelSize() * 2.0f);

    handDisplayToolIndex_ = gestureToolIndex;
    stretchPullActive_ = false;
    brushVisible_ = surfaceDistance <= std::max(localRadius * 1.15f, snapDistance);
    brushHitLocal_ = centerLocal;
    brushHitWorld_ = objectToWorldPoint(centerLocal);
    if (!brushVisible_) {
      rightHandShapeBrushActive_ = false;
      rightHandShapeBrushTool_ = -1;
      return true;
    }

    if (frameCounter_ >= nextSculptFrame_) {
      if (!rightHandShapeBrushActive_ || rightHandShapeBrushTool_ != gestureToolIndex) {
        history_.capture();
      }

      if (erase) {
        volume_.applySphereBrush(centerLocal, localRadius, large::sdf::BrushMode::Subtract, kHandEraseStrength);
      } else {
        volume_.applySmoothBrush(centerLocal, localRadius * 1.35f, kHandSmoothStrength);
      }

      rightHandShapeBrushActive_ = true;
      rightHandShapeBrushTool_ = gestureToolIndex;
      uploadSdfTexture();
      nextSculptFrame_ = frameCounter_ + (smooth ? 3 : 2);
      if (frameCounter_ >= nextSculptLogFrame_) {
        logInfo("Hand %s: center=(%.2f %.2f %.2f), radius=%.2f",
                erase ? "Erase" : "Smooth",
                centerLocal.x,
                centerLocal.y,
                centerLocal.z,
                localRadius);
        nextSculptLogFrame_ = frameCounter_ + 60;
      }
    }
    return true;
  }

  void updateControllerAndSculpt(XrTime predictedDisplayTime) {
    if (actionSet_ == XR_NULL_HANDLE || rightAimSpace_ == XR_NULL_HANDLE ||
        leftGripSpace_ == XR_NULL_HANDLE || rightGripSpace_ == XR_NULL_HANDLE) {
      brushVisible_ = false;
      rightToolVisible_ = false;
      menuPointerActive_ = false;
      menuHoverIndex_ = -1;
      return;
    }

    XrActiveActionSet activeActionSet{};
    activeActionSet.actionSet = actionSet_;
    activeActionSet.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo syncInfo{};
    syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    if (!checkXr(xrInstance_, xrSyncActions(xrSession_, &syncInfo), "xrSyncActions")) {
      brushVisible_ = false;
      rightToolVisible_ = false;
      menuPointerActive_ = false;
      menuHoverIndex_ = -1;
      return;
    }

    updateObjectManipulation(predictedDisplayTime);
    updateLocomotion(predictedDisplayTime);
    updateLeftHandPinchZoom();
    updateBrushAdjustments();
    updateToolButtons();
    updateEditButtons();
    rightTriggerValue_ = readFloatAction(rightTriggerAction_, rightHandPath_, "right trigger");
    const bool triggerDown = rightTriggerValue_ >= kTriggerThreshold;
    const float localBrushRadius = brushRadius_ / std::max(objectScale_, 0.001f);
    handDisplayToolIndex_ = -1;
    flattenPreviewVisible_ = false;
    if (!triggerDown) {
      rightControllerToolStrokeActive_ = false;
    }

    if (updateHandClapToolSelector()) {
      brushVisible_ = false;
      rightToolVisible_ = false;
      menuPointerActive_ = false;
      menuHoverIndex_ = -1;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    if (updateHandMenuPoke()) {
      brushVisible_ = false;
      rightToolVisible_ = false;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    if (updateRightHandPinchStretch(localBrushRadius)) {
      rightToolVisible_ = false;
      menuPointerActive_ = false;
      menuHoverIndex_ = -1;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    constexpr XrSpaceLocationFlags aimFlags =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    const ControllerPose aimPose = locateControllerPose(
        rightAimPoseAction_, rightHandPath_, rightAimSpace_, predictedDisplayTime, aimFlags, "right aim");
    if (!aimPose.active) {
      brushVisible_ = false;
      rightToolVisible_ = false;
      menuPointerActive_ = false;
      menuHoverIndex_ = -1;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    const large::sdf::Vec3 rayOrigin = aimPose.position;
    const large::sdf::Vec3 rayDirection =
        large::sdf::normalize(rotateByQuaternion(aimPose.orientation, {0.0f, 0.0f, -1.0f}));
    const large::sdf::Vec3 toolCenterWorld = rayOrigin + rayDirection * kControllerToolLength;
    const large::sdf::Vec3 toolCenterLocal = worldToObjectPoint(toolCenterWorld);
    const XrQuaternionf toolOrientationLocal =
        multiplyQuaternions(conjugateQuaternion(objectRotation_), aimPose.orientation);
    rightToolVisible_ = true;
    rightToolPosition_ = toolCenterWorld;
    rightToolDirection_ = rayDirection;
    updateMenuPointer(rayOrigin, rayDirection, triggerDown);
    if (menuPointerActive_) {
      brushVisible_ = false;
      rightToolVisible_ = false;
      stretchPullActive_ = false;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    large::sdf::Vec3 contactLocal{};
    const bool toolInContact = findToolSurfaceContact(toolCenterLocal, localBrushRadius, contactLocal);
    brushHitLocal_ = toolCenterLocal;
    brushHitWorld_ = toolCenterWorld;

    // Flatten preview: before pressing, show the tangent plane that would be
    // locked; during a stroke, show the locked plane itself.
    if (activeTool_ == VrTool::Flatten) {
      if (rightControllerToolStrokeActive_ && triggerDown) {
        flattenPreviewCenterWorld_ = objectToWorldPoint(flattenPlanePointLocal_);
        flattenPreviewNormalWorld_ =
            large::sdf::normalize(rotateByQuaternion(objectRotation_, flattenPlaneNormalLocal_));
        flattenPreviewVisible_ = true;
      } else if (toolInContact) {
        flattenPreviewCenterWorld_ = objectToWorldPoint(contactLocal);
        flattenPreviewNormalWorld_ = large::sdf::normalize(
            rotateByQuaternion(objectRotation_, estimateSdfNormalLocal(contactLocal)));
        flattenPreviewVisible_ = true;
      }
    }

    if (activeTool_ == VrTool::Stretch) {
      if (!triggerDown) {
        brushVisible_ = toolInContact;
        clearStretchInteraction();
        rightTriggerWasDown_ = triggerDown;
        return;
      }

      if (stretchAnchorSet_) {
        brushVisible_ = true;
        updateStretchTool(stretchAnchorLocal_, toolCenterLocal, toolOrientationLocal, localBrushRadius, triggerDown);
        rightTriggerWasDown_ = triggerDown;
        return;
      }

      if (toolInContact) {
        brushVisible_ = true;
        brushHitLocal_ = contactLocal;
        brushHitWorld_ = objectToWorldPoint(contactLocal);
        updateStretchTool(contactLocal, toolCenterLocal, toolOrientationLocal, localBrushRadius, triggerDown);
        rightTriggerWasDown_ = triggerDown;
        return;
      }

      brushVisible_ = false;
      stretchPullActive_ = false;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    const bool addInEmptySpace = activeTool_ == VrTool::Add && triggerDown;
    if (!toolInContact && !addInEmptySpace) {
      brushVisible_ = false;
      strokeHasLast_ = false;
      rightTriggerWasDown_ = triggerDown;
      return;
    }

    brushVisible_ = true;
    if (toolInContact) {
      brushHitLocal_ = contactLocal;
      brushHitWorld_ = objectToWorldPoint(contactLocal);
    } else {
      brushHitLocal_ = toolCenterLocal;
      brushHitWorld_ = toolCenterWorld;
    }
    stretchPullActive_ = false;
    const bool rightHandManipulatingObject = rightGripActive_;
    if (!rightHandManipulatingObject && triggerDown && frameCounter_ >= nextSculptFrame_) {
      if (!rightControllerToolStrokeActive_) {
        history_.capture();
        beginSculptStroke(toolCenterLocal, toolInContact, contactLocal);
        rightControllerToolStrokeActive_ = true;
      }
      applySculptStamp(toolCenterLocal, localBrushRadius, brushStrength_);
      uploadSdfTexture();
      uploadColorTexture();
      nextSculptFrame_ = frameCounter_ + (activeTool_ == VrTool::Smooth ? 3 : 2);
      if (frameCounter_ >= nextSculptLogFrame_) {
        logInfo("VR %s contact brush: trigger=%.2f, tool=(%.2f %.2f %.2f), scale=%.2f",
                toolName(activeTool_),
                rightTriggerValue_,
                toolCenterLocal.x,
                toolCenterLocal.y,
                toolCenterLocal.z,
                objectScale_);
        nextSculptLogFrame_ = frameCounter_ + 60;
      }
    }
    rightTriggerWasDown_ = triggerDown;
  }

  bool raycastSdfSurface(large::sdf::Vec3 worldOrigin,
                         large::sdf::Vec3 worldDirection,
                         large::sdf::Vec3& hitLocal) const {
    const large::sdf::Vec3 localOrigin = worldToObjectPoint(worldOrigin);
    const large::sdf::Vec3 localDirection = worldToObjectDirection(worldDirection);

    float nearT = 0.0f;
    float farT = 0.0f;
    if (!intersectVolumeBounds(volume_, localOrigin, localDirection, nearT, farT)) {
      return false;
    }

    float t = std::max(nearT, 0.0f);
    constexpr float surfaceEpsilon = 0.0070f;
    bool hasPrevious = false;
    float previousT = t;
    float previousD = 0.0f;
    for (int i = 0; i < 144 && t <= farT; ++i) {
      const large::sdf::Vec3 p = localOrigin + localDirection * t;
      const float d = volume_.sample(p);
      if (std::abs(d) <= surfaceEpsilon) {
        hitLocal = p;
        return true;
      }

      if (hasPrevious && d * previousD < 0.0f) {
        float lowT = previousT;
        float highT = t;
        float lowD = previousD;
        for (int j = 0; j < 8; ++j) {
          const float midT = (lowT + highT) * 0.5f;
          const large::sdf::Vec3 midP = localOrigin + localDirection * midT;
          const float midD = volume_.sample(midP);
          if (std::abs(midD) <= surfaceEpsilon) {
            hitLocal = midP;
            return true;
          }
          if (lowD * midD <= 0.0f) {
            highT = midT;
          } else {
            lowT = midT;
            lowD = midD;
          }
        }
        hitLocal = localOrigin + localDirection * ((lowT + highT) * 0.5f);
        return true;
      }

      hasPrevious = true;
      previousT = t;
      previousD = d;
      t += large::sdf::clamp(std::abs(d) * 0.80f, 0.0030f, 0.075f);
    }

    return false;
  }

  static large::sdf::VoxelBounds mergeVoxelBounds(large::sdf::VoxelBounds a, large::sdf::VoxelBounds b) {
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

  static large::sdf::VoxelBounds clampVoxelBounds(large::sdf::VoxelBounds bounds, large::sdf::IVec3 size) {
    if (!bounds.valid || size.x <= 0 || size.y <= 0 || size.z <= 0) {
      return {};
    }

    bounds.min.x = large::sdf::clampInt(bounds.min.x, 0, size.x - 1);
    bounds.min.y = large::sdf::clampInt(bounds.min.y, 0, size.y - 1);
    bounds.min.z = large::sdf::clampInt(bounds.min.z, 0, size.z - 1);
    bounds.max.x = large::sdf::clampInt(bounds.max.x, bounds.min.x, size.x - 1);
    bounds.max.y = large::sdf::clampInt(bounds.max.y, bounds.min.y, size.y - 1);
    bounds.max.z = large::sdf::clampInt(bounds.max.z, bounds.min.z, size.z - 1);
    return bounds;
  }

  static large::sdf::VoxelBounds paddedVoxelBounds(large::sdf::VoxelBounds bounds,
                                                   large::sdf::IVec3 size,
                                                   int paddingVoxels) {
    if (!bounds.valid) {
      return {};
    }

    bounds.min.x -= paddingVoxels;
    bounds.min.y -= paddingVoxels;
    bounds.min.z -= paddingVoxels;
    bounds.max.x += paddingVoxels;
    bounds.max.y += paddingVoxels;
    bounds.max.z += paddingVoxels;
    return clampVoxelBounds(bounds, size);
  }

  static bool isFullVoxelBounds(large::sdf::VoxelBounds bounds, large::sdf::IVec3 size) {
    return bounds.valid && bounds.min.x <= 0 && bounds.min.y <= 0 && bounds.min.z <= 0 &&
           bounds.max.x >= size.x - 1 && bounds.max.y >= size.y - 1 && bounds.max.z >= size.z - 1;
  }

  large::sdf::VoxelBounds computeSdfRenderBounds() const {
    const large::sdf::IVec3 size = volume_.size();
    const float surfaceBand = volume_.voxelSize() * 4.0f;
    large::sdf::VoxelBounds bounds{};
    const std::vector<float>& values = volume_.values();

    for (int z = 0; z < size.z; ++z) {
      for (int y = 0; y < size.y; ++y) {
        for (int x = 0; x < size.x; ++x) {
          if (values[volume_.index(x, y, z)] > surfaceBand) {
            continue;
          }

          large::sdf::VoxelBounds voxel{};
          voxel.valid = true;
          voxel.min = {x, y, z};
          voxel.max = {x, y, z};
          bounds = mergeVoxelBounds(bounds, voxel);
        }
      }
    }

    if (!bounds.valid) {
      bounds.valid = true;
      bounds.min = {0, 0, 0};
      bounds.max = {size.x - 1, size.y - 1, size.z - 1};
    }
    return paddedVoxelBounds(bounds, size, 5);
  }

  void recomputeSdfRenderBounds(const char* reason) {
    renderBounds_ = computeSdfRenderBounds();
    if (renderBounds_.valid) {
      logInfo("SDF render bounds %s: [%d %d %d]-[%d %d %d]",
              reason,
              renderBounds_.min.x,
              renderBounds_.min.y,
              renderBounds_.min.z,
              renderBounds_.max.x,
              renderBounds_.max.y,
              renderBounds_.max.z);
    }
  }

  void expandSdfRenderBounds(large::sdf::VoxelBounds changedBounds) {
    const large::sdf::IVec3 size = volume_.size();
    changedBounds = paddedVoxelBounds(changedBounds, size, 5);
    if (!changedBounds.valid) {
      return;
    }

    if (!renderBounds_.valid) {
      renderBounds_ = computeSdfRenderBounds();
      return;
    }

    renderBounds_ = clampVoxelBounds(mergeVoxelBounds(renderBounds_, changedBounds), size);
  }

  large::sdf::Vec3 renderBoundsMin() const {
    const large::sdf::IVec3 size = volume_.size();
    const large::sdf::VoxelBounds bounds =
        renderBounds_.valid ? clampVoxelBounds(renderBounds_, size) : computeSdfRenderBounds();
    return {
        volume_.origin().x + static_cast<float>(bounds.min.x) * volume_.voxelSize(),
        volume_.origin().y + static_cast<float>(bounds.min.y) * volume_.voxelSize(),
        volume_.origin().z + static_cast<float>(bounds.min.z) * volume_.voxelSize(),
    };
  }

  large::sdf::Vec3 renderBoundsExtent() const {
    const large::sdf::IVec3 size = volume_.size();
    const large::sdf::VoxelBounds bounds =
        renderBounds_.valid ? clampVoxelBounds(renderBounds_, size) : computeSdfRenderBounds();
    return {
        static_cast<float>(bounds.max.x - bounds.min.x + 1) * volume_.voxelSize(),
        static_cast<float>(bounds.max.y - bounds.min.y + 1) * volume_.voxelSize(),
        static_cast<float>(bounds.max.z - bounds.min.z + 1) * volume_.voxelSize(),
    };
  }

  void uploadSdfTexture(large::sdf::VoxelBounds extraBounds = {}) {
    if (sdfTexture_ == 0) {
      return;
    }

    const large::sdf::IVec3 size = volume_.size();
    large::sdf::VoxelBounds bounds = mergeVoxelBounds(volume_.dirtyBounds(), extraBounds);
    if (!bounds.valid) {
      return;
    }

    bounds = clampVoxelBounds(bounds, size);

    const int width = bounds.max.x - bounds.min.x + 1;
    const int height = bounds.max.y - bounds.min.y + 1;
    const int depth = bounds.max.z - bounds.min.z + 1;
    const int totalVoxels = size.x * size.y * size.z;
    const int uploadVoxels = width * height * depth;
    const bool uploadFull = uploadVoxels * 4 >= totalVoxels * 3;

    glBindTexture(GL_TEXTURE_3D, sdfTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (uploadFull) {
      glTexSubImage3D(GL_TEXTURE_3D,
                      0,
                      0,
                      0,
                      0,
                      size.x,
                      size.y,
                      size.z,
                      GL_RED,
                      GL_FLOAT,
                      volume_.values().data());
    } else {
      uploadScratch_.resize(static_cast<std::size_t>(uploadVoxels));
      const float* values = volume_.values().data();
      float* packed = uploadScratch_.data();
      for (int z = 0; z < depth; ++z) {
        for (int y = 0; y < height; ++y) {
          const std::size_t sourceIndex = volume_.index(bounds.min.x, bounds.min.y + y, bounds.min.z + z);
          const std::size_t destinationIndex =
              (static_cast<std::size_t>(z) * static_cast<std::size_t>(height) + static_cast<std::size_t>(y)) *
              static_cast<std::size_t>(width);
          std::copy_n(values + sourceIndex, width, packed + destinationIndex);
        }
      }

      glTexSubImage3D(GL_TEXTURE_3D,
                      0,
                      bounds.min.x,
                      bounds.min.y,
                      bounds.min.z,
                      width,
                      height,
                      depth,
                      GL_RED,
                      GL_FLOAT,
                      packed);
    }
    const GLenum uploadError = glGetError();
    glBindTexture(GL_TEXTURE_3D, 0);
    if (uploadError != GL_NO_ERROR) {
      logError("SDF texture sub upload failed: 0x%x, box=%dx%dx%d", uploadError, width, height, depth);
      return;
    }
    if (isFullVoxelBounds(bounds, size)) {
      recomputeSdfRenderBounds("recomputed");
    } else {
      expandSdfRenderBounds(bounds);
    }
    volume_.clearDirtyBounds();
  }

  void uploadColorTexture() {
    if (colorTexture_ == 0 || !colorDirtyBounds_.valid || colorVoxels_.empty()) {
      return;
    }

    const large::sdf::IVec3 size = volume_.size();
    const large::sdf::VoxelBounds bounds = clampVoxelBounds(colorDirtyBounds_, size);
    const int width = bounds.max.x - bounds.min.x + 1;
    const int height = bounds.max.y - bounds.min.y + 1;
    const int depth = bounds.max.z - bounds.min.z + 1;
    const int totalVoxels = size.x * size.y * size.z;
    const int uploadVoxels = width * height * depth;
    const bool uploadFull = uploadVoxels * 4 >= totalVoxels * 3;

    glBindTexture(GL_TEXTURE_3D, colorTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (uploadFull) {
      glTexSubImage3D(GL_TEXTURE_3D,
                      0,
                      0,
                      0,
                      0,
                      size.x,
                      size.y,
                      size.z,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      colorVoxels_.data());
    } else {
      colorUploadScratch_.resize(static_cast<std::size_t>(uploadVoxels) * 4);
      const std::uint8_t* values = colorVoxels_.data();
      std::uint8_t* packed = colorUploadScratch_.data();
      for (int z = 0; z < depth; ++z) {
        for (int y = 0; y < height; ++y) {
          const std::size_t sourceIndex = volume_.index(bounds.min.x, bounds.min.y + y, bounds.min.z + z) * 4;
          const std::size_t destinationIndex =
              (static_cast<std::size_t>(z) * static_cast<std::size_t>(height) + static_cast<std::size_t>(y)) *
              static_cast<std::size_t>(width) * 4;
          std::copy_n(values + sourceIndex, static_cast<std::size_t>(width) * 4, packed + destinationIndex);
        }
      }

      glTexSubImage3D(GL_TEXTURE_3D,
                      0,
                      bounds.min.x,
                      bounds.min.y,
                      bounds.min.z,
                      width,
                      height,
                      depth,
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      packed);
    }
    const GLenum uploadError = glGetError();
    glBindTexture(GL_TEXTURE_3D, 0);
    if (uploadError != GL_NO_ERROR) {
      logError("Color texture sub upload failed: 0x%x, box=%dx%dx%d", uploadError, width, height, depth);
      return;
    }
    colorDirtyBounds_ = {};
  }

  bool initializeMeshRenderer() {
    const large::sdf::SurfaceMesh mesh = large::sdf::buildSurfaceMesh(volume_);
    std::vector<MeshVertex> vertices;
    vertices.reserve(mesh.triangles.size() * 3);

    for (const large::sdf::SurfaceTriangle& triangle : mesh.triangles) {
      vertices.push_back({triangle.a, triangle.normal});
      vertices.push_back({triangle.b, triangle.normal});
      vertices.push_back({triangle.c, triangle.normal});
    }

    if (vertices.empty()) {
      logError("SDF mesh is empty");
      return false;
    }

    constexpr const char* vertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec3 vNormal;
out vec3 vWorld;
void main() {
  vec4 world = uModel * vec4(aPosition, 1.0);
  vWorld = world.xyz;
  vNormal = normalize(mat3(uModel) * aNormal);
  gl_Position = uMvp * vec4(aPosition, 1.0);
}
)";

    constexpr const char* fragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec3 vWorld;
out vec4 oColor;
void main() {
  vec3 normal = normalize(vNormal);
  vec3 light = normalize(vec3(-0.35, 0.82, 0.42));
  float diffuse = abs(dot(normal, light));
  float heightTint = clamp(vWorld.y * 0.18 + 0.5, 0.0, 1.0);
  vec3 clay = mix(vec3(0.72, 0.58, 0.48), vec3(0.92, 0.76, 0.60), heightTint);
  vec3 color = clay * (0.68 + diffuse * 0.32);
  oColor = vec4(color, 1.0);
}
)";

    meshProgram_ = linkProgram(vertexShader, fragmentShader);
    if (meshProgram_ == 0) {
      return false;
    }

    meshMvpLocation_ = glGetUniformLocation(meshProgram_, "uMvp");
    meshModelLocation_ = glGetUniformLocation(meshProgram_, "uModel");
    if (meshMvpLocation_ < 0 || meshModelLocation_ < 0) {
      logError("Mesh shader uniforms are missing");
      return false;
    }

    glGenVertexArrays(1, &meshVao_);
    glBindVertexArray(meshVao_);
    glGenBuffers(1, &meshVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, meshVbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(MeshVertex)),
                 vertices.data(),
                 GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(MeshVertex),
                          reinterpret_cast<const void*>(offsetof(MeshVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(MeshVertex),
                          reinterpret_cast<const void*>(offsetof(MeshVertex, normal)));
    glBindVertexArray(0);

    int32_t depthWidth = 0;
    int32_t depthHeight = 0;
    for (const EyeSwapchain& swapchain : eyeSwapchains_) {
      depthWidth = std::max(depthWidth, swapchain.width);
      depthHeight = std::max(depthHeight, swapchain.height);
    }

    glGenRenderbuffers(1, &depthBuffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, depthWidth, depthHeight);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    meshVertexCount_ = static_cast<GLsizei>(vertices.size());
    logInfo("SDF mesh renderer ready: %zu triangles, %d vertices",
            mesh.triangles.size(),
            meshVertexCount_);
    return true;
  }

  void renderMeshForEye(int eye, uint64_t frameIndex) {
    if (meshProgram_ == 0 || meshVao_ == 0 || meshVertexCount_ <= 0) {
      return;
    }

    const float rotation = static_cast<float>(frameIndex) * 0.0045f;
    const Mat4 model = multiply(makeTranslation(0.0f, -0.12f, -2.05f),
                                multiply(makeRotationY(rotation), makeScale(0.82f)));
    const Mat4 view = makeViewMatrix(views_[eye].pose);
    const Mat4 projection = makeProjection(views_[eye].fov, 0.05f, 50.0f);
    const Mat4 mvp = multiply(projection, multiply(view, model));

    glUseProgram(meshProgram_);
    glUniformMatrix4fv(meshMvpLocation_, 1, GL_FALSE, mvp.m.data());
    glUniformMatrix4fv(meshModelLocation_, 1, GL_FALSE, model.m.data());
    glBindVertexArray(meshVao_);
    glDrawArrays(GL_TRIANGLES, 0, meshVertexCount_);
    glBindVertexArray(0);
    glUseProgram(0);
  }

  bool initializeSdfRaymarchRenderer() {
    constexpr const char* vertexShader = R"(#version 300 es
out vec2 vUv;
void main() {
  vec2 p;
  if (gl_VertexID == 0) {
    p = vec2(-1.0, -1.0);
  } else if (gl_VertexID == 1) {
    p = vec2(3.0, -1.0);
  } else {
    p = vec2(-1.0, 3.0);
  }
  vUv = p * 0.5 + 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

    constexpr const char* fragmentShader = R"(#version 300 es
precision highp float;
precision highp int;
precision highp sampler3D;

in vec2 vUv;
out vec4 oColor;

uniform sampler3D uSdf;
uniform sampler3D uColorVol;
uniform vec3 uCameraPos;
uniform mat3 uViewRotation;
uniform vec4 uFovTangents;
uniform vec3 uVolumeMin;
uniform vec3 uVolumeExtent;
uniform vec3 uRenderMin;
uniform vec3 uRenderExtent;
uniform vec3 uObjectPos;
uniform mat3 uObjectRotation;
uniform mat3 uObjectInvRotation;
uniform float uObjectScale;
uniform float uArEnabled;
uniform vec3 uWorldOffset;  // user locomotion offset applied to the static room
uniform float uVoxelSize;   // local size of one SDF voxel

vec2 intersectBox(vec3 rayOrigin, vec3 rayDir, vec3 boxMin, vec3 boxMax) {
  vec3 invDir = 1.0 / rayDir;
  vec3 t0 = (boxMin - rayOrigin) * invDir;
  vec3 t1 = (boxMax - rayOrigin) * invDir;
  vec3 tMin = min(t0, t1);
  vec3 tMax = max(t0, t1);
  float nearT = max(max(tMin.x, tMin.y), tMin.z);
  float farT = min(min(tMax.x, tMax.y), tMax.z);
  return vec2(nearT, farT);
}

vec3 localToWorld(vec3 localPoint) {
  return uObjectPos + uObjectRotation * (localPoint * uObjectScale);
}

vec3 worldToLocal(vec3 worldPoint) {
  return uObjectInvRotation * (worldPoint - uObjectPos) / uObjectScale;
}

float raySegmentDistance(vec3 rayOrigin, vec3 rayDir, vec3 a, vec3 b, out float rayT) {
  vec3 segment = b - a;
  float segLen2 = max(dot(segment, segment), 0.00001);
  float raySeg = dot(rayDir, segment);
  vec3 originToA = rayOrigin - a;
  float originRay = dot(originToA, rayDir);
  float originSeg = dot(originToA, segment);
  float denom = max(segLen2 - raySeg * raySeg, 0.00001);
  float unclampedT = (raySeg * originSeg - originRay * segLen2) / denom;
  float segT = clamp((originSeg + raySeg * unclampedT) / segLen2, 0.0, 1.0);
  vec3 pointOnSegment = a + segment * segT;
  rayT = max(dot(pointOnSegment - rayOrigin, rayDir), 0.0);
  return length(rayOrigin + rayDir * rayT - pointOnSegment);
}

void accumulateBoundsEdge(inout float edgeAlpha,
                          vec3 rayOrigin,
                          vec3 rayDir,
                          vec3 localA,
                          vec3 localB,
                          float sceneDepth) {
  float rayT = 0.0;
  float distance = raySegmentDistance(rayOrigin, rayDir, localToWorld(localA), localToWorld(localB), rayT);
  float visible = step(0.02, rayT) * step(rayT, sceneDepth - 0.015);
  float core = 1.0 - smoothstep(0.006, 0.014, distance);
  float halo = 1.0 - smoothstep(0.014, 0.044, distance);
  edgeAlpha = max(edgeAlpha, visible * clamp(core * 0.74 + halo * 0.20, 0.0, 0.86));
}

vec3 applyVolumeBoundsCube(vec3 color, vec2 uv, float sceneDepth, inout float sceneAlpha) {
  float x = mix(uFovTangents.x, uFovTangents.y, uv.x);
  float y = mix(uFovTangents.z, uFovTangents.w, uv.y);
  vec3 rayDir = normalize(uViewRotation * normalize(vec3(x, y, -1.0)));
  vec3 rayOrigin = uCameraPos;
  vec3 boxMin = uVolumeMin;
  vec3 boxMax = uVolumeMin + uVolumeExtent;
  float edgeAlpha = 0.0;

  for (int iy = 0; iy < 2; ++iy) {
    for (int iz = 0; iz < 2; ++iz) {
      float yv = mix(boxMin.y, boxMax.y, float(iy));
      float zv = mix(boxMin.z, boxMax.z, float(iz));
      accumulateBoundsEdge(edgeAlpha, rayOrigin, rayDir, vec3(boxMin.x, yv, zv), vec3(boxMax.x, yv, zv), sceneDepth);
    }
  }
  for (int ix = 0; ix < 2; ++ix) {
    for (int iz = 0; iz < 2; ++iz) {
      float xv = mix(boxMin.x, boxMax.x, float(ix));
      float zv = mix(boxMin.z, boxMax.z, float(iz));
      accumulateBoundsEdge(edgeAlpha, rayOrigin, rayDir, vec3(xv, boxMin.y, zv), vec3(xv, boxMax.y, zv), sceneDepth);
    }
  }
  for (int ix = 0; ix < 2; ++ix) {
    for (int iy = 0; iy < 2; ++iy) {
      float xv = mix(boxMin.x, boxMax.x, float(ix));
      float yv = mix(boxMin.y, boxMax.y, float(iy));
      accumulateBoundsEdge(edgeAlpha, rayOrigin, rayDir, vec3(xv, yv, boxMin.z), vec3(xv, yv, boxMax.z), sceneDepth);
    }
  }

  vec3 boundsColor = vec3(0.58, 0.82, 0.92);
  sceneAlpha = max(sceneAlpha, edgeAlpha);
  return mix(color, boundsColor, edgeAlpha);
}

float sampleSdfLocal(vec3 localPoint) {
  vec3 uv = (localPoint - uVolumeMin) / uVolumeExtent;
  float field = texture(uSdf, clamp(uv, vec3(0.0), vec3(1.0))).r;
  // Intersect the field with the workspace box so material reaching the
  // volume limits is capped by a flat face instead of appearing open.
  vec3 boxCenter = uVolumeMin + uVolumeExtent * 0.5;
  vec3 boxHalf = uVolumeExtent * 0.5 - vec3(uVoxelSize * 0.5);
  vec3 q = abs(localPoint - boxCenter) - boxHalf;
  float boxDistance = length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
  return max(field, boxDistance);
}

vec3 estimateNormalLocal(vec3 localPoint) {
  // Sample at ~one voxel so the central differences straddle several
  // trilinear cells; sub-voxel offsets give per-cell constant gradients,
  // which shows up as triangular facets on the surface.
  float e = max(uVoxelSize * 0.75, 0.004);
  vec3 dx = vec3(e, 0.0, 0.0);
  vec3 dy = vec3(0.0, e, 0.0);
  vec3 dz = vec3(0.0, 0.0, e);
  return normalize(vec3(
    sampleSdfLocal(localPoint + dx) - sampleSdfLocal(localPoint - dx),
    sampleSdfLocal(localPoint + dy) - sampleSdfLocal(localPoint - dy),
    sampleSdfLocal(localPoint + dz) - sampleSdfLocal(localPoint - dz)));
}

float roomLine(float value, float spacing, float width) {
  float cell = abs(fract(value / spacing + 0.5) - 0.5) * spacing;
  return 1.0 - smoothstep(width, width * 2.2, cell);
}

float roomGrid(vec2 p, float spacing, float width) {
  return max(roomLine(p.x, spacing, width), roomLine(p.y, spacing, width));
}

vec3 shadeRoom(vec3 rayOrigin, vec3 rayDir, out float roomDepth) {
  const float floorY = -1.35;
  const float ceilingY = 2.00;
  const float leftX = -3.60;
  const float rightX = 3.60;
  const float backZ = -5.80;
  const float frontZ = 1.60;
  const float farDepth = 10000.0;

  float bestT = farDepth;
  int surface = 0;
  vec3 hitPoint = rayOrigin + rayDir * 4.0;

  if (abs(rayDir.y) > 0.0001) {
    float t = (floorY - rayOrigin.y) / rayDir.y;
    vec3 p = rayOrigin + rayDir * t;
    if (t > 0.02 && t < bestT && p.x >= leftX && p.x <= rightX && p.z >= backZ && p.z <= frontZ) {
      bestT = t;
      surface = 1;
      hitPoint = p;
    }

    t = (ceilingY - rayOrigin.y) / rayDir.y;
    p = rayOrigin + rayDir * t;
    if (t > 0.02 && t < bestT && p.x >= leftX && p.x <= rightX && p.z >= backZ && p.z <= frontZ) {
      bestT = t;
      surface = 2;
      hitPoint = p;
    }
  }

  if (abs(rayDir.x) > 0.0001) {
    float t = (leftX - rayOrigin.x) / rayDir.x;
    vec3 p = rayOrigin + rayDir * t;
    if (t > 0.02 && t < bestT && p.y >= floorY && p.y <= ceilingY && p.z >= backZ && p.z <= frontZ) {
      bestT = t;
      surface = 3;
      hitPoint = p;
    }

    t = (rightX - rayOrigin.x) / rayDir.x;
    p = rayOrigin + rayDir * t;
    if (t > 0.02 && t < bestT && p.y >= floorY && p.y <= ceilingY && p.z >= backZ && p.z <= frontZ) {
      bestT = t;
      surface = 3;
      hitPoint = p;
    }
  }

  if (abs(rayDir.z) > 0.0001) {
    float t = (backZ - rayOrigin.z) / rayDir.z;
    vec3 p = rayOrigin + rayDir * t;
    if (t > 0.02 && t < bestT && p.x >= leftX && p.x <= rightX && p.y >= floorY && p.y <= ceilingY) {
      bestT = t;
      surface = 4;
      hitPoint = p;
    }

    t = (frontZ - rayOrigin.z) / rayDir.z;
    p = rayOrigin + rayDir * t;
    if (t > 0.02 && t < bestT && p.x >= leftX && p.x <= rightX && p.y >= floorY && p.y <= ceilingY) {
      bestT = t;
      surface = 4;
      hitPoint = p;
    }
  }

  roomDepth = bestT;
  if (surface == 0) {
    float sky = clamp(rayDir.y * 0.5 + 0.5, 0.0, 1.0);
    return mix(vec3(0.030, 0.036, 0.041), vec3(0.060, 0.070, 0.076), sky);
  }

  vec3 color = vec3(0.19, 0.20, 0.19);
  if (surface == 1) {
    color = vec3(0.15, 0.155, 0.145);
    float grid = roomGrid(hitPoint.xz, 0.50, 0.012);
    float axis = max(roomLine(hitPoint.x, 2.0, 0.018), roomLine(hitPoint.z, 2.0, 0.018));
    color = mix(color, vec3(0.25, 0.265, 0.25), grid * 0.40);
    color = mix(color, vec3(0.32, 0.34, 0.32), axis * 0.30);
  } else if (surface == 2) {
    color = vec3(0.21, 0.215, 0.205);
  } else {
    color = vec3(0.185, 0.195, 0.19);
    vec2 wallUv = surface == 3 ? hitPoint.zy : hitPoint.xy;
    float seams = roomGrid(wallUv, 0.75, 0.010);
    color = mix(color, vec3(0.245, 0.255, 0.25), seams * 0.22);
  }

  float verticalShade = clamp((hitPoint.y - floorY) / (ceilingY - floorY), 0.0, 1.0);
  color *= 0.82 + verticalShade * 0.22;
  color = mix(color, vec3(0.032, 0.038, 0.043), clamp(bestT / 9.0, 0.0, 1.0) * 0.25);
  return color;
}

vec3 shadeUv(vec2 uv, out float sceneDepth, out float sceneAlpha) {
  sceneDepth = 10000.0;
  // In AR mode the synthetic room is fully transparent so the passthrough
  // layer underneath shows through; the sculpture stays opaque.
  sceneAlpha = 1.0 - clamp(uArEnabled, 0.0, 1.0);
  float x = mix(uFovTangents.x, uFovTangents.y, uv.x);
  float y = mix(uFovTangents.z, uFovTangents.w, uv.y);
  vec3 rayDir = normalize(uViewRotation * normalize(vec3(x, y, -1.0)));
  vec3 rayOrigin = uCameraPos;
  float roomDepth = 10000.0;
  vec3 rayOriginLocal = worldToLocal(rayOrigin);
  vec3 rayDirLocal = normalize(uObjectInvRotation * rayDir);

  vec3 renderBoxMin = uRenderMin;
  vec3 renderBoxMax = uRenderMin + uRenderExtent;
  vec2 hit = intersectBox(rayOriginLocal, rayDirLocal, renderBoxMin, renderBoxMax);

  if (hit.y <= max(hit.x, 0.0)) {
    if (uArEnabled > 0.5) {
      return vec3(0.0);
    }
    vec3 background = shadeRoom(rayOrigin + uWorldOffset, rayDir, roomDepth);
    sceneDepth = roomDepth;
    return background;
  }

  float t = max(hit.x, 0.0);
  float surface = 0.0045;
  bool found = false;
  vec3 localP = rayOriginLocal;
  float previousT = t;
  float previousD = 0.0;
  bool hasPrevious = false;

  for (int i = 0; i < 112; ++i) {
    localP = rayOriginLocal + rayDirLocal * t;
    float d = sampleSdfLocal(localP);
    if (abs(d) < surface) {
      found = true;
      break;
    }

    if (hasPrevious && d * previousD < 0.0) {
      float lowT = previousT;
      float highT = t;
      float lowD = previousD;
      for (int j = 0; j < 8; ++j) {
        float midT = (lowT + highT) * 0.5;
        float midD = sampleSdfLocal(rayOriginLocal + rayDirLocal * midT);
        if (abs(midD) < surface) {
          lowT = midT;
          highT = midT;
          break;
        }
        if (lowD * midD <= 0.0) {
          highT = midT;
        } else {
          lowT = midT;
          lowD = midD;
        }
      }
      t = (lowT + highT) * 0.5;
      localP = rayOriginLocal + rayDirLocal * t;
      found = true;
      break;
    }

    previousT = t;
    previousD = d;
    hasPrevious = true;
    t += clamp(abs(d) * 0.80, 0.0030, 0.085);
    if (t > hit.y) {
      break;
    }
  }

  if (!found) {
    if (uArEnabled > 0.5) {
      return vec3(0.0);
    }
    vec3 background = shadeRoom(rayOrigin + uWorldOffset, rayDir, roomDepth);
    sceneDepth = roomDepth;
    return background;
  }

  sceneAlpha = 1.0;
  sceneDepth = max(t * uObjectScale, 0.0);
  vec3 normalLocal = estimateNormalLocal(localP);
  vec3 normal = normalize(uObjectRotation * normalLocal);
  vec3 light = normalize(vec3(-0.35, 0.85, 0.42));
  float diffuse = max(dot(normal, light), 0.0);
  float wrap = 0.5 + 0.5 * dot(normal, light);
  // Sample the paint slightly inside the surface so the trilinear filter
  // does not dilute it with unpainted voxels just outside.
  vec3 colorPoint = localP - normalLocal * (uVoxelSize * 0.6);
  vec3 uvw = clamp((colorPoint - uVolumeMin) / uVolumeExtent, vec3(0.0), vec3(1.0));
  vec3 albedo = texture(uColorVol, uvw).rgb;
  vec3 color = albedo * (0.35 + diffuse * 0.55 + wrap * 0.10);
  return color;
}


void main() {
  float sceneDepth = 10000.0;
  float sceneAlpha = 1.0;
  vec3 color = shadeUv(vUv, sceneDepth, sceneAlpha);
  color = applyVolumeBoundsCube(color, vUv, sceneDepth, sceneAlpha);
  oColor = vec4(color, sceneAlpha);
}
)";

    sdfProgram_ = linkProgram(vertexShader, fragmentShader);
    if (sdfProgram_ == 0) {
      return false;
    }

    sdfCameraLocation_ = glGetUniformLocation(sdfProgram_, "uCameraPos");
    sdfViewRotationLocation_ = glGetUniformLocation(sdfProgram_, "uViewRotation");
    sdfFovTangentsLocation_ = glGetUniformLocation(sdfProgram_, "uFovTangents");
    sdfVolumeMinLocation_ = glGetUniformLocation(sdfProgram_, "uVolumeMin");
    sdfVolumeExtentLocation_ = glGetUniformLocation(sdfProgram_, "uVolumeExtent");
    sdfRenderMinLocation_ = glGetUniformLocation(sdfProgram_, "uRenderMin");
    sdfRenderExtentLocation_ = glGetUniformLocation(sdfProgram_, "uRenderExtent");
    sdfObjectPosLocation_ = glGetUniformLocation(sdfProgram_, "uObjectPos");
    sdfObjectRotationLocation_ = glGetUniformLocation(sdfProgram_, "uObjectRotation");
    sdfObjectInvRotationLocation_ = glGetUniformLocation(sdfProgram_, "uObjectInvRotation");
    sdfObjectScaleLocation_ = glGetUniformLocation(sdfProgram_, "uObjectScale");
    sdfArEnabledLocation_ = glGetUniformLocation(sdfProgram_, "uArEnabled");
    sdfWorldOffsetLocation_ = glGetUniformLocation(sdfProgram_, "uWorldOffset");
    sdfVoxelSizeLocation_ = glGetUniformLocation(sdfProgram_, "uVoxelSize");
    const GLint samplerLocation = glGetUniformLocation(sdfProgram_, "uSdf");
    const GLint colorSamplerLocation = glGetUniformLocation(sdfProgram_, "uColorVol");
    if (sdfCameraLocation_ < 0 || sdfViewRotationLocation_ < 0 || sdfFovTangentsLocation_ < 0 ||
        sdfVolumeMinLocation_ < 0 || sdfVolumeExtentLocation_ < 0 || sdfObjectPosLocation_ < 0 ||
        sdfRenderMinLocation_ < 0 || sdfRenderExtentLocation_ < 0 ||
        sdfObjectRotationLocation_ < 0 || sdfObjectInvRotationLocation_ < 0 || sdfObjectScaleLocation_ < 0 ||
        sdfArEnabledLocation_ < 0 || sdfWorldOffsetLocation_ < 0 || sdfVoxelSizeLocation_ < 0 ||
        samplerLocation < 0 || colorSamplerLocation < 0) {
      logError("SDF raymarch shader uniforms are missing");
      return false;
    }

    const large::sdf::IVec3 size = volume_.size();
    glGenTextures(1, &sdfTexture_);
    glBindTexture(GL_TEXTURE_3D, sdfTexture_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D,
                 0,
                 GL_R32F,
                 size.x,
                 size.y,
                 size.z,
                 0,
                 GL_RED,
                 GL_FLOAT,
                 volume_.values().data());
    glBindTexture(GL_TEXTURE_3D, 0);

    const GLenum textureError = glGetError();
    if (textureError != GL_NO_ERROR) {
      logError("SDF 3D texture upload failed: 0x%x", textureError);
      return false;
    }
    volume_.clearDirtyBounds();
    recomputeSdfRenderBounds("initial");

    initializeColorVoxels();
    glGenTextures(1, &colorTexture_);
    glBindTexture(GL_TEXTURE_3D, colorTexture_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_3D,
                 0,
                 GL_RGBA8,
                 size.x,
                 size.y,
                 size.z,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 colorVoxels_.data());
    glBindTexture(GL_TEXTURE_3D, 0);

    const GLenum colorTextureError = glGetError();
    if (colorTextureError != GL_NO_ERROR) {
      logError("Color 3D texture upload failed: 0x%x", colorTextureError);
      return false;
    }
    colorDirtyBounds_ = {};

    glGenVertexArrays(1, &sdfVao_);
    glUseProgram(sdfProgram_);
    glUniform1i(samplerLocation, 0);
    glUniform1i(colorSamplerLocation, 1);
    glUseProgram(0);

    logInfo("SDF raymarch renderer ready: %dx%dx%d texture + color volume", size.x, size.y, size.z);
    return true;
  }

  void renderSdfRaymarchForEye(int eye) {
    if (sdfProgram_ == 0 || sdfVao_ == 0 || sdfTexture_ == 0) {
      return;
    }

    const XrPosef& pose = views_[eye].pose;
    const Mat4 poseMatrix = makePoseMatrix(pose);
    std::array<float, 9> viewRotation{};
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 3; ++row) {
        viewRotation[static_cast<std::size_t>(col * 3 + row)] = poseMatrix.at(row, col);
      }
    }

    const large::sdf::IVec3 size = volume_.size();
    const large::sdf::Vec3 origin = volume_.origin();
    const large::sdf::Vec3 extent{
        static_cast<float>(size.x) * volume_.voxelSize(),
        static_cast<float>(size.y) * volume_.voxelSize(),
        static_cast<float>(size.z) * volume_.voxelSize(),
    };

    glUseProgram(sdfProgram_);
    glUniform3f(sdfCameraLocation_, pose.position.x, pose.position.y, pose.position.z);
    glUniformMatrix3fv(sdfViewRotationLocation_, 1, GL_FALSE, viewRotation.data());
    glUniform4f(sdfFovTangentsLocation_,
                std::tan(views_[eye].fov.angleLeft),
                std::tan(views_[eye].fov.angleRight),
                std::tan(views_[eye].fov.angleDown),
                std::tan(views_[eye].fov.angleUp));
    glUniform3f(sdfVolumeMinLocation_, origin.x, origin.y, origin.z);
    glUniform3f(sdfVolumeExtentLocation_, extent.x, extent.y, extent.z);
    const large::sdf::Vec3 activeMin = renderBoundsMin();
    const large::sdf::Vec3 activeExtent = renderBoundsExtent();
    glUniform3f(sdfRenderMinLocation_, activeMin.x, activeMin.y, activeMin.z);
    glUniform3f(sdfRenderExtentLocation_, activeExtent.x, activeExtent.y, activeExtent.z);
    glUniform3f(sdfObjectPosLocation_, objectPosition_.x, objectPosition_.y, objectPosition_.z);
    const std::array<float, 9> objectRotationMatrix = makeRotationMatrix3(objectRotation_);
    const std::array<float, 9> objectInvRotationMatrix = transposeMatrix3(objectRotationMatrix);
    glUniformMatrix3fv(sdfObjectRotationLocation_, 1, GL_FALSE, objectRotationMatrix.data());
    glUniformMatrix3fv(sdfObjectInvRotationLocation_, 1, GL_FALSE, objectInvRotationMatrix.data());
    glUniform1f(sdfObjectScaleLocation_, objectScale_);
    glUniform1f(sdfArEnabledLocation_, arModeEnabled_ && passthroughReady_ ? 1.0f : 0.0f);
    glUniform3f(sdfWorldOffsetLocation_, worldOffset_.x, worldOffset_.y, worldOffset_.z);
    glUniform1f(sdfVoxelSizeLocation_, volume_.voxelSize());

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, colorTexture_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, sdfTexture_);
    glBindVertexArray(sdfVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
  }

  bool initializeUiOverlayRenderer() {
    constexpr const char* vertexShader = R"(#version 300 es
out vec2 vUv;
void main() {
  vec2 p;
  if (gl_VertexID == 0) {
    p = vec2(-1.0, -1.0);
  } else if (gl_VertexID == 1) {
    p = vec2(3.0, -1.0);
  } else {
    p = vec2(-1.0, 3.0);
  }
  vUv = p * 0.5 + 0.5;
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

    constexpr const char* fragmentShader = R"(#version 300 es
precision highp float;
precision highp int;

in vec2 vUv;
out vec4 oColor;

uniform vec3 uCameraPos;
uniform mat3 uViewRotation;
uniform vec4 uFovTangents;
uniform float uBrushRadius;
uniform float uTriggerValue;
uniform int uToolIndex;
uniform vec3 uLeftUiPosition;
uniform float uLeftUiVisible;
uniform vec3 uRightToolPosition;
uniform vec3 uRightToolDirection;
uniform float uRightToolVisible;
uniform float uMenuPointerActive;
uniform vec3 uMenuPointerStart;
uniform vec3 uMenuPointerEnd;
uniform float uLeftHandVisible;
uniform float uRightHandVisible;
uniform vec3 uLeftHandJoints[26];
uniform vec3 uRightHandJoints[26];
uniform sampler2D uHud;
uniform vec2 uHudVisibleSize;  // panel pixels currently shown (header or full menu)
uniform vec2 uHudFullSize;     // full HUD texture size in pixels
uniform float uPanelWidth;     // panel width in meters
uniform vec3 uPaintColor;
uniform vec3 uFlattenPlaneCenter;
uniform vec3 uFlattenPlaneNormal;
uniform float uFlattenPlaneRadius;
uniform float uFlattenPlaneVisible;
uniform vec3 uMirrorPlaneCenter;
uniform vec3 uMirrorPlaneNormal;
uniform float uMirrorPlaneRadius;
uniform float uMirrorPlaneVisible;

void paint(inout vec3 color, inout float alpha, vec3 source, float sourceAlpha) {
  float a = clamp(sourceAlpha, 0.0, 1.0);
  float outAlpha = alpha + a * (1.0 - alpha);
  if (outAlpha > 0.0001) {
    color = (color * alpha * (1.0 - a) + source * a) / outAlpha;
  }
  alpha = outAlpha;
}

vec3 toolPalette(int index) {
  if (index == 1) {
    return vec3(1.0, 0.28, 0.32);
  }
  if (index == 2) {
    return vec3(0.28, 0.95, 0.48);
  }
  if (index == 3) {
    return vec3(0.82, 0.44, 1.0);
  }
  if (index == 4) {
    return vec3(1.0, 0.85, 0.25);
  }
  if (index == 5) {
    return vec3(1.0, 0.55, 0.20);
  }
  if (index == 6) {
    return vec3(0.55, 0.78, 0.95);
  }
  if (index == 7) {
    return uPaintColor;
  }
  return vec3(0.25, 0.85, 1.0);
}

vec3 brushUiColor() {
  vec3 idleColor = toolPalette(uToolIndex);
  vec3 activeColor = mix(idleColor, vec3(1.0), 0.35);
  return mix(idleColor, activeColor, smoothstep(0.50, 0.80, uTriggerValue));
}

float rayPointDistance(vec3 rayOrigin, vec3 rayDir, vec3 point, out float rayT) {
  rayT = max(dot(point - rayOrigin, rayDir), 0.0);
  return length(rayOrigin + rayDir * rayT - point);
}

float raySegmentDistance(vec3 rayOrigin, vec3 rayDir, vec3 a, vec3 b, out float rayT) {
  vec3 segment = b - a;
  float segLen2 = max(dot(segment, segment), 0.00001);
  float raySeg = dot(rayDir, segment);
  vec3 originToA = rayOrigin - a;
  float originRay = dot(originToA, rayDir);
  float originSeg = dot(originToA, segment);
  float denom = max(segLen2 - raySeg * raySeg, 0.00001);
  float unclampedT = (raySeg * originSeg - originRay * segLen2) / denom;
  float segT = clamp((originSeg + raySeg * unclampedT) / segLen2, 0.0, 1.0);
  vec3 pointOnSegment = a + segment * segT;
  rayT = max(dot(pointOnSegment - rayOrigin, rayDir), 0.0);
  return length(rayOrigin + rayDir * rayT - pointOnSegment);
}

void applyMenuPointerLine(inout vec3 color, inout float alpha, vec3 rayOrigin, vec3 rayDir) {
  if (uMenuPointerActive < 0.5) {
    return;
  }
  float pointerT = 0.0;
  float distance = raySegmentDistance(rayOrigin, rayDir, uMenuPointerStart, uMenuPointerEnd, pointerT);
  float core = 1.0 - smoothstep(0.006, 0.014, distance);
  float glow = 1.0 - smoothstep(0.014, 0.035, distance);
  vec3 pointerColor = mix(vec3(0.65, 0.88, 1.0), brushUiColor(), 0.35);
  paint(color, alpha, pointerColor, clamp(core * 0.95 + glow * 0.32, 0.0, 0.95));
}

void applyLeftHandUi(inout vec3 color, inout float alpha, vec3 rayOrigin, vec3 rayDir) {
  if (uLeftUiVisible < 0.5) {
    return;
  }

  vec3 center = uLeftUiPosition;
  vec3 panelForward = normalize(rayOrigin - center);
  vec3 panelRight = cross(vec3(0.0, 1.0, 0.0), panelForward);
  if (dot(panelRight, panelRight) < 0.05) {
    panelRight = vec3(1.0, 0.0, 0.0);
  } else {
    panelRight = normalize(panelRight);
  }
  vec3 panelUp = normalize(cross(panelForward, panelRight));
  float denom = dot(rayDir, panelForward);
  if (abs(denom) < 0.001) {
    return;
  }

  float t = dot(center - rayOrigin, panelForward) / denom;
  if (t <= 0.0) {
    return;
  }

  vec3 hit = rayOrigin + rayDir * t;
  vec2 local = vec2(dot(hit - center, panelRight), dot(hit - center, panelUp));
  float contentWidth = uHudVisibleSize.x;
  float contentHeight = uHudVisibleSize.y;
  float panelWidth = uPanelWidth;
  float panelHeight = panelWidth * contentHeight / contentWidth;
  if (abs(local.x) > panelWidth * 0.5 || abs(local.y) > panelHeight * 0.5) {
    return;
  }

  vec2 frag = vec2((local.x / panelWidth + 0.5) * contentWidth,
                   (0.5 - local.y / panelHeight) * contentHeight);
  vec4 hud = texture(uHud, frag / uHudFullSize);
  paint(color, alpha, hud.rgb, hud.a);
}

// Transparent disc on an arbitrary plane: used to preview the Flatten
// tangent plane and to show the mirror symmetry plane.
void applyPlaneDisc(inout vec3 color,
                    inout float alpha,
                    vec3 rayOrigin,
                    vec3 rayDir,
                    vec3 center,
                    vec3 planeNormal,
                    float radius,
                    vec3 tint,
                    float fillStrength,
                    float rimStrength) {
  vec3 n = normalize(planeNormal);
  float denom = dot(rayDir, n);
  if (abs(denom) < 0.0005) {
    return;
  }

  float t = dot(center - rayOrigin, n) / denom;
  if (t <= 0.02) {
    return;
  }

  vec3 hit = rayOrigin + rayDir * t;
  float d = length(hit - center);
  float r = max(radius, 0.01);
  if (d > r * 1.10) {
    return;
  }

  float fill = (1.0 - smoothstep(r * 0.92, r, d)) * fillStrength;
  float rim = (1.0 - smoothstep(0.0035, 0.011, abs(d - r))) * rimStrength;
  paint(color, alpha, tint, clamp(fill, 0.0, 1.0));
  paint(color, alpha, tint, rim);
}

float sdSphere(vec3 p, float r) {
  return length(p) - r;
}

float sdCapsuleSeg(vec3 p, vec3 a, vec3 b, float r) {
  vec3 pa = p - a;
  vec3 ba = b - a;
  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h) - r;
}

float sdBox(vec3 p, vec3 b) {
  vec3 q = abs(p) - b;
  return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// Capped cone along +y, from y=-h (radius r1) to y=+h (radius r2).
float sdCappedConeY(vec3 p, float h, float r1, float r2) {
  vec2 q = vec2(length(p.xz), p.y);
  vec2 k1 = vec2(r2, h);
  vec2 k2 = vec2(r2 - r1, 2.0 * h);
  vec2 ca = vec2(q.x - min(q.x, (q.y < 0.0) ? r1 : r2), abs(q.y) - h);
  vec2 cb = q - k1 + k2 * clamp(dot(k1 - q, k2) / dot(k2, k2), 0.0, 1.0);
  float s = (cb.x < 0.0 && ca.y < 0.0) ? -1.0 : 1.0;
  return s * sqrt(min(dot(ca, ca), dot(cb, cb)));
}

// Ring around the local x axis (lies in the y-z plane).
float sdTorusX(vec3 p, float ringRadius, float tubeRadius) {
  vec2 q = vec2(length(p.yz) - ringRadius, p.x);
  return length(q) - tubeRadius;
}

// Tool-specific head shape in the gizmo frame (x=right, y=up, z=forward,
// origin at the tool tip). secondary marks neutral metal parts.
float toolHeadSdf(vec3 q, float s, out float secondary) {
  secondary = 0.0;
  if (uToolIndex == 1) {
    // Subtract: hollow scoop opening toward the surface.
    return max(abs(length(q) - s) - 0.0035, q.z - s * 0.25);
  }
  if (uToolIndex == 2) {
    // Smooth: flattened pebble.
    vec3 radii = vec3(s * 1.15, s * 1.15, s * 0.42);
    return (length(q / radii) - 1.0) * (s * 0.42);
  }
  if (uToolIndex == 3) {
    // Stretch: pull hook (open torus arc).
    float d = sdTorusX(q - vec3(0.0, s * 0.35, 0.0), s * 0.75, s * 0.16);
    return max(d, -(q.y + s * 0.25));
  }
  if (uToolIndex == 4) {
    // Flatten: rigid square plate.
    return sdBox(q, vec3(s * 1.45, s * 1.45, 0.0045));
  }
  if (uToolIndex == 5) {
    // Groove: chisel point.
    return sdCappedConeY(q.xzy, s, s * 0.55, 0.002);
  }
  if (uToolIndex == 6) {
    // Crease: two pincer branches converging at the tip.
    float a = sdCapsuleSeg(q, vec3(-s * 0.75, 0.0, -s * 0.9), vec3(0.0, 0.0, s * 0.55), s * 0.17);
    float b = sdCapsuleSeg(q, vec3(s * 0.75, 0.0, -s * 0.9), vec3(0.0, 0.0, s * 0.55), s * 0.17);
    return min(a, b);
  }
  if (uToolIndex == 7) {
    // Paint: brush tuft (selected color) over a metal ferrule.
    float tuft = sdCappedConeY(vec3(q.x, q.z - s * 0.45, q.y), s * 0.75, s * 0.5, 0.003);
    float ferrule = sdCapsuleSeg(q, vec3(0.0, 0.0, -s * 0.75), vec3(0.0, 0.0, -s * 0.30), s * 0.42);
    secondary = ferrule < tuft ? 1.0 : 0.0;
    return min(tuft, ferrule);
  }
  // Add: solid ball of clay.
  return sdSphere(q, s);
}

float toolGizmoSdf(vec3 q, float s, out int material) {
  float secondary = 0.0;
  float head = toolHeadSdf(q, s, secondary);
  float handle = sdCapsuleSeg(q, vec3(0.0, 0.0, -0.19), vec3(0.0, 0.0, -0.05), 0.008);
  if (handle < head) {
    material = 2;
    return handle;
  }
  material = secondary > 0.5 ? 1 : 0;
  return head;
}

void applyRightHandTool(inout vec3 color, inout float alpha, vec3 rayOrigin, vec3 rayDir) {
  if (uRightToolVisible < 0.5 || uMenuPointerActive > 0.5) {
    return;
  }

  vec3 f = normalize(uRightToolDirection);
  vec3 r = cross(vec3(0.0, 1.0, 0.0), f);
  if (dot(r, r) < 0.01) {
    r = cross(vec3(1.0, 0.0, 0.0), f);
  }
  r = normalize(r);
  vec3 u = cross(f, r);
  float s = clamp(uBrushRadius * 0.30, 0.016, 0.045);

  // Faint ring showing the actual brush radius around the tip.
  float ringT = 0.0;
  float ringDistance = rayPointDistance(rayOrigin, rayDir, uRightToolPosition, ringT);
  float ringRadius = max(uBrushRadius, 0.010);
  float ringMask = 1.0 - smoothstep(0.0035, 0.010, abs(ringDistance - ringRadius));
  paint(color, alpha, brushUiColor(), ringMask * 0.30);

  // Sphere-trace the gizmo inside its bounding sphere only.
  vec3 boundCenter = uRightToolPosition - f * 0.07;
  float boundRadius = 0.155 + s * 2.2;
  vec3 oc = rayOrigin - boundCenter;
  float ob = dot(oc, rayDir);
  float oc2 = dot(oc, oc) - boundRadius * boundRadius;
  float disc = ob * ob - oc2;
  if (disc < 0.0) {
    return;
  }
  float sqrtDisc = sqrt(disc);
  float t = max(-ob - sqrtDisc, 0.0);
  float tEnd = -ob + sqrtDisc;

  for (int i = 0; i < 28; ++i) {
    vec3 p = rayOrigin + rayDir * t;
    vec3 rel = p - uRightToolPosition;
    vec3 q = vec3(dot(rel, r), dot(rel, u), dot(rel, f));
    int material = 0;
    float d = toolGizmoSdf(q, s, material);
    if (d < 0.0012) {
      int dummy = 0;
      vec2 e = vec2(0.0015, 0.0);
      vec3 nLocal = normalize(vec3(
          toolGizmoSdf(q + e.xyy, s, dummy) - toolGizmoSdf(q - e.xyy, s, dummy),
          toolGizmoSdf(q + e.yxy, s, dummy) - toolGizmoSdf(q - e.yxy, s, dummy),
          toolGizmoSdf(q + e.yyx, s, dummy) - toolGizmoSdf(q - e.yyx, s, dummy)));
      vec3 n = normalize(r * nLocal.x + u * nLocal.y + f * nLocal.z);
      vec3 light = normalize(vec3(-0.35, 0.85, 0.42));
      float diffuse = max(dot(n, light), 0.0);
      vec3 baseColor = material == 2 ? vec3(0.55, 0.59, 0.65)
                                     : (material == 1 ? vec3(0.75, 0.78, 0.82) : brushUiColor());
      paint(color, alpha, baseColor * (0.40 + diffuse * 0.60), 0.96);
      return;
    }
    t += max(d, 0.0008);
    if (t > tEnd) {
      return;
    }
  }
}

vec3 handJoint(int hand, int index) {
  return hand == 0 ? uLeftHandJoints[index] : uRightHandJoints[index];
}

void applyHandJoint(inout vec3 color,
                    inout float alpha,
                    vec3 rayOrigin,
                    vec3 rayDir,
                    vec3 point,
                    vec3 jointColor,
                    float radius,
                    float opacity) {
  float rayT = 0.0;
  float distance = rayPointDistance(rayOrigin, rayDir, point, rayT);
  float core = 1.0 - smoothstep(radius, radius * 1.75, distance);
  paint(color, alpha, jointColor, core * opacity);
}

void applyHandBone(inout vec3 color,
                   inout float alpha,
                   vec3 rayOrigin,
                   vec3 rayDir,
                   vec3 a,
                   vec3 b,
                   vec3 boneColor,
                   float radius,
                   float opacity) {
  float rayT = 0.0;
  float distance = raySegmentDistance(rayOrigin, rayDir, a, b, rayT);
  float core = 1.0 - smoothstep(radius, radius * 1.75, distance);
  paint(color, alpha, boneColor, core * opacity);
}

void applyHandBoneByIndex(inout vec3 color,
                          inout float alpha,
                          vec3 rayOrigin,
                          vec3 rayDir,
                          int hand,
                          int a,
                          int b,
                          vec3 boneColor) {
  applyHandBone(color, alpha, rayOrigin, rayDir, handJoint(hand, a), handJoint(hand, b), boneColor, 0.012, 0.30);
}

void applyTrackedHand(inout vec3 color, inout float alpha, vec3 rayOrigin, vec3 rayDir, int hand) {
  float visible = hand == 0 ? uLeftHandVisible : uRightHandVisible;
  if (visible < 0.5) {
    return;
  }

  // Classic neutral hands, like the system ones: pale translucent capsules,
  // identical for both hands; the active tool no longer recolors the hand.
  vec3 baseColor = vec3(0.80, 0.83, 0.87);
  vec3 jointColor = baseColor;
  vec3 palmColor = baseColor;

  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 1, 0, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 0, 2, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 0, 6, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 0, 11, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 0, 16, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 0, 21, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 6, 11, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 11, 16, palmColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 16, 21, palmColor);

  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 2, 3, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 3, 4, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 4, 5, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 6, 7, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 7, 8, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 8, 9, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 9, 10, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 11, 12, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 12, 13, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 13, 14, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 14, 15, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 16, 17, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 17, 18, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 18, 19, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 19, 20, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 21, 22, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 22, 23, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 23, 24, baseColor);
  applyHandBoneByIndex(color, alpha, rayOrigin, rayDir, hand, 24, 25, baseColor);

  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 0), palmColor, 0.026, 0.22);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 5), jointColor, 0.012, 0.26);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 10), jointColor, 0.012, 0.26);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 15), jointColor, 0.012, 0.26);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 20), jointColor, 0.012, 0.26);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 25), jointColor, 0.012, 0.26);

  // Small tool-color tip on the right index: it is the finger that sculpts
  // (pinch) and presses the menu, so it keeps a discreet accent.
  if (hand == 1) {
    applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(1, 10), toolPalette(uToolIndex), 0.007, 0.55);
  }
}

void main() {
  float x = mix(uFovTangents.x, uFovTangents.y, vUv.x);
  float y = mix(uFovTangents.z, uFovTangents.w, vUv.y);
  vec3 rayDir = normalize(uViewRotation * normalize(vec3(x, y, -1.0)));
  vec3 rayOrigin = uCameraPos;
  vec3 color = vec3(0.0);
  float alpha = 0.0;
  applyMenuPointerLine(color, alpha, rayOrigin, rayDir);
  if (uMirrorPlaneVisible > 0.5) {
    applyPlaneDisc(color, alpha, rayOrigin, rayDir, uMirrorPlaneCenter, uMirrorPlaneNormal, uMirrorPlaneRadius,
                   vec3(0.45, 0.75, 1.0), 0.10, 0.45);
  }
  if (uFlattenPlaneVisible > 0.5) {
    applyPlaneDisc(color, alpha, rayOrigin, rayDir, uFlattenPlaneCenter, uFlattenPlaneNormal, uFlattenPlaneRadius,
                   vec3(1.0, 0.85, 0.25), 0.15, 0.70);
  }
  applyLeftHandUi(color, alpha, rayOrigin, rayDir);
  applyRightHandTool(color, alpha, rayOrigin, rayDir);
  applyTrackedHand(color, alpha, rayOrigin, rayDir, 0);
  applyTrackedHand(color, alpha, rayOrigin, rayDir, 1);
  oColor = vec4(color, alpha);
}
)";

    uiProgram_ = linkProgram(vertexShader, fragmentShader);
    if (uiProgram_ == 0) {
      return false;
    }

    uiCameraLocation_ = glGetUniformLocation(uiProgram_, "uCameraPos");
    uiViewRotationLocation_ = glGetUniformLocation(uiProgram_, "uViewRotation");
    uiFovTangentsLocation_ = glGetUniformLocation(uiProgram_, "uFovTangents");
    uiBrushRadiusLocation_ = glGetUniformLocation(uiProgram_, "uBrushRadius");
    uiTriggerValueLocation_ = glGetUniformLocation(uiProgram_, "uTriggerValue");
    uiToolIndexLocation_ = glGetUniformLocation(uiProgram_, "uToolIndex");
    uiLeftUiPositionLocation_ = glGetUniformLocation(uiProgram_, "uLeftUiPosition");
    uiLeftUiVisibleLocation_ = glGetUniformLocation(uiProgram_, "uLeftUiVisible");
    uiRightToolPositionLocation_ = glGetUniformLocation(uiProgram_, "uRightToolPosition");
    uiRightToolDirectionLocation_ = glGetUniformLocation(uiProgram_, "uRightToolDirection");
    uiRightToolVisibleLocation_ = glGetUniformLocation(uiProgram_, "uRightToolVisible");
    uiMenuPointerActiveLocation_ = glGetUniformLocation(uiProgram_, "uMenuPointerActive");
    uiMenuPointerStartLocation_ = glGetUniformLocation(uiProgram_, "uMenuPointerStart");
    uiMenuPointerEndLocation_ = glGetUniformLocation(uiProgram_, "uMenuPointerEnd");
    uiLeftHandVisibleLocation_ = glGetUniformLocation(uiProgram_, "uLeftHandVisible");
    uiRightHandVisibleLocation_ = glGetUniformLocation(uiProgram_, "uRightHandVisible");
    uiLeftHandJointsLocation_ = glGetUniformLocation(uiProgram_, "uLeftHandJoints[0]");
    uiRightHandJointsLocation_ = glGetUniformLocation(uiProgram_, "uRightHandJoints[0]");
    uiHudVisibleSizeLocation_ = glGetUniformLocation(uiProgram_, "uHudVisibleSize");
    uiHudFullSizeLocation_ = glGetUniformLocation(uiProgram_, "uHudFullSize");
    uiPanelWidthLocation_ = glGetUniformLocation(uiProgram_, "uPanelWidth");
    uiPaintColorLocation_ = glGetUniformLocation(uiProgram_, "uPaintColor");
    uiFlattenCenterLocation_ = glGetUniformLocation(uiProgram_, "uFlattenPlaneCenter");
    uiFlattenNormalLocation_ = glGetUniformLocation(uiProgram_, "uFlattenPlaneNormal");
    uiFlattenRadiusLocation_ = glGetUniformLocation(uiProgram_, "uFlattenPlaneRadius");
    uiFlattenVisibleLocation_ = glGetUniformLocation(uiProgram_, "uFlattenPlaneVisible");
    uiMirrorCenterLocation_ = glGetUniformLocation(uiProgram_, "uMirrorPlaneCenter");
    uiMirrorNormalLocation_ = glGetUniformLocation(uiProgram_, "uMirrorPlaneNormal");
    uiMirrorRadiusLocation_ = glGetUniformLocation(uiProgram_, "uMirrorPlaneRadius");
    uiMirrorVisibleLocation_ = glGetUniformLocation(uiProgram_, "uMirrorPlaneVisible");
    const GLint hudSamplerLocation = glGetUniformLocation(uiProgram_, "uHud");

    if (uiCameraLocation_ < 0 || uiViewRotationLocation_ < 0 || uiFovTangentsLocation_ < 0 ||
        uiBrushRadiusLocation_ < 0 || uiTriggerValueLocation_ < 0 ||
        uiToolIndexLocation_ < 0 || uiLeftUiPositionLocation_ < 0 || uiLeftUiVisibleLocation_ < 0 ||
        uiRightToolPositionLocation_ < 0 || uiRightToolDirectionLocation_ < 0 ||
        uiRightToolVisibleLocation_ < 0 || uiMenuPointerActiveLocation_ < 0 ||
        uiMenuPointerStartLocation_ < 0 || uiMenuPointerEndLocation_ < 0 || uiLeftHandVisibleLocation_ < 0 ||
        uiRightHandVisibleLocation_ < 0 || uiLeftHandJointsLocation_ < 0 || uiRightHandJointsLocation_ < 0 ||
        uiHudVisibleSizeLocation_ < 0 || uiHudFullSizeLocation_ < 0 || uiPanelWidthLocation_ < 0 ||
        uiPaintColorLocation_ < 0 || uiFlattenCenterLocation_ < 0 || uiFlattenNormalLocation_ < 0 ||
        uiFlattenRadiusLocation_ < 0 || uiFlattenVisibleLocation_ < 0 || uiMirrorCenterLocation_ < 0 ||
        uiMirrorNormalLocation_ < 0 || uiMirrorRadiusLocation_ < 0 || uiMirrorVisibleLocation_ < 0 ||
        hudSamplerLocation < 0) {
      logError("UI overlay shader uniforms are missing");
      return false;
    }

    glGenTextures(1, &hudTexture_);
    glBindTexture(GL_TEXTURE_2D, hudTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 large::hud::kContentWidth,
                 large::hud::kContentHeight,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    const GLenum hudTextureError = glGetError();
    if (hudTextureError != GL_NO_ERROR) {
      logError("HUD texture allocation failed: 0x%x", hudTextureError);
      return false;
    }
    hudPainted_ = false;

    glUseProgram(uiProgram_);
    glUniform1i(hudSamplerLocation, 0);
    glUseProgram(0);

    glGenVertexArrays(1, &uiVao_);
    logInfo("UI overlay renderer ready: HUD texture %dx%d",
            large::hud::kContentWidth,
            large::hud::kContentHeight);
    return true;
  }

  struct HudSnapshot {
    int toolIndex = -1;
    float radius = 0.0f;
    float strength = 0.0f;
    bool menuVisible = false;
    int hoverIndex = -1;
    bool arEnabled = false;
    bool arAvailable = false;
    bool mirrorEnabled = false;
    bool lockEnabled = false;
    int paintColor = 0;
    bool triggerPressed = false;

    bool operator==(const HudSnapshot& other) const {
      return toolIndex == other.toolIndex && radius == other.radius && strength == other.strength &&
             menuVisible == other.menuVisible && hoverIndex == other.hoverIndex && arEnabled == other.arEnabled &&
             arAvailable == other.arAvailable && mirrorEnabled == other.mirrorEnabled &&
             lockEnabled == other.lockEnabled && paintColor == other.paintColor &&
             triggerPressed == other.triggerPressed;
    }
  };

  large::hud::Color toolUiColor(int index) const {
    switch (index) {
      case 1:
        return {255, 71, 82, 255};
      case 2:
        return {71, 242, 122, 255};
      case 3:
        return {209, 112, 255, 255};
      case 4:
        return {255, 217, 64, 255};
      case 5:
        return {255, 140, 51, 255};
      case 6:
        return {140, 199, 242, 255};
      case 7: {
        const auto& paint = kPaintPalette[static_cast<std::size_t>(paintColorIndex_)];
        return {static_cast<std::uint8_t>(paint[0] * 255.0f + 0.5f),
                static_cast<std::uint8_t>(paint[1] * 255.0f + 0.5f),
                static_cast<std::uint8_t>(paint[2] * 255.0f + 0.5f),
                255};
      }
      default:
        return {64, 217, 255, 255};
    }
  }

  // Small silhouette of each tool, matching its 3D gizmo: drawn in the menu
  // rows and in the header bar so tools are identified by shape, not color.
  void drawToolIcon(int tool, int centerX, int centerY, large::hud::Color color, large::hud::Color accent) {
    large::hud::Painter& p = hudPainter_;
    switch (tool) {
      case 1:  // Subtract: hollow scoop
        p.ring(centerX, centerY, 7, 3, color);
        break;
      case 2:  // Smooth: flat pebble
        p.fillRect(centerX - 6, centerY - 3, centerX + 6, centerY + 3, color);
        p.fillCircle(centerX - 6, centerY, 3, color);
        p.fillCircle(centerX + 6, centerY, 3, color);
        break;
      case 3:  // Stretch: pull arrow
        p.fillTriangle(centerX - 6, centerY - 1, centerX + 6, centerY - 1, centerX, centerY - 9, color);
        p.fillRect(centerX - 2, centerY - 1, centerX + 2, centerY + 9, color);
        break;
      case 4:  // Flatten: plate on a stem
        p.fillRect(centerX - 8, centerY - 7, centerX + 8, centerY - 3, color);
        p.fillRect(centerX - 2, centerY - 3, centerX + 2, centerY + 8, color);
        break;
      case 5:  // Groove: chisel point
        p.fillTriangle(centerX - 7, centerY - 7, centerX + 7, centerY - 7, centerX, centerY + 9, color);
        break;
      case 6:  // Crease: converging pincer
        p.fillTriangle(centerX - 8, centerY + 8, centerX - 4, centerY + 8, centerX, centerY - 8, color);
        p.fillTriangle(centerX + 4, centerY + 8, centerX + 8, centerY + 8, centerX, centerY - 8, color);
        break;
      case 7:  // Paint: brush with the selected color as tuft
        p.fillTriangle(centerX - 5, centerY, centerX + 5, centerY, centerX, centerY - 9, accent);
        p.fillRect(centerX - 5, centerY, centerX + 5, centerY + 4, color);
        p.fillRect(centerX - 2, centerY + 4, centerX + 2, centerY + 9, color);
        break;
      default:  // Add: solid ball of clay
        p.fillCircle(centerX, centerY, 7, color);
        break;
    }
  }

  void paintHud(const HudSnapshot& snap) {
    namespace hud = large::hud;
    hud::Painter& p = hudPainter_;
    p.clear();

    const hud::Color panelBg{5, 6, 7, 205};
    const hud::Color panelEdge{77, 92, 102, 185};
    const hud::Color barBg{20, 26, 31, 235};
    const hud::Color textMain{235, 242, 248, 255};
    const hud::Color textDim{150, 162, 172, 255};
    const hud::Color textDark{12, 14, 16, 255};

    // Header: active tool, brush size and strength, shortcut reminders.
    p.fillRect(hud::kHeaderLeft, hud::kHeaderTop, hud::kHeaderRight, hud::kHeaderBottom, panelBg);
    p.outlineRect(hud::kHeaderLeft, hud::kHeaderTop, hud::kHeaderRight, hud::kHeaderBottom, 2, panelEdge);

    hud::Color toolColor = toolUiColor(snap.toolIndex);
    toolColor.a = snap.triggerPressed ? 255 : 210;
    p.fillRect(hud::kToolBarLeft, hud::kToolBarTop, hud::kToolBarRight, hud::kToolBarBottom, toolColor);
    char nameBuffer[16] = {};
    const char* name = toolName(toolFromIndex(snap.toolIndex));
    for (int i = 0; name[i] != '\0' && i < 15; ++i) {
      nameBuffer[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[i])));
    }
    const int toolBarCenterY = (hud::kToolBarTop + hud::kToolBarBottom) / 2;
    drawToolIcon(snap.toolIndex, hud::kToolBarLeft + 16, toolBarCenterY, textDark, textDark);
    p.drawTextCentered((hud::kToolBarLeft + hud::kToolBarRight) / 2 + 10, hud::kToolBarTop + 3, nameBuffer, 2,
                       textDark);

    // MENU toggle button (finger poke or controller ray).
    const bool menuButtonHovered = snap.hoverIndex == kMenuToggleChoice;
    hud::Color menuButtonColor = snap.menuVisible ? hud::Color{61, 179, 148, 235} : hud::Color{66, 76, 86, 220};
    p.fillRect(hud::kMenuButtonLeft, hud::kMenuButtonTop, hud::kMenuButtonRight, hud::kMenuButtonBottom,
               menuButtonColor);
    if (menuButtonHovered) {
      p.fillRect(hud::kMenuButtonLeft, hud::kMenuButtonTop, hud::kMenuButtonRight, hud::kMenuButtonBottom,
                 {200, 230, 245, 95});
    }
    p.outlineRect(hud::kMenuButtonLeft, hud::kMenuButtonTop, hud::kMenuButtonRight, hud::kMenuButtonBottom, 2,
                  snap.menuVisible ? textMain : panelEdge);
    p.drawTextCentered((hud::kMenuButtonLeft + hud::kMenuButtonRight) / 2,
                       (hud::kMenuButtonTop + hud::kMenuButtonBottom) / 2 - 3, "MENU", 1, textMain);

    char valueBuffer[16] = {};
    p.drawText(hud::kToolBarLeft, hud::kSizeRowY, "SIZE", 1, textDim);
    p.fillRect(hud::kBarLeft, hud::kSizeRowY - 1, hud::kBarRight, hud::kSizeRowY - 1 + hud::kBarHeight, barBg);
    const float sizeNorm = large::sdf::clamp(
        (snap.radius - kMinimumBrushRadius) / (kMaximumBrushRadius - kMinimumBrushRadius), 0.0f, 1.0f);
    p.fillRect(hud::kBarLeft,
               hud::kSizeRowY - 1,
               hud::kBarLeft + static_cast<int>((hud::kBarRight - hud::kBarLeft) * sizeNorm),
               hud::kSizeRowY - 1 + hud::kBarHeight,
               {64, 217, 255, 255});
    std::snprintf(valueBuffer, sizeof(valueBuffer), "%.0fCM", snap.radius * 100.0f);
    p.drawText(hud::kBarRight + 6, hud::kSizeRowY, valueBuffer, 1, textMain);

    p.drawText(hud::kToolBarLeft, hud::kPowerRowY, "POWER", 1, textDim);
    p.fillRect(hud::kBarLeft, hud::kPowerRowY - 1, hud::kBarRight, hud::kPowerRowY - 1 + hud::kBarHeight, barBg);
    const float powerNorm = large::sdf::clamp(snap.strength, 0.0f, 1.0f);
    p.fillRect(hud::kBarLeft,
               hud::kPowerRowY - 1,
               hud::kBarLeft + static_cast<int>((hud::kBarRight - hud::kBarLeft) * powerNorm),
               hud::kPowerRowY - 1 + hud::kBarHeight,
               {255, 153, 61, 255});
    std::snprintf(valueBuffer, sizeof(valueBuffer), "%.0f%%", powerNorm * 100.0f);
    p.drawText(hud::kBarRight + 6, hud::kPowerRowY, valueBuffer, 1, textMain);

    p.drawText(hud::kToolBarLeft, hud::kFooterY, "X:UNDO Y:REDO A/B:TOOL", 1, textDim);

    if (!snap.menuVisible) {
      return;
    }

    // Menu: tools column on the left, action column on the right, palette
    // full width below.
    p.fillRect(hud::kHeaderLeft, hud::kMenuPanelTop, hud::kHeaderRight, hud::kMenuPanelBottom, panelBg);
    p.outlineRect(hud::kHeaderLeft, hud::kMenuPanelTop, hud::kHeaderRight, hud::kMenuPanelBottom, 2, panelEdge);

    static const char* kToolLabels[hud::kMenuToolRowCount] = {
        "ADD", "SUBTRACT", "SMOOTH", "STRETCH", "FLATTEN", "GROOVE", "CREASE", "PAINT",
    };
    const auto& selectedPaint = kPaintPalette[static_cast<std::size_t>(snap.paintColor)];
    const hud::Color paintAccent{static_cast<std::uint8_t>(selectedPaint[0] * 255.0f + 0.5f),
                                 static_cast<std::uint8_t>(selectedPaint[1] * 255.0f + 0.5f),
                                 static_cast<std::uint8_t>(selectedPaint[2] * 255.0f + 0.5f),
                                 255};
    for (int i = 0; i < hud::kMenuToolRowCount; ++i) {
      const int top = hud::kMenuRowTop + i * hud::kMenuRowPitch;
      const bool active = i == snap.toolIndex;
      const bool hovered = i == snap.hoverIndex;

      hud::Color rowColor = toolUiColor(i);
      rowColor.a = active ? 235 : 110;
      p.fillRect(hud::kMenuToolColumnLeft, top, hud::kMenuToolColumnRight, top + hud::kMenuRowHeight, rowColor);
      if (hovered) {
        p.fillRect(hud::kMenuToolColumnLeft, top, hud::kMenuToolColumnRight, top + hud::kMenuRowHeight,
                   {200, 230, 245, 95});
      }
      p.outlineRect(hud::kMenuToolColumnLeft, top, hud::kMenuToolColumnRight, top + hud::kMenuRowHeight, 1,
                    active ? textMain : panelEdge);
      drawToolIcon(i, hud::kMenuToolColumnLeft + hud::kMenuIconCenterOffset, top + hud::kMenuRowHeight / 2,
                   textMain, paintAccent);
      p.drawTextCentered((hud::kMenuToolColumnLeft + 2 * hud::kMenuIconCenterOffset + hud::kMenuToolColumnRight) / 2,
                         top + 5, kToolLabels[i], 2, textMain);
    }

    static const char* kActionLabels[hud::kMenuActionRowCount] = {
        "SAVE", "LOAD", "EXPORT", "QUIT", "AR", "MIRROR", "LOCK",
    };
    for (int i = 0; i < hud::kMenuActionRowCount; ++i) {
      const int top = hud::kMenuRowTop + i * hud::kMenuRowPitch;
      const int choice = kMenuToolCount + i;
      const bool isArRow = choice == kMenuArChoice;
      const bool isMirrorRow = choice == kMenuMirrorChoice;
      const bool isLockRow = choice == kMenuLockChoice;
      const bool toggledOn = (isArRow && snap.arEnabled) || (isMirrorRow && snap.mirrorEnabled) ||
                             (isLockRow && snap.lockEnabled);
      const bool hovered = choice == snap.hoverIndex;

      hud::Color rowColor = toggledOn ? hud::Color{61, 179, 148, 225} : hud::Color{66, 76, 86, 190};
      if (isArRow && !snap.arAvailable) {
        rowColor = {40, 45, 50, 190};
      }
      p.fillRect(hud::kMenuActionColumnLeft, top, hud::kMenuActionColumnRight, top + hud::kMenuRowHeight, rowColor);
      if (hovered) {
        p.fillRect(hud::kMenuActionColumnLeft, top, hud::kMenuActionColumnRight, top + hud::kMenuRowHeight,
                   {200, 230, 245, 95});
      }
      p.outlineRect(hud::kMenuActionColumnLeft, top, hud::kMenuActionColumnRight, top + hud::kMenuRowHeight, 1,
                    toggledOn ? textMain : panelEdge);
      const hud::Color labelColor = (isArRow && !snap.arAvailable) ? textDim : textMain;
      p.drawTextCentered((hud::kMenuActionColumnLeft + hud::kMenuActionColumnRight) / 2, top + 5, kActionLabels[i],
                         2, labelColor);
    }

    p.drawText(hud::kPaletteLeft, hud::kPaletteLabelY, "PAINT COLOR", 1, textDim);
    for (int i = 0; i < hud::kPaletteCount; ++i) {
      const int column = i % hud::kPaletteColumns;
      const int row = i / hud::kPaletteColumns;
      const int x = hud::kPaletteLeft + column * hud::kPalettePitch;
      const int y = hud::kPaletteTop + row * hud::kPalettePitch;
      const auto& paint = kPaintPalette[static_cast<std::size_t>(i)];
      const hud::Color swatch{static_cast<std::uint8_t>(paint[0] * 255.0f + 0.5f),
                              static_cast<std::uint8_t>(paint[1] * 255.0f + 0.5f),
                              static_cast<std::uint8_t>(paint[2] * 255.0f + 0.5f),
                              255};
      p.fillRect(x, y, x + hud::kPaletteSwatch, y + hud::kPaletteSwatch, swatch);
      const bool selected = i == snap.paintColor;
      const bool hovered = snap.hoverIndex == kMenuPaletteFirstChoice + i;
      if (selected || hovered) {
        p.outlineRect(x, y, x + hud::kPaletteSwatch, y + hud::kPaletteSwatch, 3,
                      selected ? textMain : textDim);
      }
    }
  }

  void updateHudTexture() {
    if (hudTexture_ == 0) {
      return;
    }

    HudSnapshot snap{};
    snap.toolIndex = displayToolIndex();
    snap.radius = brushRadius_;
    snap.strength = brushStrength_;
    snap.menuVisible = menuVisible_;
    snap.hoverIndex = menuHoverIndex_;
    snap.arEnabled = arModeEnabled_;
    snap.arAvailable = passthroughReady_;
    snap.mirrorEnabled = mirrorEnabled_;
    snap.lockEnabled = objectLocked_;
    snap.paintColor = paintColorIndex_;
    snap.triggerPressed = rightTriggerValue_ >= kTriggerThreshold;
    if (hudPainted_ && snap == hudSnapshot_) {
      return;
    }

    paintHud(snap);
    glBindTexture(GL_TEXTURE_2D, hudTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    large::hud::kContentWidth,
                    large::hud::kContentHeight,
                    GL_RGBA,
                    GL_UNSIGNED_BYTE,
                    hudPainter_.pixels());
    glBindTexture(GL_TEXTURE_2D, 0);
    hudSnapshot_ = snap;
    hudPainted_ = true;
  }

  void renderUiOverlayForEye(int eye) {
    if (uiProgram_ == 0 || uiVao_ == 0) {
      return;
    }

    const XrPosef& pose = views_[eye].pose;
    const Mat4 poseMatrix = makePoseMatrix(pose);
    std::array<float, 9> viewRotation{};
    for (int col = 0; col < 3; ++col) {
      for (int row = 0; row < 3; ++row) {
        viewRotation[static_cast<std::size_t>(col * 3 + row)] = poseMatrix.at(row, col);
      }
    }

    const large::sdf::Vec3 uiPosition = leftUiPosition();
    const auto buildHandUniform = [](const HandState& hand) {
      std::array<float, XR_HAND_JOINT_COUNT_EXT * 3> data{};
      for (std::size_t i = 0; i < hand.joints.size(); ++i) {
        data[i * 3 + 0] = hand.joints[i].x;
        data[i * 3 + 1] = hand.joints[i].y;
        data[i * 3 + 2] = hand.joints[i].z;
      }
      return data;
    };
    const std::array<float, XR_HAND_JOINT_COUNT_EXT * 3> leftHandJoints = buildHandUniform(leftHandState_);
    const std::array<float, XR_HAND_JOINT_COUNT_EXT * 3> rightHandJoints = buildHandUniform(rightHandState_);
    glEnable(GL_BLEND);
    // Separate alpha blend so the UI also raises the destination alpha,
    // keeping it visible over passthrough in AR mode.
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(uiProgram_);
    glUniform3f(uiCameraLocation_, pose.position.x, pose.position.y, pose.position.z);
    glUniformMatrix3fv(uiViewRotationLocation_, 1, GL_FALSE, viewRotation.data());
    glUniform4f(uiFovTangentsLocation_,
                std::tan(views_[eye].fov.angleLeft),
                std::tan(views_[eye].fov.angleRight),
                std::tan(views_[eye].fov.angleDown),
                std::tan(views_[eye].fov.angleUp));
    glUniform1f(uiBrushRadiusLocation_, brushRadius_);
    glUniform1f(uiTriggerValueLocation_, rightTriggerValue_);
    glUniform1i(uiToolIndexLocation_, displayToolIndex());
    glUniform3f(uiLeftUiPositionLocation_, uiPosition.x, uiPosition.y, uiPosition.z);
    glUniform1f(uiLeftUiVisibleLocation_, isLeftUiVisible() ? 1.0f : 0.0f);
    glUniform3f(uiRightToolPositionLocation_, rightToolPosition_.x, rightToolPosition_.y, rightToolPosition_.z);
    glUniform3f(uiRightToolDirectionLocation_, rightToolDirection_.x, rightToolDirection_.y, rightToolDirection_.z);
    glUniform1f(uiRightToolVisibleLocation_, rightToolVisible_ ? 1.0f : 0.0f);
    glUniform1f(uiMenuPointerActiveLocation_, menuPointerActive_ ? 1.0f : 0.0f);
    glUniform3f(uiMenuPointerStartLocation_, menuPointerStart_.x, menuPointerStart_.y, menuPointerStart_.z);
    glUniform3f(uiMenuPointerEndLocation_, menuPointerEnd_.x, menuPointerEnd_.y, menuPointerEnd_.z);
    glUniform1f(uiLeftHandVisibleLocation_, leftHandState_.active ? 1.0f : 0.0f);
    glUniform1f(uiRightHandVisibleLocation_, rightHandState_.active ? 1.0f : 0.0f);
    glUniform3fv(uiLeftHandJointsLocation_, XR_HAND_JOINT_COUNT_EXT, leftHandJoints.data());
    glUniform3fv(uiRightHandJointsLocation_, XR_HAND_JOINT_COUNT_EXT, rightHandJoints.data());
    glUniform2f(uiHudVisibleSizeLocation_,
                static_cast<float>(large::hud::kContentWidth),
                static_cast<float>(menuVisible_ ? large::hud::kContentHeight : large::hud::kHeaderVisibleHeight));
    glUniform2f(uiHudFullSizeLocation_,
                static_cast<float>(large::hud::kContentWidth),
                static_cast<float>(large::hud::kContentHeight));
    glUniform1f(uiPanelWidthLocation_, large::hud::kPanelWidthMeters);
    const auto& paintColor = kPaintPalette[static_cast<std::size_t>(paintColorIndex_)];
    glUniform3f(uiPaintColorLocation_, paintColor[0], paintColor[1], paintColor[2]);
    glUniform3f(uiFlattenCenterLocation_,
                flattenPreviewCenterWorld_.x,
                flattenPreviewCenterWorld_.y,
                flattenPreviewCenterWorld_.z);
    glUniform3f(uiFlattenNormalLocation_,
                flattenPreviewNormalWorld_.x,
                flattenPreviewNormalWorld_.y,
                flattenPreviewNormalWorld_.z);
    glUniform1f(uiFlattenRadiusLocation_, brushRadius_ * 1.25f);
    glUniform1f(uiFlattenVisibleLocation_, flattenPreviewVisible_ ? 1.0f : 0.0f);
    // Mirror plane disc: local X=0, sized to the sculpted region.
    const large::sdf::Vec3 boundsMin = renderBoundsMin();
    const large::sdf::Vec3 boundsExtent = renderBoundsExtent();
    const large::sdf::Vec3 mirrorCenterLocal{
        0.0f, boundsMin.y + boundsExtent.y * 0.5f, boundsMin.z + boundsExtent.z * 0.5f};
    const float mirrorRadiusLocal =
        large::sdf::clamp(std::max(boundsExtent.y, boundsExtent.z) * 0.5f + 0.08f, 0.25f, 0.90f);
    const large::sdf::Vec3 mirrorCenterWorld = objectToWorldPoint(mirrorCenterLocal);
    const large::sdf::Vec3 mirrorNormalWorld =
        large::sdf::normalize(rotateByQuaternion(objectRotation_, {1.0f, 0.0f, 0.0f}));
    glUniform3f(uiMirrorCenterLocation_, mirrorCenterWorld.x, mirrorCenterWorld.y, mirrorCenterWorld.z);
    glUniform3f(uiMirrorNormalLocation_, mirrorNormalWorld.x, mirrorNormalWorld.y, mirrorNormalWorld.z);
    glUniform1f(uiMirrorRadiusLocation_, mirrorRadiusLocal * objectScale_);
    glUniform1f(uiMirrorVisibleLocation_, mirrorEnabled_ ? 1.0f : 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hudTexture_);
    glBindVertexArray(uiVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
  }

  void renderEye(int eye) {
    EyeSwapchain& swapchain = eyeSwapchains_[eye];

    XrSwapchainImageAcquireInfo acquireInfo{};
    acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    uint32_t imageIndex = 0;
    if (!checkXr(xrInstance_,
                 xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &imageIndex),
                 "xrAcquireSwapchainImage")) {
      return;
    }

    XrSwapchainImageWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (!checkXr(xrInstance_,
                 xrWaitSwapchainImage(swapchain.handle, &waitInfo),
                 "xrWaitSwapchainImage")) {
      return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER,
                           GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D,
                           swapchain.images[imageIndex].image,
                           0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthBuffer_);
    glViewport(0, 0, swapchain.width, swapchain.height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glClearColor(0.026f, 0.035f, 0.044f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    renderSdfRaymarchForEye(eye);
    renderUiOverlayForEye(eye);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo{};
    releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    checkXr(xrInstance_,
            xrReleaseSwapchainImage(swapchain.handle, &releaseInfo),
            "xrReleaseSwapchainImage");
  }

  void shutdownOpenXr() {
    if (sdfTexture_ != 0) {
      glDeleteTextures(1, &sdfTexture_);
      sdfTexture_ = 0;
    }
    if (colorTexture_ != 0) {
      glDeleteTextures(1, &colorTexture_);
      colorTexture_ = 0;
    }
    if (hudTexture_ != 0) {
      glDeleteTextures(1, &hudTexture_);
      hudTexture_ = 0;
    }
    if (sdfVao_ != 0) {
      glDeleteVertexArrays(1, &sdfVao_);
      sdfVao_ = 0;
    }
    if (sdfProgram_ != 0) {
      glDeleteProgram(sdfProgram_);
      sdfProgram_ = 0;
    }
    if (uiVao_ != 0) {
      glDeleteVertexArrays(1, &uiVao_);
      uiVao_ = 0;
    }
    if (uiProgram_ != 0) {
      glDeleteProgram(uiProgram_);
      uiProgram_ = 0;
    }
    if (depthBuffer_ != 0) {
      glDeleteRenderbuffers(1, &depthBuffer_);
      depthBuffer_ = 0;
    }
    if (meshVbo_ != 0) {
      glDeleteBuffers(1, &meshVbo_);
      meshVbo_ = 0;
    }
    if (meshVao_ != 0) {
      glDeleteVertexArrays(1, &meshVao_);
      meshVao_ = 0;
    }
    if (meshProgram_ != 0) {
      glDeleteProgram(meshProgram_);
      meshProgram_ = 0;
    }
    if (framebuffer_ != 0) {
      glDeleteFramebuffers(1, &framebuffer_);
      framebuffer_ = 0;
    }

    for (EyeSwapchain& swapchain : eyeSwapchains_) {
      if (swapchain.handle != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain.handle);
        swapchain.handle = XR_NULL_HANDLE;
      }
      swapchain.images.clear();
    }

    if (rightAimSpace_ != XR_NULL_HANDLE) {
      xrDestroySpace(rightAimSpace_);
      rightAimSpace_ = XR_NULL_HANDLE;
    }
    if (leftGripSpace_ != XR_NULL_HANDLE) {
      xrDestroySpace(leftGripSpace_);
      leftGripSpace_ = XR_NULL_HANDLE;
    }
    if (rightGripSpace_ != XR_NULL_HANDLE) {
      xrDestroySpace(rightGripSpace_);
      rightGripSpace_ = XR_NULL_HANDLE;
    }
    if (xrSpace_ != XR_NULL_HANDLE) {
      xrDestroySpace(xrSpace_);
      xrSpace_ = XR_NULL_HANDLE;
    }
    if (leftHandTracker_ != XR_NULL_HANDLE && xrDestroyHandTrackerEXT_ != nullptr) {
      xrDestroyHandTrackerEXT_(leftHandTracker_);
      leftHandTracker_ = XR_NULL_HANDLE;
    }
    if (rightHandTracker_ != XR_NULL_HANDLE && xrDestroyHandTrackerEXT_ != nullptr) {
      xrDestroyHandTrackerEXT_(rightHandTracker_);
      rightHandTracker_ = XR_NULL_HANDLE;
    }
    handTrackingReady_ = false;
    if (passthroughLayer_ != XR_NULL_HANDLE && xrDestroyPassthroughLayerFB_ != nullptr) {
      xrDestroyPassthroughLayerFB_(passthroughLayer_);
      passthroughLayer_ = XR_NULL_HANDLE;
    }
    if (passthrough_ != XR_NULL_HANDLE && xrDestroyPassthroughFB_ != nullptr) {
      xrDestroyPassthroughFB_(passthrough_);
      passthrough_ = XR_NULL_HANDLE;
    }
    passthroughReady_ = false;
    arModeEnabled_ = false;
    if (xrSession_ != XR_NULL_HANDLE) {
      xrDestroySession(xrSession_);
      xrSession_ = XR_NULL_HANDLE;
    }

    if (actionSet_ != XR_NULL_HANDLE) {
      xrDestroyActionSet(actionSet_);
      actionSet_ = XR_NULL_HANDLE;
      rightAimPoseAction_ = XR_NULL_HANDLE;
      rightTriggerAction_ = XR_NULL_HANDLE;
      gripPoseAction_ = XR_NULL_HANDLE;
      gripValueAction_ = XR_NULL_HANDLE;
      nextToolAction_ = XR_NULL_HANDLE;
      previousToolAction_ = XR_NULL_HANDLE;
      brushAdjustAction_ = XR_NULL_HANDLE;
      locomotionAction_ = XR_NULL_HANDLE;
      undoAction_ = XR_NULL_HANDLE;
      redoAction_ = XR_NULL_HANDLE;
      menuAction_ = XR_NULL_HANDLE;
      leftHandPath_ = XR_NULL_PATH;
      rightHandPath_ = XR_NULL_PATH;
    }

    egl_.destroy();

    if (xrInstance_ != XR_NULL_HANDLE) {
      xrDestroyInstance(xrInstance_);
      xrInstance_ = XR_NULL_HANDLE;
      xrSystemId_ = XR_NULL_SYSTEM_ID;
    }
  }
#endif

  android_app* app_ = nullptr;
  large::sdf::SdfVolume volume_;
  large::sdf::SdfVolume stretchSourceVolume_;
  large::sdf::SdfHistory history_;
  bool isFocused_ = false;
  bool hasWindow_ = false;
  std::chrono::steady_clock::time_point nextStatsLog_{};

#if LARGE_USE_OPENXR
  bool openXrReady_ = false;
  EglState egl_;
  XrInstance xrInstance_ = XR_NULL_HANDLE;
  XrSystemId xrSystemId_ = XR_NULL_SYSTEM_ID;
  XrSession xrSession_ = XR_NULL_HANDLE;
  XrSpace xrSpace_ = XR_NULL_HANDLE;
  XrSessionState xrSessionState_ = XR_SESSION_STATE_UNKNOWN;
  bool xrSessionRunning_ = false;
  std::vector<XrViewConfigurationView> viewConfigs_;
  std::vector<XrView> views_;
  std::array<EyeSwapchain, kEyeCount> eyeSwapchains_{};
  GLuint framebuffer_ = 0;
  GLuint depthBuffer_ = 0;
  GLuint sdfProgram_ = 0;
  GLuint sdfVao_ = 0;
  GLuint sdfTexture_ = 0;
  GLuint colorTexture_ = 0;
  GLuint hudTexture_ = 0;
  std::vector<float> uploadScratch_;
  std::vector<std::uint8_t> colorVoxels_;
  std::vector<std::uint8_t> colorUploadScratch_;
  large::sdf::VoxelBounds colorDirtyBounds_{};
  large::sdf::VoxelBounds renderBounds_{};
  large::hud::Painter hudPainter_{large::hud::kContentWidth, large::hud::kContentHeight};
  HudSnapshot hudSnapshot_{};
  bool hudPainted_ = false;
  int paintColorIndex_ = 3;
  GLuint uiProgram_ = 0;
  GLuint uiVao_ = 0;
  GLint sdfCameraLocation_ = -1;
  GLint sdfViewRotationLocation_ = -1;
  GLint sdfFovTangentsLocation_ = -1;
  GLint sdfVolumeMinLocation_ = -1;
  GLint sdfVolumeExtentLocation_ = -1;
  GLint sdfRenderMinLocation_ = -1;
  GLint sdfRenderExtentLocation_ = -1;
  GLint sdfObjectPosLocation_ = -1;
  GLint sdfObjectRotationLocation_ = -1;
  GLint sdfObjectInvRotationLocation_ = -1;
  GLint sdfObjectScaleLocation_ = -1;
  GLint sdfArEnabledLocation_ = -1;
  GLint sdfWorldOffsetLocation_ = -1;
  GLint sdfVoxelSizeLocation_ = -1;
  GLint uiCameraLocation_ = -1;
  GLint uiViewRotationLocation_ = -1;
  GLint uiFovTangentsLocation_ = -1;
  GLint uiBrushRadiusLocation_ = -1;
  GLint uiTriggerValueLocation_ = -1;
  GLint uiToolIndexLocation_ = -1;
  GLint uiLeftUiPositionLocation_ = -1;
  GLint uiLeftUiVisibleLocation_ = -1;
  GLint uiRightToolPositionLocation_ = -1;
  GLint uiRightToolDirectionLocation_ = -1;
  GLint uiRightToolVisibleLocation_ = -1;
  GLint uiMenuPointerActiveLocation_ = -1;
  GLint uiMenuPointerStartLocation_ = -1;
  GLint uiMenuPointerEndLocation_ = -1;
  GLint uiLeftHandVisibleLocation_ = -1;
  GLint uiRightHandVisibleLocation_ = -1;
  GLint uiLeftHandJointsLocation_ = -1;
  GLint uiRightHandJointsLocation_ = -1;
  GLint uiHudVisibleSizeLocation_ = -1;
  GLint uiHudFullSizeLocation_ = -1;
  GLint uiPanelWidthLocation_ = -1;
  GLint uiPaintColorLocation_ = -1;
  GLint uiFlattenCenterLocation_ = -1;
  GLint uiFlattenNormalLocation_ = -1;
  GLint uiFlattenRadiusLocation_ = -1;
  GLint uiFlattenVisibleLocation_ = -1;
  GLint uiMirrorCenterLocation_ = -1;
  GLint uiMirrorNormalLocation_ = -1;
  GLint uiMirrorRadiusLocation_ = -1;
  GLint uiMirrorVisibleLocation_ = -1;
  XrActionSet actionSet_ = XR_NULL_HANDLE;
  XrAction rightAimPoseAction_ = XR_NULL_HANDLE;
  XrAction rightTriggerAction_ = XR_NULL_HANDLE;
  XrAction gripPoseAction_ = XR_NULL_HANDLE;
  XrAction gripValueAction_ = XR_NULL_HANDLE;
  XrAction nextToolAction_ = XR_NULL_HANDLE;
  XrAction previousToolAction_ = XR_NULL_HANDLE;
  XrAction brushAdjustAction_ = XR_NULL_HANDLE;
  XrAction locomotionAction_ = XR_NULL_HANDLE;
  XrAction undoAction_ = XR_NULL_HANDLE;
  XrAction redoAction_ = XR_NULL_HANDLE;
  XrAction menuAction_ = XR_NULL_HANDLE;
  XrSpace rightAimSpace_ = XR_NULL_HANDLE;
  XrSpace leftGripSpace_ = XR_NULL_HANDLE;
  XrSpace rightGripSpace_ = XR_NULL_HANDLE;
  XrPath leftHandPath_ = XR_NULL_PATH;
  XrPath rightHandPath_ = XR_NULL_PATH;
  bool handTrackingExtensionEnabled_ = false;
  bool handTrackingSupported_ = false;
  bool handTrackingReady_ = false;
  bool handTrackingCreateAttempted_ = false;
  bool passthroughExtensionEnabled_ = false;
  bool passthroughSupported_ = false;
  bool passthroughReady_ = false;
  bool alphaBlendEnvironmentSupported_ = false;
  bool arModeEnabled_ = false;
  PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_ = nullptr;
  PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT_ = nullptr;
  PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_ = nullptr;
  PFN_xrCreatePassthroughFB xrCreatePassthroughFB_ = nullptr;
  PFN_xrDestroyPassthroughFB xrDestroyPassthroughFB_ = nullptr;
  PFN_xrPassthroughStartFB xrPassthroughStartFB_ = nullptr;
  PFN_xrPassthroughPauseFB xrPassthroughPauseFB_ = nullptr;
  PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_ = nullptr;
  PFN_xrDestroyPassthroughLayerFB xrDestroyPassthroughLayerFB_ = nullptr;
  PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_ = nullptr;
  PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_ = nullptr;
  PFN_xrPassthroughLayerSetStyleFB xrPassthroughLayerSetStyleFB_ = nullptr;
  XrHandTrackerEXT leftHandTracker_ = XR_NULL_HANDLE;
  XrHandTrackerEXT rightHandTracker_ = XR_NULL_HANDLE;
  XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
  XrPassthroughLayerFB passthroughLayer_ = XR_NULL_HANDLE;
  HandState leftHandState_{};
  HandState rightHandState_{};
  large::sdf::Vec3 objectPosition_ = kInitialObjectPosition;
  XrQuaternionf objectRotation_{0.0f, 0.0f, 0.0f, 1.0f};
  float objectScale_ = 1.0f;
  large::sdf::Vec3 brushHitLocal_{};
  large::sdf::Vec3 brushHitWorld_{};
  bool brushVisible_ = false;
  large::sdf::Vec3 rightToolPosition_{};
  large::sdf::Vec3 rightToolDirection_{0.0f, 0.0f, -1.0f};
  bool rightToolVisible_ = false;
  float brushRadius_ = kDefaultBrushRadius;
  float brushStrength_ = kDefaultBrushStrength;
  float rightTriggerValue_ = 0.0f;
  XrVector2f leftStickValue_{0.0f, 0.0f};
  VrTool activeTool_ = VrTool::Add;
  std::array<ToolSettings, kVrToolCount> toolSettings_{};
  int handDisplayToolIndex_ = -1;
  bool nextToolWasDown_ = false;
  bool previousToolWasDown_ = false;
  bool undoWasDown_ = false;
  bool redoWasDown_ = false;
  bool menuWasDown_ = false;
  bool menuSelectWasDown_ = false;
  bool menuVisible_ = false;
  bool menuPointerActive_ = false;
  int menuHoverIndex_ = -1;
  large::sdf::Vec3 menuPointerStart_{};
  large::sdf::Vec3 menuPointerEnd_{};
  bool rightTriggerWasDown_ = false;
  bool rightControllerToolStrokeActive_ = false;
  bool stretchAnchorSet_ = false;
  bool stretchPullActive_ = false;
  bool rightHandPinchStretchActive_ = false;
  bool rightHandPinchWasActive_ = false;
  bool rightHandPinchToolActive_ = false;
  int rightHandPinchTool_ = -1;
  bool rightHandShapeBrushActive_ = false;
  int rightHandShapeBrushTool_ = -1;
  bool handClapWasClosed_ = false;
  bool leftHandPinchZoomActive_ = false;
  float leftHandPinchZoomStartX_ = 0.0f;
  float leftHandPinchZoomStartScale_ = 1.0f;
  large::sdf::Vec3 stretchAnchorLocal_{};
  large::sdf::Vec3 stretchPullStartControllerLocal_{};
  XrQuaternionf stretchPullStartToolOrientationLocal_{0.0f, 0.0f, 0.0f, 1.0f};
  large::sdf::VoxelBounds stretchUploadBounds_{};
  large::sdf::Vec3 strokeLastLocal_{};
  bool strokeHasLast_ = false;
  large::sdf::Vec3 flattenPlanePointLocal_{};
  large::sdf::Vec3 flattenPlaneNormalLocal_{0.0f, 1.0f, 0.0f};
  bool flattenPreviewVisible_ = false;
  large::sdf::Vec3 flattenPreviewCenterWorld_{};
  large::sdf::Vec3 flattenPreviewNormalWorld_{0.0f, 1.0f, 0.0f};
  bool mirrorEnabled_ = false;
  bool objectLocked_ = false;
  bool handPokeWasTouching_ = false;
  large::sdf::Vec3 worldOffset_{};
  XrTime lastLocomotionTime_ = 0;
  ControllerPose leftGripPose_{};
  ControllerPose rightGripPose_{};
  bool leftGripActive_ = false;
  bool rightGripActive_ = false;
  float leftGripValue_ = 0.0f;
  float rightGripValue_ = 0.0f;
  bool oneHandGrabActive_ = false;
  int oneHandGrabHand_ = 0;
  large::sdf::Vec3 grabStartHandPosition_{};
  XrQuaternionf grabStartHandOrientation_{0.0f, 0.0f, 0.0f, 1.0f};
  large::sdf::Vec3 grabStartObjectPosition_{};
  XrQuaternionf grabStartObjectRotation_{0.0f, 0.0f, 0.0f, 1.0f};
  bool twoHandGrabActive_ = false;
  float twoHandStartDistance_ = 1.0f;
  float twoHandStartScale_ = 1.0f;
  large::sdf::Vec3 twoHandStartMidpoint_{};
  large::sdf::Vec3 twoHandStartVector_{1.0f, 0.0f, 0.0f};
  large::sdf::Vec3 twoHandStartObjectPosition_{};
  XrQuaternionf twoHandStartObjectRotation_{0.0f, 0.0f, 0.0f, 1.0f};
  uint64_t nextSculptFrame_ = 0;
  uint64_t nextSculptLogFrame_ = 0;
  uint64_t nextBrushAdjustLogFrame_ = 0;
  uint64_t nextHandLogFrame_ = 0;
  GLuint meshProgram_ = 0;
  GLuint meshVao_ = 0;
  GLuint meshVbo_ = 0;
  GLint meshMvpLocation_ = -1;
  GLint meshModelLocation_ = -1;
  GLsizei meshVertexCount_ = 0;
  uint64_t frameCounter_ = 0;
#endif
};

}  // namespace

void android_main(android_app* app) {
  QuestSdfApp questApp(app);
  questApp.run();
}
