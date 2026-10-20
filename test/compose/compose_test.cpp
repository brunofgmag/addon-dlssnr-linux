#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "../../src/nr_compose.hpp"

namespace {

using DirectX::PackedVector::HALF;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
using DirectX::PackedVector::XMFLOAT3PK;

constexpr DXGI_FORMAT kGameFormat = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr DXGI_FORMAT kModelFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr float kRatioFloor = 1.0f / 1024.0f;

struct Rgb {
  float r, g, b;
};

float Luma(const Rgb& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

float EncodeChannel(float x) {
  const float o = x > 0.0f ? x : 0.0f;
  return std::pow(o / (1.0f + o), 1.0f / 2.2f);
}

float DecodeChannel(float x) {
  const float p = x < 0.0f ? 0.0f : (x > 0.999f ? 0.999f : x);
  const float t = std::pow(p, 2.2f);
  return t / (1.0f - t);
}

struct Gpu {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  UINT64 fence_value = 0;
  HANDLE event = nullptr;
};

bool Fail(const char* what) {
  std::printf("FAIL: %s\n", what);
  return false;
}

bool CreateGpu(Gpu& g) {
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.device))))
    return Fail("D3D12CreateDevice");
  D3D12_COMMAND_QUEUE_DESC queue = {};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(g.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&g.queue)))) return Fail("queue");
  if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g.allocator))))
    return Fail("allocator");
  if (FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr,
                                         IID_PPV_ARGS(&g.list))))
    return Fail("command list");
  if (FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
    return Fail("fence");
  g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  return true;
}

void Submit(Gpu& g) {
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list};
  g.queue->ExecuteCommandLists(1, lists);
  g.queue->Signal(g.fence, ++g.fence_value);
  if (g.fence->GetCompletedValue() < g.fence_value) {
    g.fence->SetEventOnCompletion(g.fence_value, g.event);
    WaitForSingleObject(g.event, INFINITE);
  }
  g.allocator->Reset();
  g.list->Reset(g.allocator, nullptr);
}

ID3D12Resource* CreateTexture(Gpu& g, UINT w, UINT h, DXGI_FORMAT format,
                              D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = w;
  desc.Height = h;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ID3D12Resource* texture = nullptr;
  g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                    IID_PPV_ARGS(&texture));
  return texture;
}

ID3D12Resource* CreateBuffer(Gpu& g, D3D12_HEAP_TYPE type, UINT64 size,
                             D3D12_RESOURCE_STATES state) {
  D3D12_HEAP_PROPERTIES heap = {};
  heap.Type = type;
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = size;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* buffer = nullptr;
  g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                    IID_PPV_ARGS(&buffer));
  return buffer;
}

void Upload(Gpu& g, ID3D12Resource* texture, const void* pixels, UINT pixel_bytes,
            D3D12_RESOURCE_STATES state) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT64 total = 0;
  g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  ID3D12Resource* staging =
      CreateBuffer(g, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  unsigned char* mapped = nullptr;
  staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  const UINT row = (UINT)desc.Width * pixel_bytes;
  for (UINT y = 0; y < desc.Height; ++y)
    std::memcpy(mapped + footprint.Offset + (UINT64)y * footprint.Footprint.RowPitch,
                static_cast<const unsigned char*>(pixels) + (UINT64)y * row, row);
  staging->Unmap(0, nullptr);

  const bool transition = state != D3D12_RESOURCE_STATE_COPY_DEST;
  if (transition) nr_compose::Barrier(g.list, texture, state, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  D3D12_TEXTURE_COPY_LOCATION src = {staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  src.PlacedFootprint = footprint;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  if (transition) nr_compose::Barrier(g.list, texture, D3D12_RESOURCE_STATE_COPY_DEST, state);
  Submit(g);
  staging->Release();
}

std::vector<unsigned char> Download(Gpu& g, ID3D12Resource* texture, UINT pixel_bytes,
                                    D3D12_RESOURCE_STATES state) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
  UINT64 total = 0;
  g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  ID3D12Resource* staging =
      CreateBuffer(g, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);

  nr_compose::Barrier(g.list, texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION src = {texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  D3D12_TEXTURE_COPY_LOCATION dst = {staging, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  dst.PlacedFootprint = footprint;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  nr_compose::Barrier(g.list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
  Submit(g);

  const UINT row = (UINT)desc.Width * pixel_bytes;
  std::vector<unsigned char> pixels((size_t)row * desc.Height);
  unsigned char* mapped = nullptr;
  staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < desc.Height; ++y)
    std::memcpy(pixels.data() + (size_t)y * row,
                mapped + footprint.Offset + (UINT64)y * footprint.Footprint.RowPitch, row);
  staging->Unmap(0, nullptr);
  staging->Release();
  return pixels;
}

std::vector<Rgb> UnpackGame(const std::vector<unsigned char>& bytes) {
  std::vector<Rgb> out(bytes.size() / 4);
  for (size_t i = 0; i < out.size(); ++i) {
    XMFLOAT3PK packed;
    std::memcpy(&packed, bytes.data() + i * 4, 4);
    DirectX::XMFLOAT3 f;
    DirectX::XMStoreFloat3(&f, DirectX::PackedVector::XMLoadFloat3PK(&packed));
    out[i] = {f.x, f.y, f.z};
  }
  return out;
}

std::vector<unsigned char> PackGame(const std::vector<Rgb>& pixels) {
  std::vector<unsigned char> bytes(pixels.size() * 4);
  for (size_t i = 0; i < pixels.size(); ++i) {
    const DirectX::XMFLOAT3 f = {pixels[i].r, pixels[i].g, pixels[i].b};
    XMFLOAT3PK packed;
    DirectX::PackedVector::XMStoreFloat3PK(&packed, DirectX::XMLoadFloat3(&f));
    std::memcpy(bytes.data() + i * 4, &packed, 4);
  }
  return bytes;
}

std::vector<Rgb> UnpackModel(const std::vector<unsigned char>& bytes) {
  std::vector<Rgb> out(bytes.size() / 8);
  for (size_t i = 0; i < out.size(); ++i) {
    HALF h[4];
    std::memcpy(h, bytes.data() + i * 8, 8);
    out[i] = {XMConvertHalfToFloat(h[0]), XMConvertHalfToFloat(h[1]), XMConvertHalfToFloat(h[2])};
  }
  return out;
}

std::vector<unsigned char> PackModel(const std::vector<Rgb>& pixels) {
  std::vector<unsigned char> bytes(pixels.size() * 8);
  for (size_t i = 0; i < pixels.size(); ++i) {
    const HALF h[4] = {XMConvertFloatToHalf(pixels[i].r), XMConvertFloatToHalf(pixels[i].g),
                       XMConvertFloatToHalf(pixels[i].b), XMConvertFloatToHalf(1.0f)};
    std::memcpy(bytes.data() + i * 8, h, 8);
  }
  return bytes;
}

constexpr UINT kEdgeX = 129;

std::vector<Rgb> Pattern(UINT w, UINT h) {
  std::vector<Rgb> pixels((size_t)w * h);
  for (UINT y = 0; y < h; ++y) {
    for (UINT x = 0; x < w; ++x) {
      const float level = x < kEdgeX ? 0.05f : 2.0f;
      const float texture = 1.0f + 0.2f * std::sin(x * 0.7f) * std::cos(y * 0.5f);
      Rgb c = {level * texture, 0.8f * level * texture, 0.6f * level * texture};
      if (y == 7) c = {0.0f, 0.0f, 0.0f};
      if (y == 9) c = {1e-5f, 1e-5f, 1e-5f};
      pixels[(size_t)y * w + x] = c;
    }
  }
  return pixels;
}

struct ModelView {
  const std::vector<Rgb>& linear;
  UINT w, h;
  const Rgb& At(int x, int y) const {
    return linear[(size_t)std::clamp(y, 0, (int)h - 1) * w + std::clamp(x, 0, (int)w - 1)];
  }
};

using ModelFn = std::function<float(const ModelView& proxy, UINT x, UINT y)>;

struct CaseResult {
  std::vector<Rgb> original;
  std::vector<Rgb> out;
};

uint32_t g_frame = 0;

bool RunCase(Gpu& g, const std::vector<Rgb>& scene, UINT w, UINT h, UINT work_w, UINT work_h,
             const ModelFn& gain, float colour, CaseResult& result) {
  const std::vector<unsigned char> packed = PackGame(scene);
  result.original = UnpackGame(packed);

  ID3D12Resource* game_out = CreateTexture(g, w, h, kGameFormat, D3D12_RESOURCE_STATE_COPY_DEST);
  ID3D12Resource* color_copy =
      CreateTexture(g, w, h, kGameFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* proxy =
      CreateTexture(g, work_w, work_h, kModelFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* model =
      CreateTexture(g, work_w, work_h, kModelFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (!game_out || !color_copy || !proxy || !model) return Fail("textures");
  Upload(g, game_out, packed.data(), 4, D3D12_RESOURCE_STATE_COPY_DEST);
  nr_compose::Barrier(g.list, game_out, D3D12_RESOURCE_STATE_COPY_DEST,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  const uint32_t frame = g_frame++;
  if (!nr_compose::RecordPre(g.list, game_out, color_copy, proxy, w, h, work_w, work_h, frame,
                             1.0f))
    return Fail("RecordPre");
  Submit(g);

  const std::vector<Rgb> proxy_pixels =
      UnpackModel(Download(g, proxy, 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
  std::vector<Rgb> proxy_linear(proxy_pixels.size());
  for (size_t i = 0; i < proxy_linear.size(); ++i)
    proxy_linear[i] = {DecodeChannel(proxy_pixels[i].r), DecodeChannel(proxy_pixels[i].g),
                       DecodeChannel(proxy_pixels[i].b)};
  const ModelView view = {proxy_linear, work_w, work_h};
  std::vector<Rgb> answer(proxy_pixels.size());
  for (UINT y = 0; y < work_h; ++y) {
    for (UINT x = 0; x < work_w; ++x) {
      const size_t i = (size_t)y * work_w + x;
      const Rgb& linear = proxy_linear[i];
      const float k = gain(view, x, y);
      answer[i] = k == 1.0f ? proxy_pixels[i]
                            : Rgb{EncodeChannel(linear.r * k), EncodeChannel(linear.g * k),
                                  EncodeChannel(linear.b * k)};
    }
  }
  const std::vector<unsigned char> answer_bytes = PackModel(answer);
  Upload(g, model, answer_bytes.data(), 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  nr_compose::RecordPost(g.list, game_out, color_copy, proxy, model, w, h, work_w, work_h, frame,
                         1.0f, 2.0f, colour, 1.0f, 0);
  Submit(g);
  result.out = UnpackGame(Download(g, game_out, 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

  game_out->Release();
  color_copy->Release();
  proxy->Release();
  model->Release();
  return true;
}

bool Measurable(const Rgb& c) {
  return c.r >= kRatioFloor && c.g >= kRatioFloor && c.b >= kRatioFloor;
}

float WorstRatioError(const CaseResult& r, float expected) {
  float worst = 0.0f;
  size_t at = 0;
  for (size_t i = 0; i < r.out.size(); ++i) {
    const Rgb& o = r.original[i];
    if (!Measurable(o)) continue;
    const float ratio = Luma(r.out[i]) / Luma(o);
    const float error = std::fabs(ratio - expected) / expected;
    if (error > worst) {
      worst = error;
      at = i;
    }
  }
  const Rgb& o = r.original[at];
  const Rgb& out = r.out[at];
  std::printf("  worst pixel %zu: original %.6f %.6f %.6f, out %.6f %.6f %.6f\n", at, o.r, o.g,
              o.b, out.r, out.g, out.b);
  return worst;
}

float Ulp(float value, int mantissa_bits) {
  return std::ldexp(1.0f, std::ilogb(value) - mantissa_bits);
}

size_t PixelsOffByMoreThanOneStep(const CaseResult& r) {
  size_t off = 0;
  for (size_t i = 0; i < r.out.size(); ++i) {
    const Rgb& o = r.original[i];
    if (!Measurable(o)) continue;
    const Rgb& out = r.out[i];
    if (std::fabs(out.r - o.r) > Ulp(o.r, 6) || std::fabs(out.g - o.g) > Ulp(o.g, 6) ||
        std::fabs(out.b - o.b) > Ulp(o.b, 5))
      ++off;
  }
  return off;
}

bool FloorPixelsHeld(const CaseResult& r, UINT w) {
  for (UINT x = 0; x < w; ++x) {
    const Rgb& black = r.out[(size_t)7 * w + x];
    if (black.r != 0.0f || black.g != 0.0f || black.b != 0.0f) return false;
  }
  return true;
}

float ColumnRatio(const CaseResult& r, UINT w, UINT h, UINT x) {
  float sum = 0.0f;
  UINT n = 0;
  for (UINT y = 12; y + 12 < h; ++y) {
    const size_t i = (size_t)y * w + x;
    if (!Measurable(r.original[i])) continue;
    sum += Luma(r.out[i]) / Luma(r.original[i]);
    ++n;
  }
  return n ? sum / n : 0.0f;
}

bool Check(const char* name, bool ok, const char* detail) {
  std::printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, detail);
  return ok;
}

bool Correctness(Gpu& g) {
  constexpr UINT w = 257, h = 163;
  const ModelFn identity = [](const ModelView&, UINT, UINT) { return 1.0f; };
  const ModelFn brighter = [](const ModelView&, UINT, UINT) { return 1.5f; };
  const ModelFn bright_only = [](const ModelView& p, UINT x, UINT y) {
    return Luma(p.At(x, y)) > 0.5f ? 1.5f : 1.0f;
  };
  struct Scale {
    const char* label;
    UINT work_w, work_h;
  } scales[] = {{"1.00", w, h}, {"0.50", 129, 82}, {"0.75", 193, 122}};

  const std::vector<Rgb> pattern = Pattern(w, h);
  bool all = true;
  char detail[160];
  for (const Scale& s : scales) {
    CaseResult r;
    if (!RunCase(g, pattern, w, h, s.work_w, s.work_h, identity, 0.0f, r)) return false;
    float worst = WorstRatioError(r, 1.0f);
    const size_t off = PixelsOffByMoreThanOneStep(r);
    std::snprintf(detail, sizeof(detail),
                  "worst luminance error %.5f, pixels more than one format step away %zu", worst,
                  off);
    char name[64];
    std::snprintf(name, sizeof(name), "identity model, scale %s", s.label);
    all &= Check(name, worst < 0.02f && off == 0, detail);

    if (!RunCase(g, pattern, w, h, s.work_w, s.work_h, brighter, 0.0f, r)) return false;
    worst = WorstRatioError(r, 1.5f);
    std::snprintf(detail, sizeof(detail), "worst gain error %.5f, black row held %s", worst,
                  FloorPixelsHeld(r, w) ? "yes" : "no");
    std::snprintf(name, sizeof(name), "x1.5 model, scale %s", s.label);
    all &= Check(name, worst < 0.02f && FloorPixelsHeld(r, w), detail);
  }

  CaseResult r;
  if (!RunCase(g, pattern, w, h, 129, 82, bright_only, 0.0f, r)) return false;
  const float dark_next = ColumnRatio(r, w, h, kEdgeX - 1);
  const float dark_far = ColumnRatio(r, w, h, kEdgeX - 6);
  const float bright_next = ColumnRatio(r, w, h, kEdgeX);
  const float bright_far = ColumnRatio(r, w, h, kEdgeX + 6);
  std::snprintf(detail, sizeof(detail), "dark side %.3f (far %.3f), bright side %.3f (far %.3f)",
                dark_next, dark_far, bright_next, bright_far);
  all &= Check("edit stays on its side of an edge, scale 0.50",
               dark_next < 1.05f && dark_far < 1.01f && bright_next > 1.45f &&
                   bright_far > 1.49f,
               detail);
  return all;
}

uint32_t Hash(uint32_t x, uint32_t y, uint32_t salt) {
  uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ salt * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  return h;
}

float Unit(uint32_t x, uint32_t y, uint32_t salt) {
  return float(Hash(x, y, salt) & 0xffffffu) / 16777215.0f;
}

constexpr UINT kPanW = 256, kPanH = 160, kPanFrames = 8, kPanMargin = 8;

float PanDepth(UINT u) { return std::exp2(-2.0f - 9.0f * float(u) / float(kPanW)); }

std::vector<Rgb> PanFrame(UINT frame) {
  std::vector<Rgb> pixels((size_t)kPanW * kPanH);
  for (UINT y = 0; y < kPanH; ++y) {
    for (UINT x = 0; x < kPanW; ++x) {
      const UINT u = x + frame;
      const float level = PanDepth(u) * std::exp2(2.0f * Unit(u, y, 1) - 1.0f);
      pixels[(size_t)y * kPanW + x] = {level, 0.9f * level, 0.8f * level};
    }
  }
  return pixels;
}

struct Spread {
  double mean = 0.0, p99 = 0.0;
};

Spread Summarise(std::vector<double>& values) {
  Spread s;
  if (values.empty()) return s;
  for (double v : values) s.mean += v;
  s.mean /= double(values.size());
  std::sort(values.begin(), values.end());
  s.p99 = values[std::min(values.size() - 1, size_t(0.99 * double(values.size())))];
  return s;
}

bool MotionHolds(Gpu& g, const char* model_label, const ModelFn& model, const char* scale_label,
                 UINT work_w, UINT work_h) {
  std::vector<CaseResult> frames(kPanFrames);
  for (UINT f = 0; f < kPanFrames; ++f)
    if (!RunCase(g, PanFrame(f), kPanW, kPanH, work_w, work_h, model, 0.0f, frames[f]))
      return false;

  std::vector<double> bands[3];
  for (UINT y = kPanMargin; y + kPanMargin < kPanH; ++y) {
    for (UINT u = kPanMargin + kPanFrames; u + kPanMargin < kPanW; ++u) {
      double sum = 0.0, sum_sq = 0.0;
      for (UINT f = 0; f < kPanFrames; ++f) {
        const size_t i = (size_t)y * kPanW + (u - f);
        const double gain =
            std::log2(double(Luma(frames[f].out[i])) / double(Luma(frames[f].original[i])));
        sum += gain;
        sum_sq += gain * gain;
      }
      const double mean = sum / kPanFrames;
      const double spread = std::sqrt(std::max(sum_sq / kPanFrames - mean * mean, 0.0));
      const float depth = PanDepth(u);
      bands[depth > 1.0f / 32.0f ? 0 : (depth > 1.0f / 1024.0f ? 1 : 2)].push_back(spread);
    }
  }
  const Spread mid = Summarise(bands[0]), dark = Summarise(bands[1]), deep = Summarise(bands[2]);
  char name[96], detail[200];
  std::snprintf(name, sizeof(name), "gain holds still under motion, %s model, scale %s",
                model_label, scale_label);
  std::snprintf(detail, sizeof(detail),
                "spread in stops, mean/p99: mid %.4f/%.4f, dark %.4f/%.4f, below 1/1024 %.4f/%.4f",
                mid.mean, mid.p99, dark.mean, dark.p99, deep.mean, deep.p99);
  return Check(name, mid.p99 < 0.06 && dark.p99 < 0.06 && deep.p99 < 0.03, detail);
}

bool Motion(Gpu& g) {
  const ModelFn relight = [](const ModelView& p, UINT x, UINT y) {
    return std::clamp(std::pow(std::max(Luma(p.At(x, y)), 1e-6f) / 0.18f, -0.2f), 0.5f, 3.0f);
  };
  const ModelFn contrast = [&relight](const ModelView& p, UINT x, UINT y) {
    float around = 0.0f;
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) around += Luma(p.At(int(x) + dx, int(y) + dy));
    const float local = std::max(Luma(p.At(x, y)), 1e-6f) / std::max(around / 9.0f, 1e-6f);
    return relight(p, x, y) * std::clamp(std::pow(local, 0.3f), 0.5f, 2.0f);
  };
  struct Scale {
    const char* label;
    UINT work_w, work_h;
  } scales[] = {{"1.00", 256, 160}, {"0.95", 243, 152}, {"0.90", 230, 144},
                {"0.75", 192, 120}, {"0.50", 128, 80}};
  bool all = true;
  for (const Scale& s : scales)
    all &= MotionHolds(g, "relight", relight, s.label, s.work_w, s.work_h);
  for (const Scale& s : scales)
    all &= MotionHolds(g, "contrast", contrast, s.label, s.work_w, s.work_h);
  return all;
}

void Timing(Gpu& g, UINT w, UINT h, UINT work_w, UINT work_h) {
  ID3D12Resource* game_out =
      CreateTexture(g, w, h, kGameFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* color_copy =
      CreateTexture(g, w, h, kGameFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* proxy =
      CreateTexture(g, work_w, work_h, kModelFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12Resource* model =
      CreateTexture(g, work_w, work_h, kModelFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_QUERY_HEAP_DESC heap_desc = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
  ID3D12QueryHeap* queries = nullptr;
  g.device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&queries));
  ID3D12Resource* stamps =
      CreateBuffer(g, D3D12_HEAP_TYPE_READBACK, 32, D3D12_RESOURCE_STATE_COPY_DEST);
  UINT64 frequency = 1;
  g.queue->GetTimestampFrequency(&frequency);

  constexpr int kRuns = 40;
  double pre_ms = 0.0, post_ms = 0.0;
  for (int run = 0; run < kRuns; ++run) {
    const uint32_t frame = g_frame++;
    g.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    nr_compose::RecordPre(g.list, game_out, color_copy, proxy, w, h, work_w, work_h, frame, 1.0f);
    g.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    g.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 2);
    nr_compose::RecordPost(g.list, game_out, color_copy, proxy, model, w, h, work_w, work_h,
                           frame, 1.0f, 2.0f, 0.0f, 1.0f, 0);
    g.list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, 3);
    g.list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, stamps, 0);
    Submit(g);
    UINT64* t = nullptr;
    stamps->Map(0, nullptr, reinterpret_cast<void**>(&t));
    if (run >= 5) {
      pre_ms += double(t[1] - t[0]) * 1000.0 / double(frequency);
      post_ms += double(t[3] - t[2]) * 1000.0 / double(frequency);
    }
    const D3D12_RANGE none = {0, 0};
    stamps->Unmap(0, &none);
  }
  std::printf("TIME %ux%u, model %ux%u: before the model %.3f ms, after it %.3f ms\n", w, h,
              work_w, work_h, pre_ms / (kRuns - 5), post_ms / (kRuns - 5));

  stamps->Release();
  queries->Release();
  game_out->Release();
  color_copy->Release();
  proxy->Release();
  model->Release();
}

}  // namespace

int main() {
  Gpu g;
  if (!CreateGpu(g)) return 2;
  bool ok = Correctness(g);
  ok &= Motion(g);
  Timing(g, 3840, 2160, 3840, 2160);
  Timing(g, 3840, 2160, 2880, 1620);
  Timing(g, 3840, 2160, 1920, 1080);
  std::printf("%s\n", ok ? "ALL PASS" : "SOME FAILED");
  return ok ? 0 : 1;
}
