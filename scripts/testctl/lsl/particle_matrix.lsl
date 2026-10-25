// Slipstream particle test rig (#693): one prim cycles through particle-system
// configurations that each exercise one emitter feature, with hover text saying
// what you should see. Compare against another viewer, or against the viewer's
// RenderVKParticleAudit log (expected vs actual spawns per emitter).
//
// Use: drop into a prim (a 0.5 m cube is fine), touch to advance, or say on
// channel 7:  "pt <n>" jump to test n, "pt next" / "pt prev", "pt off",
// "pt list", "pt auto <seconds>" cycle automatically, "pt stop". Anyone nearby may touch it
// or send these commands (so a test-control alt can drive the rig); results go to local chat.
// The prim must stay put for the bounce / target / follow tests to read right.

integer CH = 7;
integer gTest = 0;
integer gAuto = 0;

list NAMES = [
    "1 burst 1 / 1.0 s, age 3",
    "2 burst 5 / 2.0 s, age 1.5",
    "3 burst 10 / 0.05 s, age 1",
    "4 burst rate 0",
    "5 firefly",
    "6 pattern DROP",
    "7 pattern ANGLE (fan)",
    "8 pattern ANGLE_CONE",
    "9 burst radius 1 m",
    "10 accel up + speed range",
    "11 FOLLOW_VELOCITY",
    "12 BOUNCE",
    "13 WIND",
    "14 TARGET owner",
    "15 TARGET_LINEAR owner",
    "16 RIBBON",
    "17 interp colour + scale",
    "18 additive blend + glow",
    "19 OMEGA spin",
    "20 source max age 5 s",
    "21 big flame",
    "22 FOLLOW_SRC (orbit)"
];

list EXPECT = [
    "One particle every second, 3 alive at a time.",
    "A puff of 5 every 2 s; gaps with nothing visible between puffs.",
    "Steady dense stream: ~200 spawned/s, ~200 alive.",
    "One particle per viewer frame (LL's minimum burst interval).",
    "A few slow drifting dots (1 every 0.5 s, 8 alive), glowing, blinking colour.",
    "Particles fall straight down from the prim (DROP + gravity-like accel), no spread.",
    "A flat fan in the prim's local XZ plane (+/- 0.5 rad).",
    "A 3D cone opening upward (0.3 rad half-angle).",
    "Spawn points on a 1 m sphere around the prim, then drift out.",
    "Particles rise faster over their life; mixed speeds (0.1-1.0 m/s).",
    "Stretched particles pointing along their direction of travel.",
    "Particles fall but never below the prim's height.",
    "Particles drift with the region wind.",
    "Particles home in on the owner and vanish at the owner.",
    "Particles travel in straight lines to the owner, arriving exactly at max age.",
    "Ribbon strips join each particle to the one spawned before it (LL): with this cone pattern, a zig-zag web of strings poking out of the prim.",
    "Red->blue and small->large over each particle's life.",
    "Bright additive glowing particles that add up where they overlap.",
    "The emitter direction spins around the prim (angle pattern rotating).",
    "Emits for 5 s after selecting this test, then stops by itself.",
    "Large flame sprites born at the prim centre, rising ~0.4 m and fading: mostly inside the prim, pulsing with each spawn (same as LL).",
    "The prim orbits; particles stay attached to it (move with the prim)."
];

stopAll()
{
    llParticleSystem([]);
    llTargetOmega(<0,0,0>, 0.0, 0.0);
    llSetText("", <1,1,1>, 0.0);
}

// Common base: default texture, white, visible, emissive off.
list base(float age, float rate, integer count, integer pattern)
{
    return [
        PSYS_PART_MAX_AGE, age,
        PSYS_SRC_BURST_RATE, rate,
        PSYS_SRC_BURST_PART_COUNT, count,
        PSYS_SRC_PATTERN, pattern,
        PSYS_PART_START_SCALE, <0.2, 0.2, 0.0>,
        PSYS_PART_START_COLOR, <1, 1, 1>,
        PSYS_PART_START_ALPHA, 1.0,
        PSYS_SRC_BURST_SPEED_MIN, 0.5,
        PSYS_SRC_BURST_SPEED_MAX, 0.5
    ];
}

runTest(integer t)
{
    stopAll();
    list r;
    integer f = 0;
    if (t == 0)       r = base(3.0, 1.0, 1, PSYS_SRC_PATTERN_EXPLODE);
    else if (t == 1)  r = base(1.5, 2.0, 5, PSYS_SRC_PATTERN_EXPLODE);
    else if (t == 2)  r = base(1.0, 0.05, 10, PSYS_SRC_PATTERN_EXPLODE);
    else if (t == 3)  r = base(1.0, 0.0, 1, PSYS_SRC_PATTERN_EXPLODE);
    else if (t == 4)
    {
        f = PSYS_PART_EMISSIVE_MASK | PSYS_PART_INTERP_COLOR_MASK;
        r = base(4.0, 0.5, 1, PSYS_SRC_PATTERN_EXPLODE) + [
            PSYS_SRC_BURST_SPEED_MIN, 0.05, PSYS_SRC_BURST_SPEED_MAX, 0.15,
            PSYS_SRC_BURST_RADIUS, 1.5,
            PSYS_PART_START_COLOR, <1.0, 1.0, 0.3>, PSYS_PART_END_COLOR, <0.3, 1.0, 0.1>,
            PSYS_PART_START_SCALE, <0.08, 0.08, 0.0>,
            PSYS_PART_START_GLOW, 0.4, PSYS_PART_END_GLOW, 0.0];
    }
    else if (t == 5)  r = base(2.0, 0.2, 2, PSYS_SRC_PATTERN_DROP) + [PSYS_SRC_ACCEL, <0, 0, -1.0>];   // (DROP alone has no velocity: without accel LL leaves them at the source)
    else if (t == 6)  r = base(2.0, 0.1, 2, PSYS_SRC_PATTERN_ANGLE) + [PSYS_SRC_ANGLE_BEGIN, 0.0, PSYS_SRC_ANGLE_END, 0.5];
    else if (t == 7)  r = base(2.0, 0.1, 3, PSYS_SRC_PATTERN_ANGLE_CONE) + [PSYS_SRC_ANGLE_BEGIN, 0.0, PSYS_SRC_ANGLE_END, 0.3];
    else if (t == 8)  r = base(2.0, 0.1, 3, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_BURST_RADIUS, 1.0, PSYS_SRC_BURST_SPEED_MIN, 0.1, PSYS_SRC_BURST_SPEED_MAX, 0.1];
    else if (t == 9)  r = base(3.0, 0.1, 2, PSYS_SRC_PATTERN_ANGLE_CONE) + [PSYS_SRC_ANGLE_END, 0.4, PSYS_SRC_ACCEL, <0, 0, 1.0>, PSYS_SRC_BURST_SPEED_MIN, 0.1, PSYS_SRC_BURST_SPEED_MAX, 1.0];
    else if (t == 10) { f = PSYS_PART_FOLLOW_VELOCITY_MASK; r = base(2.0, 0.1, 3, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_PART_START_SCALE, <0.05, 0.4, 0.0>, PSYS_SRC_BURST_SPEED_MIN, 1.0, PSYS_SRC_BURST_SPEED_MAX, 1.0]; }
    else if (t == 11) { f = PSYS_PART_BOUNCE_MASK; r = base(4.0, 0.2, 3, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_ACCEL, <0, 0, -2.0>]; }
    else if (t == 12) { f = PSYS_PART_WIND_MASK; r = base(4.0, 0.2, 3, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_BURST_SPEED_MIN, 0.05, PSYS_SRC_BURST_SPEED_MAX, 0.1]; }
    else if (t == 13) { f = PSYS_PART_TARGET_POS_MASK; r = base(4.0, 0.2, 2, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_TARGET_KEY, llGetOwner()]; }
    else if (t == 14) { f = PSYS_PART_TARGET_POS_MASK | PSYS_PART_TARGET_LINEAR_MASK; r = base(3.0, 0.2, 2, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_TARGET_KEY, llGetOwner()]; }
    else if (t == 15) { f = PSYS_PART_RIBBON_MASK | PSYS_PART_EMISSIVE_MASK; r = base(3.0, 0.05, 1, PSYS_SRC_PATTERN_ANGLE_CONE) + [PSYS_SRC_ANGLE_END, 0.6, PSYS_PART_START_SCALE, <0.1, 0.1, 0.0>, PSYS_SRC_ACCEL, <0, 0, -0.5>]; }
    else if (t == 16) { f = PSYS_PART_INTERP_COLOR_MASK | PSYS_PART_INTERP_SCALE_MASK; r = base(3.0, 0.1, 2, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_PART_START_COLOR, <1, 0, 0>, PSYS_PART_END_COLOR, <0, 0, 1>, PSYS_PART_START_SCALE, <0.05, 0.05, 0.0>, PSYS_PART_END_SCALE, <0.6, 0.6, 0.0>]; }
    else if (t == 17) { f = PSYS_PART_EMISSIVE_MASK; r = base(2.0, 0.05, 3, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_PART_BLEND_FUNC_SOURCE, PSYS_PART_BF_SOURCE_ALPHA, PSYS_PART_BLEND_FUNC_DEST, PSYS_PART_BF_ONE, PSYS_PART_START_GLOW, 0.3, PSYS_PART_START_COLOR, <1.0, 0.5, 0.1>, PSYS_PART_START_ALPHA, 0.5]; }
    else if (t == 18) r = base(2.0, 0.05, 1, PSYS_SRC_PATTERN_ANGLE) + [PSYS_SRC_ANGLE_BEGIN, 0.0, PSYS_SRC_ANGLE_END, 0.1, PSYS_SRC_OMEGA, <0, 0, 2.0>];
    else if (t == 19) r = base(2.0, 0.1, 3, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_MAX_AGE, 5.0];
    else if (t == 20)
    {
        f = PSYS_PART_EMISSIVE_MASK | PSYS_PART_INTERP_COLOR_MASK | PSYS_PART_INTERP_SCALE_MASK;
        r = base(1.2, 0.3, 1, PSYS_SRC_PATTERN_ANGLE_CONE) + [
            PSYS_SRC_ANGLE_END, 0.05, PSYS_SRC_BURST_SPEED_MIN, 0.3, PSYS_SRC_BURST_SPEED_MAX, 0.4,
            PSYS_PART_START_SCALE, <1.2, 1.6, 0.0>, PSYS_PART_END_SCALE, <0.6, 1.0, 0.0>,
            PSYS_PART_START_COLOR, <0.3, 0.6, 1.0>, PSYS_PART_END_COLOR, <0.1, 0.2, 1.0>,
            PSYS_PART_START_ALPHA, 0.9, PSYS_PART_END_ALPHA, 0.0];
    }
    else if (t == 21)
    {
        f = PSYS_PART_FOLLOW_SRC_MASK;
        r = base(3.0, 0.1, 2, PSYS_SRC_PATTERN_EXPLODE) + [PSYS_SRC_BURST_RADIUS, 0.3, PSYS_SRC_BURST_SPEED_MIN, 0.0, PSYS_SRC_BURST_SPEED_MAX, 0.0];
        llTargetOmega(<0, 0, 1>, 1.0, 1.0);   // (client-side spin: the prim turns, particles must turn with it)
    }
    llParticleSystem([PSYS_PART_FLAGS, f] + r);
    llSetText("Particle test " + llList2String(NAMES, t) + "\n" + llList2String(EXPECT, t)
              + "\n(touch: next | ch 7: pt <n> / next / prev / off / auto <s>)", <1, 1, 0.6>, 1.0);
    llSay(0, "Test " + llList2String(NAMES, t) + ": " + llList2String(EXPECT, t));
}

default
{
    state_entry()
    {
        llListen(CH, "", NULL_KEY, "");
        runTest(gTest);
    }
    on_rez(integer p) { llResetScript(); }
    touch_start(integer n)
    {
        gTest = (gTest + 1) % llGetListLength(NAMES);
        runTest(gTest);
    }
    timer()
    {
        gTest = (gTest + 1) % llGetListLength(NAMES);
        runTest(gTest);
    }
    listen(integer ch, string name, key id, string msg)
    {
        list w = llParseString2List(llToLower(msg), [" "], []);
        if (llList2String(w, 0) != "pt") return;
        string c = llList2String(w, 1);
        integer n = llGetListLength(NAMES);
        if (c == "off") { llSetTimerEvent(0.0); stopAll(); return; }
        if (c == "next") gTest = (gTest + 1) % n;
        else if (c == "prev") gTest = (gTest + n - 1) % n;
        else if (c == "list") { integer i; for (i = 0; i < n; ++i) llSay(0, llList2String(NAMES, i)); return; }
        else if (c == "auto") { gAuto = (integer)llList2String(w, 2); if (gAuto < 5) gAuto = 10; llSetTimerEvent((float)gAuto); }
        else if (c == "stop") { llSetTimerEvent(0.0); return; }
        else
        {
            integer t = (integer)c - 1;
            if (t < 0 || t >= n) { llSay(0, "pt 1.." + (string)n); return; }
            gTest = t;
        }
        runTest(gTest);
    }
}
