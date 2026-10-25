#!/usr/bin/env bash
# LL-render-removal static audit (verification artifact #1 of 4; see
# the clean-pipeline-only rule). Each check greps for an invariant that keeps LL
# scene rendering unreachable. Exit 0 = all hold.
set -u
cd "$(dirname "$0")/.."
fail=0
chk() {  # chk <desc> <expect-nonzero-matches:1|0> <pattern> <file...>
    local desc="$1" want="$2" pat="$3"; shift 3
    local n
    n=$(grep -c -E "$pat" "$@" 2>/dev/null | awk -F: '{s+=$NF} END {print s+0}')
    if { [ "$want" = 1 ] && [ "$n" -gt 0 ]; } || { [ "$want" = 0 ] && [ "$n" -eq 0 ]; }; then
        echo "PASS: $desc"
    else
        echo "FAIL: $desc (matches=$n)"
        fail=1
    fi
}

N=indra/newview

chk "CLEAN_RENDER_ONLY hard-enabled"                    1 'constexpr bool CLEAN_RENDER_ONLY = true' $N/pipeline.cpp
chk "LL occlusion forced off"                           1 'LLPipeline::sUseOcclusion = 0'           $N/pipeline.cpp
chk "impostor scene render removed"                     0 'LLVOAvatar::updateImpostors\(\);'        $N/llviewerdisplay.cpp
chk "impostors forced to zero"                          1 'updateImpostorRendering\(0\)'            $N/pipeline.cpp
chk "LL soften program not selectable in lighting"      0 'soften_shader = clean_soften \?'         $N/pipeline.cpp
chk "clean soften is the bound soften"                  1 'gDeferredCleanSoftenProgram'             $N/pipeline.cpp
chk "LL point-light pass deleted"                       0 'bindDeferredShader\(gDeferredLightProgram\)'          $N/pipeline.cpp
chk "LL spot-light pass deleted"                        0 'bindDeferredShader\(gDeferredSpotLightProgram\)'      $N/pipeline.cpp
chk "LL multi-light pass deleted"                       0 'bindDeferredShader\(gDeferredMultiLightProgram'       $N/pipeline.cpp
chk "LL multi-spot pass deleted"                        0 'bindDeferredShader\(gDeferredMultiSpotLightProgram\)' $N/pipeline.cpp
chk "clean lights pass present"                         1 'CleanLights|cleanLightsF'                $N/pipeline.cpp
chk "sky dome draw gone from WLSky pool"                0 'renderDome\(|drawDome\(|->updateGeometry\(' $N/lldrawpoolwlsky.cpp
chk "LL WLSky program fallback gone"                    0 'gDeferredWLSkyProgram\.|gDeferredWLSkyProgram,' $N/lldrawpoolwlsky.cpp
chk "clean fullscreen sky pass wired"                   1 'renderSkyFull'                           $N/lldrawpoolwlsky.cpp
chk "LL water plane fallback gone"                      0 'pushWaterPlanes'                         $N/lldrawpoolwater.cpp
chk "LL water programs not bound by pool"               0 'gWaterProgram\.bind|gUnderWaterProgram\.bind' $N/lldrawpoolwater.cpp
chk "terrain LL body disabled"                          1 'CLEAN: LL terrain rendering permanently disabled' $N/lldrawpoolterrain.cpp
chk "shadow LL pool loop gated"                         1 'if \(CLEAN_RENDER_ONLY\) return; // gate: no LL shadow geom dispatch' $N/pipeline.cpp
chk "violation tripwires planted (>=6 sites)"           1 'CLEAN_VERIFY_LL_DRAW_UNREACHABLE'        $N/lldrawpoolavatar.cpp $N/lldrawpooltree.cpp $N/lldrawpoolterrain.cpp $N/lldrawpoolsimple.cpp $N/pipeline.cpp
chk "CLEAN_PROF defaults off"                           1 '#define CLEAN_PROF 0'                    $N/llcleandebug.h
chk "census default-off setting"                        1 'RenderCleanCensus'                       $N/llcleanrenderer.cpp
chk "residency heal default-off setting"                1 'RenderCleanResidencyHeal'                $N/llcleanrenderer.cpp
chk "GL guard default-off setting"                      1 'RenderCleanGLGuard'                      $N/llcleanrenderer.cpp

echo
if [ "$fail" = 0 ]; then echo "ALL INVARIANTS HOLD"; else echo "INVARIANT VIOLATIONS PRESENT"; fi
exit $fail
