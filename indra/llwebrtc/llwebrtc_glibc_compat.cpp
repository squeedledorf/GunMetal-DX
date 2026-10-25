/**
 * @file llwebrtc_glibc_compat.cpp
 * @brief Linux x86_64: route the prebuilt libwebrtc's libm calls to the
 *        long-standing glibc symbol versions (see indra/cmake/glibc_compat.h).
 *
 * glibc 2.43 re-versioned sqrtf/log10f; the prebuilt webrtc objects bind to the
 * newest version at link time, which the force-included .symver header can't
 * reach. llwebrtc links with --wrap=sqrtf,--wrap=log10f so every reference lands
 * here and is forwarded to the GLIBC_2.2.5 versions.
 */
#if defined(__linux__) && defined(__x86_64__)
extern "C"
{
__asm__(".symver llwebrtc_sqrtf_v225,sqrtf@GLIBC_2.2.5");
__asm__(".symver llwebrtc_log10f_v225,log10f@GLIBC_2.2.5");
float llwebrtc_sqrtf_v225(float);
float llwebrtc_log10f_v225(float);

__attribute__((visibility("default"))) float __wrap_sqrtf(float x)  { return llwebrtc_sqrtf_v225(x); }
__attribute__((visibility("default"))) float __wrap_log10f(float x) { return llwebrtc_log10f_v225(x); }
}
#endif
