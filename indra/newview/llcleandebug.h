/**
 * @file llcleandebug.h
 * @brief Master switch for ALL clean-pipeline diagnostics.
 *
 * When CLEAN_DEBUG is 1 the clean GPU-driven pipeline compiles in its heavy
 * instrumentation: per-frame health logging (#Clean29), GPU readbacks
 * (glGetBufferSubData of counter/command/skin buffers -- each a full GPU sync
 * stall), the transform / stranded-slot audits, the selection material dump,
 * and the shader-side dbg[] reject counters + normal/material debug viz.
 *
 * When CLEAN_DEBUG is 0 every one of those is #if'd out so the pipeline runs
 * with zero diagnostic overhead -- the "clean" / production build. The actual
 * render FIXES (cull OOB guard, double-tint, two-sided sculpt lighting, etc.)
 * are NOT gated by this and always compile in.
 *
 * Default is 0 (production). Nearly all of this instruments the retired GL clean
 * renderer (#29); the Vulkan diagnostics (RenderVKSectionLog + the GLResidue line,
 * RenderVKIngestDiag) are settings-gated and do NOT depend on this switch.
 * To opt in: `make CLEAN_DEBUG=1` (the Makefile passes -DCLEAN_DEBUG explicitly
 * on every build so a stale CMakeCache value can't override the request).
 * The C++ flag also drives the GLSL side: shader setup adds a "CLEAN_DEBUG"
 * permutation when this is 1, and the clean shaders wrap their dbg[] writes in
 * `#ifdef CLEAN_DEBUG`.
 */
#ifndef LL_CLEANDEBUG_H
#define LL_CLEANDEBUG_H

#ifndef CLEAN_DEBUG
#define CLEAN_DEBUG 0
#endif

// CLEAN_PROF: lightweight CPU-wall frame profiling (#FrameProf: idle vs display,
// hero-mirror, probe update). LLTimers + one log line/frame. Default OFF:
// production builds run diagnostic-free (a log line per frame is still 60
// lines/s of I/O + megabytes of Slipstream.log per session). Build with
// CLEAN_PROF=1 (or edit here) to profile.
#ifndef CLEAN_PROF
#define CLEAN_PROF 0
#endif

#endif // LL_CLEANDEBUG_H
