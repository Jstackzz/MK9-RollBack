// The C library's math functions the translated code can call (clang decides which of them it can do inline).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
double acos(double);
float acosf(float);
double asin(double);
float asinf(float);
double atan(double);
float atanf(float);
double atan2(double, double);
float atan2f(float, float);
double ceil(double);
float ceilf(float);
double copysign(double, double);
float copysignf(float, float);
double cos(double);
float cosf(float);
double exp(double);
float expf(float);
double exp2(double);
float exp2f(float);
double fabs(double);
float fabsf(float);
double floor(double);
float floorf(float);
double fma(double, double, double);
float fmaf(float, float, float);
double fmod(double, double);
float fmodf(float, float);
double log(double);
float logf(float);
double log2(double);
float log2f(float);
double log10(double);
float log10f(float);
double nearbyint(double);
float nearbyintf(float);
double pow(double, double);
float powf(float, float);
double rint(double);
float rintf(float);
double round(double);
float roundf(float);
double sin(double);
float sinf(float);
double sqrt(double);
float sqrtf(float);
double tan(double);
float tanf(float);
double trunc(double);
float truncf(float);
#ifdef __cplusplus
}
#endif
