#!/usr/bin/env python3
"""GunMetal-DX teleport effects: makes the placeholder sounds (gmdxtpfx*.cpp) from scratch.

  tpfx_shatter.wav  Shatter: a glass crack and shatter (~1.1 s): a sharp crack, a noise crash, falling tinkles
  tpfx_whoosh.wav   Shatter's arrival: a soft whoosh (~0.9 s): noise through a sweeping low-pass
  tpfx_smoke.wav    Ninja smoke: a thick, low "poof" (~0.7 s) and a hiss of smoke
  tpfx_puff.wav     Smoke puff: a light, cartoony "pff" with a little pop (~0.5 s)
  tpfx_thunder.wav  Lightning strike: a crack and a rolling rumble (~1.4 s)
  tpfx_crt_off.wav  CRT power-off: a click and a falling whine (~0.5 s)
  tpfx_crt_on.wav   CRT power-on: a thunk, a crackle and a rising whine (~0.5 s)
  tpfx_portal.wav   Portal swirl: a whirling hum drawn down into a soft low thoom (~1.3 s)
  tpfx_glitch.wav   Glitch: bursts of bit-crushed buzz and data noise, then a hard stop (~0.5 s)
  tpfx_fold.wav     Fold-away: a quick papery "fwip" (~0.35 s)
  tpfx_hyperspace.wav      Hyperspace: a whine and a roar rising to a soft boom (~1.4 s)
  tpfx_hyperspace_out.wav  Hyperspace arrival: a falling whine easing into a whoosh (~0.9 s)

Pure Python (wave, math, random): no downloads, no samples, a fixed seed (the same files every run).
Mono, 16-bit, 44.1 kHz. Run it from anywhere; it writes into
indra/newview/app_settings/sounds/teleport/ next to this repository's viewer.
"""
import math
import os
import random
import struct
import wave

RATE = 44100
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..',
                   'indra', 'newview', 'app_settings', 'sounds', 'teleport')


def write(name, samples, peak_db):
    peak = max(1e-9, max(abs(s) for s in samples))
    gain = 10 ** (peak_db / 20.0) / peak
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(s * gain * 32767)))) for s in samples))
    print('wrote', os.path.normpath(path), '%.2f s' % (len(samples) / RATE))


def fade_out(buf, secs):
    n = int(secs * RATE)
    for i in range(n):
        buf[-1 - i] *= i / n


def shatter(rng):
    n = int(1.1 * RATE)
    out = [0.0] * n
    # the crack: 4 ms of bright noise, then a short ringing "tink" pair
    for i in range(int(0.004 * RATE)):
        out[i] += rng.uniform(-1, 1) * (1 - i / (0.004 * RATE))
    for f, a in ((3150.0, 0.5), (4870.0, 0.35)):
        for i in range(int(0.12 * RATE)):
            t = i / RATE
            out[i] += a * math.sin(2 * math.pi * f * t) * math.exp(-t / 0.025)
    # the crash body: high-passed noise (first difference), decaying over ~0.3 s, starting at 15 ms
    prev = 0.0
    start = int(0.015 * RATE)
    for i in range(start, n):
        t = (i - start) / RATE
        x = rng.uniform(-1, 1)
        hp = x - prev
        prev = x
        out[i] += 0.32 * hp * math.exp(-t / 0.11) * min(1.0, t / 0.004)
    # falling pieces: tinkles (2-3 decaying partials each), denser early, quieter late
    for k in range(70):
        u = rng.random() ** 1.7
        t0 = 0.02 + 0.85 * u
        amp = 0.28 * (1 - 0.8 * u) * rng.uniform(0.4, 1.0)
        base = rng.uniform(2200, 6800)
        decay = rng.uniform(0.012, 0.05)
        partials = [(base, 1.0), (base * rng.uniform(1.4, 1.9), 0.5)]
        if rng.random() < 0.5:
            partials.append((base * rng.uniform(2.2, 2.9), 0.25))
        i0 = int(t0 * RATE)
        for i in range(int(decay * 6 * RATE)):
            j = i0 + i
            if j >= n:
                break
            t = i / RATE
            env = math.exp(-t / decay)
            out[j] += amp * env * sum(a * math.sin(2 * math.pi * f * t) for f, a in partials)
    fade_out(out, 0.15)
    return out


def whoosh(rng):
    n = int(0.9 * RATE)
    out = [0.0] * n
    lp = 0.0
    lp2 = 0.0
    for i in range(n):
        u = i / n
        # the cut-off sweeps up then back down (300 Hz -> 2.4 kHz -> 500 Hz)
        fc = 300 + 2100 * math.sin(math.pi * min(1.0, u * 1.2)) ** 2 if u < 0.833 else 500
        a = 1 - math.exp(-2 * math.pi * fc / RATE)
        x = rng.uniform(-1, 1)
        lp += a * (x - lp)
        lp2 += a * (lp - lp2)   # two poles: softer
        env = math.sin(math.pi * u) ** 1.6
        out[i] = lp2 * env
    fade_out(out, 0.05)
    return out


def lowpass(x, fc_of):
    """Two-pole low-pass; fc_of(u) is the cut-off (Hz) at u = 0..1 through the buffer."""
    out, a1, a2 = [0.0] * len(x), 0.0, 0.0
    for i, v in enumerate(x):
        a = 1 - math.exp(-2 * math.pi * fc_of(i / len(x)) / RATE)
        a1 += a * (v - a1)
        a2 += a * (a1 - a2)
        out[i] = a2
    return out


def smoke(rng):
    n = int(0.7 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    body = lowpass(noise, lambda u: 1100 * math.exp(-u * 4) + 180)   # the poof: dark, closing
    hiss = lowpass(noise, lambda u: 5000)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        att = min(1.0, t / 0.006)
        out[i] = 1.0 * body[i] * att * math.exp(-t / 0.16) + 0.08 * (hiss[i] - body[i]) * att * math.exp(-t / 0.3)
    fade_out(out, 0.08)
    return out


def puff(rng):
    n = int(0.5 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    pff = lowpass(noise, lambda u: 2400 * math.exp(-u * 3) + 400)
    out = [0.0] * n
    ph = 0.0
    for i in range(n):
        t = i / RATE
        out[i] = pff[i] * min(1.0, t / 0.004) * math.exp(-t / 0.09)
        if t < 0.08:   # the pop: a sine falling 260 -> 120 Hz
            ph += 2 * math.pi * (120 + 140 * math.exp(-t / 0.02)) / RATE
            out[i] += 0.35 * math.sin(ph) * math.exp(-t / 0.025)
    fade_out(out, 0.05)
    return out


def thunder(rng):
    n = int(1.4 * RATE)
    out = [0.0] * n
    for i in range(int(0.006 * RATE)):   # the crack
        out[i] += rng.uniform(-1, 1) * (1 - i / (0.006 * RATE))
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    crackle = lowpass(noise, lambda u: 3500)
    rumble = lowpass(noise, lambda u: 220 - 120 * u)
    # slow, lumpy swells in the rumble
    swell = [0.6 + 0.4 * math.sin(2 * math.pi * 3.1 * i / RATE) * math.sin(2 * math.pi * 1.3 * i / RATE + 1) for i in range(n)]
    for i in range(n):
        t = i / RATE
        out[i] += 0.5 * crackle[i] * math.exp(-t / 0.05)
        out[i] += 3.0 * rumble[i] * swell[i] * min(1.0, t / 0.05) * math.exp(-t / 0.45)
    fade_out(out, 0.2)
    return out


def crt_off(rng):
    n = int(0.5 * RATE)
    out = [0.0] * n
    for i in range(int(0.002 * RATE)):   # the click
        out[i] += 0.8 * (1 - i / (0.002 * RATE)) * (1 if i % 8 < 4 else -1)
    ph = 0.0
    for i in range(n):
        t = i / RATE
        ph += 2 * math.pi * (60 + 1100 * math.exp(-t / 0.08)) / RATE   # the whine, falling away
        out[i] += 0.4 * math.sin(ph) * math.exp(-t / 0.15)
        out[i] += 0.05 * rng.uniform(-1, 1) * math.exp(-t / 0.06)        # a breath of static
    fade_out(out, 0.05)
    return out


def crt_on(rng):
    n = int(0.5 * RATE)
    out = [0.0] * n
    ph = 0.0
    for i in range(n):
        t = i / RATE
        out[i] += 0.7 * math.sin(2 * math.pi * 70 * t) * math.exp(-t / 0.05)    # the thunk
        if rng.random() < 0.004:                                                # crackle
            out[i] += rng.uniform(-0.6, 0.6)
        ph += 2 * math.pi * (300 + 2700 * min(1.0, t / 0.25)) / RATE           # the rising whine, quiet
        out[i] += 0.12 * math.sin(ph) * min(1.0, t / 0.05) * math.exp(-max(0.0, t - 0.2) / 0.1)
    fade_out(out, 0.05)
    return out


def portal(rng):
    n = int(1.3 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    out = [0.0] * n
    ph = 0.0
    lp = lp2 = 0.0
    for i in range(n):
        t = i / RATE
        u = i / n
        # the whirl: noise through a low-pass that swings faster and lower as it is drawn in
        swing = 2 * math.pi * (1.5 * t + 3.0 * t * t)
        fc = (700 - 400 * u) * (1.0 + 0.6 * math.sin(swing))
        a = 1 - math.exp(-2 * math.pi * fc / RATE)
        lp += a * (noise[i] - lp)
        lp2 += a * (lp - lp2)
        env = min(1.0, t / 0.25) * (1 - 0.7 * u)
        out[i] += 1.4 * lp2 * env
        ph += 2 * math.pi * (140 - 60 * u + 6 * math.sin(swing)) / RATE   # the hum, sinking
        out[i] += 0.25 * math.sin(ph) * env
        if t > 0.95:   # the thoom as it closes
            tt = t - 0.95
            out[i] += 0.5 * math.sin(2 * math.pi * (55 + 40 * math.exp(-tt / 0.05)) * tt) * min(1.0, tt / 0.01) * math.exp(-tt / 0.12)
    fade_out(out, 0.08)
    return out


def glitch(rng):
    n = int(0.5 * RATE)
    out = [0.0] * n
    i = 0
    while i < int(0.45 * RATE):
        seg = int(rng.uniform(0.015, 0.06) * RATE)
        kind = rng.randrange(3)
        f = rng.uniform(180, 1800)
        hold = rng.randrange(4, 40)   # sample-and-hold: the bit-crush
        level = rng.uniform(0.3, 0.8)
        v = 0.0
        for j in range(min(seg, n - i)):
            t = j / RATE
            if kind == 0:     # a square buzz, crushed to 4 steps
                v = round(2 * (1 if math.sin(2 * math.pi * f * t) >= 0 else -1) * 0.5) / 2
            elif kind == 1:   # held noise
                if j % hold == 0:
                    v = rng.uniform(-1, 1)
            else:             # a dropout
                v = 0.0
            out[i + j] = level * v
        i += seg
    for k in range(int(0.002 * RATE)):   # no click at the hard stop
        j = int(0.45 * RATE) - 1 - k
        out[j] *= k / (0.002 * RATE)
    return out


def fold(rng):
    n = int(0.35 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    lo = lowpass(noise, lambda u: 500 + 4000 * u)     # the sweep up
    hi = lowpass(noise, lambda u: 9000)
    out = [0.0] * n
    for i in range(n):
        t = i / RATE
        u = i / n
        env = math.sin(math.pi * min(1.0, u * 1.15)) ** 2
        out[i] = (lo[i] + 0.3 * (hi[i] - lo[i])) * env
    fade_out(out, 0.04)
    return out


def hyperspace(rng):
    n = int(1.4 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    roar = lowpass(noise, lambda u: 200 + 3000 * u * u)
    out = [0.0] * n
    ph = 0.0
    jump = 1.05
    for i in range(n):
        t = i / RATE
        rise = min(1.0, t / jump)
        after = math.exp(-max(0.0, t - jump) / 0.08)
        ph += 2 * math.pi * (200 * 12 ** rise) / RATE   # the whine: 200 Hz -> 2.4 kHz
        out[i] += 0.18 * math.sin(ph) * rise * after
        out[i] += 1.2 * roar[i] * rise * rise * after
        if t > jump:   # the boom
            tt = t - jump
            out[i] += 0.8 * math.sin(2 * math.pi * (45 + 60 * math.exp(-tt / 0.03)) * tt) * min(1.0, tt / 0.004) * math.exp(-tt / 0.18)
    fade_out(out, 0.08)
    return out


def hyperspace_out(rng):
    n = int(0.9 * RATE)
    noise = [rng.uniform(-1, 1) for _ in range(n)]
    air = lowpass(noise, lambda u: 2500 * (1 - u) + 300)
    out = [0.0] * n
    ph = 0.0
    for i in range(n):
        t = i / RATE
        u = i / n
        ph += 2 * math.pi * (2000 * math.exp(-t / 0.18) + 150) / RATE   # the whine falling away
        out[i] += 0.2 * math.sin(ph) * math.exp(-t / 0.3)
        out[i] += 1.0 * air[i] * min(1.0, t / 0.03) * math.sin(math.pi * u) ** 1.2
    fade_out(out, 0.06)
    return out


if __name__ == '__main__':
    rng = random.Random(20261028)
    s = shatter(rng)
    assert len(s) == int(1.1 * RATE) and max(abs(v) for v in s) > 0
    write('tpfx_shatter.wav', s, -3.0)
    w = whoosh(rng)
    assert len(w) == int(0.9 * RATE) and max(abs(v) for v in w) > 0
    write('tpfx_whoosh.wav', w, -6.0)
    # the rest each have their own seed: adding one never changes another
    for k, (name, make, secs, peak) in enumerate((
            ('tpfx_smoke.wav', smoke, 0.7, -4.0),
            ('tpfx_puff.wav', puff, 0.5, -6.0),
            ('tpfx_thunder.wav', thunder, 1.4, -3.0),
            ('tpfx_crt_off.wav', crt_off, 0.5, -8.0),
            ('tpfx_crt_on.wav', crt_on, 0.5, -8.0),
            ('tpfx_portal.wav', portal, 1.3, -9.0),
            ('tpfx_glitch.wav', glitch, 0.5, -15.0),
            ('tpfx_fold.wav', fold, 0.35, -8.0),
            ('tpfx_hyperspace.wav', hyperspace, 1.4, -4.0),
            ('tpfx_hyperspace_out.wav', hyperspace_out, 0.9, -9.0))):
        buf = make(random.Random(20261028 + 100 * (k + 1)))
        assert len(buf) == int(secs * RATE) and max(abs(v) for v in buf) > 0, name
        write(name, buf, peak)
