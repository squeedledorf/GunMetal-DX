#!/usr/bin/env python3
"""Release-candidate validation pass over the test-control channel (see slctl.py).

Run on the test box against a logged-in TESTCTL viewer started under the Vulkan validation
layers (slvalidate.sh does the launch). Walks every graphics setting through its values, swaps
the icon set, opens and closes the common floaters, takes snapshots, moves the camera and
teleports, writing a "SLVAL step <name>" marker into Slipstream.log before each step so the
validation messages can be attributed to the step that caused them (slval_report.py).
Every setting is restored to its original value afterwards.

  slval.py [PLAN.json] [OUTDIR]

PLAN.json (optional) overrides the defaults below: {"settle": 4, "matrix": [[name, {..}], ...],
"teleports": [[region, x, y, z], ...], "floaters": [...]}.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from slctl import Channel  # noqa: E402

SETTLE = 4.0

# One entry per step: a name and the settings it applies (on top of the originals). Each
# group returns to the originals before the next, so steps don't compound across groups.
MATRIX = [
    ("preset", [{"RenderQualityPerformance": v} for v in (0, 1, 2, 3, 4, 5, 6)]),
    ("shadows", [{"RenderShadowDetail": 0}, {"RenderShadowDetail": 1}, {"RenderShadowDetail": 2},
                 {"RenderVKSunShadows": 0}, {"RenderVKSunShadows": 1},
                 {"RenderVKProjectorShadows": 0}, {"RenderVKProjectorShadows": 1},
                 {"RenderVKShadowResolution": 512}, {"RenderVKShadowResolution": 2048},
                 {"RenderVKShadowResolution": 4096}, {"RenderVKTranslucentShadows": 0},
                 {"RenderVKTranslucentShadows": 1}, {"RenderVKShadowUpdate": 0},
                 {"RenderVKShadowUpdate": 1}]),
    ("probes", [{"RenderReflectionsEnabled": 0}, {"RenderReflectionsEnabled": 1},
                {"RenderReflectionProbeDetail": 0}, {"RenderReflectionProbeDetail": 1},
                {"RenderReflectionProbeDetail": 2}, {"RenderReflectionProbeDetail": 3},
                {"RenderReflectionProbeLevel": 0}, {"RenderReflectionProbeLevel": 1},
                {"RenderReflectionProbeLevel": 2}, {"RenderReflectionProbeLevel": 3},
                {"RenderReflectionProbeResolution": 128}, {"RenderReflectionProbeResolution": 512},
                {"RenderReflectionProbeCount": 16}, {"RenderReflectionProbeCount": 256},
                {"RenderVKProbeFormat": 0}, {"RenderVKProbeFormat": 1}]),
    ("reflections", [{"RenderScreenSpaceReflections": 1}, {"RenderScreenSpaceReflections": 0},
                     {"RenderMirrors": 1}, {"RenderMirrors": 0},
                     {"RenderTransparentWater": False}, {"RenderTransparentWater": True},
                     {"RenderHeroProbeResolution": 256}, {"RenderHeroProbeResolution": 1024}]),
    ("upscale_aa", [{"RenderVKUpscaler": 0}, {"RenderVKUpscaler": 1}, {"RenderVKUpscaler": 2},
                    {"RenderVKUpscaler": 1, "RenderVKRenderScale": 0.5},
                    {"RenderVKUpscaler": 1, "RenderVKRenderScale": 0.67},
                    {"RenderVKUpscaler": 0, "RenderVKRenderScale": 1},
                    {"RenderFSAAType": 0}, {"RenderFSAAType": 1}, {"RenderFSAAType": 2},
                    {"RenderCASSharpness": 0.0}, {"RenderCASSharpness": 1.0}]),
    ("ao", [{"RenderDeferredSSAO": False}, {"RenderDeferredSSAO": True},
            {"RenderVKAOMethod": 0}, {"RenderVKAOMethod": 1}, {"RenderVKAOMethod": 2},
            {"RenderVKGTAOQuality": 0}, {"RenderVKGTAOQuality": 2},
            {"RenderVKAOStandard": 0}, {"RenderVKAOStandard": 1},
            {"RenderVKSpecularOcclusion": 0}, {"RenderVKSpecularOcclusion": 1}]),
    ("atmosphere", [{"RenderVKVolumetricFog": 0}, {"RenderVKVolumetricFog": 1},
                    {"RenderVKVolumetricFogQuality": 0}, {"RenderVKVolumetricFogQuality": 2},
                    {"RenderVKVolumetricClouds": 0}, {"RenderVKVolumetricClouds": 1},
                    {"RenderVKVolCloudQuality": 0}, {"RenderVKVolCloudQuality": 2},
                    {"RenderVKVolCloudShadows": 0}, {"RenderVKVolCloudShadows": 1},
                    {"RenderVKGodRays": 0}, {"RenderVKGodRays": 1}]),
    ("post", [{"RenderGlow": False}, {"RenderGlow": True}, {"RenderDepthOfField": 1},
              {"RenderDepthOfField": 0}, {"RenderVKMotionBlur": 1}, {"RenderVKMotionBlur": 0},
              {"RenderVKCel": 1}, {"RenderVKCel": 0}, {"RenderDynamicExposureEnabled": False},
              {"RenderDynamicExposureEnabled": True}, {"SlipstreamRenderTonemapType": 0},
              {"SlipstreamRenderTonemapType": 1}, {"SlipstreamRenderTonemapType": 2},
              {"RenderDisableVintageMode": False}, {"RenderDisableVintageMode": True}]),
    ("particles", [{"RenderVKParticleFog": 0}, {"RenderVKParticleFog": 1},
                   {"RenderVKParticleShadows": 0}, {"RenderVKParticleShadows": 1},
                   {"RenderVKParticleLowRes": 0}, {"RenderVKParticleLowRes": 1},
                   {"RenderMaxPartCount": 0}, {"RenderMaxPartCount": 8192}]),
    ("lights", [{"RenderVKLightGrid": 0}, {"RenderVKLightGrid": 1},
                {"RenderLocalLightCount": 8}, {"RenderLocalLightCount": 256},
                {"RenderAttachedLights": False}, {"RenderAttachedLights": True}]),
    ("geometry", [{"RenderFarClip": 64}, {"RenderFarClip": 256}, {"RenderFarClip": 512},
                  {"RenderVolumeLODFactor": 0.5}, {"RenderVolumeLODFactor": 4.0},
                  {"RenderVKClusterLOD": 0}, {"RenderVKClusterLOD": 1},
                  {"RenderVKTerrainSmooth": 0}, {"RenderVKTerrainSmooth": 1},
                  {"RenderTerrainPBRDetail": 0}, {"RenderTerrainPBRDetail": -1},
                  {"SlipstreamRenderTerrainHexTiling": False},
                  {"SlipstreamRenderTerrainHexTiling": True}, {"RenderVKReducedAvatars": 1},
                  {"RenderVKReducedAvatars": 0}]),
    ("textures", [{"RenderVKTextureCompression": v} for v in (0, 1, 2, 3, 4)]
                 + [{"RenderMaxTextureResolution": 512}, {"RenderMaxTextureResolution": 2048},
                    {"RenderAnisotropicLevel": 0}, {"RenderAnisotropicLevel": 16}]),
    # Ray tracing: on a GPU without ray queries these must be harmless no-ops (the no-RT
    # paths); on an RT GPU they exercise every RT feature switch.
    ("raytracing", [{"RenderVKRTReflections": 1}, {"RenderVKRTReflections": 0},
                    {"RenderVKGI": 1}, {"RenderVKGI": 0}, {"RenderVKReSTIRDI": 1},
                    {"RenderVKReSTIRDI": 0}, {"RenderVKProjStochastic": 1},
                    {"RenderVKProjStochastic": 0}, {"RenderVKRTShadowMerge": 1},
                    {"RenderVKRTShadowMerge": 0}, {"RenderVKVisQueue": 2},
                    {"RenderVKVisQueue": 0}]),
    ("ui", [{"SLClassicIcons": True}, {"SLClassicIcons": False}, {"UIScaleFactor": 1.5},
            {"UIScaleFactor": 1.0}, {"PrimTextMaxDrawDistance": 64.0},
            {"PrimTextMaxDrawDistance": 16.0}]),
]

FLOATERS = ["preferences", "inventory", "snapshot", "world_map", "build", "im_container",
            "people", "camera", "about_land", "debug_settings", "my_environments",
            "report_bug"]


def step(ch, name, settle):
    ch.call({"cmd": "log", "text": "SLVAL step " + name})
    time.sleep(settle)
    st = ch.call({"cmd": "status"})
    return {"step": name, "alive": bool(st.get("ok", True)), "fps": st.get("fps")}


def main():
    args = [a for a in sys.argv[1:]]
    plan = json.load(open(args[0])) if args and args[0].endswith(".json") else {}
    outdir = os.path.abspath(args[-1]) if args and not args[-1].endswith(".json") \
        else os.path.expanduser("~/slipstream/slval")
    os.makedirs(outdir, exist_ok=True)
    settle = float(plan.get("settle", SETTLE))
    matrix = plan.get("matrix", MATRIX)
    floaters = plan.get("floaters", FLOATERS)

    ch = Channel(timeout=120.0)
    if not ch.call({"cmd": "status"}).get("logged_in"):
        raise SystemExit("not logged in")

    keys = sorted({k for _, steps in matrix for s in steps for k in s})
    original = {k: ch.call({"cmd": "get", "name": k}).get("value") for k in keys}
    json.dump(original, open(os.path.join(outdir, "original.json"), "w"), indent=1)

    results = []
    ch.call({"cmd": "log", "text": "SLVAL begin"})
    results.append(step(ch, "baseline", settle * 2))
    try:
        for group, steps in matrix:
            for i, settings in enumerate(steps):
                for k, v in settings.items():
                    ch.call({"cmd": "set", "name": k, "value": v})
                label = "%s/%d %s" % (group, i, " ".join("%s=%s" % kv for kv in settings.items()))
                results.append(step(ch, label, settle))
            for k in {k for s in steps for k in s}:
                ch.call({"cmd": "set", "name": k, "value": original[k]})
            results.append(step(ch, group + "/restored", settle))

        for name in floaters:
            ch.call({"cmd": "event", "pump": "LLFloaterReg", "data": {"op": "showInstance", "name": name}})
            results.append(step(ch, "floater/open " + name, 1.5))
            ch.call({"cmd": "event", "pump": "LLFloaterReg", "data": {"op": "hideInstance", "name": name}})
        results.append(step(ch, "floater/closed", settle))

        ch.call({"cmd": "log", "text": "SLVAL step snapshot"})
        snap = ch.call({"cmd": "snapshot", "path": os.path.join(outdir, "snap.png"), "showui": True})
        results.append({"step": "snapshot", "ok": snap.get("ok", False)})
        ch.call({"cmd": "snapshot", "path": os.path.join(outdir, "snap_noui.png"), "showui": False,
                 "width": 3840, "height": 2160})
        results.append(step(ch, "snapshot_4k", settle))

        st = ch.call({"cmd": "status"})
        pos = st.get("pos") or [128, 128, 30]
        for j, (dx, dy) in enumerate(((20, 0), (0, 20), (-20, 0), (0, -20))):
            ch.call({"cmd": "camera", "pos": [pos[0] + dx, pos[1] + dy, pos[2] + 8], "focus": pos})
            results.append(step(ch, "camera/%d" % j, 2.0))
        ch.call({"cmd": "camera", "reset": True})

        for region, x, y, z in plan.get("teleports", []):
            ch.call({"cmd": "teleport", "region": region, "x": x, "y": y, "z": z})
            time.sleep(15.0)
            results.append(step(ch, "teleport " + region, settle * 2))
    finally:
        for k, v in original.items():
            ch.call({"cmd": "set", "name": k, "value": v})
        ch.call({"cmd": "log", "text": "SLVAL end"})
        json.dump(results, open(os.path.join(outdir, "steps.json"), "w"), indent=1)
    print(json.dumps({"ok": True, "steps": len(results), "outdir": outdir}))


if __name__ == "__main__":
    main()
