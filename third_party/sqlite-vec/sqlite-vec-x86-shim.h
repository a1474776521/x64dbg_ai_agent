/* third_party/sqlite-vec/sqlite-vec-x86-shim.h
 *
 * 32-bit MSVC has no __popcnt64 intrinsic (only x64 provides it).
 * sqlite-vec.c (non-ARM _MSC_VER branch) does:
 *     #include <intrin.h>
 *     #define __builtin_popcountl __popcnt64
 * which makes distance_hamming_u64 emit a call to __popcnt64. On x86
 * that symbol is unresolved at link time.
 *
 * This header is force-included via /FI on the sqlite_vec target.
 * On x86 it redefines __popcnt64 to a software popcount implementation
 * (provided as a static inline function). On x64 it is a no-op.
 */
#ifndef X64DBG_AI_SQLITE_VEC_X86_SHIM_H
#define X64DBG_AI_SQLITE_VEC_X86_SHIM_H

#if defined(_M_IX86) && !defined(_M_X64)

/* Software popcount64. C-compatible (no C++ keywords). */
static __inline unsigned __int64 __x64ai_popcnt64_sw(unsigned __int64 x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0f0f0f0f0f0f0f0fULL;
    return (x * 0x0101010101010101ULL) >> 56;
}

#ifdef __popcnt64
#undef __popcnt64
#endif
#define __popcnt64(x) __x64ai_popcnt64_sw((unsigned __int64)(x))

#endif /* _M_IX86 */

#endif /* X64DBG_AI_SQLITE_VEC_X86_SHIM_H */
