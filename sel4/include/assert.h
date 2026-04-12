/* sel4/include/assert.h -- shadows system assert.h to avoid conflict with
   the seL4 SDK's __assert_fail(int line) signature (vs system's unsigned int) */
#pragma once
/* Match seL4 SDK's signature exactly */
void __assert_fail(const char *str, const char *file, int line, const char *function);
#define assert(e) ((void)((e)||((__assert_fail(#e,__FILE__,__LINE__,__func__)),0)))
