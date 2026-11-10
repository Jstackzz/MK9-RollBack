// (the translated code never uses setjmp: only unused helpers in the generated header name it)
#pragma once
typedef struct { unsigned long long words[32]; } jmp_buf;
#ifdef __cplusplus
extern "C" {
#endif
int setjmp(jmp_buf& buffer);
[[noreturn]] void longjmp(jmp_buf& buffer, int value);
#ifdef __cplusplus
}
#endif
