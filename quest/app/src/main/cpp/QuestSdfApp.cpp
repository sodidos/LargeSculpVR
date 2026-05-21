#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
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
constexpr float kUiPanelWidthMeters = 0.17f;
constexpr float kUiContentWidth = 264.0f;
constexpr float kMenuContentHeight = 392.0f;
constexpr float kMenuArButtonX = 220.0f;
constexpr float kMenuArButtonY = 84.0f;
constexpr float kMenuArButtonRadius = 18.0f;
constexpr float kHandPinchDistance = 0.035f;
constexpr float kHandFistTipDistance = 0.090f;
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
constexpr float kHandAddStrength = 1.0f;
constexpr float kHandSmoothStrength = 1.0f;
constexpr float kHandEraseStrength = 1.0f;
constexpr int kMenuToolCount = 4;
constexpr int kMenuFileActionCount = 4;
constexpr int kMenuChoiceCount = kMenuToolCount + kMenuFileActionCount;
constexpr int kMenuArChoice = kMenuChoiceCount;

enum class VrTool {
  Add,
  Subtract,
  Smooth,
  Stretch,
};

constexpr int kVrToolCount = 4;

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
  }
  return "Unknown";
}

VrTool toolFromIndex(int index) {
  const int wrapped = (index % kVrToolCount + kVrToolCount) % kVrToolCount;
  if (wrapped == 1) {
    return VrTool::Subtract;
  }
  if (wrapped == 2) {
    return VrTool::Smooth;
  }
  if (wrapped == 3) {
    return VrTool::Stretch;
  }
  return VrTool::Add;
}

VrTool nextHandTool(VrTool tool) {
  switch (tool) {
    case VrTool::Add:
      return VrTool::Stretch;
    case VrTool::Stretch:
      return VrTool::Subtract;
    case VrTool::Subtract:
      return VrTool::Smooth;
    case VrTool::Smooth:
      return VrTool::Add;
  }
  return VrTool::Add;
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

  large::sdf::SdfVolume volume({resolution, resolution, resolution}, voxelSize, origin, 10.0f);
  volume.fillSphere({0.0f, 0.0f, 0.0f}, 0.55f);
  return volume;
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

    XrPassthroughCreateInfoFB passthroughInfo{};
    passthroughInfo.type = XR_TYPE_PASSTHROUGH_CREATE_INFO_FB;
    passthroughInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XrResult result = xrCreatePassthroughFB_(xrSession_, &passthroughInfo, &passthrough_);
    if (XR_FAILED(result) || passthrough_ == XR_NULL_HANDLE) {
      logError("xrCreatePassthroughFB failed: %s", xrResultName(xrInstance_, result));
      passthroughReady_ = false;
      return;
    }

    XrPassthroughLayerCreateInfoFB layerInfo{};
    layerInfo.type = XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB;
    layerInfo.passthrough = passthrough_;
    layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
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

    XrPassthroughStyleFB style{};
    style.type = XR_TYPE_PASSTHROUGH_STYLE_FB;
    style.textureOpacityFactor = 0.45f;
    style.edgeColor = {0.0f, 0.0f, 0.0f, 0.0f};
    xrPassthroughLayerSetStyleFB_(passthroughLayer_, &style);

    passthroughReady_ = true;
    logInfo("OpenXR passthrough ready");
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
      if (XR_FAILED(result)) {
        logError("xrPassthroughStartFB failed: %s", xrResultName(xrInstance_, result));
        arModeEnabled_ = false;
        return;
      }
      result = xrPassthroughLayerResumeFB_(passthroughLayer_);
      if (XR_FAILED(result)) {
        logError("xrPassthroughLayerResumeFB failed: %s", xrResultName(xrInstance_, result));
        xrPassthroughPauseFB_(passthrough_);
        arModeEnabled_ = false;
        return;
      }
      arModeEnabled_ = true;
      logInfo("AR passthrough enabled: compositor=opaque safe overlay, alphaBlendSupported=%d",
              alphaBlendEnvironmentSupported_ ? 1 : 0);
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

    std::array<XrActionSuggestedBinding, 12> bindings{{
        {gripPoseAction_, leftGripPosePath},
        {gripValueAction_, leftGripValuePath},
        {brushAdjustAction_, leftStickPath},
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
          app_->destroyRequested = 1;
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
      app_->destroyRequested = 1;
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
      for (int eye = 0; eye < kEyeCount; ++eye) {
        renderEye(eye);

        projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projectionViews[eye].pose = views_[eye].pose;
        projectionViews[eye].fov = views_[eye].fov;
        projectionViews[eye].subImage.swapchain = eyeSwapchains_[eye].handle;
        projectionViews[eye].subImage.imageRect.offset = {0, 0};
        projectionViews[eye].subImage.imageRect.extent = {eyeSwapchains_[eye].width, eyeSwapchains_[eye].height};
      }

      projectionLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
      projectionLayer.layerFlags = 0;
      projectionLayer.space = xrSpace_;
      projectionLayer.viewCount = kEyeCount;
      projectionLayer.views = projectionViews.data();
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projectionLayer);

      if (arModeEnabled_ && passthroughReady_) {
        passthroughCompositionLayer.type = XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB;
        passthroughCompositionLayer.space = xrSpace_;
        passthroughCompositionLayer.layerHandle = passthroughLayer_;
        layers[layerCount++] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughCompositionLayer);
      }
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
    rightHandPinchStretchActive_ = false;
    rightHandPinchWasActive_ = false;
    rightHandPinchToolActive_ = false;
    rightHandPinchTool_ = -1;
    rightHandShapeBrushActive_ = false;
    rightHandShapeBrushTool_ = -1;
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

      constexpr char magic[8] = {'L', 'S', 'D', 'F', 'V', 'R', '1', '\0'};
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

      constexpr char expectedMagic[8] = {'L', 'S', 'D', 'F', 'V', 'R', '1', '\0'};
      const large::sdf::IVec3 size = volume_.size();
      if (std::memcmp(magic, expectedMagic, sizeof(expectedMagic)) != 0 || dims[0] != size.x || dims[1] != size.y ||
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

      history_.capture();
      volume_.restoreValues(std::move(values));
      stretchSourceVolume_ = volume_;
      clearStretchInteraction();
      uploadSdfTexture();
      logInfo("Menu LOAD: %s", path.string().c_str());
    } catch (const std::exception& e) {
      logError("Menu LOAD failed: %s", e.what());
    }
  }

  void exportSdfVolume() {
    try {
      const std::filesystem::path path = appDataPath() / "large_sdf_export.obj";
      const large::sdf::ObjExportStats stats = large::sdf::exportSdfSurfaceAsObj(volume_, path);
      logInfo("Menu EXPORT: %s, vertices=%d faces=%d", path.string().c_str(), stats.vertices, stats.faces);
    } catch (const std::exception& e) {
      logError("Menu EXPORT failed: %s", e.what());
    }
  }

  void activateMenuChoice(int choice) {
    if (choice == kMenuArChoice) {
      setArModeEnabled(!arModeEnabled_);
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
        if (xrSession_ != XR_NULL_HANDLE) {
          xrRequestExitSession(xrSession_);
        }
        app_->destroyRequested = 1;
        break;
      default:
        break;
    }
  }

  int activeToolIndex() const {
    switch (activeTool_) {
      case VrTool::Add:
        return 0;
      case VrTool::Subtract:
        return 1;
      case VrTool::Smooth:
        return 2;
      case VrTool::Stretch:
        return 3;
    }
    return 0;
  }

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
    state.fist = indexDistance < kHandFistTipDistance && middleDistance < kHandFistTipDistance &&
                 ringDistance < kHandFistTipDistance && littleDistance < kHandFistTipDistance &&
                 thumbDistance < kHandFistTipDistance * 1.35f;
    state.pinch = !state.fist && pinchDistance < kHandPinchDistance;
    state.open = !state.fist && !state.pinch && pinchDistance > kHandOpenPinchDistance &&
                 indexDistance > kHandOpenFingerDistance && middleDistance > kHandOpenFingerDistance &&
                 ringDistance > kHandOpenFingerDistance * 0.92f &&
                 littleDistance > kHandOpenLittleDistance && thumbDistance > kHandOpenThumbDistance;
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
  };

  bool leftUiPanelFrame(UiPanelFrame& frame) const {
    if (!menuVisible_ || !isLeftUiVisible()) {
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
    frame.height = kUiPanelWidthMeters * kMenuContentHeight / kUiContentWidth;
    return true;
  }

  bool resolveLeftUiHit(const UiPanelFrame& frame, large::sdf::Vec3 point, UiPointerHit& hit) const {
    const large::sdf::Vec3 offset = point - frame.center;
    const float localX = large::sdf::dot(offset, frame.right);
    const float localY = large::sdf::dot(offset, frame.up);
    if (std::abs(localX) > kUiPanelWidthMeters * 0.5f || std::abs(localY) > frame.height * 0.5f) {
      return false;
    }

    const float fragX = (localX / kUiPanelWidthMeters + 0.5f) * kUiContentWidth;
    const float fragY = (0.5f - localY / frame.height) * kMenuContentHeight;

    hit.point = point;
    hit.menuChoice = -1;
    const float arDx = fragX - kMenuArButtonX;
    const float arDy = fragY - kMenuArButtonY;
    if (arDx * arDx + arDy * arDy <= kMenuArButtonRadius * kMenuArButtonRadius) {
      hit.menuChoice = kMenuArChoice;
      return true;
    }

    if (fragX >= 28.0f && fragX <= 226.0f) {
      for (int i = 0; i < kMenuChoiceCount; ++i) {
        const float rowTop = 132.0f + static_cast<float>(i) * 29.0f;
        const float rowBottom = rowTop + 23.0f;
        if (fragY >= rowTop && fragY <= rowBottom) {
          hit.menuChoice = i;
          break;
        }
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

  void updateLeftHandPinchZoom() {
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

  void applyActiveSculptTool(large::sdf::Vec3 center, float localBrushRadius) {
    switch (activeTool_) {
      case VrTool::Add:
        volume_.applySphereBrush(center, localBrushRadius, large::sdf::BrushMode::Add, brushStrength_);
        break;
      case VrTool::Subtract:
        volume_.applySphereBrush(center, localBrushRadius, large::sdf::BrushMode::Subtract, brushStrength_);
        break;
      case VrTool::Smooth:
        volume_.applySmoothBrush(center, localBrushRadius * 1.35f, brushStrength_ * 0.65f);
        break;
      case VrTool::Stretch:
        break;
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
      volume_.applyStretchBrush(stretchSourceVolume_,
                                stretchAnchorLocal_,
                                delta,
                                rotationX,
                                rotationY,
                                rotationZ,
                                influenceRadius,
                                brushStrength_);
      uploadSdfTexture();
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
    const int toolIndex = activeToolIndex();
    handDisplayToolIndex_ = toolIndex;
    const large::sdf::Vec3 pinchLocal = worldToObjectPoint(rightHandState_.pinchPosition);
    const float surfaceDistance = std::abs(volume_.sample(pinchLocal));
    const float snapDistance = std::max(kHandSculptSurfaceSnapDistance / std::max(objectScale_, 0.001f),
                                        volume_.voxelSize() * 2.0f);

    if (activeTool_ != VrTool::Stretch) {
      rightHandPinchStretchActive_ = false;
      rightHandPinchWasActive_ = false;
      stretchPullActive_ = false;
      brushVisible_ = surfaceDistance <= std::max(influenceRadius * 1.15f, snapDistance);
      brushHitLocal_ = pinchLocal;
      brushHitWorld_ = objectToWorldPoint(pinchLocal);
      if (!brushVisible_) {
        rightHandPinchToolActive_ = false;
        rightHandPinchTool_ = -1;
        return true;
      }

      if (frameCounter_ >= nextSculptFrame_) {
        if (!rightHandPinchToolActive_ || rightHandPinchTool_ != toolIndex) {
          history_.capture();
        }

        switch (activeTool_) {
          case VrTool::Add:
            volume_.applySphereBrush(pinchLocal, influenceRadius, large::sdf::BrushMode::Add, kHandAddStrength);
            break;
          case VrTool::Subtract:
            volume_.applySphereBrush(pinchLocal, influenceRadius, large::sdf::BrushMode::Subtract, kHandEraseStrength);
            break;
          case VrTool::Smooth:
            volume_.applySmoothBrush(pinchLocal, influenceRadius * 1.35f, kHandSmoothStrength);
            break;
          case VrTool::Stretch:
            break;
        }

        rightHandPinchToolActive_ = true;
        rightHandPinchTool_ = toolIndex;
        uploadSdfTexture();
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
      volume_.applyStretchBrush(stretchSourceVolume_, stretchAnchorLocal_, delta, influenceRadius, 1.0f);
      uploadSdfTexture();
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

    const int toolIndex = erase ? 1 : 2;
    const large::sdf::Vec3 centerWorld = erase ? rightHandState_.fistToolPosition : rightHandState_.openToolPosition;
    const large::sdf::Vec3 centerLocal = worldToObjectPoint(centerWorld);
    const float localRadius = handGestureLocalRadius(localBrushRadius);
    const float surfaceDistance = std::abs(volume_.sample(centerLocal));
    const float snapDistance =
        std::max(kHandSculptSurfaceSnapDistance / std::max(objectScale_, 0.001f), volume_.voxelSize() * 2.0f);

    handDisplayToolIndex_ = toolIndex;
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
      if (!rightHandShapeBrushActive_ || rightHandShapeBrushTool_ != toolIndex) {
        history_.capture();
      }

      if (erase) {
        volume_.applySphereBrush(centerLocal, localRadius, large::sdf::BrushMode::Subtract, kHandEraseStrength);
      } else {
        volume_.applySmoothBrush(centerLocal, localRadius * 1.35f, kHandSmoothStrength);
      }

      rightHandShapeBrushActive_ = true;
      rightHandShapeBrushTool_ = toolIndex;
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
    updateLeftHandPinchZoom();
    updateBrushAdjustments();
    updateToolButtons();
    updateEditButtons();
    rightTriggerValue_ = readFloatAction(rightTriggerAction_, rightHandPath_, "right trigger");
    const bool triggerDown = rightTriggerValue_ >= kTriggerThreshold;
    const float localBrushRadius = brushRadius_ / std::max(objectScale_, 0.001f);
    handDisplayToolIndex_ = -1;
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
        rightControllerToolStrokeActive_ = true;
      }
      applyActiveSculptTool(toolCenterLocal, localBrushRadius);
      uploadSdfTexture();
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

  void uploadSdfTexture() {
    if (sdfTexture_ == 0) {
      return;
    }

    const large::sdf::IVec3 size = volume_.size();
    glBindTexture(GL_TEXTURE_3D, sdfTexture_);
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
    glBindTexture(GL_TEXTURE_3D, 0);
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
uniform vec3 uCameraPos;
uniform mat3 uViewRotation;
uniform vec4 uFovTangents;
uniform vec3 uVolumeMin;
uniform vec3 uVolumeExtent;
uniform vec3 uObjectPos;
uniform mat3 uObjectRotation;
uniform mat3 uObjectInvRotation;
uniform float uObjectScale;
uniform vec3 uBrushCenter;
uniform float uBrushRadius;
uniform float uBrushVisible;
uniform float uTriggerValue;
uniform int uToolIndex;
uniform float uBrushStrength;
uniform float uArEnabled;
uniform float uMenuVisible;
uniform vec3 uLeftUiPosition;
uniform float uLeftUiVisible;
uniform vec3 uRightToolPosition;
uniform vec3 uRightToolDirection;
uniform float uRightToolVisible;
uniform int uMenuHoverIndex;
uniform float uMenuPointerActive;
uniform vec3 uMenuPointerStart;
uniform vec3 uMenuPointerEnd;

#if 0
int toolChar(int tool, int index) {
  if (tool == 0) {
    if (index == 0) return 65;
    if (index == 1) return 68;
    if (index == 2) return 68;
  } else if (tool == 1) {
    if (index == 0) return 83;
    if (index == 1) return 85;
    if (index == 2) return 66;
  } else if (tool == 2) {
    if (index == 0) return 83;
    if (index == 1) return 77;
    if (index == 2) return 79;
    if (index == 3) return 79;
    if (index == 4) return 84;
    if (index == 5) return 72;
  } else {
    if (index == 0) return 83;
    if (index == 1) return 84;
    if (index == 2) return 82;
    if (index == 3) return 69;
    if (index == 4) return 84;
    if (index == 5) return 67;
    if (index == 6) return 72;
  }
  return 0;
}

int sizeChar(int index) {
  if (index == 0) return 83;
  if (index == 1) return 73;
  if (index == 2) return 90;
  if (index == 3) return 69;
  return 0;
}

int powerChar(int index) {
  if (index == 0) return 80;
  if (index == 1) return 79;
  if (index == 2) return 87;
  if (index == 3) return 69;
  if (index == 4) return 82;
  return 0;
}

int menuChar(int item, int index) {
  if (item == 0) {
    if (index == 0) return 83;
    if (index == 1) return 65;
    if (index == 2) return 86;
    if (index == 3) return 69;
  } else if (item == 1) {
    if (index == 0) return 76;
    if (index == 1) return 79;
    if (index == 2) return 65;
    if (index == 3) return 68;
  } else if (item == 2) {
    if (index == 0) return 69;
    if (index == 1) return 88;
    if (index == 2) return 80;
    if (index == 3) return 79;
    if (index == 4) return 82;
    if (index == 5) return 84;
  } else if (item == 3) {
    if (index == 0) return 81;
    if (index == 1) return 85;
    if (index == 2) return 73;
    if (index == 3) return 84;
  }
  return 0;
}

int glyphRow(int code, int row) {
  if (code == 65) { if (row == 0) return 14; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 31; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 17; }
  if (code == 66) { if (row == 0) return 30; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 30; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 30; }
  if (code == 67) { if (row == 0) return 14; if (row == 1) return 17; if (row == 2) return 16; if (row == 3) return 16; if (row == 4) return 16; if (row == 5) return 17; if (row == 6) return 14; }
  if (code == 68) { if (row == 0) return 30; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 17; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 30; }
  if (code == 69) { if (row == 0) return 31; if (row == 1) return 16; if (row == 2) return 16; if (row == 3) return 30; if (row == 4) return 16; if (row == 5) return 16; if (row == 6) return 31; }
  if (code == 72) { if (row == 0) return 17; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 31; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 17; }
  if (code == 73) { if (row == 0) return 14; if (row == 1) return 4; if (row == 2) return 4; if (row == 3) return 4; if (row == 4) return 4; if (row == 5) return 4; if (row == 6) return 14; }
  if (code == 76) { if (row == 0) return 16; if (row == 1) return 16; if (row == 2) return 16; if (row == 3) return 16; if (row == 4) return 16; if (row == 5) return 16; if (row == 6) return 31; }
  if (code == 77) { if (row == 0) return 17; if (row == 1) return 27; if (row == 2) return 21; if (row == 3) return 21; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 17; }
  if (code == 78) { if (row == 0) return 17; if (row == 1) return 25; if (row == 2) return 21; if (row == 3) return 19; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 17; }
  if (code == 79) { if (row == 0) return 14; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 17; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 14; }
  if (code == 80) { if (row == 0) return 30; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 30; if (row == 4) return 16; if (row == 5) return 16; if (row == 6) return 16; }
  if (code == 81) { if (row == 0) return 14; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 17; if (row == 4) return 21; if (row == 5) return 18; if (row == 6) return 13; }
  if (code == 82) { if (row == 0) return 30; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 30; if (row == 4) return 20; if (row == 5) return 18; if (row == 6) return 17; }
  if (code == 83) { if (row == 0) return 15; if (row == 1) return 16; if (row == 2) return 16; if (row == 3) return 14; if (row == 4) return 1; if (row == 5) return 1; if (row == 6) return 30; }
  if (code == 84) { if (row == 0) return 31; if (row == 1) return 4; if (row == 2) return 4; if (row == 3) return 4; if (row == 4) return 4; if (row == 5) return 4; if (row == 6) return 4; }
  if (code == 85) { if (row == 0) return 17; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 17; if (row == 4) return 17; if (row == 5) return 17; if (row == 6) return 14; }
  if (code == 86) { if (row == 0) return 17; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 17; if (row == 4) return 10; if (row == 5) return 10; if (row == 6) return 4; }
  if (code == 87) { if (row == 0) return 17; if (row == 1) return 17; if (row == 2) return 17; if (row == 3) return 21; if (row == 4) return 21; if (row == 5) return 27; if (row == 6) return 17; }
  if (code == 88) { if (row == 0) return 17; if (row == 1) return 17; if (row == 2) return 10; if (row == 3) return 4; if (row == 4) return 10; if (row == 5) return 17; if (row == 6) return 17; }
  if (code == 90) { if (row == 0) return 31; if (row == 1) return 1; if (row == 2) return 2; if (row == 3) return 4; if (row == 4) return 8; if (row == 5) return 16; if (row == 6) return 31; }
  return 0;
}

float glyphAlpha(int code, vec2 origin, vec2 frag, float scale) {
  if (code == 0) {
    return 0.0;
  }
  vec2 rel = (frag - origin) / scale;
  ivec2 cell = ivec2(floor(rel));
  if (cell.x < 0 || cell.x >= 5 || cell.y < 0 || cell.y >= 7) {
    return 0.0;
  }
  int bits = glyphRow(code, cell.y);
  int mask = 1 << (4 - cell.x);
  return ((bits & mask) != 0) ? 1.0 : 0.0;
}

float drawToolName(vec2 frag) {
  float a = 0.0;
  for (int i = 0; i < 7; ++i) {
    a = max(a, glyphAlpha(toolChar(uToolIndex, i), vec2(26.0 + float(i) * 18.0, 24.0), frag, 3.0));
  }
  return a;
}

float drawLabel(vec2 frag, int label, vec2 origin) {
  float a = 0.0;
  for (int i = 0; i < 5; ++i) {
    int code = label == 0 ? sizeChar(i) : powerChar(i);
    a = max(a, glyphAlpha(code, origin + vec2(float(i) * 12.0, 0.0), frag, 2.0));
  }
  return a;
}

float drawMenuItem(vec2 frag, int item, vec2 origin) {
  float a = 0.0;
  for (int i = 0; i < 6; ++i) {
    a = max(a, glyphAlpha(menuChar(item, i), origin + vec2(float(i) * 12.0, 0.0), frag, 2.0));
  }
  return a;
}

float rectMask(vec2 frag, vec2 minP, vec2 maxP) {
  vec2 a = step(minP, frag);
  vec2 b = step(frag, maxP);
  return a.x * a.y * b.x * b.y;
}
#endif

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

vec3 applyVolumeBoundsCube(vec3 color, vec2 uv, float sceneDepth) {
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
  return mix(color, boundsColor, edgeAlpha);
}

float sampleSdfLocal(vec3 localPoint) {
  vec3 uv = (localPoint - uVolumeMin) / uVolumeExtent;
  return texture(uSdf, clamp(uv, vec3(0.0), vec3(1.0))).r;
}

vec3 estimateNormal(vec3 localPoint) {
  float e = max(0.006, 0.012 / max(uObjectScale, 0.001));
  vec3 dx = vec3(e, 0.0, 0.0);
  vec3 dy = vec3(0.0, e, 0.0);
  vec3 dz = vec3(0.0, 0.0, e);
  vec3 normalLocal = normalize(vec3(
    sampleSdfLocal(localPoint + dx) - sampleSdfLocal(localPoint - dx),
    sampleSdfLocal(localPoint + dy) - sampleSdfLocal(localPoint - dy),
    sampleSdfLocal(localPoint + dz) - sampleSdfLocal(localPoint - dz)));
  return normalize(uObjectRotation * normalLocal);
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
  sceneAlpha = 1.0 - clamp(uArEnabled, 0.0, 1.0) * 0.0;
  float x = mix(uFovTangents.x, uFovTangents.y, uv.x);
  float y = mix(uFovTangents.z, uFovTangents.w, uv.y);
  vec3 rayDir = normalize(uViewRotation * normalize(vec3(x, y, -1.0)));
  vec3 rayOrigin = uCameraPos;
  float roomDepth = 10000.0;
  vec3 background = shadeRoom(rayOrigin, rayDir, roomDepth);
  vec3 rayOriginLocal = worldToLocal(rayOrigin);
  vec3 rayDirLocal = normalize(uObjectInvRotation * rayDir);

  vec3 boxMin = uVolumeMin;
  vec3 boxMax = uVolumeMin + uVolumeExtent;
  vec2 hit = intersectBox(rayOriginLocal, rayDirLocal, boxMin, boxMax);

  if (hit.y <= max(hit.x, 0.0)) {
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
    sceneDepth = roomDepth;
    return background;
  }

  sceneAlpha = 1.0;
  sceneDepth = max(t * uObjectScale, 0.0);
  vec3 normal = estimateNormal(localP);
  vec3 light = normalize(vec3(-0.35, 0.85, 0.42));
  float diffuse = max(dot(normal, light), 0.0);
  float wrap = 0.5 + 0.5 * dot(normal, light);
  vec3 clay = vec3(0.86, 0.68, 0.54);
  vec3 color = clay * (0.35 + diffuse * 0.55 + wrap * 0.10);
  float brushKeepAlive = uBrushVisible * 0.00000001 + uBrushRadius * 0.00000001 +
                         uTriggerValue * 0.00000001 + uBrushStrength * 0.00000001 +
                         float(uToolIndex) * 0.00000001 + dot(uBrushCenter, vec3(0.00000001));
  color += vec3(brushKeepAlive);
  return color;
}

#if 0
vec3 brushUiColor() {
  vec3 idleColor = vec3(0.25, 0.85, 1.0);
  vec3 activeColor = vec3(1.0, 0.60, 0.24);
  if (uToolIndex == 1) {
    idleColor = vec3(1.0, 0.35, 0.36);
    activeColor = vec3(1.0, 0.12, 0.18);
  } else if (uToolIndex == 2) {
    idleColor = vec3(0.40, 1.0, 0.62);
    activeColor = vec3(0.18, 0.95, 0.36);
  } else if (uToolIndex == 3) {
    idleColor = vec3(0.72, 0.48, 1.0);
    activeColor = vec3(0.95, 0.30, 1.0);
  }
  return mix(idleColor, activeColor, smoothstep(0.50, 0.80, uTriggerValue));
}

vec3 applyHudAtFrag(vec3 color, vec2 frag) {
  float panel = rectMask(frag, vec2(16.0, 16.0), vec2(248.0, 112.0));
  color = mix(color, vec3(0.018, 0.022, 0.026), panel * 0.74);

  float toolText = drawToolName(frag);
  color = mix(color, vec3(0.92, 0.95, 0.98), toolText);

  float sizeLabel = drawLabel(frag, 0, vec2(28.0, 62.0));
  float powerLabel = drawLabel(frag, 1, vec2(116.0, 62.0));
  color = mix(color, vec3(0.72, 0.78, 0.84), max(sizeLabel, powerLabel));

  float sizeBack = rectMask(frag, vec2(86.0, 56.0), vec2(98.0, 104.0));
  float powerBack = rectMask(frag, vec2(116.0, 88.0), vec2(226.0, 98.0));
  color = mix(color, vec3(0.08, 0.10, 0.12), max(sizeBack, powerBack));

  float sizeNorm = clamp((uBrushRadius - 0.015) / (0.60 - 0.015), 0.0, 1.0);
  float powerNorm = clamp(uBrushStrength, 0.0, 1.0);
  float sizeTop = mix(104.0, 56.0, sizeNorm);
  float sizeFill = rectMask(frag, vec2(86.0, sizeTop), vec2(98.0, 104.0));
  float powerFill = rectMask(frag, vec2(116.0, 88.0), vec2(116.0 + 110.0 * powerNorm, 98.0));
  color = mix(color, vec3(0.25, 0.85, 1.0), sizeFill);
  color = mix(color, vec3(1.0, 0.60, 0.24), powerFill);

  float menuOn = step(0.5, uMenuVisible);
  float menuPanel = rectMask(frag, vec2(16.0, 128.0), vec2(248.0, 264.0)) * menuOn;
  color = mix(color, vec3(0.016, 0.019, 0.023), menuPanel * 0.86);
  float row0 = rectMask(frag, vec2(28.0, 144.0), vec2(226.0, 168.0));
  float row1 = rectMask(frag, vec2(28.0, 174.0), vec2(226.0, 198.0));
  float row2 = rectMask(frag, vec2(28.0, 204.0), vec2(226.0, 228.0));
  float row3 = rectMask(frag, vec2(28.0, 234.0), vec2(226.0, 258.0));
  float menuRows = max(max(row0, row1), max(row2, row3));
  color = mix(color, vec3(0.08, 0.10, 0.12), menuRows * menuOn * 0.72);
  float hoveredRow = 0.0;
  hoveredRow = max(hoveredRow, row0 * (uMenuHoverIndex == 0 ? 1.0 : 0.0));
  hoveredRow = max(hoveredRow, row1 * (uMenuHoverIndex == 1 ? 1.0 : 0.0));
  hoveredRow = max(hoveredRow, row2 * (uMenuHoverIndex == 2 ? 1.0 : 0.0));
  hoveredRow = max(hoveredRow, row3 * (uMenuHoverIndex == 3 ? 1.0 : 0.0));
  color = mix(color, vec3(0.30, 0.42, 0.48), hoveredRow * menuOn * 0.88);
  float menuText = 0.0;
  menuText = max(menuText, drawMenuItem(frag, 0, vec2(46.0, 149.0)));
  menuText = max(menuText, drawMenuItem(frag, 1, vec2(46.0, 179.0)));
  menuText = max(menuText, drawMenuItem(frag, 2, vec2(46.0, 209.0)));
  menuText = max(menuText, drawMenuItem(frag, 3, vec2(46.0, 239.0)));
  color = mix(color, vec3(0.90, 0.94, 0.98), menuText * menuOn);
  return color;
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

vec3 applyMenuPointerLine(vec3 color, vec3 rayOrigin, vec3 rayDir, float sceneDepth) {
  if (uMenuPointerActive < 0.5) {
    return color;
  }
  float pointerT = 0.0;
  float distance = raySegmentDistance(rayOrigin, rayDir, uMenuPointerStart, uMenuPointerEnd, pointerT);
  if (pointerT > sceneDepth - 0.01) {
    return color;
  }
  float core = 1.0 - smoothstep(0.006, 0.014, distance);
  float glow = 1.0 - smoothstep(0.014, 0.035, distance);
  vec3 pointerColor = mix(vec3(0.65, 0.88, 1.0), brushUiColor(), 0.35);
  color = mix(color, pointerColor, clamp(core * 0.95 + glow * 0.32, 0.0, 0.95));
  return color;
}

vec3 applyLeftHandUi(vec3 color, vec3 rayOrigin, vec3 rayDir, float sceneDepth) {
  if (uLeftUiVisible < 0.5) {
    return color;
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
    return color;
  }

  float t = dot(center - rayOrigin, panelForward) / denom;
  if (t <= 0.0) {
    return color;
  }
  if (t > sceneDepth - 0.01) {
    return color;
  }

  vec3 hit = rayOrigin + rayDir * t;
  vec2 local = vec2(dot(hit - center, panelRight), dot(hit - center, panelUp));
  float contentWidth = 264.0;
  float contentHeight = mix(128.0, 280.0, step(0.5, uMenuVisible));
  float panelWidth = 0.34;
  float panelHeight = panelWidth * contentHeight / contentWidth;
  if (abs(local.x) > panelWidth * 0.5 || abs(local.y) > panelHeight * 0.5) {
    return color;
  }

  vec2 frag = vec2((local.x / panelWidth + 0.5) * contentWidth,
                   (0.5 - local.y / panelHeight) * contentHeight);
  return applyHudAtFrag(color, frag);
}

vec3 applyRightHandTool(vec3 color, vec3 rayOrigin, vec3 rayDir, float sceneDepth) {
  if (uRightToolVisible < 0.5 || uMenuPointerActive > 0.5) {
    return color;
  }

  vec3 toolDir = normalize(uRightToolDirection);
  vec3 base = uRightToolPosition + toolDir * 0.015;
  vec3 tip = uRightToolPosition + toolDir * 0.20;
  vec3 toolColor = brushUiColor();

  float shaftT = 0.0;
  float shaftDistance = raySegmentDistance(rayOrigin, rayDir, base, tip, shaftT);
  float shaftVisible = step(0.0, shaftT) * step(shaftT, sceneDepth - 0.01);
  float shaft = (1.0 - smoothstep(0.010, 0.022, shaftDistance)) * shaftVisible;

  float headT = 0.0;
  float headDistance = rayPointDistance(rayOrigin, rayDir, tip, headT);
  float headRadius = clamp(0.018 + uBrushRadius * 0.12, 0.026, 0.085);
  float headVisible = step(0.0, headT) * step(headT, sceneDepth - 0.01);
  float head = (1.0 - smoothstep(headRadius, headRadius * 1.55, headDistance)) * headVisible;
  float ring = (1.0 - smoothstep(0.004, 0.014, abs(headDistance - headRadius))) * headVisible;

  color = mix(color, vec3(0.74, 0.80, 0.86), shaft * 0.75);
  color = mix(color, toolColor, clamp(head * 0.70 + ring * 0.90, 0.0, 0.95));
  return color;
}

vec3 applySpatialInterface(vec3 color, vec2 uv, float sceneDepth) {
  float x = mix(uFovTangents.x, uFovTangents.y, uv.x);
  float y = mix(uFovTangents.z, uFovTangents.w, uv.y);
  vec3 rayDir = normalize(uViewRotation * normalize(vec3(x, y, -1.0)));
  vec3 rayOrigin = uCameraPos;
  color = applyMenuPointerLine(color, rayOrigin, rayDir, sceneDepth);
  color = applyLeftHandUi(color, rayOrigin, rayDir, sceneDepth);
  color = applyRightHandTool(color, rayOrigin, rayDir, sceneDepth);
  return color;
}
#endif

void main() {
  float sceneDepth = 10000.0;
  float sceneAlpha = 1.0;
  vec3 color = shadeUv(vUv, sceneDepth, sceneAlpha);
  color = applyVolumeBoundsCube(color, vUv, sceneDepth);
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
    sdfObjectPosLocation_ = glGetUniformLocation(sdfProgram_, "uObjectPos");
    sdfObjectRotationLocation_ = glGetUniformLocation(sdfProgram_, "uObjectRotation");
    sdfObjectInvRotationLocation_ = glGetUniformLocation(sdfProgram_, "uObjectInvRotation");
    sdfObjectScaleLocation_ = glGetUniformLocation(sdfProgram_, "uObjectScale");
    sdfBrushCenterLocation_ = glGetUniformLocation(sdfProgram_, "uBrushCenter");
    sdfBrushRadiusLocation_ = glGetUniformLocation(sdfProgram_, "uBrushRadius");
    sdfBrushVisibleLocation_ = glGetUniformLocation(sdfProgram_, "uBrushVisible");
    sdfTriggerValueLocation_ = glGetUniformLocation(sdfProgram_, "uTriggerValue");
    sdfToolIndexLocation_ = glGetUniformLocation(sdfProgram_, "uToolIndex");
    sdfBrushStrengthLocation_ = glGetUniformLocation(sdfProgram_, "uBrushStrength");
    sdfArEnabledLocation_ = glGetUniformLocation(sdfProgram_, "uArEnabled");
    sdfMenuVisibleLocation_ = glGetUniformLocation(sdfProgram_, "uMenuVisible");
    sdfLeftUiPositionLocation_ = glGetUniformLocation(sdfProgram_, "uLeftUiPosition");
    sdfLeftUiVisibleLocation_ = glGetUniformLocation(sdfProgram_, "uLeftUiVisible");
    sdfRightToolPositionLocation_ = glGetUniformLocation(sdfProgram_, "uRightToolPosition");
    sdfRightToolDirectionLocation_ = glGetUniformLocation(sdfProgram_, "uRightToolDirection");
    sdfRightToolVisibleLocation_ = glGetUniformLocation(sdfProgram_, "uRightToolVisible");
    sdfMenuHoverIndexLocation_ = glGetUniformLocation(sdfProgram_, "uMenuHoverIndex");
    sdfMenuPointerActiveLocation_ = glGetUniformLocation(sdfProgram_, "uMenuPointerActive");
    sdfMenuPointerStartLocation_ = glGetUniformLocation(sdfProgram_, "uMenuPointerStart");
    sdfMenuPointerEndLocation_ = glGetUniformLocation(sdfProgram_, "uMenuPointerEnd");
    const GLint samplerLocation = glGetUniformLocation(sdfProgram_, "uSdf");
    if (sdfCameraLocation_ < 0 || sdfViewRotationLocation_ < 0 || sdfFovTangentsLocation_ < 0 ||
        sdfVolumeMinLocation_ < 0 || sdfVolumeExtentLocation_ < 0 || sdfObjectPosLocation_ < 0 ||
        sdfObjectRotationLocation_ < 0 || sdfObjectInvRotationLocation_ < 0 || sdfObjectScaleLocation_ < 0 ||
        sdfBrushCenterLocation_ < 0 || sdfBrushRadiusLocation_ < 0 || sdfBrushVisibleLocation_ < 0 ||
        sdfTriggerValueLocation_ < 0 || sdfToolIndexLocation_ < 0 || sdfArEnabledLocation_ < 0 ||
        samplerLocation < 0) {
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

    glGenVertexArrays(1, &sdfVao_);
    glUseProgram(sdfProgram_);
    glUniform1i(samplerLocation, 0);
    glUseProgram(0);

    logInfo("SDF raymarch renderer ready: %dx%dx%d texture", size.x, size.y, size.z);
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
    glUniform3f(sdfObjectPosLocation_, objectPosition_.x, objectPosition_.y, objectPosition_.z);
    const std::array<float, 9> objectRotationMatrix = makeRotationMatrix3(objectRotation_);
    const std::array<float, 9> objectInvRotationMatrix = transposeMatrix3(objectRotationMatrix);
    glUniformMatrix3fv(sdfObjectRotationLocation_, 1, GL_FALSE, objectRotationMatrix.data());
    glUniformMatrix3fv(sdfObjectInvRotationLocation_, 1, GL_FALSE, objectInvRotationMatrix.data());
    glUniform1f(sdfObjectScaleLocation_, objectScale_);
    glUniform3f(sdfBrushCenterLocation_, brushHitWorld_.x, brushHitWorld_.y, brushHitWorld_.z);
    glUniform1f(sdfBrushRadiusLocation_, brushRadius_);
    glUniform1f(sdfBrushVisibleLocation_, brushVisible_ ? 1.0f : 0.0f);
    glUniform1f(sdfTriggerValueLocation_, rightTriggerValue_);
    glUniform1i(sdfToolIndexLocation_, displayToolIndex());
    glUniform1f(sdfBrushStrengthLocation_, brushStrength_);
    glUniform1f(sdfArEnabledLocation_, arModeEnabled_ ? 1.0f : 0.0f);
    glUniform1f(sdfMenuVisibleLocation_, menuVisible_ ? 1.0f : 0.0f);
    const bool leftUiVisible = leftGripPose_.active;
    const large::sdf::Vec3 uiPosition = leftUiPosition();
    glUniform3f(sdfLeftUiPositionLocation_, uiPosition.x, uiPosition.y, uiPosition.z);
    glUniform1f(sdfLeftUiVisibleLocation_, leftUiVisible ? 1.0f : 0.0f);
    glUniform3f(sdfRightToolPositionLocation_, rightToolPosition_.x, rightToolPosition_.y, rightToolPosition_.z);
    glUniform3f(sdfRightToolDirectionLocation_, rightToolDirection_.x, rightToolDirection_.y, rightToolDirection_.z);
    glUniform1f(sdfRightToolVisibleLocation_, rightToolVisible_ ? 1.0f : 0.0f);
    glUniform1i(sdfMenuHoverIndexLocation_, menuHoverIndex_);
    glUniform1f(sdfMenuPointerActiveLocation_, menuPointerActive_ ? 1.0f : 0.0f);
    glUniform3f(sdfMenuPointerStartLocation_, menuPointerStart_.x, menuPointerStart_.y, menuPointerStart_.z);
    glUniform3f(sdfMenuPointerEndLocation_, menuPointerEnd_.x, menuPointerEnd_.y, menuPointerEnd_.z);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, sdfTexture_);
    glBindVertexArray(sdfVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_3D, 0);
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
uniform float uBrushStrength;
uniform float uTriggerValue;
uniform int uToolIndex;
uniform float uArEnabled;
uniform float uArAvailable;
uniform float uMenuVisible;
uniform vec3 uLeftUiPosition;
uniform float uLeftUiVisible;
uniform vec3 uRightToolPosition;
uniform vec3 uRightToolDirection;
uniform float uRightToolVisible;
uniform int uMenuHoverIndex;
uniform float uMenuPointerActive;
uniform vec3 uMenuPointerStart;
uniform vec3 uMenuPointerEnd;
uniform float uLeftHandVisible;
uniform float uRightHandVisible;
uniform vec3 uLeftHandJoints[26];
uniform vec3 uRightHandJoints[26];

void paint(inout vec3 color, inout float alpha, vec3 source, float sourceAlpha) {
  float a = clamp(sourceAlpha, 0.0, 1.0);
  float outAlpha = alpha + a * (1.0 - alpha);
  if (outAlpha > 0.0001) {
    color = (color * alpha * (1.0 - a) + source * a) / outAlpha;
  }
  alpha = outAlpha;
}

vec3 brushUiColor() {
  vec3 idleColor = vec3(0.25, 0.85, 1.0);
  vec3 activeColor = vec3(1.0, 0.60, 0.24);
  if (uToolIndex == 1) {
    idleColor = vec3(1.0, 0.35, 0.36);
    activeColor = vec3(1.0, 0.12, 0.18);
  } else if (uToolIndex == 2) {
    idleColor = vec3(0.40, 1.0, 0.62);
    activeColor = vec3(0.18, 0.95, 0.36);
  } else if (uToolIndex == 3) {
    idleColor = vec3(0.72, 0.48, 1.0);
    activeColor = vec3(0.95, 0.30, 1.0);
  }
  return mix(idleColor, activeColor, smoothstep(0.50, 0.80, uTriggerValue));
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
  return vec3(0.25, 0.85, 1.0);
}

float rectMask(vec2 frag, vec2 minP, vec2 maxP) {
  vec2 a = step(minP, frag);
  vec2 b = step(frag, maxP);
  return a.x * a.y * b.x * b.y;
}

float rectEdge(vec2 frag, vec2 minP, vec2 maxP, float width) {
  float outer = rectMask(frag, minP, maxP);
  float inner = rectMask(frag, minP + vec2(width), maxP - vec2(width));
  return max(outer - inner, 0.0);
}

void applyHudAtFrag(inout vec3 color, inout float alpha, vec2 frag) {
  float panel = rectMask(frag, vec2(16.0, 16.0), vec2(248.0, 112.0));
  paint(color, alpha, vec3(0.018, 0.022, 0.026), panel * 0.78);
  paint(color, alpha, vec3(0.30, 0.36, 0.40), rectEdge(frag, vec2(16.0, 16.0), vec2(248.0, 112.0), 3.0) * 0.70);

  vec3 toolColor = brushUiColor();
  paint(color, alpha, toolColor, rectMask(frag, vec2(28.0, 28.0), vec2(226.0, 44.0)) * 0.86);

  float sizeBack = rectMask(frag, vec2(54.0, 56.0), vec2(72.0, 104.0));
  float powerBack = rectMask(frag, vec2(104.0, 84.0), vec2(226.0, 98.0));
  paint(color, alpha, vec3(0.08, 0.10, 0.12), max(sizeBack, powerBack) * 0.90);

  float sizeNorm = clamp((uBrushRadius - 0.015) / (0.60 - 0.015), 0.0, 1.0);
  float powerNorm = clamp(uBrushStrength, 0.0, 1.0);
  float sizeTop = mix(104.0, 56.0, sizeNorm);
  paint(color, alpha, vec3(0.25, 0.85, 1.0), rectMask(frag, vec2(54.0, sizeTop), vec2(72.0, 104.0)) * 0.95);
  paint(color, alpha, vec3(1.0, 0.60, 0.24), rectMask(frag, vec2(104.0, 84.0), vec2(104.0 + 122.0 * powerNorm, 98.0)) * 0.95);

  float menuOn = step(0.5, uMenuVisible);
  vec2 arCenter = vec2(220.0, 84.0);
  float arDist = length(frag - arCenter);
  float arFill = (1.0 - smoothstep(17.0, 18.5, arDist)) * menuOn;
  float arRing = (1.0 - smoothstep(1.8, 3.8, abs(arDist - 18.0))) * menuOn;
  float arDot = (1.0 - smoothstep(4.0, 8.0, arDist)) * menuOn * step(0.5, uArEnabled);
  float arHover = (uMenuHoverIndex == 8 ? 1.0 : 0.0) * arFill;
  vec3 arBase = mix(vec3(0.16, 0.18, 0.20), vec3(0.24, 0.70, 0.58), step(0.5, uArEnabled));
  arBase = mix(vec3(0.10, 0.11, 0.12), arBase, step(0.5, uArAvailable));
  paint(color, alpha, arBase, arFill * 0.92);
  paint(color, alpha, vec3(0.74, 0.88, 0.92), arRing * (0.60 + arHover * 0.35));
  paint(color, alpha, vec3(0.72, 1.0, 0.84), arDot * 0.90);

  float menuPanel = rectMask(frag, vec2(16.0, 120.0), vec2(248.0, 376.0)) * menuOn;
  paint(color, alpha, vec3(0.016, 0.019, 0.023), menuPanel * 0.88);
  paint(color, alpha, vec3(0.30, 0.36, 0.40), rectEdge(frag, vec2(16.0, 120.0), vec2(248.0, 376.0), 3.0) * menuOn * 0.70);

  for (int i = 0; i < 8; ++i) {
    float top = 132.0 + float(i) * 29.0;
    float row = rectMask(frag, vec2(28.0, top), vec2(226.0, top + 23.0));
    vec3 rowColor = i < 4 ? toolPalette(i) : vec3(0.26, 0.30, 0.34);
    float rowAlpha = i < 4 ? 0.42 : 0.72;
    paint(color, alpha, rowColor, row * menuOn * rowAlpha);
    float activeRow = (i == uToolIndex && i < 4) ? 1.0 : 0.0;
    float hovered = (i == uMenuHoverIndex) ? 1.0 : 0.0;
    paint(color, alpha, rowColor, row * menuOn * activeRow * 0.46);
    paint(color, alpha, vec3(0.78, 0.90, 0.96), row * menuOn * hovered * 0.46);
    paint(color, alpha, vec3(0.82, 0.88, 0.92), rectEdge(frag, vec2(28.0, top), vec2(226.0, top + 23.0), 2.0) * menuOn * (0.18 + activeRow * 0.34));
  }
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
  float contentWidth = 264.0;
  float contentHeight = mix(128.0, 392.0, step(0.5, uMenuVisible));
  float panelWidth = 0.34;
  float panelHeight = panelWidth * contentHeight / contentWidth;
  if (abs(local.x) > panelWidth * 0.5 || abs(local.y) > panelHeight * 0.5) {
    return;
  }

  vec2 frag = vec2((local.x / panelWidth + 0.5) * contentWidth,
                   (0.5 - local.y / panelHeight) * contentHeight);
  applyHudAtFrag(color, alpha, frag);
}

void applyRightHandTool(inout vec3 color, inout float alpha, vec3 rayOrigin, vec3 rayDir) {
  if (uRightToolVisible < 0.5 || uMenuPointerActive > 0.5) {
    return;
  }

  vec3 toolDir = normalize(uRightToolDirection);
  vec3 tip = uRightToolPosition;
  vec3 base = tip - toolDir * 0.185;
  vec3 toolColor = brushUiColor();

  float shaftT = 0.0;
  float shaftDistance = raySegmentDistance(rayOrigin, rayDir, base, tip, shaftT);
  float shaft = 1.0 - smoothstep(0.006, 0.014, shaftDistance);

  float headT = 0.0;
  float headDistance = rayPointDistance(rayOrigin, rayDir, tip, headT);
  float headRadius = max(uBrushRadius, 0.010);
  float fill = 1.0 - smoothstep(headRadius * 0.72, headRadius, headDistance);
  float shell = 1.0 - smoothstep(0.004, 0.012, abs(headDistance - headRadius));
  float center = 1.0 - smoothstep(0.006, 0.014, headDistance);

  paint(color, alpha, mix(vec3(0.74, 0.80, 0.86), toolColor, 0.35), shaft * 0.56);
  paint(color, alpha, toolColor, fill * 0.08);
  paint(color, alpha, toolColor, shell * 0.58);
  paint(color, alpha, vec3(0.92, 0.96, 1.0), center * 0.42);
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
  applyHandBone(color, alpha, rayOrigin, rayDir, handJoint(hand, a), handJoint(hand, b), boneColor, 0.010, 0.74);
}

void applyTrackedHand(inout vec3 color, inout float alpha, vec3 rayOrigin, vec3 rayDir, int hand) {
  float visible = hand == 0 ? uLeftHandVisible : uRightHandVisible;
  if (visible < 0.5) {
    return;
  }

  vec3 baseColor = hand == 0 ? vec3(0.54, 0.95, 1.0) : toolPalette(uToolIndex);
  vec3 jointColor = mix(baseColor, vec3(1.0), 0.28);
  vec3 palmColor = mix(baseColor, vec3(0.88, 0.94, 1.0), 0.42);

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

  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 0), palmColor, 0.030, 0.42);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 5), jointColor, 0.016, 0.76);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 10), jointColor, 0.016, 0.76);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 15), jointColor, 0.016, 0.76);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 20), jointColor, 0.016, 0.76);
  applyHandJoint(color, alpha, rayOrigin, rayDir, handJoint(hand, 25), jointColor, 0.016, 0.76);
}

void main() {
  float x = mix(uFovTangents.x, uFovTangents.y, vUv.x);
  float y = mix(uFovTangents.z, uFovTangents.w, vUv.y);
  vec3 rayDir = normalize(uViewRotation * normalize(vec3(x, y, -1.0)));
  vec3 rayOrigin = uCameraPos;
  vec3 color = vec3(0.0);
  float alpha = 0.0;
  applyMenuPointerLine(color, alpha, rayOrigin, rayDir);
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
    uiBrushStrengthLocation_ = glGetUniformLocation(uiProgram_, "uBrushStrength");
    uiTriggerValueLocation_ = glGetUniformLocation(uiProgram_, "uTriggerValue");
    uiToolIndexLocation_ = glGetUniformLocation(uiProgram_, "uToolIndex");
    uiArEnabledLocation_ = glGetUniformLocation(uiProgram_, "uArEnabled");
    uiArAvailableLocation_ = glGetUniformLocation(uiProgram_, "uArAvailable");
    uiMenuVisibleLocation_ = glGetUniformLocation(uiProgram_, "uMenuVisible");
    uiLeftUiPositionLocation_ = glGetUniformLocation(uiProgram_, "uLeftUiPosition");
    uiLeftUiVisibleLocation_ = glGetUniformLocation(uiProgram_, "uLeftUiVisible");
    uiRightToolPositionLocation_ = glGetUniformLocation(uiProgram_, "uRightToolPosition");
    uiRightToolDirectionLocation_ = glGetUniformLocation(uiProgram_, "uRightToolDirection");
    uiRightToolVisibleLocation_ = glGetUniformLocation(uiProgram_, "uRightToolVisible");
    uiMenuHoverIndexLocation_ = glGetUniformLocation(uiProgram_, "uMenuHoverIndex");
    uiMenuPointerActiveLocation_ = glGetUniformLocation(uiProgram_, "uMenuPointerActive");
    uiMenuPointerStartLocation_ = glGetUniformLocation(uiProgram_, "uMenuPointerStart");
    uiMenuPointerEndLocation_ = glGetUniformLocation(uiProgram_, "uMenuPointerEnd");
    uiLeftHandVisibleLocation_ = glGetUniformLocation(uiProgram_, "uLeftHandVisible");
    uiRightHandVisibleLocation_ = glGetUniformLocation(uiProgram_, "uRightHandVisible");
    uiLeftHandJointsLocation_ = glGetUniformLocation(uiProgram_, "uLeftHandJoints[0]");
    uiRightHandJointsLocation_ = glGetUniformLocation(uiProgram_, "uRightHandJoints[0]");

    if (uiCameraLocation_ < 0 || uiViewRotationLocation_ < 0 || uiFovTangentsLocation_ < 0 ||
        uiBrushRadiusLocation_ < 0 || uiBrushStrengthLocation_ < 0 || uiTriggerValueLocation_ < 0 ||
        uiToolIndexLocation_ < 0 || uiArEnabledLocation_ < 0 || uiArAvailableLocation_ < 0 ||
        uiMenuVisibleLocation_ < 0 || uiLeftUiPositionLocation_ < 0 || uiLeftUiVisibleLocation_ < 0 ||
        uiRightToolPositionLocation_ < 0 || uiRightToolDirectionLocation_ < 0 ||
        uiRightToolVisibleLocation_ < 0 || uiMenuHoverIndexLocation_ < 0 || uiMenuPointerActiveLocation_ < 0 ||
        uiMenuPointerStartLocation_ < 0 || uiMenuPointerEndLocation_ < 0 || uiLeftHandVisibleLocation_ < 0 ||
        uiRightHandVisibleLocation_ < 0 || uiLeftHandJointsLocation_ < 0 || uiRightHandJointsLocation_ < 0) {
      logError("UI overlay shader uniforms are missing");
      return false;
    }

    glGenVertexArrays(1, &uiVao_);
    logInfo("UI overlay renderer ready");
    return true;
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
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(uiProgram_);
    glUniform3f(uiCameraLocation_, pose.position.x, pose.position.y, pose.position.z);
    glUniformMatrix3fv(uiViewRotationLocation_, 1, GL_FALSE, viewRotation.data());
    glUniform4f(uiFovTangentsLocation_,
                std::tan(views_[eye].fov.angleLeft),
                std::tan(views_[eye].fov.angleRight),
                std::tan(views_[eye].fov.angleDown),
                std::tan(views_[eye].fov.angleUp));
    glUniform1f(uiBrushRadiusLocation_, brushRadius_);
    glUniform1f(uiBrushStrengthLocation_, brushStrength_);
    glUniform1f(uiTriggerValueLocation_, rightTriggerValue_);
    glUniform1i(uiToolIndexLocation_, displayToolIndex());
    glUniform1f(uiArEnabledLocation_, arModeEnabled_ ? 1.0f : 0.0f);
    glUniform1f(uiArAvailableLocation_, passthroughReady_ ? 1.0f : 0.0f);
    glUniform1f(uiMenuVisibleLocation_, menuVisible_ ? 1.0f : 0.0f);
    glUniform3f(uiLeftUiPositionLocation_, uiPosition.x, uiPosition.y, uiPosition.z);
    glUniform1f(uiLeftUiVisibleLocation_, isLeftUiVisible() ? 1.0f : 0.0f);
    glUniform3f(uiRightToolPositionLocation_, rightToolPosition_.x, rightToolPosition_.y, rightToolPosition_.z);
    glUniform3f(uiRightToolDirectionLocation_, rightToolDirection_.x, rightToolDirection_.y, rightToolDirection_.z);
    glUniform1f(uiRightToolVisibleLocation_, rightToolVisible_ ? 1.0f : 0.0f);
    glUniform1i(uiMenuHoverIndexLocation_, menuHoverIndex_);
    glUniform1f(uiMenuPointerActiveLocation_, menuPointerActive_ ? 1.0f : 0.0f);
    glUniform3f(uiMenuPointerStartLocation_, menuPointerStart_.x, menuPointerStart_.y, menuPointerStart_.z);
    glUniform3f(uiMenuPointerEndLocation_, menuPointerEnd_.x, menuPointerEnd_.y, menuPointerEnd_.z);
    glUniform1f(uiLeftHandVisibleLocation_, leftHandState_.active ? 1.0f : 0.0f);
    glUniform1f(uiRightHandVisibleLocation_, rightHandState_.active ? 1.0f : 0.0f);
    glUniform3fv(uiLeftHandJointsLocation_, XR_HAND_JOINT_COUNT_EXT, leftHandJoints.data());
    glUniform3fv(uiRightHandJointsLocation_, XR_HAND_JOINT_COUNT_EXT, rightHandJoints.data());
    glBindVertexArray(uiVao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
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
  GLuint uiProgram_ = 0;
  GLuint uiVao_ = 0;
  GLint sdfCameraLocation_ = -1;
  GLint sdfViewRotationLocation_ = -1;
  GLint sdfFovTangentsLocation_ = -1;
  GLint sdfVolumeMinLocation_ = -1;
  GLint sdfVolumeExtentLocation_ = -1;
  GLint sdfObjectPosLocation_ = -1;
  GLint sdfObjectRotationLocation_ = -1;
  GLint sdfObjectInvRotationLocation_ = -1;
  GLint sdfObjectScaleLocation_ = -1;
  GLint sdfBrushCenterLocation_ = -1;
  GLint sdfBrushRadiusLocation_ = -1;
  GLint sdfBrushVisibleLocation_ = -1;
  GLint sdfTriggerValueLocation_ = -1;
  GLint sdfToolIndexLocation_ = -1;
  GLint sdfBrushStrengthLocation_ = -1;
  GLint sdfArEnabledLocation_ = -1;
  GLint sdfMenuVisibleLocation_ = -1;
  GLint sdfLeftUiPositionLocation_ = -1;
  GLint sdfLeftUiVisibleLocation_ = -1;
  GLint sdfRightToolPositionLocation_ = -1;
  GLint sdfRightToolDirectionLocation_ = -1;
  GLint sdfRightToolVisibleLocation_ = -1;
  GLint sdfMenuHoverIndexLocation_ = -1;
  GLint sdfMenuPointerActiveLocation_ = -1;
  GLint sdfMenuPointerStartLocation_ = -1;
  GLint sdfMenuPointerEndLocation_ = -1;
  GLint uiCameraLocation_ = -1;
  GLint uiViewRotationLocation_ = -1;
  GLint uiFovTangentsLocation_ = -1;
  GLint uiBrushRadiusLocation_ = -1;
  GLint uiBrushStrengthLocation_ = -1;
  GLint uiTriggerValueLocation_ = -1;
  GLint uiToolIndexLocation_ = -1;
  GLint uiArEnabledLocation_ = -1;
  GLint uiArAvailableLocation_ = -1;
  GLint uiMenuVisibleLocation_ = -1;
  GLint uiLeftUiPositionLocation_ = -1;
  GLint uiLeftUiVisibleLocation_ = -1;
  GLint uiRightToolPositionLocation_ = -1;
  GLint uiRightToolDirectionLocation_ = -1;
  GLint uiRightToolVisibleLocation_ = -1;
  GLint uiMenuHoverIndexLocation_ = -1;
  GLint uiMenuPointerActiveLocation_ = -1;
  GLint uiMenuPointerStartLocation_ = -1;
  GLint uiMenuPointerEndLocation_ = -1;
  GLint uiLeftHandVisibleLocation_ = -1;
  GLint uiRightHandVisibleLocation_ = -1;
  GLint uiLeftHandJointsLocation_ = -1;
  GLint uiRightHandJointsLocation_ = -1;
  XrActionSet actionSet_ = XR_NULL_HANDLE;
  XrAction rightAimPoseAction_ = XR_NULL_HANDLE;
  XrAction rightTriggerAction_ = XR_NULL_HANDLE;
  XrAction gripPoseAction_ = XR_NULL_HANDLE;
  XrAction gripValueAction_ = XR_NULL_HANDLE;
  XrAction nextToolAction_ = XR_NULL_HANDLE;
  XrAction previousToolAction_ = XR_NULL_HANDLE;
  XrAction brushAdjustAction_ = XR_NULL_HANDLE;
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
  std::array<ToolSettings, kVrToolCount> toolSettings_{
      ToolSettings{},
      ToolSettings{},
      ToolSettings{},
      ToolSettings{},
  };
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
