#pragma once
#define FE_INEXACT 0x01
#define FE_UNDERFLOW 0x02
#define FE_OVERFLOW 0x04
#define FE_DIVBYZERO 0x08
#define FE_INVALID 0x10
#define FE_ALL_EXCEPT (FE_DIVBYZERO | FE_INEXACT | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)
#define FE_TONEAREST 0x0000
#define FE_UPWARD 0x0100
#define FE_DOWNWARD 0x0200
#define FE_TOWARDZERO 0x0300
#ifdef __cplusplus
extern "C" {
#endif
int fegetround(void);
int fesetround(int round);
int feclearexcept(int excepts);
int fetestexcept(int excepts);
#ifdef __cplusplus
}
#endif
