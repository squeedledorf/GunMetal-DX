/**
 * @file glibc_compat.h
 * @brief Force-included on Linux (00-Common.cmake): pins libm symbols to their
 *        long-standing versions so binaries built on a bleeding-edge distro still
 *        load on older glibc.
 *
 * glibc 2.43 added new symbol versions for several single-precision libm
 * functions; building against it makes the viewer require GLIBC_2.43 (e.g.
 * Fedora 42 ships 2.41). The GLIBC_2.2.5 versions exist in every x86_64 glibc.
 * An unreferenced .symver emits nothing, so TUs that don't call these are
 * unaffected. The Makefile's glibc check flags any new >2.38 requirement.
 */
#if defined(__linux__) && defined(__x86_64__) && !defined(LL_NO_GLIBC_COMPAT)
__asm__(".symver acosf,acosf@GLIBC_2.2.5");
__asm__(".symver asinf,asinf@GLIBC_2.2.5");
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver log10f,log10f@GLIBC_2.2.5");
__asm__(".symver sqrtf,sqrtf@GLIBC_2.2.5");
#endif
