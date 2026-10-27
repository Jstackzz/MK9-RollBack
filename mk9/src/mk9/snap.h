#pragma once
// Screenshots of the game's own output for tests, taken at chosen frames:
//   MK9_SNAP_FRAMES = "2600,2900"   frames (mk9::frame counts) to capture
//   MK9_SNAP_DIR    = <folder>       where snap-<frame>.png go (default: the log folder)
//   MK9_SNAP_WIDTH  = 1280           the PNG's width (the output is scaled down to it)
// Snap(name) captures now, from any thread.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mk9::snap {

// what captures the presented image: RGBX rows (the app sets it once the window exists)
struct Image {
  uint32_t width = 0, height = 0;
  size_t stride = 0;
  std::vector<uint8_t> data;
};
using Capture = std::function<bool(Image& out)>;
void SetCapture(Capture capture);

void Init();
// at the end of each frame (mk9::frame's end hook)
void OnFrame(uint64_t frame);
// capture now into <dir>\<name>.png; false when nothing can be captured yet
bool Snap(const std::string& name);

}  // namespace mk9::snap
