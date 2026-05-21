#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/ObjExporter.h"
#include "core/SdfHistory.h"
#include "core/SdfVolume.h"

using DirectX::XMFLOAT4;
using large::sdf::BrushMode;
using large::sdf::IVec3;
using large::sdf::ObjExportStats;
using large::sdf::SdfHistory;
using large::sdf::SdfVolume;
using large::sdf::Vec3;
using large::sdf::clamp;
using large::sdf::clampInt;
using large::sdf::exportSdfSurfaceAsObj;
using large::sdf::normalize;

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 820;

struct ShaderParams {
  XMFLOAT4 volumeOrigin;   // xyz origin, w extent
  XMFLOAT4 cameraCenter;   // xyz center, w ray extent
  XMFLOAT4 cameraRight;    // xyz right, w aspect
  XMFLOAT4 cameraUp;       // xyz up, w half height
  XMFLOAT4 cameraForward;  // xyz forward, w voxel size
  XMFLOAT4 brush;          // xyz hit, w radius
  XMFLOAT4 flags;          // x hit valid, y tool, z/w unused
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
    textureDirty = true;
    status = L"Sphere SDF initialisee";
  }

  void changeResolution(int delta) {
    const int nextIndex = clampInt(resolutionIndex + delta, 0, static_cast<int>(resolutionPresets.size()) - 1);
    if (nextIndex == resolutionIndex) {
      return;
    }
    resolutionIndex = nextIndex;
    volume = volume.resampled(resolutionPresets[resolutionIndex]);
    history.clear();
    textureDirty = true;
    std::wstringstream ss;
    ss << L"Precision voxel " << volume.size().x << L"^3";
    status = ss.str();
  }

  SdfVolume volume;
  SdfHistory history;
  std::vector<int> resolutionPresets{48, 72, 96};
  int resolutionIndex = 1;
  BrushMode mode = BrushMode::Add;
  float brushRadius = 0.12f;
  float sculptStrength = 0.18f;
  float smoothStrength = 0.35f;
  float stretchStrength = 0.85f;
  float viewYaw = 0.72f;
  float viewPitch = -0.32f;
  float viewZoom = 1.0f;
  bool leftDown = false;
  bool rightDown = false;
  bool textureDirty = true;
  bool brushHitValid = false;
  Vec3 brushHit{};
  std::unique_ptr<SdfVolume> strokeSurface;
  Vec3 stretchAnchor{};
  POINT stretchStartMouse{0, 0};
  POINT lastMouse{0, 0};
  std::wstring status;
};

std::unique_ptr<AppState> gApp;
HWND gWindow = nullptr;
ID3D11Device* gDevice = nullptr;
ID3D11DeviceContext* gContext = nullptr;
IDXGISwapChain* gSwapChain = nullptr;
ID3D11RenderTargetView* gRenderTarget = nullptr;
ID3D11VertexShader* gVertexShader = nullptr;
ID3D11PixelShader* gPixelShader = nullptr;
ID3D11Buffer* gConstantBuffer = nullptr;
ID3D11Texture3D* gSdfTexture = nullptr;
ID3D11ShaderResourceView* gSdfSrv = nullptr;
ID3D11SamplerState* gSampler = nullptr;

std::wstring modeName(BrushMode mode) {
  if (mode == BrushMode::Add) {
    return L"Add";
  }
  if (mode == BrushMode::Subtract) {
    return L"Subtract";
  }
  if (mode == BrushMode::Stretch) {
    return L"Stretch";
  }
  return L"Smooth";
}

void releaseTexture() {
  if (gSdfSrv) {
    gSdfSrv->Release();
    gSdfSrv = nullptr;
  }
  if (gSdfTexture) {
    gSdfTexture->Release();
    gSdfTexture = nullptr;
  }
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

Vec3 cameraRight() {
  return normalize(unrotateFromView({1.0f, 0.0f, 0.0f}, gApp->viewYaw, gApp->viewPitch));
}

Vec3 cameraUp() {
  return normalize(unrotateFromView({0.0f, 1.0f, 0.0f}, gApp->viewYaw, gApp->viewPitch));
}

Vec3 cameraForward() {
  return normalize(unrotateFromView({0.0f, 0.0f, -1.0f}, gApp->viewYaw, gApp->viewPitch));
}

float halfViewHeight() {
  return gApp->volume.worldExtent() * 0.64f / gApp->viewZoom;
}

bool compileShader(const char* source, const char* entry, const char* target, ID3DBlob** blob) {
  ID3DBlob* error = nullptr;
  const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, blob, &error);
  if (error) {
    OutputDebugStringA(static_cast<const char*>(error->GetBufferPointer()));
    error->Release();
  }
  return SUCCEEDED(hr);
}

void updateWindowTitle() {
  std::wstringstream ss;
  ss << L"Large SDF GPU Preview - " << modeName(gApp->mode)
     << L" | brush " << static_cast<int>(gApp->brushRadius * 100.0f) << L"cm"
     << L" | force " << static_cast<int>(gApp->sculptStrength * 100.0f) << L"%"
     << L" | smooth " << static_cast<int>(gApp->smoothStrength * 100.0f) << L"%"
     << L" | stretch " << static_cast<int>(gApp->stretchStrength * 100.0f) << L"%"
     << L" | voxel " << gApp->volume.size().x << L"^3"
     << L" | zoom " << static_cast<int>(gApp->viewZoom * 100.0f) << L"%"
     << L" | " << gApp->status;
  SetWindowTextW(gWindow, ss.str().c_str());
}

bool uploadSdfTexture() {
  if (!gApp->textureDirty) {
    return true;
  }

  releaseTexture();
  const IVec3 size = gApp->volume.size();
  D3D11_TEXTURE3D_DESC desc{};
  desc.Width = static_cast<UINT>(size.x);
  desc.Height = static_cast<UINT>(size.y);
  desc.Depth = static_cast<UINT>(size.z);
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA data{};
  data.pSysMem = gApp->volume.values().data();
  data.SysMemPitch = static_cast<UINT>(size.x * sizeof(float));
  data.SysMemSlicePitch = static_cast<UINT>(size.x * size.y * sizeof(float));
  if (FAILED(gDevice->CreateTexture3D(&desc, &data, &gSdfTexture))) {
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = desc.Format;
  srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
  srvDesc.Texture3D.MipLevels = 1;
  if (FAILED(gDevice->CreateShaderResourceView(gSdfTexture, &srvDesc, &gSdfSrv))) {
    return false;
  }

  gApp->textureDirty = false;
  return true;
}

ShaderParams makeParams() {
  const Vec3 origin = gApp->volume.origin();
  const Vec3 center = gApp->volume.boundsCenter();
  const Vec3 right = cameraRight();
  const Vec3 up = cameraUp();
  const Vec3 forward = cameraForward();
  const float extent = gApp->volume.worldExtent();
  const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
  const float rayExtent = extent * 0.86f;
  const int tool = gApp->mode == BrushMode::Add ? 0 : (gApp->mode == BrushMode::Subtract ? 1 : (gApp->mode == BrushMode::Smooth ? 2 : 3));

  ShaderParams p{};
  p.volumeOrigin = XMFLOAT4(origin.x, origin.y, origin.z, extent);
  p.cameraCenter = XMFLOAT4(center.x, center.y, center.z, rayExtent);
  p.cameraRight = XMFLOAT4(right.x, right.y, right.z, aspect);
  p.cameraUp = XMFLOAT4(up.x, up.y, up.z, halfViewHeight());
  p.cameraForward = XMFLOAT4(forward.x, forward.y, forward.z, gApp->volume.voxelSize());
  p.brush = XMFLOAT4(gApp->brushHit.x, gApp->brushHit.y, gApp->brushHit.z, gApp->brushRadius);
  p.flags = XMFLOAT4(gApp->brushHitValid ? 1.0f : 0.0f, static_cast<float>(tool), 0.0f, 0.0f);
  return p;
}

bool raycastSurface(const SdfVolume& volume, int px, int py, Vec3& hit) {
  const float aspect = static_cast<float>(kWidth) / static_cast<float>(kHeight);
  const float sx = ((static_cast<float>(px) / static_cast<float>(kWidth)) * 2.0f - 1.0f) * aspect * halfViewHeight();
  const float sy = (1.0f - (static_cast<float>(py) / static_cast<float>(kHeight)) * 2.0f) * halfViewHeight();
  const float rayExtent = volume.worldExtent() * 0.86f;
  const Vec3 center = volume.boundsCenter();
  const Vec3 right = cameraRight();
  const Vec3 up = cameraUp();
  const Vec3 forward = cameraForward();

  Vec3 previous = center + right * sx + up * sy - forward * rayExtent;
  float previousValue = volume.sample(previous);
  constexpr int steps = 160;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const Vec3 current = center + right * sx + up * sy - forward * rayExtent + forward * (2.0f * rayExtent * t);
    const float currentValue = volume.sample(current);
    if (previousValue > 0.0f && currentValue <= 0.0f) {
      const float isoT = previousValue / (previousValue - currentValue);
      hit = previous + (current - previous) * isoT;
      return true;
    }
    previous = current;
    previousValue = currentValue;
  }
  return false;
}

bool raycastSurface(int px, int py, Vec3& hit) {
  const SdfVolume& source = gApp->strokeSurface ? *gApp->strokeSurface : gApp->volume;
  return raycastSurface(source, px, py, hit);
}

void applyBrushAt(Vec3 p) {
  if (gApp->mode == BrushMode::Smooth) {
    gApp->volume.applySmoothBrush(p, gApp->brushRadius, gApp->smoothStrength);
  } else if (gApp->mode == BrushMode::Stretch) {
    return;
  } else {
    gApp->volume.applySphereBrush(p, gApp->brushRadius, gApp->mode, gApp->sculptStrength);
  }
  gApp->brushHit = p;
  gApp->brushHitValid = true;
  gApp->textureDirty = true;
  gApp->status = modeName(gApp->mode) + L" applique";
}

Vec3 dragDeltaFromMouse(int px, int py) {
  const float worldPerPixel = (2.0f * halfViewHeight()) / static_cast<float>(kHeight);
  const float dx = static_cast<float>(px - gApp->stretchStartMouse.x) * worldPerPixel;
  const float dy = static_cast<float>(py - gApp->stretchStartMouse.y) * worldPerPixel;
  return cameraRight() * dx + cameraUp() * -dy;
}

void applyStretchAtMouse(int px, int py) {
  if (!gApp->strokeSurface) {
    return;
  }
  const Vec3 delta = dragDeltaFromMouse(px, py);
  gApp->volume.applyStretchBrush(*gApp->strokeSurface, gApp->stretchAnchor, delta, gApp->brushRadius, gApp->stretchStrength);
  gApp->brushHit = gApp->stretchAnchor + delta * gApp->stretchStrength;
  gApp->brushHitValid = true;
  gApp->textureDirty = true;
  gApp->status = L"Stretch applique";
}

void sculptAtMouse(int px, int py) {
  if (gApp->mode == BrushMode::Stretch) {
    applyStretchAtMouse(px, py);
    return;
  }

  Vec3 hit{};
  if (!raycastSurface(px, py, hit)) {
    gApp->brushHitValid = false;
    return;
  }
  applyBrushAt(hit);
}

bool initD3D(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC swap{};
  swap.BufferCount = 2;
  swap.BufferDesc.Width = kWidth;
  swap.BufferDesc.Height = kHeight;
  swap.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap.OutputWindow = hwnd;
  swap.SampleDesc.Count = 1;
  swap.Windowed = TRUE;
  swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &swap, &gSwapChain, &gDevice, nullptr, &gContext))) {
    return false;
  }

  ID3D11Texture2D* backBuffer = nullptr;
  if (FAILED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
    return false;
  }
  gDevice->CreateRenderTargetView(backBuffer, nullptr, &gRenderTarget);
  backBuffer->Release();

  const char* shader = R"(
Texture3D<float> sdfTex : register(t0);
SamplerState sdfSampler : register(s0);

cbuffer Params : register(b0) {
  float4 volumeOrigin;
  float4 cameraCenter;
  float4 cameraRight;
  float4 cameraUp;
  float4 cameraForward;
  float4 brush;
  float4 flags;
};

struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

VSOut vsMain(uint id : SV_VertexID) {
  float2 pos[3] = {
    float2(-1.0, -1.0),
    float2(-1.0,  3.0),
    float2( 3.0, -1.0)
  };
  VSOut o;
  o.pos = float4(pos[id], 0.0, 1.0);
  o.uv = pos[id] * float2(0.5, -0.5) + 0.5;
  return o;
}

float sampleSdf(float3 world) {
  float3 uvw = (world - volumeOrigin.xyz) / volumeOrigin.w;
  if (any(uvw < 0.0) || any(uvw > 1.0)) {
    return volumeOrigin.w;
  }
  return sdfTex.SampleLevel(sdfSampler, uvw, 0);
}

float3 estimateNormal(float3 p) {
  float e = cameraForward.w;
  float3 n = float3(
    sampleSdf(p + float3(e, 0, 0)) - sampleSdf(p - float3(e, 0, 0)),
    sampleSdf(p + float3(0, e, 0)) - sampleSdf(p - float3(0, e, 0)),
    sampleSdf(p + float3(0, 0, e)) - sampleSdf(p - float3(0, 0, e))
  );
  return normalize(n);
}

float4 psMain(VSOut input) : SV_TARGET {
  float2 screen = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
  float viewX = screen.x * cameraRight.w * cameraUp.w;
  float viewY = screen.y * cameraUp.w;
  float3 origin = cameraCenter.xyz + cameraRight.xyz * viewX + cameraUp.xyz * viewY - cameraForward.xyz * cameraCenter.w;
  float3 dir = cameraForward.xyz;

  float rayLength = cameraCenter.w * 2.0;
  float stepSize = rayLength / 176.0;
  float3 prevP = origin;
  float prevV = sampleSdf(prevP);

  [loop]
  for (int i = 1; i <= 176; ++i) {
    float3 p = origin + dir * (stepSize * i);
    float v = sampleSdf(p);
    if (prevV > 0.0 && v <= 0.0) {
      float t = prevV / max(prevV - v, 0.00001);
      float3 hit = lerp(prevP, p, t);
      float3 n = estimateNormal(hit);
      float3 light = normalize(float3(-0.45, 0.65, -0.58));
      float lambert = saturate(dot(n, light)) * 0.68 + 0.28;
      float rim = pow(saturate(1.0 - abs(dot(n, -dir))), 2.0) * 0.22;
      float3 col = float3(0.52, 0.62, 0.74) * lambert + rim;

      if (flags.x > 0.5) {
        float d = distance(hit, brush.xyz);
        float band = abs(d - brush.w);
        if (band < cameraForward.w * 1.75) {
          float3 brushCol = flags.y < 0.5 ? float3(1.0, 0.76, 0.18) : (flags.y < 1.5 ? float3(0.25, 0.62, 1.0) : (flags.y < 2.5 ? float3(0.32, 1.0, 0.55) : float3(1.0, 0.42, 0.95)));
          col = lerp(col, brushCol, 0.85);
        } else if (d < brush.w) {
          col = lerp(col, float3(0.85, 0.90, 1.0), 0.12);
        }
      }

      return float4(col, 1.0);
    }
    prevP = p;
    prevV = v;
  }

  float g = 0.055 + input.uv.y * 0.05;
  return float4(g, g + 0.012, g + 0.024, 1.0);
}
)";

  ID3DBlob* vs = nullptr;
  ID3DBlob* ps = nullptr;
  if (!compileShader(shader, "vsMain", "vs_4_0", &vs) || !compileShader(shader, "psMain", "ps_4_0", &ps)) {
    return false;
  }
  gDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &gVertexShader);
  gDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &gPixelShader);
  vs->Release();
  ps->Release();

  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(ShaderParams);
  cb.Usage = D3D11_USAGE_DEFAULT;
  cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  if (FAILED(gDevice->CreateBuffer(&cb, nullptr, &gConstantBuffer))) {
    return false;
  }

  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler.MinLOD = 0;
  sampler.MaxLOD = 0;
  if (FAILED(gDevice->CreateSamplerState(&sampler, &gSampler))) {
    return false;
  }

  return uploadSdfTexture();
}

void render() {
  if (!uploadSdfTexture()) {
    return;
  }

  const ShaderParams params = makeParams();
  gContext->UpdateSubresource(gConstantBuffer, 0, nullptr, &params, 0, 0);

  FLOAT clear[] = {0.05f, 0.055f, 0.065f, 1.0f};
  gContext->ClearRenderTargetView(gRenderTarget, clear);
  gContext->OMSetRenderTargets(1, &gRenderTarget, nullptr);

  D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
  gContext->RSSetViewports(1, &viewport);
  gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  gContext->VSSetShader(gVertexShader, nullptr, 0);
  gContext->PSSetShader(gPixelShader, nullptr, 0);
  gContext->PSSetShaderResources(0, 1, &gSdfSrv);
  gContext->PSSetSamplers(0, 1, &gSampler);
  gContext->PSSetConstantBuffers(0, 1, &gConstantBuffer);
  gContext->Draw(3, 0);
  gSwapChain->Present(1, 0);
}

void releaseD3D() {
  releaseTexture();
  if (gSampler) gSampler->Release();
  if (gConstantBuffer) gConstantBuffer->Release();
  if (gPixelShader) gPixelShader->Release();
  if (gVertexShader) gVertexShader->Release();
  if (gRenderTarget) gRenderTarget->Release();
  if (gSwapChain) gSwapChain->Release();
  if (gContext) gContext->Release();
  if (gDevice) gDevice->Release();
}

void exportObj() {
  try {
    const ObjExportStats stats = exportSdfSurfaceAsObj(gApp->volume, std::filesystem::path("out") / "gpu_interactive_sculpt.obj");
    std::wstringstream ss;
    ss << L"Export OBJ " << stats.faces << L" faces";
    gApp->status = ss.str();
  } catch (...) {
    gApp->status = L"Export OBJ echoue";
  }
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lParam);
      const int y = GET_Y_LPARAM(lParam);
      if (gApp->rightDown) {
        const int dx = x - gApp->lastMouse.x;
        const int dy = y - gApp->lastMouse.y;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
          gApp->viewZoom = clamp(gApp->viewZoom * std::exp(static_cast<float>(-dy) * 0.01f), 0.35f, 3.5f);
          gApp->status = L"Zoom";
        } else {
          gApp->viewYaw += static_cast<float>(dx) * 0.01f;
          gApp->viewPitch = clamp(gApp->viewPitch + static_cast<float>(dy) * 0.01f, -1.35f, 1.35f);
          gApp->status = L"Rotation";
        }
        gApp->lastMouse = {x, y};
      } else if (gApp->leftDown) {
        sculptAtMouse(x, y);
      } else {
        Vec3 hit{};
        gApp->brushHitValid = raycastSurface(x, y, hit);
        if (gApp->brushHitValid) {
          gApp->brushHit = hit;
        }
      }
      return 0;
    }

    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        Vec3 hit{};
        if (!raycastSurface(x, y, hit)) {
          ReleaseCapture();
          return 0;
        }
        gApp->leftDown = true;
        gApp->history.capture();
        gApp->strokeSurface = std::make_unique<SdfVolume>(gApp->volume);
        if (gApp->mode == BrushMode::Stretch) {
          gApp->stretchAnchor = hit;
          gApp->stretchStartMouse = {x, y};
          gApp->brushHit = hit;
          gApp->brushHitValid = true;
          gApp->status = L"Stretch ancre";
        } else {
          applyBrushAt(hit);
        }
      }
      return 0;

    case WM_LBUTTONUP:
      gApp->leftDown = false;
      gApp->strokeSurface.reset();
      if (!gApp->rightDown) {
        ReleaseCapture();
      }
      return 0;

    case WM_RBUTTONDOWN:
      SetCapture(hwnd);
      gApp->rightDown = true;
      gApp->lastMouse = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      return 0;

    case WM_RBUTTONUP:
      gApp->rightDown = false;
      if (!gApp->leftDown) {
        ReleaseCapture();
      }
      return 0;

    case WM_MOUSEWHEEL: {
      const float scale = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.08f : 0.92f;
      gApp->brushRadius = clamp(gApp->brushRadius * scale, 0.025f, 0.55f);
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
      } else if (wParam == '6') {
        gApp->mode = BrushMode::Stretch;
        gApp->status = L"Outil Stretch";
      } else if (wParam == '4') {
        gApp->changeResolution(-1);
      } else if (wParam == '5') {
        gApp->changeResolution(1);
      } else if (wParam == VK_ADD || wParam == VK_OEM_PLUS) {
        if (gApp->mode == BrushMode::Smooth) {
          gApp->smoothStrength = clamp(gApp->smoothStrength + 0.05f, 0.05f, 1.0f);
        } else if (gApp->mode == BrushMode::Stretch) {
          gApp->stretchStrength = clamp(gApp->stretchStrength + 0.05f, 0.05f, 1.5f);
        } else {
          gApp->sculptStrength = clamp(gApp->sculptStrength + 0.03f, 0.03f, 1.0f);
        }
      } else if (wParam == VK_SUBTRACT || wParam == VK_OEM_MINUS) {
        if (gApp->mode == BrushMode::Smooth) {
          gApp->smoothStrength = clamp(gApp->smoothStrength - 0.05f, 0.05f, 1.0f);
        } else if (gApp->mode == BrushMode::Stretch) {
          gApp->stretchStrength = clamp(gApp->stretchStrength - 0.05f, 0.05f, 1.5f);
        } else {
          gApp->sculptStrength = clamp(gApp->sculptStrength - 0.03f, 0.03f, 1.0f);
        }
      } else if (wParam == 'Z') {
        if (gApp->history.undo()) {
          gApp->textureDirty = true;
          gApp->status = L"Undo";
        }
      } else if (wParam == 'Y') {
        if (gApp->history.redo()) {
          gApp->textureDirty = true;
          gApp->status = L"Redo";
        }
      } else if (wParam == 'R') {
        gApp->reset(true);
      } else if (wParam == 'P') {
        exportObj();
      }
      updateWindowTitle();
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
  gApp = std::make_unique<AppState>();

  WNDCLASSW wc{};
  wc.lpfnWndProc = windowProc;
  wc.hInstance = instance;
  wc.lpszClassName = L"LargeSdfGpuPreviewWindow";
  wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
  RegisterClassW(&wc);

  RECT rect{0, 0, kWidth, kHeight};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  gWindow = CreateWindowExW(0,
                            wc.lpszClassName,
                            L"Large SDF GPU Preview",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT,
                            CW_USEDEFAULT,
                            rect.right - rect.left,
                            rect.bottom - rect.top,
                            nullptr,
                            nullptr,
                            instance,
                            nullptr);
  if (!gWindow || !initD3D(gWindow)) {
    MessageBoxW(gWindow, L"Direct3D 11 initialization failed.", L"Large SDF", MB_ICONERROR);
    return 1;
  }

  ShowWindow(gWindow, showCmd);
  UpdateWindow(gWindow);
  updateWindowTitle();

  MSG msg{};
  bool running = true;
  while (running) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        running = false;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    render();
    updateWindowTitle();
  }

  releaseD3D();
  gApp.reset();
  return 0;
}
