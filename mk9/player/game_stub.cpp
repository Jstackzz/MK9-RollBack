// Part of mk9game.dll, compiled with the translated code on the player's PC (README.md here).

#include <rex/image_info.h>

// A DLL with no C runtime of its own: the compiler names this symbol wherever a program uses floating point, and the
// runtime that would define it is mk9.exe's.
extern "C" const int _fltused = 0;

extern const rex::PPCImageInfo PPCImageConfig;

extern "C" const rex::PPCImageInfo* Mk9GameImage() { return &PPCImageConfig; }
