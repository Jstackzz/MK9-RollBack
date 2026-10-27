// Test screenshots, see snap.h. The PNG writer stores the pixels uncompressed (deflate "stored"
// blocks): big files, no dependency.

#include "mk9/snap.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <rex/logging.h>

namespace mk9::snap {
namespace {

namespace fs = std::filesystem;

std::mutex g_lock;
Capture g_capture;
std::set<uint64_t> g_frames;
fs::path g_dir;
uint32_t g_width = 1280;

uint32_t Crc(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void Be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& body) {
  Be32(png, uint32_t(body.size()));
  std::vector<uint8_t> tb(type, type + 4);
  tb.insert(tb.end(), body.begin(), body.end());
  png.insert(png.end(), tb.begin(), tb.end());
  Be32(png, Crc(tb.data(), tb.size()));
}

bool WritePng(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> raw;  // filter byte 0 per row
  raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
  for (uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + size_t(y) * w * 3, rgb.begin() + size_t(y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t off = 0; off < raw.size() || raw.empty(); off += 65535) {
    size_t n = std::min<size_t>(65535, raw.size() - off);
    bool last = off + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    if (last) break;
  }
  Be32(z, (b << 16) | a);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Be32(ihdr, w);
  Be32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  FILE* f = _wfopen(path.wstring().c_str(), L"wb");
  if (!f) return false;
  bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  std::fclose(f);
  return ok;
}

// box-filter the RGBX image down to `width` (keeping the aspect)
std::vector<uint8_t> Scale(const Image& img, uint32_t width, uint32_t& out_h) {
  width = std::min(width, img.width);
  out_h = uint32_t(uint64_t(img.height) * width / img.width);
  std::vector<uint8_t> out(size_t(width) * out_h * 3);
  for (uint32_t y = 0; y < out_h; y++) {
    uint32_t y0 = uint32_t(uint64_t(y) * img.height / out_h), y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * img.height / out_h));
    for (uint32_t x = 0; x < width; x++) {
      uint32_t x0 = uint32_t(uint64_t(x) * img.width / width), x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * img.width / width));
      uint32_t sum[3] = {}, n = 0;
      for (uint32_t yy = y0; yy < y1; yy++) {
        const uint8_t* row = img.data.data() + size_t(yy) * img.stride;
        for (uint32_t xx = x0; xx < x1; xx++) {
          for (int c = 0; c < 3; c++) sum[c] += row[xx * 4 + c];
          n++;
        }
      }
      for (int c = 0; c < 3; c++) out[(size_t(y) * width + x) * 3 + c] = uint8_t(sum[c] / n);
    }
  }
  return out;
}

}  // namespace

void SetCapture(Capture capture) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_capture = std::move(capture);
}

void Init() {
  if (const char* d = std::getenv("MK9_SNAP_DIR"); d && *d) g_dir = d;
  if (const char* w = std::getenv("MK9_SNAP_WIDTH"); w && *w) g_width = uint32_t(std::max(64, std::atoi(w)));
  if (const char* f = std::getenv("MK9_SNAP_FRAMES"); f && *f) {
    std::stringstream ss(f);
    std::string item;
    while (std::getline(ss, item, ',')) g_frames.insert(std::strtoull(item.c_str(), nullptr, 10));
  }
}

void OnFrame(uint64_t frame) {
  if (g_frames.empty() || !g_frames.count(frame)) return;
  std::thread([frame] { Snap("snap-" + std::to_string(frame)); }).detach();
}

bool Snap(const std::string& name) {
  Capture capture;
  fs::path dir;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    capture = g_capture;
    dir = g_dir;
  }
  if (!capture) return false;
  Image img;
  if (!capture(img) || !img.width || !img.height) {
    REXLOG_WARN("mk9: snap {}: nothing to capture", name);
    return false;
  }
  uint32_t h = 0;
  auto rgb = Scale(img, g_width, h);
  if (dir.empty()) dir = fs::current_path();
  std::error_code ec;
  fs::create_directories(dir, ec);
  fs::path path = dir / (name + ".png");
  bool ok = WritePng(path, std::min(g_width, img.width), h, rgb);
  REXLOG_INFO("mk9: snap {} -> {} ({})", name, path.string(), ok ? "ok" : "failed");
  return ok;
}

}  // namespace mk9::snap
