/**
 * @file llsdl.cpp
 * @brief SDL2 initialization
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include <initializer_list>
#include <list>

#if LL_LINUX
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#endif

#include "SDL3/SDL.h"

#ifndef LL_SDL_APP
#define SDL_MAIN_HANDLED 1
#include "SDL3/SDL_main.h"
#endif

#include "llerror.h"
#include "llwindow.h"

void sdl_logger(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    switch (priority)
    {
        case SDL_LOG_PRIORITY_TRACE:
        case SDL_LOG_PRIORITY_VERBOSE:
        case SDL_LOG_PRIORITY_DEBUG:
            LL_DEBUGS("SDL") << "log='" << message << "'" << LL_ENDL;
            break;
        case SDL_LOG_PRIORITY_INFO:
            LL_INFOS("SDL") << "log='" << message << "'" << LL_ENDL;
            break;
        case SDL_LOG_PRIORITY_WARN:
        case SDL_LOG_PRIORITY_ERROR:
        case SDL_LOG_PRIORITY_CRITICAL:
            LL_WARNS("SDL") << "log='" << message << "'" << LL_ENDL;
            break;
        case SDL_LOG_PRIORITY_INVALID:
        default:
            break;
    }
}

namespace
{
std::string sVideoBackend = "auto";

#if LL_LINUX && LL_SDL_WINDOW
// ---------------------------------------------------------------------------
// Linux display backend selection (#196/#197).
//
// Native Wayland gives per-monitor fractional scaling without XWayland's blur,
// but a Wayland client only gets a title bar and border when either the
// compositor draws them (xdg-decoration: KDE, wlroots, Hyprland, ...) or the
// client draws them itself. GNOME/Mutter has no xdg-decoration, so SDL3 draws
// the frame through libdecor-0.so.0 plus a libdecor plugin. Without libdecor
// the window has no title bar, border or move/resize handles.
//
// The package carries libdecor and its cairo plugin in lib/libdecor/ (NOT on
// LD_LIBRARY_PATH, so a system copy wins). Before SDL initializes we:
//   1. ask the compositor whether it offers xdg-decoration;
//   2. if not, make sure a libdecor is loaded: the system's when it has a
//      plugin directory with plugins (native GTK plugin, desktop theme),
//      otherwise ours, preloaded by absolute path -- SDL's later
//      dlopen("libdecor-0.so.0") and the plugin's DT_NEEDED both resolve to
//      the already-loaded object by SONAME -- with LIBDECOR_PLUGIN_DIR
//      pointed at our plugin;
//   3. in Auto mode, if neither is available, put X11 first for this session,
//      where the window manager draws the frame.
// ---------------------------------------------------------------------------

std::string loaded_path(void* handle)
{
    struct link_map* map = nullptr;
    if (handle && dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map && map->l_name && map->l_name[0])
    {
        return map->l_name;
    }
    return {};
}

std::string dir_of(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool same_path(const std::string& a, const std::string& b)
{
    char ra[PATH_MAX];
    char rb[PATH_MAX];
    if (realpath(a.c_str(), ra) && realpath(b.c_str(), rb))
    {
        return strcmp(ra, rb) == 0;
    }
    return a == b;
}

// The .so files in a libdecor plugin directory (LIBDECOR_PLUGIN_DIR may list
// several, colon-separated), sorted.
std::vector<std::string> plugin_files(const std::string& dirs)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= dirs.size())
    {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos)
        {
            end = dirs.size();
        }
        const std::string dir = dirs.substr(start, end - start);
        if (!dir.empty())
        {
            if (DIR* d = opendir(dir.c_str()))
            {
                while (dirent* e = readdir(d))
                {
                    const size_t n = strlen(e->d_name);
                    if (n > 3 && strcmp(e->d_name + n - 3, ".so") == 0)
                    {
                        out.push_back(dir + "/" + e->d_name);
                    }
                }
                closedir(d);
            }
        }
        start = end + 1;
    }
    std::sort(out.begin(), out.end());
    return out;
}

enum class EWaylandProbe
{
    NO_COMPOSITOR,  // not a Wayland session (or libwayland-client missing)
    NO_SSD,         // compositor without xdg-decoration (GNOME/Mutter, Weston)
    SSD             // compositor draws decorations
};

struct RegistryListener
{
    void (*global)(void* data, void* registry, uint32_t name, const char* iface, uint32_t version);
    void (*global_remove)(void* data, void* registry, uint32_t name);
};

void registry_global(void* data, void*, uint32_t, const char* iface, uint32_t)
{
    if (iface && strcmp(iface, "zxdg_decoration_manager_v1") == 0)
    {
        *static_cast<bool*>(data) = true;
    }
}

void registry_global_remove(void*, void*, uint32_t)
{
}

// A throwaway connection and one registry roundtrip. libwayland-client is
// loaded at runtime so the viewer doesn't link it (and wayland-client.h's
// inline wl_display_get_registry is not used: it would).
EWaylandProbe probe_wayland(std::string& detail)
{
    void* lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        detail = "libwayland-client.so.0 not found";
        return EWaylandProbe::NO_COMPOSITOR;
    }
    auto display_connect = reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "wl_display_connect"));
    auto display_disconnect = reinterpret_cast<void (*)(void*)>(dlsym(lib, "wl_display_disconnect"));
    auto display_roundtrip = reinterpret_cast<int (*)(void*)>(dlsym(lib, "wl_display_roundtrip"));
    auto marshal_constructor = reinterpret_cast<void* (*)(void*, uint32_t, const void*, ...)>(dlsym(lib, "wl_proxy_marshal_constructor"));
    auto add_listener = reinterpret_cast<int (*)(void*, void (**)(void), void*)>(dlsym(lib, "wl_proxy_add_listener"));
    auto proxy_destroy = reinterpret_cast<void (*)(void*)>(dlsym(lib, "wl_proxy_destroy"));
    const void* registry_interface = dlsym(lib, "wl_registry_interface");
    if (!display_connect || !display_disconnect || !display_roundtrip || !marshal_constructor || !add_listener
        || !proxy_destroy || !registry_interface)
    {
        detail = "libwayland-client.so.0 lacks the registry API";
        dlclose(lib);
        return EWaylandProbe::NO_COMPOSITOR;
    }

    void* display = display_connect(nullptr);
    if (!display)
    {
        const char* wd = getenv("WAYLAND_DISPLAY");
        detail = std::string("no Wayland compositor (WAYLAND_DISPLAY=") + (wd ? wd : "unset") + ")";
        dlclose(lib);
        return EWaylandProbe::NO_COMPOSITOR;
    }

    bool ssd = false;
    static RegistryListener listener = { &registry_global, &registry_global_remove };
    // 1 = WL_DISPLAY_GET_REGISTRY; the trailing nullptr is the new_id placeholder.
    void* registry = marshal_constructor(display, 1, registry_interface, nullptr);
    if (registry)
    {
        if (add_listener(registry, reinterpret_cast<void (**)(void)>(&listener), &ssd) == 0)
        {
            display_roundtrip(display);
        }
        proxy_destroy(registry);
    }
    display_disconnect(display);
    dlclose(lib);
    return ssd ? EWaylandProbe::SSD : EWaylandProbe::NO_SSD;
}

struct LibdecorState
{
    bool        available = false;  // a libdecor with a loadable plugin is in place
    std::string source;             // "system" or "bundled"
    std::string lib;                // path of the loaded libdecor-0.so.0
    std::string plugin_dir;         // where libdecor will look for plugins
    std::string plugin;             // a plugin that loads (bundled) / is present (system)
    std::string why;                // why not available
};

void* sLibdecorHandle = nullptr;    // kept loaded so SDL's dlopen finds this copy
void* sLibdecorPluginHandle = nullptr;  // the test-loaded bundled plugin, kept loaded

// <install>/lib/libdecor, from <install>/bin/do-not-directly-run-slipstream-bin.
std::string bundled_libdecor_dir()
{
    char exe[PATH_MAX];
    if (!realpath("/proc/self/exe", exe))
    {
        return {};
    }
    return dir_of(dir_of(exe)) + "/lib/libdecor";
}

LibdecorState prepare_libdecor()
{
    LibdecorState st;
    const char* env_dir = getenv("LIBDECOR_PLUGIN_DIR");
    if (env_dir && !*env_dir)
    {
        env_dir = nullptr;  // set but empty: libdecor ignores it too
    }
    const std::string bundled = bundled_libdecor_dir();
    const std::string bundled_lib = bundled + "/libdecor-0.so.0";
    const std::string bundled_plugins = bundled + "/plugins-1";
    const bool have_bundled = !bundled.empty() && access(bundled_lib.c_str(), R_OK) == 0;

    // The launcher points LIBDECOR_PLUGIN_DIR at our plugin when the system has
    // no libdecor plugin directory: then use our libdecor too (a matched pair).
    bool use_bundled = env_dir && have_bundled && same_path(env_dir, bundled_plugins);
    if (!use_bundled)
    {
        // The system copy first: it may carry the GTK plugin and match the theme.
        if (void* sys = dlopen("libdecor-0.so.0", RTLD_NOW | RTLD_GLOBAL))
        {
            const std::string path = loaded_path(sys);
            const std::string dir = env_dir ? std::string(env_dir) : dir_of(path) + "/libdecor/plugins-1";
            const std::vector<std::string> plugins = plugin_files(dir);
            if (!plugins.empty())
            {
                st.available = true;
                st.source = "system";
                st.lib = path;
                st.plugin_dir = dir;
                st.plugin = plugins.front();
                sLibdecorHandle = sys;
                return st;
            }
            st.why = "system " + path + " has no plugins in " + dir;
            dlclose(sys);
            if (void* still = dlopen("libdecor-0.so.0", RTLD_NOW | RTLD_NOLOAD))
            {
                // Something else holds it: SDL would get the system copy anyway.
                dlclose(still);
                st.why += " (and it stays loaded, so the bundled copy can't replace it)";
                return st;
            }
        }
        else
        {
            st.why = "no system libdecor-0.so.0";
        }
        use_bundled = have_bundled;
    }
    if (!use_bundled)
    {
        st.why += "; no bundled copy in " + (bundled.empty() ? std::string("<unknown install dir>") : bundled);
        return st;
    }

    void* own = dlopen(bundled_lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!own)
    {
        const char* err = dlerror();
        st.why += (st.why.empty() ? "" : "; ") + ("bundled " + bundled_lib + " failed to load: ") + (err ? err : "?");
        return st;
    }
    sLibdecorHandle = own;
    st.source = "bundled";
    st.lib = loaded_path(own);
    if (!env_dir)
    {
        // The bundled lib's compiled-in plugin dir is the build host's
        // (/usr/lib/x86_64-linux-gnu/...), wrong or empty here.
        setenv("LIBDECOR_PLUGIN_DIR", bundled_plugins.c_str(), 1);
    }
    const char* dir = getenv("LIBDECOR_PLUGIN_DIR");
    st.plugin_dir = dir ? dir : "";
    // Our cairo plugin needs cairo, pango and dbus from the system: load it once
    // to be sure, or libdecor silently falls back to no decorations. The handle
    // is kept, never dlclose'd: pango registers GObject types, and a module that
    // did must not be unloaded and reloaded (GLib refuses the re-registration
    // when libdecor loads the plugin for real). libdecor's dlopen of the same
    // path just takes another reference.
    std::string plugin_err;
    for (const std::string& p : plugin_files(st.plugin_dir))
    {
        if (void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL))
        {
            st.plugin = p;
            sLibdecorPluginHandle = h;
            break;
        }
        const char* err = dlerror();
        plugin_err = err ? err : p;
    }
    st.available = !st.plugin.empty();
    if (!st.available)
    {
        st.why += (st.why.empty() ? "" : "; ") + ("no loadable plugin in " + st.plugin_dir)
                  + (plugin_err.empty() ? "" : " (" + plugin_err + ")");
    }
    return st;
}

struct VideoBackendChoice
{
    bool          set_hint = false;
    std::string   hint;
    std::string   reason;
    EWaylandProbe wayland = EWaylandProbe::NO_COMPOSITOR;
    bool          probed = false;
    std::string   wayland_detail;
    bool          libdecor_tried = false;
    LibdecorState libdecor;
};

VideoBackendChoice sVideoChoice;

std::string lowercase(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

void choose_video_backend()
{
    VideoBackendChoice& c = sVideoChoice;
    const char* env = getenv("SDL_VIDEO_DRIVER");
    const std::string pref = lowercase(sVideoBackend);
    bool may_use_wayland = true;

    if (env && *env)
    {
        // SDL reads the environment itself; a normal-priority hint would lose anyway.
        c.hint = env;
        c.reason = "SDL_VIDEO_DRIVER environment variable";
        may_use_wayland = lowercase(env).find("wayland") != std::string::npos;
    }
    else if (pref == "x11")
    {
        // X11 (XWayland under a Wayland session); Wayland only if there is no X server.
        c.set_hint = true;
        c.hint = "x11,wayland";
        c.reason = "setting LinuxDisplayBackend=x11";
        may_use_wayland = false;
    }
    else if (pref == "wayland")
    {
        c.set_hint = true;
        c.hint = "wayland,x11";
        c.reason = "setting LinuxDisplayBackend=wayland";
    }
    else
    {
        c.set_hint = true;
        c.hint = "wayland,x11";
        c.reason = "auto";
    }

    if (may_use_wayland)
    {
        c.probed = true;
        c.wayland = probe_wayland(c.wayland_detail);
        if (c.wayland == EWaylandProbe::NO_SSD)
        {
            c.libdecor_tried = true;
            c.libdecor = prepare_libdecor();
        }
    }

    if (c.reason == "auto")
    {
        if (c.wayland == EWaylandProbe::NO_COMPOSITOR)
        {
            c.reason = "auto: " + c.wayland_detail + ", SDL will use X11";
        }
        else if (c.wayland == EWaylandProbe::SSD)
        {
            c.reason = "auto: the compositor draws window decorations (xdg-decoration)";
        }
        else if (c.libdecor.available)
        {
            c.reason = "auto: client-side decorations through " + c.libdecor.source + " libdecor";
        }
        else
        {
            // The safety net: a native Wayland window here would have no title
            // bar or border. X11/XWayland gets one from the window manager.
            c.hint = "x11,wayland";
            c.reason = "auto FALLBACK to X11: no server-side decorations and libdecor is unusable, "
                       "so a Wayland window would have no title bar";
        }
    }
    else if (may_use_wayland && c.wayland == EWaylandProbe::NO_SSD && !c.libdecor.available)
    {
        LL_WARNS("SDL") << "Display backend: Wayland requested (" << c.reason << ") but the compositor has no server-side "
                        << "decorations and libdecor is unusable (" << c.libdecor.why
                        << "): the window will have no title bar or border." << LL_ENDL;
    }

    if (c.set_hint)
    {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, c.hint.c_str());
    }

    std::string ssd = !c.probed ? "not checked"
                    : c.wayland == EWaylandProbe::SSD ? "yes"
                    : c.wayland == EWaylandProbe::NO_SSD ? "no" : "n/a (" + c.wayland_detail + ")";
    std::string decor;
    if (!c.libdecor_tried)
    {
        decor = "not needed";
    }
    else if (c.libdecor.available)
    {
        decor = c.libdecor.source + " " + c.libdecor.lib + ", plugin dir " + c.libdecor.plugin_dir + " (" + c.libdecor.plugin + ")";
    }
    else
    {
        decor = "UNAVAILABLE: " + c.libdecor.why + (c.libdecor.lib.empty() ? "" : " [loaded " + c.libdecor.lib + "]");
    }
    LL_INFOS("SDL") << "Display backend: video driver order '" << c.hint << "' (" << c.reason << "); xdg-decoration: " << ssd
                    << "; libdecor: " << decor << LL_ENDL;
}

// After SDL_INIT_VIDEO: what SDL actually picked, and whether it pulled libdecor in.
void log_video_backend_result()
{
    const char* driver = SDL_GetCurrentVideoDriver();
    std::string decor_path;
    if (void* h = dlopen("libdecor-0.so.0", RTLD_NOW | RTLD_NOLOAD))
    {
        decor_path = loaded_path(h);
        dlclose(h);
    }
    const char* plugin_dir = getenv("LIBDECOR_PLUGIN_DIR");
    LL_INFOS("SDL") << "Display backend: SDL video driver '" << (driver ? driver : "?") << "'; libdecor in process: "
                    << (decor_path.empty() ? std::string("no") : decor_path) << "; LIBDECOR_PLUGIN_DIR="
                    << (plugin_dir ? plugin_dir : "(unset, libdecor's built-in dir)") << LL_ENDL;
    if (driver && strcmp(driver, "wayland") == 0 && sVideoChoice.wayland == EWaylandProbe::NO_SSD
        && (decor_path.empty() || !sVideoChoice.libdecor.available))
    {
        LL_WARNS("SDL") << "Native Wayland without server-side decorations or libdecor: the window has no title bar or border. "
                        << "Choose X11 in Preferences > Graphics > Advanced > Display backend, or start with SDL_VIDEO_DRIVER=x11."
                        << LL_ENDL;
    }
}
#endif // LL_LINUX && LL_SDL_WINDOW
} // namespace

void set_sdl_video_backend(const std::string& backend)
{
    sVideoBackend = backend.empty() ? std::string("auto") : backend;
}

void init_sdl(const std::string& app_name)
{
#ifndef LL_SDL_APP
    SDL_SetMainReady();

    SDL_SetLogOutputFunction(&sdl_logger, nullptr);
#endif

#if LL_WINDOWS && defined(LL_SDL_WINDOW)
    Uint32 style = 0;
#if defined(CS_BYTEALIGNCLIENT) && defined(CS_OWNDC)
    style = (CS_BYTEALIGNCLIENT | CS_OWNDC);
#endif
    SDL_RegisterApp(app_name.c_str(), style, nullptr);
#endif

    const int c_sdl_version = SDL_VERSION;
    LL_INFOS() << "Compiled against SDL "
               << SDL_VERSIONNUM_MAJOR(c_sdl_version) << "."
               << SDL_VERSIONNUM_MINOR(c_sdl_version) << "."
               << SDL_VERSIONNUM_MICRO(c_sdl_version) << LL_ENDL;
    const int r_sdl_version = SDL_GetVersion();
    LL_INFOS() << "Running with SDL "
               << SDL_VERSIONNUM_MAJOR(r_sdl_version) << "."
               << SDL_VERSIONNUM_MINOR(r_sdl_version) << "."
               << SDL_VERSIONNUM_MICRO(r_sdl_version) << LL_ENDL;

#if LL_SDL_WINDOW
    // For linux we SDL_INIT_VIDEO and _AUDIO
    std::initializer_list<std::tuple< char const*, char const * > > hintList =
            {
                    {SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR,"0"},
                    {SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH,"1"},
                    // The viewer owns relative mode explicitly (LLViewerWindow::
                    // moveCursorToCenter / updateMouseDelta). SDL's warp emulation
                    // (auto-relative on repeated center warps with a hidden cursor)
                    // is torn down behind our back by SDL_ShowCursor, leaving the
                    // window relative flag set with global relative mode off -> a
                    // drag started right after another receives no motion at all.
                    {SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE,"0"},
            };

    for (auto hint: hintList)
    {
        SDL_SetHint(std::get<0>(hint), std::get<1>(hint));
    }

#if LL_LINUX
    // Native Wayland first (per-monitor fractional scaling, no XWayland) when the
    // window will get decorations, X11 otherwise; the LinuxDisplayBackend setting
    // can force either. A normal-priority hint: SDL_VIDEO_DRIVER in the
    // environment still overrides it.
    choose_video_backend();
#endif

    std::initializer_list<std::tuple<uint32_t, char const*, bool>> initList=
            {
                {SDL_INIT_VIDEO,"SDL_INIT_VIDEO", true},
            };
#else
    // For non-linux platforms we still SDL_INIT_VIDEO because it is a pre-requisite
    // for SDL_INIT_GAMECONTROLLER.
    std::initializer_list<std::tuple<uint32_t, char const*, bool>> initList=
            {
                {SDL_INIT_VIDEO,"SDL_INIT_VIDEO", false},
            };
#endif // LL_LINUX
    // We SDL_INIT_GAMECONTROLLER later in the startup process to make it
    // more likely we'll catch initial SDL_CONTROLLERDEVICEADDED events.

    for (auto subSystem : initList)
    {
        if (!SDL_InitSubSystem(std::get<0>(subSystem)))
        {
            LL_WARNS() << "SDL_InitSubSystem for " << std::get<1>(subSystem) << " failed " << SDL_GetError() << LL_ENDL;

            if (std::get<2>(subSystem))
            {
                OSMessageBox("SDL_Init() failure", "error", OSMB_OK);
                return;
            }
        }
    }
#if LL_LINUX && LL_SDL_WINDOW
    log_video_backend_result();
#endif
}

void quit_sdl()
{
#if LL_WINDOWS && defined(LL_SDL_WINDOW)
    SDL_UnregisterApp();
#endif
    SDL_Quit();
}
