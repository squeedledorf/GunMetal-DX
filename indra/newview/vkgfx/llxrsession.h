/**
 * @file llxrsession.h
 * @brief VR milestone M0 (#1073, epic #1072): the OpenXR session driven by the Vulkan viewer.
 *
 * STARTUP-ONLY. VR is chosen at launch (SlipstreamVREnabled, or --vr / the login screen's
 * "Start in VR" relaunch). Without it nothing in here runs: active() stays false, the OpenXR
 * loader is never opened, no hook is installed into VkDeviceContext, and the per-frame entry
 * points in the viewer are skipped by that one flag. The desktop path is unchanged.
 *
 * With VR chosen:
 *  1. startProbe() (right after the settings load, on a worker: xrCreateInstance can take
 *     seconds when it brings SteamVR up): loader, instance with XR_KHR_vulkan_enable2, the HMD
 *     system, view configuration, Vulkan requirements. finishProbe() joins it before the Vulkan
 *     context is created. Any failure (no loader, no runtime, no headset) logs one reason and
 *     leaves VR off: the context is then created exactly as on the desktop, and the viewer posts
 *     a notice.
 *  2. installDeviceHooks(): the runtime creates OUR VkInstance / VkDevice from our unchanged
 *     create infos (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR) on the GPU it requires
 *     (xrGetVulkanGraphicsDevice2KHR, which overrides the automatic choice). A GPU that does not
 *     meet the requirements, or a runtime that refuses the device, refuses VR: desktop continues.
 *  3. createSession(): session on the graphics queue (family of the device, index 0), the app
 *     reference space (M1: see below) + VIEW, one swapchain per eye (B8G8R8A8_SRGB images with MUTABLE_FORMAT and
 *     UNORM views: the viewer's final passes write sRGB-encoded bytes into a UNORM window image,
 *     the UNORM view keeps those bytes and the compositor decodes them as sRGB).
 *  4. Per frame, all on the main thread (every queue submit is on the main thread, so the
 *     runtime's use of the graphics queue inside the XR calls needs no lock):
 *       waitFrame()   top of the frame, instead of waitFrameSlot: events, xrWaitFrame (the
 *                     pacing), xrBeginFrame;
 *       recordFrame() inside renderFrame, after the UI: the eyes (M1: the world image, see
 *                     below; without a world, dark), and the UI copied into a UI swapchain
 *                     shown as the UI wall (#1076: the window image before login, the
 *                     renderer's UI-only image with the world, while the wall is shown);
 *       endFrame()    after the frame's submit, before the window present: release, xrEndFrame.
 *     A begun frame is always ended: endFrame() with nothing recorded submits zero layers, and
 *     waitFrame() ends a frame still open from an early-out before it waits again.
 *  5. shutdownSession() before the device goes, shutdownInstance() after the VkInstance.
 *
 * Log tag "VR": every step and every runtime answer (system, recommended sizes, formats, refresh).
 *
 * M1 (#1074): the real world in the headset, mono and head tracked.
 *  - The app reference space is LOCAL_FLOOR (XR_EXT_local_floor) where offered, else STAGE,
 *    else LOCAL. waitFrame() locates both views for the frame's predicted display time right
 *    after xrBeginFrame, so the camera (LLVRCamera, in idle) and the projection layer use the
 *    same poses; headPose() hands the cyclopean pose and the render frustum out.
 *  - ONE image is rendered: a symmetric frustum covering both eyes' FOV at the eye swapchain
 *    aspect (renderTanHalfY). recordFrame() copies that image (B8G8R8A8_UNORM bytes, the
 *    viewer's sRGB-encoded output) bit-exact into both eye images (B8G8R8A8_SRGB: no
 *    re-encode, unlike a blit). Each eye's layer view declares the rendered orientation at
 *    that eye's own located position and (#1076) only its part of the image: the sub-rect of
 *    the symmetric render that covers that eye's own located FOV, with exactly that rect's FOV
 *    -- so what is declared equals the runtime's native per-eye projection, and no runtime
 *    has to reconcile a wider, symmetric declaration with its own eye FOVs (the first headset
 *    run, SteamVR on Linux: the quad layers looked different in each eye and moved with the
 *    head). Canted displays (eye orientation off the head's by > 1 degree) keep the whole
 *    image at the rendered FOV. Mono content: both eyes see the same image (no stereo
 *    depth) -- M1 by design.
 *
 * M3 (#1076): controllers and the UI layers.
 *  - Input: one action set, both hands (createActions has the binding table); synced and
 *    located in waitFrame for the head's display time; inputState() / haptic(). Consumers:
 *    LLVRInput (pointer, wall, locomotion, fly cam), LLVRHands (hand models + beam),
 *    LLVRCamera::handPose (agent-space poses for Pose Studio).
 *  - Layers beside the projection layer, set per frame (setUiLayers / setFade): the UI wall
 *    (cylinder, or flat facets along the same arc; world-locked in the app space), the wrist
 *    panel (a rect of the same UI image on the left aim pose), the comfort fade (head-locked).
 *    wallHit() tests a ray against exactly the submitted wall geometry.
 *  - frameTiming(): display period + missed slots per frame (LLVRSafety).
 */
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class VkDeviceContext;

// #1074: this frame's head pose (waitFrame), app reference space, OpenXR axes (+X right, +Y up,
// -Z forward), metres. Plain data: LLVRCamera turns it into the SL camera.
struct LLXrHeadPose
{
    bool valid = false;           // a pose was located (this frame, or held from the last tracked one)
    bool tracked = false;         // orientation valid THIS frame
    // #1076: the runtime TRACKS the head this frame (XR_VIEW_STATE_ORIENTATION_TRACKED_BIT and
    // POSITION_TRACKED_BIT, not just "valid": a held or inferred pose), and the session is
    // VISIBLE / FOCUSED (the frames reach the headset: someone is looking). A placement (UI
    // wall, recentre) needs both -- and a plausible height: SteamVR has reported a fully
    // tracked LOCAL_FLOOR head at 5 cm before it settled (the first headset run). See
    // LLXrHeadPose::placeable().
    bool headTracked = false;
    bool shown = false;
    bool floorSpace = false;      // y is the height above the floor (LOCAL_FLOOR / STAGE); LOCAL: y = 0 at the start head height
    float position[3] = {0.f, 0.f, 0.f};          // cyclopean: midpoint of the two eyes
    float orientation[4] = {0.f, 0.f, 0.f, 1.f};  // x y z w (the head's, = the render orientation)
    float tanHalfY = 1.f;         // the rendered symmetric frustum: tan(vertical half angle)
    float aspect = 1.f;           // ... and its width / height (= the eye swapchain aspect)
    uint32_t renderWidth = 0, renderHeight = 0;   // the eye swapchain size (the world renders at it)
    uint32_t spaceGeneration = 0; // bumps when the runtime re-centres the app space
    // #1075 (layered stereo): each EYE's located pose (app space, OpenXR axes) and FOV
    // (angleLeft, angleRight, angleUp, angleDown, radians; left / down negative) for the same
    // display time, eye 0 = left. eyesValid: both were located this frame (or held from the last
    // tracked one, with the head). The stereo camera (LLVRCamera::eyeView) builds one view per eye.
    bool  eyesValid = false;
    float eyePos[2][3] = {};
    float eyeOri[2][4] = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f, 1.f}};
    float eyeFov[2][4] = {};

    // #1076: this pose can anchor something in the room (the UI wall, the recentre reference):
    // tracked this frame, shown, and in a floor space a head height a person can have (0.4 m:
    // sitting on the floor .. 2.5 m). LOCAL has no floor: its height is not judged.
    bool placeable() const
    {
        if (!valid || !tracked || !headTracked || !shown) return false;
        return !floorSpace || (position[1] >= 0.4f && position[1] <= 2.5f);
    }
};

// #1076 (M3): the controllers, located for the same predicted display time as the head
// (waitFrame), app reference space, OpenXR axes, metres. Plain data: LLVRInput / LLVRCamera
// turn it into pointer, locomotion and agent-space hand poses (LLVRCamera::handPose, the
// Pose Studio hook).
enum : uint32_t { LLXR_HAND_LEFT = 0, LLXR_HAND_RIGHT = 1, LLXR_HAND_COUNT = 2 };
struct LLXrPose
{
    bool valid = false;                           // position and orientation valid this frame
    float position[3] = {0.f, 0.f, 0.f};
    float orientation[4] = {0.f, 0.f, 0.f, 1.f};  // x y z w
};
struct LLXrHandState
{
    bool active = false;          // a controller is bound to this hand and reports (FOCUSED session)
    LLXrPose grip;                // the hand holding the controller (hand model, wrist panel)
    LLXrPose aim;                 // the pointing ray: from position along the pose's -Z
    bool select = false;          // trigger (simple controller: select): left click
    // #1076: how select was judged this frame (diagnostics): the boolean action (Index
    // trigger/click, simple select/click) and the trigger's analogue value (Index / Touch
    // trigger/value; -1 = that action is not active for this hand).
    bool selectClick = false;
    float selectValue = -1.f;
    bool context = false;         // Index / Touch A (Touch left: X): right click
    bool menu = false;            // Index left B, Touch / simple menu: show / hide the UI wall
    bool flyCam = false;          // Index / Touch right B: fly cam on / off
    bool stickClick = false;      // thumbstick press (left, held 1 s without pushing: recentre)
    float stick[2] = {0.f, 0.f};  // thumbstick: x right +, y forward / up +
};
struct LLXrInputState
{
    bool focused = false;         // the session has input focus: actions report real values
    LLXrHandState hand[LLXR_HAND_COUNT];
};

// #1076: the composition layers beside the world, set each frame by LLVRInput.
// The UI WALL: the desktop UI image on a curved wall, world-locked in the app space where it
// was summoned. Its vertical axis passes through wallAxis (the head position when placed; y =
// the wall's centre height); its centre lies kWallRadius away in direction yaw (rotation about
// +Y: the centre is at axis + R * (-sin yaw, 0, -cos yaw)); it spans kWallAngle of arc and
// wallAspect = UI image width / height. A cylinder layer where the runtime offers
// XR_KHR_composition_layer_cylinder, else kWallFacets flat quads along the same arc (each a
// column strip of the image). wallHit() uses exactly the geometry the layers are given.
// The WRIST PANEL (wall hidden): the part of the same UI image that holds the visible
// notification / IM toasts (wristRect, image pixels, top-left origin), on a small quad lying
// on the back of the left wrist (the left aim pose, kWristOffset behind / above it, facing the
// pose's +Y, text up along its -Z), kWristWidth wide.
struct LLXrUiLayers
{
    bool wall = false;
    float wallAxis[3] = {0.f, 0.f, 0.f};
    float wallYaw = 0.f;
    float wallAspect = 16.f / 9.f;
    bool wrist = false;
    int32_t wristRect[4] = {0, 0, 0, 0};   // x, y, width, height
    // #1134 the FLOATING MENU PANEL: the open context menu's part of the same UI image
    // (panelRect, image pixels, top-left origin), on one flat quad world-locked in the app
    // space: centre panelCenter, turned panelYaw about +Y (its +Z faces the viewer), panelSize
    // metres. LLVRInput places it near the pointed spot and hit-tests it itself.
    bool panel = false;
    int32_t panelRect[4] = {0, 0, 0, 0};   // x, y, width, height
    float panelCenter[3] = {0.f, 0.f, 0.f};
    float panelYaw = 0.f;
    float panelSize[2] = {0.f, 0.f};
    // #1076 diagnostics: bumped by every wall placement (the next submit logs the exact layer
    // poses once), and SlipstreamVRDebugLayers (0 normal; 1 wall layers only, no world; 2 the
    // wall as one flat quad; 3 the world declared as rendered -- whole image, symmetric FOV).
    uint32_t placeSerial = 0;
    uint32_t debugLayers = 0;
};

// #1075 (layered stereo): the world's depth for the depth layer (XR_KHR_composition_layer_depth):
// the image (layers = the world's views, layer v = eye v) in `layout` (left there), at the world
// extent, and the eye distance (m) a depth of 0 and of 1 stands for (reversed-Z: distAtZero is the
// far distance, distAtOne the near one). image null: no depth layer this frame.
struct LLXrWorldDepth
{
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D extent = {0, 0};
    uint32_t layers = 1;
    float distAtZero = 0.f, distAtOne = 0.f;
};

class LLXrSession
{
public:
    static LLXrSession& instance();

    // VR was chosen for this process and has not been given up: the per-frame entry points are
    // meaningful. False on the desktop (never set), and after a startup failure.
    static bool active() { return sActive; }

    struct Config
    {
        std::string viewerLibDir;   // the viewer's own library folder (loader looked up there first)
        std::string appVersion;     // "1.6.0.123" (logged; XR application version)
        float renderScale = 1.f;    // #1074 SlipstreamVRRenderScale: x the runtime's recommended eye size
        // #1075 (layered stereo, SlipstreamVRStereo, startup): the world renders one layer per
        // eye. The eyes then go out through ONE 2-layer array swapchain (fallback: one swapchain
        // per eye), each projection view declared at its own located pose / FOV with
        // imageArrayIndex = eye, plus the depth layer (XR_KHR_composition_layer_depth) where
        // offered. Off: M1 mono, exactly as before.
        bool stereo = false;
    };
    // Starts the probe worker (loader, instance, system, requirements). Main thread, once.
    void startProbe(const Config& cfg);
    // Joins the probe. True: VR can go on to the device hooks. False: failure() says why (VR off).
    bool finishProbe();

    // Before VkDeviceContext::initForPresent (VR only).
    void installDeviceHooks(VkDeviceContext& dev);
    // After the device exists. False: VR refused (failure()), the desktop continues.
    bool createSession(VkDeviceContext& dev);
    // Called with true when the session starts running (READY -> xrBeginSession) and false when
    // it stops / is destroyed: the context switches the window between non-FIFO and the user's
    // vsync.
    void setRunningCallback(std::function<void(bool)> cb) { mRunningCb = std::move(cb); }

    // Top of the viewer frame. True: an XR frame is begun (xrWaitFrame paced it) and its views
    // located. False: no session running, pace as on the desktop.
    bool waitFrame();
    // #1074: the pose located by the last waitFrame (held while tracking is lost). False: none yet.
    bool headPose(LLXrHeadPose& out) const;
    // #1074: the size the world renders at in VR (the eye swapchains). False: no session.
    bool renderExtent(uint32_t& w, uint32_t& h) const;
    // #1076: this frame's controller state (synced and located in waitFrame). False: no
    // session running (out stays default: nothing active).
    bool inputState(LLXrInputState& out) const;
    // #1076: a short vibration on a hand's controller (no-op without a session / binding).
    void haptic(uint32_t hand, float amplitude, float seconds);
    // #1076 (safety governor): this frame's pacing from xrWaitFrame -- the headset's display
    // period and the display slots missed since the previous frame (predicted display times
    // more than one period apart). False: no frame was begun this frame.
    bool frameTiming(double& periodSec, uint32_t& missedSlots) const;
    // #1076: black over everything in the headset, 0 (none) .. 1 (black): the comfort blink
    // (fly cam return). A head-locked layer; set each frame by LLVRInput.
    void setFade(float a) { mFade = a; }
    // #1076: this frame's UI layers (LLVRInput, before renderFrame).
    void setUiLayers(const LLXrUiLayers& l) { mUi = l; }
    const LLXrUiLayers& uiLayers() const { return mUi; }
    // #1076: the renderer should give recordFrame a UI-only image this frame (a VR world frame
    // with the wall shown). Without a world the window image is the wall's content.
    bool uiImageWanted() const { return mUi.wall || mUi.wrist || mUi.panel; }
    // #1076: a ray (app space: origin, unit direction) against the wall as submitted. True:
    // it hits; u, v in [0, 1] across the UI image (v down), dist along the ray (m).
    bool wallHit(const float origin[3], const float dir[3], float& u, float& v, float& dist) const;
    // Wall geometry (metres / radians): what LLVRInput places and the layers / wallHit use.
    static constexpr float kWallRadius = 2.5f;
    static constexpr float kWallAngle = 1.3962634f;   // 80 degrees: ~3.5 m of arc at 2.5 m
    static constexpr uint32_t kWallFacets = 7;       // flat-quad approximation without the cylinder extension
    // Wrist panel: in the left aim pose's frame (OpenXR axes: -Z along the forearm, forward).
    static constexpr float kWristOffset[3] = {0.f, 0.035f, 0.12f};
    static constexpr float kWristWidth = 0.18f;
    static constexpr float kWristMaxHeight = 0.22f;
    // The wrist panel's size (metres) for its rect: kWristWidth wide, narrower when the
    // height would pass kWristMaxHeight (the rect's aspect is kept).
    static void wristSize(const LLXrUiLayers& l, float& w, float& h);
    // #1074: LLVRCamera put this frame's head pose into the viewer camera (reset by waitFrame).
    // Only such a frame renders the world for the headset (eye size, VR frustum).
    void setWorldCameraApplied(bool on) { mCameraApplied = on; }
    bool worldCameraApplied() const { return mCameraApplied; }
    // Inside renderFrame, before the window image's present transition. No-op without a begun
    // frame. windowImage (may be null: no window image this frame, e.g. minimised) is in
    // COLOR_ATTACHMENT_OPTIMAL and left there. #1074: worldImage (null: no world this frame)
    // is the rendered world at renderExtent(), in TRANSFER_SRC_OPTIMAL (left there): copied
    // into both eyes. Without it the eyes are cleared dark and the window image is the UI
    // wall's content. #1076: uiImage (null: none) is the UI-only image of a VR world frame
    // (window format and size, premultiplied alpha), in TRANSFER_SRC_OPTIMAL (left there).
    // #1075: worldLayers = the world image's layers (1: mono, copied into both eyes as M1; the
    // stereo session's eye count: layer v is eye v, declared at that eye's located pose / FOV).
    // depth (optional, stereo only): the world depth for the depth layer.
    void recordFrame(VkCommandBuffer cmd, VkImage windowImage, VkFormat windowFormat, VkExtent2D windowExtent,
                     VkImage worldImage = VK_NULL_HANDLE, VkFormat worldFormat = VK_FORMAT_UNDEFINED,
                     VkExtent2D worldExtent = {0, 0}, VkImage uiImage = VK_NULL_HANDLE, uint32_t worldLayers = 1,
                     const LLXrWorldDepth* depth = nullptr);
    // #1075: stereo was requested for this session (Config::stereo; known from startProbe on).
    bool stereoWanted() const;
    // ... and the session renders it: the eye swapchains were made for stereo (array, or one per
    // eye) -- the world then renders one layer per eye (eyeCount layers).
    bool stereo() const;
    // #1075 (XR_KHR_visibility_mask, stereo sessions): eye's hidden-area mesh -- the pixels the
    // lens never shows -- as a triangle list, x y per vertex, in that eye's view space on the
    // z = -1 plane (tangents of the angles, y up). Empty: no mask for that eye (no extension, a
    // runtime without one, mono). The generation changes whenever the meshes are (re)fetched.
    uint32_t visibilityMaskGeneration() const;
    const std::vector<float>& visibilityMask(uint32_t eye) const;
    // After the frame's queue submit: release the images, xrEndFrame (zero layers when nothing
    // was recorded). No-op without a begun frame.
    void endFrame();

    void shutdownSession();    // before vkDestroyDevice (device must be idle)
    void shutdownInstance();   // after vkDestroyInstance

    // The session is running (READY -> xrBeginSession until STOPPING): xrWaitFrame paces the
    // viewer, so the main loop's own frame caps and background sleeps stay out of the way.
    bool sessionRunning() const;

    bool failed() const { return !mFailure.empty(); }
    const std::string& failure() const { return mFailure; }
    // A user-facing notice to show once ("" = none). Main thread.
    std::string takeNotice();

private:
    LLXrSession() = default;
    ~LLXrSession();
    LLXrSession(const LLXrSession&) = delete;
    LLXrSession& operator=(const LLXrSession&) = delete;

    struct Impl;
    Impl* mImpl = nullptr;
    std::string mFailure;
    std::string mNotice;
    std::function<void(bool)> mRunningCb;
    LLXrUiLayers mUi;
    float mFade = 0.f;
    bool mCameraApplied = false;
    static bool sActive;

    void probe();                         // worker
    void fail(const std::string& why, bool notice = true);
    void pollEvents();
    void fetchVisibilityMask();           // #1075: pollEvents, when the session asks for it
    void locateViews();                  // #1074: waitFrame, after xrBeginFrame
    void createActions();                 // #1076: createSession (action set, bindings, spaces)
    void syncInput();                     // #1076: waitFrame, after locateViews
    void logInteractionProfiles();        // #1076: on INTERACTION_PROFILE_CHANGED
    void setRunning(bool on);
    void destroySessionObjects();
    bool ensureUiSwapchain(VkFormat windowFormat, VkExtent2D windowExtent);
    void recordFade(VkCommandBuffer cmd);   // #1076: the fade layer's image for this frame
    void endFrameNow(bool withLayers);

    // VkDeviceContext hooks.
    VkResult hookCreateInstance(const VkInstanceCreateInfo& ci, VkInstance* out);
    VkPhysicalDevice hookPhysicalDevice(VkInstance inst);
    VkResult hookCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo& ci, VkDevice* out);
};
