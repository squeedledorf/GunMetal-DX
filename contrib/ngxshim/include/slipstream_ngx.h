/*
 * slipstream_ngx.h -- the "slip_nr" C ABI between the Slipstream viewer and an optional,
 * separately built neural denoise + upscale library (today: libslipstream_ngx.so /
 * slipstream_ngx.dll, a thin wrapper around NVIDIA NGX: DLSS Ray Reconstruction and, from
 * library version 1.2, DLSS Super Resolution).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 The Slipstream Viewer contributors. See contrib/ngxshim/LICENSE.
 *
 * Rules of this header (it is shared verbatim by the viewer and the shim):
 *  - plain C99, no vendor types: only <stdint.h> and the Khronos Vulkan headers;
 *  - every struct starts with `struct_size` (sizeof as the caller knows it) so either side
 *    can grow a struct without breaking the other; a callee reads only the fields that fit;
 *  - strings are UTF-8, booleans are uint32_t (0 / 1);
 *  - the viewer never links this library: it loads it at run time (dlopen / LoadLibrary) and
 *    resolves every entry point by name (SLIP_NR_FN_* below). Define SLIP_NR_NO_PROTOTYPES
 *    before including to get only the types and the PFN_ typedefs.
 *  - not thread safe: one thread (the viewer's render thread) makes every call.
 *  - ABI 1 only ever GROWS: new fields are appended to structs (the callee checks struct_size,
 *    the *_SIZE_V1 macros are the first release's sizes) and new entry points are optional
 *    (a caller resolves them by name and treats a missing one as "this library predates it").
 *    So an old viewer runs a new library and a new viewer runs an old one, each using what
 *    both know.
 *
 * Features (slip_nr_feature_type): RAY RECONSTRUCTION (denoise + upscale; the only feature of
 * libraries before 1.2) and SUPER RESOLUTION (upscale / anti-alias only; library 1.2+, found by
 * the optional slip_nr_optimal_render_size2 entry point + slip_nr_caps.super_resolution). Each
 * needs its own vendor feature library; either may be missing.
 *
 * Call order:
 *   slip_nr_abi_version()                          any time (must equal SLIP_NR_ABI_VERSION)
 *   slip_nr_instance_extensions()                  before vkCreateInstance
 *   slip_nr_device_extensions()                    after picking the GPU, before vkCreateDevice
 *   slip_nr_init()                                 after vkCreateDevice
 *   slip_nr_get_caps(), slip_nr_optimal_render_size() (RR) / slip_nr_optimal_render_size2() (1.2+)
 *   slip_nr_create() / slip_nr_evaluate() / slip_nr_destroy()   (create + evaluate record into
 *                                                  the caller's command buffer)
 *   slip_nr_shutdown()                             before vkDestroyDevice
 */
#ifndef SLIPSTREAM_NGX_H
#define SLIPSTREAM_NGX_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLIP_NR_ABI_VERSION 1u

/* File names the viewer looks for (next to its own libraries, or in the configured folder). */
#define SLIP_NR_LIBRARY_NAME_LINUX   "libslipstream_ngx.so"
#define SLIP_NR_LIBRARY_NAME_WINDOWS "slipstream_ngx.dll"

#if defined(_WIN32)
#  define SLIP_NR_CALL __cdecl
#  if defined(SLIP_NR_BUILDING_LIBRARY)
#    define SLIP_NR_API __declspec(dllexport)
#  else
#    define SLIP_NR_API
#  endif
#else
#  define SLIP_NR_CALL
#  if defined(SLIP_NR_BUILDING_LIBRARY)
#    define SLIP_NR_API __attribute__((visibility("default")))
#  else
#    define SLIP_NR_API
#  endif
#endif

typedef enum slip_nr_result
{
    SLIP_NR_OK                       = 0,
    SLIP_NR_ERROR_INVALID_ARGUMENT   = 1,
    SLIP_NR_ERROR_NOT_INITIALIZED    = 2,
    SLIP_NR_ERROR_UNSUPPORTED        = 3,   /* GPU, OS or driver can't run the feature */
    SLIP_NR_ERROR_DRIVER_TOO_OLD     = 4,
    SLIP_NR_ERROR_OUT_OF_MEMORY      = 5,
    SLIP_NR_ERROR_FEATURE_MISSING    = 6,   /* the vendor's feature library was not found */
    SLIP_NR_ERROR_FAILED             = 7,   /* anything else; see slip_nr_last_error() */
    SLIP_NR_RESULT_MAX_ENUM          = 0x7fffffff
} slip_nr_result;

/* The network a feature runs (slip_nr_create_info.feature, slip_nr_optimal_render_size2). */
typedef enum slip_nr_feature_type
{
    SLIP_NR_FEATURE_RAY_RECONSTRUCTION = 0,   /* DLSS-D: joint denoise + upscale (needs the guide images) */
    SLIP_NR_FEATURE_SUPER_RESOLUTION   = 1,   /* DLSS: upscale / anti-alias (colour, depth, motion) -- library 1.2+ */
    SLIP_NR_FEATURE_TYPE_MAX_ENUM      = 0x7fffffff
} slip_nr_feature_type;

/* Fixed quality modes (the viewer uses no dynamic resolution). */
typedef enum slip_nr_quality
{
    SLIP_NR_QUALITY_DLAA              = 0,   /* render = display */
    SLIP_NR_QUALITY_QUALITY           = 1,
    SLIP_NR_QUALITY_BALANCED          = 2,
    SLIP_NR_QUALITY_PERFORMANCE       = 3,
    SLIP_NR_QUALITY_ULTRA_PERFORMANCE = 4,
    SLIP_NR_QUALITY_MAX_ENUM          = 0x7fffffff
} slip_nr_quality;

typedef enum slip_nr_log_level
{
    SLIP_NR_LOG_INFO    = 0,
    SLIP_NR_LOG_VERBOSE = 1
} slip_nr_log_level;

/* Thread safe, may be called from any thread while the library is loaded; msg is only valid
 * during the call. */
typedef void (SLIP_NR_CALL *slip_nr_log_fn)(void* user, uint32_t level, const char* msg);

/* Who is asking. Every pointer may be NULL / 0 except where noted. */
typedef struct slip_nr_app_info
{
    uint32_t           struct_size;
    const char*        project_id;           /* a stable GUID string for the application (NULL: the shim's default) */
    const char*        engine_version;       /* e.g. "1.4.0.5" */
    const char*        app_data_path;        /* writable folder for the vendor runtime's logs / caches (required by init) */
    const char* const* feature_paths;        /* folders searched for the vendor feature library, in order */
    uint32_t           feature_path_count;
    slip_nr_log_fn     log_fn;
    void*              log_user;
    uint32_t           log_verbose;          /* 1: forward verbose vendor logging */
} slip_nr_app_info;

typedef struct slip_nr_library_info
{
    uint32_t struct_size;
    uint32_t abi_version;                    /* SLIP_NR_ABI_VERSION the library was built with */
    char     backend[32];                    /* e.g. "NVIDIA NGX" */
    char     sdk_version[32];                /* the vendor SDK the library was built against, e.g. "310.9.16" */
    char     library_version[32];            /* the shim's own version */
} slip_nr_library_info;

typedef struct slip_nr_vulkan_device
{
    uint32_t                  struct_size;
    VkInstance                instance;
    VkPhysicalDevice          physical_device;
    VkDevice                  device;
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;   /* the caller's loader entry points (NULL: the vendor's default) */
    PFN_vkGetDeviceProcAddr   get_device_proc_addr;
} slip_nr_vulkan_device;

typedef struct slip_nr_caps
{
    uint32_t struct_size;
    uint32_t ray_reconstruction;             /* 1: available on this GPU + driver */
    uint32_t needs_newer_driver;
    uint32_t min_driver_major, min_driver_minor;
    uint32_t vendor_init_result;             /* the vendor's raw feature-init status (diagnostics) */
    char     reason[160];                    /* why not, when ray_reconstruction == 0 */
    /* ---- appended in library 1.2 (written only when struct_size covers them; an older library
     * leaves them as the caller set them: zero them first) ---- */
    uint32_t super_resolution;               /* 1: DLSS Super Resolution available on this GPU + driver */
    uint32_t sr_needs_newer_driver;
    uint32_t sr_min_driver_major, sr_min_driver_minor;
    uint32_t sr_vendor_init_result;
    char     sr_reason[160];                 /* why not, when super_resolution == 0 */
} slip_nr_caps;
#define SLIP_NR_CAPS_SIZE_V1 ((uint32_t)(offsetof(slip_nr_caps, super_resolution)))

/* slip_nr_create_info.flags */
#define SLIP_NR_CREATE_HDR              0x0001u   /* colour input is linear HDR */
#define SLIP_NR_CREATE_DEPTH_INVERTED   0x0002u   /* reversed-Z (1 = near) */
#define SLIP_NR_CREATE_MV_RENDER_RES    0x0004u   /* motion vectors at render resolution */
#define SLIP_NR_CREATE_MV_JITTERED      0x0008u   /* motion vectors include the jitter */
#define SLIP_NR_CREATE_ROUGHNESS_PACKED 0x0010u   /* roughness in normals.a (else a separate image) */
#define SLIP_NR_CREATE_HW_DEPTH         0x0020u   /* depth is the hardware depth (else linear view depth; RR only) */
#define SLIP_NR_CREATE_AUTO_EXPOSURE    0x0040u   /* SR: the network estimates the exposure (no exposure image) */

typedef struct slip_nr_create_info
{
    uint32_t        struct_size;
    uint32_t        render_width, render_height;
    uint32_t        display_width, display_height;
    slip_nr_quality quality;
    uint32_t        flags;                   /* SLIP_NR_CREATE_* */
    uint32_t        preset;                  /* 0 = the vendor default; otherwise a vendor preset number */
    /* ---- appended in library 1.2 (an older library ignores it and always creates RR) ---- */
    uint32_t        feature;                 /* slip_nr_feature_type (0 = Ray Reconstruction) */
} slip_nr_create_info;
#define SLIP_NR_CREATE_INFO_SIZE_V1 ((uint32_t)(offsetof(slip_nr_create_info, feature)))

/* One image the library reads or writes. Inputs: SAMPLED usage, in a shader-readable layout,
 * prior writes made visible to compute. The output: STORAGE usage, GENERAL layout. The
 * library may change pipeline / descriptor bindings in the command buffer: the caller
 * re-binds everything it needs afterwards. image == VK_NULL_HANDLE means "not provided". */
typedef struct slip_nr_image
{
    VkImage            image;
    VkImageView        view;
    VkFormat           format;
    uint32_t           width, height;
    VkImageAspectFlags aspect;               /* 0 = COLOR */
} slip_nr_image;

/* Ray Reconstruction reads every "required" image below; Super Resolution reads only colour,
 * output, depth and motion (+ exposure, responsivity -> its bias-current-colour mask, values
 * in [0, 1]) and ignores the guides, the matrices and the transparency layer. */
typedef struct slip_nr_eval_info
{
    uint32_t      struct_size;
    /* required */
    slip_nr_image color;                     /* noisy, jittered linear HDR scene (render size) */
    slip_nr_image output;                    /* display size, STORAGE */
    slip_nr_image depth;                     /* render size */
    slip_nr_image motion;                    /* RG16F/RG32F, render size; current -> previous */
    slip_nr_image diffuse_albedo;            /* linear (not sRGB); sky 0.5 */
    slip_nr_image specular_albedo;           /* linear; sky 0 */
    slip_nr_image normals;                   /* world-space shading normal (+ roughness in .a when packed); sky 0 */
    /* optional */
    slip_nr_image roughness;                 /* when not packed */
    slip_nr_image specular_hit_distance;     /* world distance primary surface -> specular hit; sky 65504 */
    slip_nr_image exposure;                  /* 1x1 R32F */
    slip_nr_image color_before_transparency;
    float         jitter_x, jitter_y;        /* render pixels, the vendor's sign convention (see the shim README) */
    float         mv_scale_x, mv_scale_y;    /* motion * scale = pixels (0 = 1) */
    uint32_t      render_width, render_height;   /* the valid render sub-rectangle at (0, 0) */
    uint32_t      reset;                     /* 1: history invalid (camera cut) */
    float         frame_time_ms;
    float         pre_exposure;              /* 0 = 1 */
    float         world_to_view[16];         /* ROW-major, left multiplication (needed with a hit distance) */
    float         view_to_clip[16];          /* ROW-major, unjittered */
    /* ---- appended after ABI 1's first release (a library reads them only when struct_size
     * covers them; SLIP_NR_EVAL_INFO_SIZE_V1 is the size without them) ---- */
    slip_nr_image transparency_layer;        /* render size, RGBA: premultiplied transparent colour + its
                                                alpha, composited over the reconstructed colour (upscaled,
                                                not denoised); color is then the scene BEFORE transparency */
    slip_nr_image responsivity;              /* render size, R16F in [-1, 1]: > 0 more responsive (less history) */
} slip_nr_eval_info;
#define SLIP_NR_EVAL_INFO_SIZE_V1 ((uint32_t)(offsetof(slip_nr_eval_info, transparency_layer)))

typedef struct slip_nr_feature slip_nr_feature;   /* opaque */

/* Entry points. Names for dlsym / GetProcAddress: */
#define SLIP_NR_FN_ABI_VERSION          "slip_nr_abi_version"
#define SLIP_NR_FN_GET_INFO             "slip_nr_get_info"
#define SLIP_NR_FN_INSTANCE_EXTENSIONS  "slip_nr_instance_extensions"
#define SLIP_NR_FN_DEVICE_EXTENSIONS    "slip_nr_device_extensions"
#define SLIP_NR_FN_INIT                 "slip_nr_init"
#define SLIP_NR_FN_GET_CAPS             "slip_nr_get_caps"
#define SLIP_NR_FN_OPTIMAL_RENDER_SIZE  "slip_nr_optimal_render_size"
#define SLIP_NR_FN_OPTIMAL_RENDER_SIZE2 "slip_nr_optimal_render_size2"   /* optional: library 1.2+ */
#define SLIP_NR_FN_CREATE               "slip_nr_create"
#define SLIP_NR_FN_EVALUATE             "slip_nr_evaluate"
#define SLIP_NR_FN_DESTROY              "slip_nr_destroy"
#define SLIP_NR_FN_SHUTDOWN             "slip_nr_shutdown"
#define SLIP_NR_FN_LAST_ERROR           "slip_nr_last_error"

typedef uint32_t       (SLIP_NR_CALL *PFN_slip_nr_abi_version)(void);
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_get_info)(slip_nr_library_info* out);
/* names: valid until slip_nr_shutdown or the library is unloaded. */
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_instance_extensions)(const slip_nr_app_info* app,
                                                                     uint32_t* count, const char* const** names);
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_device_extensions)(const slip_nr_app_info* app, VkInstance instance,
                                                                   VkPhysicalDevice physical_device,
                                                                   uint32_t* count, const char* const** names);
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_init)(const slip_nr_app_info* app, const slip_nr_vulkan_device* dev);
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_get_caps)(slip_nr_caps* out);
/* min / max may be NULL. */
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_optimal_render_size)(slip_nr_quality quality,
                                                                     uint32_t display_width, uint32_t display_height,
                                                                     uint32_t* render_width, uint32_t* render_height,
                                                                     uint32_t* min_width, uint32_t* min_height,
                                                                     uint32_t* max_width, uint32_t* max_height);
/* Per feature (slip_nr_feature_type); min / max may be NULL. Library 1.2+: resolve it optionally. */
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_optimal_render_size2)(uint32_t feature, slip_nr_quality quality,
                                                                      uint32_t display_width, uint32_t display_height,
                                                                      uint32_t* render_width, uint32_t* render_height,
                                                                      uint32_t* min_width, uint32_t* min_height,
                                                                      uint32_t* max_width, uint32_t* max_height);
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_create)(VkCommandBuffer cmd, const slip_nr_create_info* info,
                                                        slip_nr_feature** out);
typedef slip_nr_result (SLIP_NR_CALL *PFN_slip_nr_evaluate)(VkCommandBuffer cmd, slip_nr_feature* feature,
                                                          const slip_nr_eval_info* info);
/* The caller guarantees the GPU no longer uses the feature (a device wait / its fence). */
typedef void           (SLIP_NR_CALL *PFN_slip_nr_destroy)(slip_nr_feature* feature);
typedef void           (SLIP_NR_CALL *PFN_slip_nr_shutdown)(void);
/* The last failure's text (UTF-8, never NULL; "" when none). */
typedef const char*    (SLIP_NR_CALL *PFN_slip_nr_last_error)(void);

#if !defined(SLIP_NR_NO_PROTOTYPES)
SLIP_NR_API uint32_t       SLIP_NR_CALL slip_nr_abi_version(void);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_get_info(slip_nr_library_info* out);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_instance_extensions(const slip_nr_app_info* app,
                                                                    uint32_t* count, const char* const** names);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_device_extensions(const slip_nr_app_info* app, VkInstance instance,
                                                                  VkPhysicalDevice physical_device,
                                                                  uint32_t* count, const char* const** names);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_init(const slip_nr_app_info* app, const slip_nr_vulkan_device* dev);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_get_caps(slip_nr_caps* out);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_optimal_render_size(slip_nr_quality quality,
                                                                    uint32_t display_width, uint32_t display_height,
                                                                    uint32_t* render_width, uint32_t* render_height,
                                                                    uint32_t* min_width, uint32_t* min_height,
                                                                    uint32_t* max_width, uint32_t* max_height);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_optimal_render_size2(uint32_t feature, slip_nr_quality quality,
                                                                     uint32_t display_width, uint32_t display_height,
                                                                     uint32_t* render_width, uint32_t* render_height,
                                                                     uint32_t* min_width, uint32_t* min_height,
                                                                     uint32_t* max_width, uint32_t* max_height);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_create(VkCommandBuffer cmd, const slip_nr_create_info* info,
                                                       slip_nr_feature** out);
SLIP_NR_API slip_nr_result SLIP_NR_CALL slip_nr_evaluate(VkCommandBuffer cmd, slip_nr_feature* feature,
                                                         const slip_nr_eval_info* info);
SLIP_NR_API void           SLIP_NR_CALL slip_nr_destroy(slip_nr_feature* feature);
SLIP_NR_API void           SLIP_NR_CALL slip_nr_shutdown(void);
SLIP_NR_API const char*    SLIP_NR_CALL slip_nr_last_error(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SLIPSTREAM_NGX_H */
