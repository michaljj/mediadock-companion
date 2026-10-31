// Minimal stand-in for clang's built-in <cpuid.h>.
//
// The Windows SDK's directxmath.h does '#include <cpuid.h>' on x86/x64 when
// compiled by clang, but the LLVM toolset bundled with Visual Studio does not
// ship that resource header, which makes every clang-tidy run emit a
// 'file not found' error and exit non-zero. MSVC never reads this file (it
// uses <intrin.h> instead), so this shim only affects clang-based tools
// (clang-tidy / clangd).
#ifndef WINAPP_COMPAT_CPUID_H
#define WINAPP_COMPAT_CPUID_H

#if defined(__x86_64__) || defined(__i386__)
static __inline void __cpuid (unsigned int __info[4], unsigned int __leaf)
{
  __asm__ ("cpuid"
           : "=a" (__info[0]), "=b" (__info[1]), "=c" (__info[2]), "=d" (__info[3])
           : "a" (__leaf));
}

static __inline void __cpuid_count (unsigned int __info[4], unsigned int __leaf,
                                    unsigned int __subleaf)
{
  __asm__ ("cpuid"
           : "=a" (__info[0]), "=b" (__info[1]), "=c" (__info[2]), "=d" (__info[3])
           : "a" (__leaf), "c" (__subleaf));
}
#endif

#endif // WINAPP_COMPAT_CPUID_H
