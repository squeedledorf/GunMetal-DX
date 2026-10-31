#!/usr/bin/env python3
"""Source fixtures for the stochastic denoiser; no Vulkan device or viewer required."""
import math
from pathlib import Path
import unittest
ROOT = Path(__file__).resolve().parents[2]
SH = ROOT / 'indra/vkclean/shaders'

def exact_hc(q, r, hc):
    b = math.floor(hc)
    c0, c1 = math.floor((b + .5) * r) + .5, math.floor((b + 1.5) * r) + .5
    if q < c0:
        b -= 1
        c1, c0 = c0, math.floor((b + .5) * r) + .5
    elif q >= c1:
        b += 1
        c0, c1 = c1, math.floor((b + 1.5) * r) + .5
    return b + max(0, min(1, (q - c0) / max(c1 - c0, 1e-3)))

class HalfHistory(unittest.TestCase):
    def test_static_sample_centres(self):
        for render, denoise in [(1920,1920), (1920,960), (1919,959), (1081,540)]:
            r = render / denoise
            for m in range(denoise):
                q = math.floor((m + .5) * r) + .5
                self.assertAlmostEqual(exact_hc(q, r, q / r - .5), m)
        self.assertEqual(1.5 / 2 - .5, .25)  # baseline half-grid drift

    def test_translating_sample(self):
        for r in [1, 2, 1919 / 959]:
            for m in range(2, 20):
                q0 = math.floor((m + .5) * r) + .5
                q1 = math.floor((m + 1.5) * r) + .5
                q = q0 + .3 * (q1 - q0)
                self.assertAlmostEqual(exact_hc(q,r,q/r-.5),m+.3)

    def test_radiance_budget_and_feedback(self):
        host = (ROOT/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
        self.assertIn('if (mPdnRadiance) { moveRays = 1u; moveLights = 0u; }',host)
        self.assertIn('k == 0u && !pdnMetaConv(g.meta)', (SH/'clean_dn_atrous.comp').read_text())
        self.assertIn('h.moved > 0.0 && fMv < 0.05', (SH/'clean_dn_temporal.comp').read_text())


REF_MS = 1000 / 60

def alpha_time(alpha_ref, dt_ms):
    dt_ms = max(.1, min(100, dt_ms))
    return 1 if alpha_ref >= 1 else -math.expm1(math.log1p(-alpha_ref) * dt_ms / REF_MS)


class TimeHistory(unittest.TestCase):
    def test_reference_weights(self):
        for a in [1, .5, .33, .25, .2, 1/8, 1/16, 1/24, 1/32, 1/63, 1/64]:
            self.assertAlmostEqual(alpha_time(a, REF_MS), a)

    def test_equal_elapsed_decay(self):
        for a in [.5, .33, .25, .2, 1/8, 1/16, 1/24, 1/32, 1/63, 1/64]:
            target = (1 - a)**60
            for fps in [15, 20, 30, 60, 120, 144, 240, 480, 1000]:
                actual = (1 - alpha_time(a, 1000/fps))**fps
                self.assertAlmostEqual(actual, target, places=12)

    def test_reference_warmup_matches_old_filter(self):
        old = new = 0
        old_len = new_age = 0
        for frame in range(200):
            raw = .5 + .1*math.sin(frame*.17)
            old_len = min(old_len+1,32)
            new_age = min(new_age+REF_MS,32*REF_MS)
            old += (raw-old)/old_len
            new += alpha_time(1/max(new_age/REF_MS,1),REF_MS)*(raw-new)
            self.assertAlmostEqual(old,new,places=12)

    def test_partial_reseed_and_motion_horizon(self):
        for fps in [15,30,60,120,240]:
            dt = 1000/fps
            # An identical velocity has the same reference displacement and cap.
            velocity = 3.0 # texels per second
            ref_move = (velocity*dt/1000)/(dt/REF_MS)
            self.assertAlmostEqual(ref_move,velocity/60)
            self.assertAlmostEqual((1-alpha_time(.3,dt))**fps,.7**60,places=12)

    def test_capture_does_not_advance_live_clock(self):
        # Live -> several frozen-scene snapshot recordings -> live: the next live
        # interval still starts at the preceding live recording, not the last sample.
        live_stamp = 1.0
        last_capture = 1.0166
        next_live = live_stamp + 1/60
        self.assertAlmostEqual((next_live-live_stamp)*1000,REF_MS)
        self.assertLess((next_live-last_capture)*1000,.1) # contaminated-clock regression
        source = (ROOT/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
        start = source.index('            VKSSRInfo si{};')
        end = source.index('            if (gVKPrevViewShift', start)
        block = source[start:end]
        self.assertEqual(block.count('mLightingHistoryClock = historyNow;'),1)
        self.assertLess(block.index('if (!mOffscreen)'),block.index('mLightingHistoryClock = historyNow;'))
        self.assertNotIn('for (',block) # one shared upload, outside all per-eye loops

    def test_real_delta_and_hitch(self):
        for a in [.25, 1/32]:
            self.assertEqual(alpha_time(a, 5000), alpha_time(a, 100))
            self.assertEqual(alpha_time(a, .001), alpha_time(a, .1))
        self.assertEqual(alpha_time(1, .1), 1)
        self.assertEqual(alpha_time(1, 100), 1)
        src = (SH/'clean_history_time.glsl').read_text()
        self.assertIn('log(max(1.0 - alphaRef, 1e-8)) * htStep()',src)
        self.assertIn('ssr.history_time.x', src)

    def test_age_growth_and_youth(self):
        for fps in [15, 30, 60, 120, 240, 480, 1000]:
            age = 0
            for _ in range(fps):
                age = min(age + 1000/fps, 32*REF_MS)
            self.assertAlmostEqual(age, 32*REF_MS)
            # A fixed duration, rather than four rendered frames, ends the young boost.
            age = 0
            n = 0
            while age < 4*REF_MS - 1e-8:
                age += 1000/fps
                n += 1
            self.assertLessEqual(abs(n*1000/fps - 4*REF_MS), 1000/fps + 1e-8)

    def test_mover_timer_quantization(self):
        import struct
        f32 = lambda v: struct.unpack('f',struct.pack('f',v))[0]
        for sun in range(0, 4096, 17):
            for proj in [0,1,1067,2133,4095]:
                word = sun | (proj << 12)
                # Decode the exact float32 integer WITHOUT +0.5 at the 24-bit boundary.
                packed = f32(word)
                self.assertEqual(int(packed) & 4095, sun)
                self.assertEqual(int(packed) >> 12, proj)
        for fps in [15,30,60,120,240,480,1000]:
            remaining = 5*REF_MS
            n = 0
            while remaining > 1e-9:
                remaining = round(max(remaining-1000/fps,0)*16)/16
                n += 1
                self.assertLess(n, 200)
            # At most a frame of event quantization plus 1/16 ms per decrement.
            self.assertLessEqual(abs(n*1000/fps - 5*REF_MS), 1000/fps + n/32 + 1e-6)

    def test_rt_projector_age_word(self):
        for age_ms in [0,.1,1,4.1666667,REF_MS,516.6667,1066.6667]:
            for fast in [0,.1,.5,1]:
                ticks = round(age_ms*128)
                word = ticks | (round(fast*16383) << 18)
                self.assertLessEqual(abs((word & 262143)/128-age_ms), 1/256+1e-8)
                self.assertLessEqual(abs((word >> 18)/16383-fast), 1/32766+1e-8)

    def test_float_age_layers_and_raw_bypass(self):
        common = (SH/'clean_dn_common.glsl').read_text()
        temporal = (SH/'clean_dn_temporal.comp').read_text()
        spatial = (SH/'clean_dn_atrous.comp').read_text()
        self.assertIn('imageSize(uDnHist).z - (dnSplitOn() ? 4 : 2) + int(par)',common)
        self.assertIn('imageSize(uDnHist).z - 2 + int(par)',common)
        self.assertIn('nD * HT_REF_MS, nS * HT_REF_MS',temporal)
        self.assertIn('dnSplitAgeLayer(pl))))[j] / HT_REF_MS',temporal)
        self.assertNotIn('min(floor(lenD',temporal)
        self.assertNotIn('pdnMetaLenD(g.meta)',spatial)
        self.assertIn('!dnRawView()',temporal)
        self.assertIn('if (!rawV && !idle)',spatial)
        host = (ROOT/'indra/newview/vkgfx/llcleanprojstochvk.cpp').read_text()
        self.assertIn('dp.raw = rrRawSignal(',host)
        self.assertIn('dp.rawSun = rrRawSignal(',host)

    def test_age_layers_never_alias_signals(self):
        for sun in [False,True]:
            for split in [False,True]:
                layers = (4 if not sun else 6) + (2 if split else 0)
                age_start = layers - (4 if split else 2)
                signals = set(range(4 if sun else 2))
                ages = {age_start,age_start+1}
                self.assertFalse(signals & ages)
                if split:
                    split_ages = {layers-2,layers-1}
                    self.assertFalse((signals | ages) & split_ages)

    def test_gpu_cpu_format_contract(self):
        src = (ROOT/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
        import re
        for image in ['mRtMaskMeta','mRtMaskHist[4]','mRtMaskGrad','mRtMaskHistGrad']:
            self.assertRegex(src, r'\{&'+re.escape(image)+r',\s*VK_FORMAT_R32G32B32A32_SFLOAT')
        for image in ['mRtMaskProjMeta[0]','mRtMaskProjMeta[1]']:
            self.assertRegex(src, r'\{&'+re.escape(image)+r',\s*VK_FORMAT_R32G32B32A32_UINT')
        sp = (SH/'clean_rt_mask_spatial.comp').read_text()
        self.assertIn('if (pf) pmo[k] = rtPackPM(plen,',(SH/'clean_rt_mask_temporal.comp').read_text())
        self.assertIn('binding = 17, rgba32f',sp) # scratch aliases the meta image
        self.assertIn('{4, 8, 16, 4, 4, 4, 8}',src) # diagnostic readback strides
        gi = (ROOT/'indra/newview/vkgfx/llcleangivk.cpp').read_text()
        self.assertIn('(VkDeviceSize)slots * 64u + 16u',gi)
        self.assertIn('(uint64_t)slots * 64u',gi)
        self.assertIn('(VkDeviceSize)mGiStateSlots * 64u',gi)
        self.assertIn('sizeof(GiHeader) + slots * 32u',gi) # forward copy only resolved records
        self.assertIn('gi_data[3u * cap + s]',(SH/'clean_gi_common.glsl').read_text())

if __name__ == '__main__':
    unittest.main()
