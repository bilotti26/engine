/* sel4/include/setjmp.h -- aarch64 setjmp / longjmp for bare-metal seL4.
   Shadows the system <setjmp.h> to prevent glibc's __longjmp_chk references. */
#pragma once

/* jmp_buf layout (AArch64, 192 bytes):
     [0..9]   x19-x28   callee-saved GP registers
     [10..11] x29, x30  frame pointer, link register
     [12]     sp         stack pointer
     [13..20] d8-d15    callee-saved FP registers  */
typedef long jmp_buf[24];
typedef long sigjmp_buf[24];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

int  _setjmp(jmp_buf env);
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* sigsetjmp / siglongjmp -- no signals on seL4, ignore savemask */
static inline int  sigsetjmp(sigjmp_buf env, int s) { (void)s; return setjmp(env); }
static inline void siglongjmp(sigjmp_buf env, int v) { longjmp(env, v); }
