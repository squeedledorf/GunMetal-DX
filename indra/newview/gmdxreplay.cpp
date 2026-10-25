/**
 * @file gmdxreplay.cpp
 * @brief GunMetal-DX instant replay: settings, saving, notifications (the encoder is vkgfx/gmdxreplayvk.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "gmdxreplay.h"
#include "gmdxreplayaudio.h"

#include "llfile.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "vkgfx/gmdxreplayvk.h"
#include "vkgfx/vk_device.h"
#include "vk_world_shaders_spv.h"   // (generated at build time, build/newview/vkshaders)

#include <chrono>
#include <ctime>
#include <fstream>
#include <memory>

#if LL_WINDOWS
#include <shlobj.h>
#endif

namespace
{
std::unique_ptr<GMDXReplayEncoder> s_enc;
std::unique_ptr<GMDXReplayAudio> s_audio;   // with s_enc, while GMDXReplayAudio is on
bool s_failed = false;          // init failed on this device: don't retry every frame
double s_startedAt = -1.0;
double s_lastStatus = 0.0;
bool s_testSaved = false;

double now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void notify(const std::string& msg)
{
    LLSD args;
    args["MESSAGE"] = msg;
    LLNotificationsUtil::add("SystemMessageTip", args);
}

// <Videos>/GunMetal-DX, or GMDXReplayFolder
std::string replayFolder()
{
    std::string dir = gSavedSettings.getString("GMDXReplayFolder");
    if (!dir.empty()) return dir;
    std::string videos;
#if LL_WINDOWS
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p)) && p)
        videos = ll_convert_wide_to_string(p);
    CoTaskMemFree(p);
#else
    const char* home = getenv("HOME");
    const std::string h = home ? home : ".";
    // XDG_VIDEOS_DIR from user-dirs.dirs (e.g. XDG_VIDEOS_DIR="$HOME/Videos")
    const char* cfg = getenv("XDG_CONFIG_HOME");
    std::ifstream in((cfg && *cfg ? std::string(cfg) : h + "/.config") + "/user-dirs.dirs");
    for (std::string line; std::getline(in, line);)
    {
        if (line.rfind("XDG_VIDEOS_DIR=", 0) != 0) continue;
        std::string v = line.substr(15);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = h + v.substr(5);
        if (!v.empty() && v != h) videos = v;
    }
    if (videos.empty()) videos = h + "/Videos";
#endif
    return videos + gDirUtilp->getDirDelimiter() + "GunMetal-DX";
}

GMDXReplayEncoder::Config config()
{
    GMDXReplayEncoder::Config c;
    c.seconds = gSavedSettings.getU32("GMDXReplaySeconds");
    c.maxHeight = gSavedSettings.getU32("GMDXReplayMaxHeight");
    c.fps = gSavedSettings.getU32("GMDXReplayFPS");
    c.bitrateKbps = gSavedSettings.getU32("GMDXReplayBitrateKbps");
    return c;
}

void reportSaves()
{
    bool ok = false;
    std::string msg;
    if (s_enc && s_enc->takeError(msg))
    {
        LL_WARNS("GMDXReplay") << msg << LL_ENDL;
        notify("Instant replay " + msg);
        msg.clear();
    }
    if (s_audio && s_audio->takeError(msg))
    {
        LL_WARNS("GMDXReplay") << "audio: " << msg << LL_ENDL;
        notify("Replay audio isn't recording: " + msg);
        msg.clear();
    }
    if (s_enc && s_enc->takeSaveResult(ok, msg))
    {
        LL_INFOS("GMDXReplay") << (ok ? "saved " : "save failed: ") << msg << LL_ENDL;
        notify(ok ? "Replay saved: " + msg : "Replay not saved: " + msg);
    }
}
} // namespace

void GMDXReplay::captureFrame(VkCommandBuffer cmd, VkImage image, VkFormat format, uint32_t w, uint32_t h, VkDeviceContext& dev)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "GMDXReplayEnabled", false);
    reportSaves();
    if (!s_on)
    {
        if (s_enc && !s_enc->saving())
        {
            LL_INFOS("GMDXReplay") << "off; encoder released" << LL_ENDL;
            s_enc.reset();
            s_audio.reset();
        }
        return;
    }
    if (!s_enc)
    {
        if (s_failed) return;
        if (!dev.encodeQueue())
        {
            s_failed = true;
            LL_WARNS("GMDXReplay") << "this GPU / driver has no H.264 video encode queue: instant replay unavailable" << LL_ENDL;
            notify("Instant replay isn't available: this graphics driver can't encode H.264 video.");
            return;
        }
        GMDXReplayEncoder::Device d;
        d.instance = dev.instance();
        d.physical = dev.physicalDevice();
        d.device = dev.device();
        d.gfxFamily = dev.queueFamily();
        d.gfxQueue = dev.queue();
        d.encodeFamily = dev.encodeFamily();
        d.encodeQueue = dev.encodeQueue();
        s_enc = std::make_unique<GMDXReplayEncoder>();
        std::string err;
        if (!s_enc->init(d, kGmdxReplayNv12Spv, kGmdxReplayNv12Spv_bytes, err))
        {
            s_failed = true;
            s_enc.reset();
            LL_WARNS("GMDXReplay") << "encoder init failed: " << err << LL_ENDL;
            notify("Instant replay couldn't start: " + err);
            return;
        }
        s_startedAt = now();
        LL_INFOS("GMDXReplay") << "on: keeping the last " << gSavedSettings.getU32("GMDXReplaySeconds") << " s" << LL_ENDL;
    }
    if (!s_enc->ready()) return;
    static LLCachedControl<bool> s_audioOn(gSavedSettings, "GMDXReplayAudio", true);
    if (s_audioOn && !s_audio)
    {
        s_audio = std::make_unique<GMDXReplayAudio>();
        s_audio->start();
    }
    else if (!s_audioOn && s_audio)
    {
        s_audio.reset();
    }
    const double t = now();
    const GMDXReplayEncoder::Config cfg = config();
    if (s_audio) s_audio->setKeepSeconds(cfg.seconds);
    s_enc->recordCapture(cmd, image, format, w, h, t, cfg);

    // status line every 30 s; GMDX_REPLAY_SAVE_AFTER=<seconds>: one save that long after start (tests)
    if (t - s_lastStatus > 30.0)
    {
        s_lastStatus = t;
        LL_INFOS("GMDXReplay") << s_enc->describe() << (s_audio ? "; " + s_audio->describe() : std::string()) << LL_ENDL;
    }
    static const char* s_test = getenv("GMDX_REPLAY_SAVE_AFTER");
    if (s_test && !s_testSaved && t - s_startedAt > atof(s_test))
    {
        s_testSaved = true;
        save();
    }
}

void GMDXReplay::afterSubmit(bool frameRan)
{
    if (s_enc && s_enc->ready())
    {
        if (frameRan) s_enc->submit();
        else s_enc->cancel();
        s_enc->poll(now());
    }
}

void GMDXReplay::shutdown(bool deviceLost)
{
    s_audio.reset();
    if (s_enc)
    {
        if (deviceLost) s_enc->abandon();
        else s_enc->shutdown();
        s_enc.reset();
    }
}

void GMDXReplay::save()
{
    if (!gSavedSettings.getBOOL("GMDXReplayEnabled"))
    {
        notify("Instant replay is off. Turn it on in Combat > Instant Replay; it keeps the last "
               + std::to_string(gSavedSettings.getU32("GMDXReplaySeconds")) + " seconds from then on.");
        return;
    }
    if (!s_enc || !s_enc->ready())
    {
        notify("Instant replay isn't running on this graphics driver.");
        return;
    }
    const std::string dir = replayFolder();
    LLFile::mkdir(gDirUtilp->getDirName(dir));
    LLFile::mkdir(dir);
    const std::time_t tt = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", std::localtime(&tt));
    const std::string path = dir + gDirUtilp->getDirDelimiter() + "GunMetal-DX " + stamp + ".mp4";
    const double buffered = s_enc->bufferedSeconds();
    const uint32_t seconds = gSavedSettings.getU32("GMDXReplaySeconds");
    const double t = now();
    if (!s_enc->saveAsync(path, seconds, t, s_audio ? s_audio->clip(t, seconds) : GMDXReplayAudioClip()))
    {
        notify(buffered < 1.0 ? "Nothing to save yet." : "A replay is still being saved.");
        return;
    }
    LL_INFOS("GMDXReplay") << "saving " << path << " (" << buffered << " s buffered)" << LL_ENDL;
}
