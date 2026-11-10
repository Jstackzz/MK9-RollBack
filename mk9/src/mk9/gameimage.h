// The recompiled image (generated/default/mk9_init.cpp). In a player's game it is in mk9game.dll, the part built on
// their PC from their disc, and this mk9.exe comes ready made with the package (mk9/player/README.md); a developer's
// build links it into mk9.exe.

#pragma once

#include <rex/image_info.h>

#if defined(MK9_PLAYER_HOST)
extern "C" __declspec(dllimport) const rex::PPCImageInfo* Mk9GameImage();
namespace mk9 {
inline const rex::PPCImageInfo* GameImage() { return Mk9GameImage(); }
inline constexpr const wchar_t* kGameModule = L"mk9game.dll";
}  // namespace mk9
#else
extern const rex::PPCImageInfo PPCImageConfig;
namespace mk9 {
inline const rex::PPCImageInfo* GameImage() { return &PPCImageConfig; }
inline constexpr const wchar_t* kGameModule = nullptr;  // the executable itself
}  // namespace mk9
#endif
