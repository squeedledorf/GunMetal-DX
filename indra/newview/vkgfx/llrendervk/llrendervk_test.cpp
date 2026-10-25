// llrendervk_test — offscreen verification of the LLRenderVK immediate-mode
// facade. Renders a representative "UI frame" using ONLY gGL-style calls
// (begin/vertex/color/texCoord/end + matrix stack + blend/texture state) to a
// 1024x768 RGBA8 PNG. No direct Vulkan appears in this file — that is the point:
// it exercises the same surface the viewer's UI/text code calls.
//
// The frame contains, in a y-up (origin bottom-left) UI ortho space, so it maps
// to OpenGL/gGL conventions:
//   * a full-window background panel (dark blue), via QUADS
//   * three solid colored quads (red / green / blue) in a row
//   * a textured quad using a generated checker "glyph atlas", via TRIANGLES
//   * two overlapping alpha-blended translucent panels (verify blend + order)
//   * a few colored lines (LINES) forming an X and a border
//   * a nested pushMatrix/translate/scale group (a small quad transformed)
//
//   llrendervk_test --out <file.png> [--shaders <dir>]
#include "llrendervk.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const int W = 1024;
static const int H = 768;

// Convenience: emit an axis-aligned quad [x,y]..[x+w,y+h] with current color,
// untextured, via QUADS.
static void solidQuad(float x, float y, float w, float h) {
    gVK.begin(llrvk::QUADS);
    gVK.vertex2f(x,     y);
    gVK.vertex2f(x + w, y);
    gVK.vertex2f(x + w, y + h);
    gVK.vertex2f(x,     y + h);
    gVK.end();
}

// Textured quad with UV 0..1 across the rect.
static void texQuad(float x, float y, float w, float h) {
    gVK.begin(llrvk::TRIANGLES);
    // tri 1
    gVK.texCoord2f(0, 0); gVK.vertex2f(x,     y);
    gVK.texCoord2f(1, 0); gVK.vertex2f(x + w, y);
    gVK.texCoord2f(1, 1); gVK.vertex2f(x + w, y + h);
    // tri 2
    gVK.texCoord2f(0, 0); gVK.vertex2f(x,     y);
    gVK.texCoord2f(1, 1); gVK.vertex2f(x + w, y + h);
    gVK.texCoord2f(0, 1); gVK.vertex2f(x,     y + h);
    gVK.end();
}

int main(int argc, char** argv) {
    std::string out;
    std::string shaderDir = "shaders";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--shaders" && i + 1 < argc) shaderDir = argv[++i];
    }
    if (out.empty()) { std::fprintf(stderr, "usage: --out file.png [--shaders dir]\n"); return 2; }

    VkDeviceContext ctx;
    ctx.init(/*enableValidation=*/true);

    OffscreenTarget target;
    target.init(ctx, W, H, /*withDepth=*/false);

    gVK.init(ctx, shaderDir);

    // Build a 32x32 "glyph atlas" checker: bright yellow / dark, tinted by U so
    // the sampler + UV orientation are both verifiable.
    const uint32_t TW = 32, TH = 32;
    std::vector<uint8_t> pix(TW * TH * 4);
    for (uint32_t yy = 0; yy < TH; ++yy)
        for (uint32_t xx = 0; xx < TW; ++xx) {
            bool chk = ((xx / 4) + (yy / 4)) & 1;
            uint8_t* p = &pix[(yy * TW + xx) * 4];
            if (chk) { p[0] = 255; p[1] = 220; p[2] = 40; p[3] = 255; }   // yellow
            else     { p[0] = 20;  p[1] = 20;  p[2] = 60;  p[3] = 255; }   // dark blue
        }
    uint32_t tex = gVK.createTexture(TW, TH, pix.data(), /*nearest=*/true);

    // ---- the frame ---------------------------------------------------------
    gVK.beginFrame(target);

    // UI ortho: y-up, origin bottom-left (matches gGL 2D UI space).
    gVK.matrixMode(LLRenderVK::MM_PROJECTION);
    gVK.loadIdentity();
    gVK.ortho(0.0f, (float)W, 0.0f, (float)H, -1.0f, 1.0f);
    gVK.matrixMode(LLRenderVK::MM_MODELVIEW);
    gVK.loadIdentity();

    // (1) background panel: opaque, covers the window. Blend REPLACE.
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.bindTexture(0);
    gVK.color4ub(30, 40, 90, 255);
    solidQuad(0, 0, (float)W, (float)H);

    // (2) three solid colored quads in a row near the bottom.
    gVK.color4ub(220, 40, 40, 255);  solidQuad(80,  80, 180, 140);  // red
    gVK.color4ub(40, 200, 60, 255);  solidQuad(320, 80, 180, 140);  // green
    gVK.color4ub(60, 90, 230, 255);  solidQuad(560, 80, 180, 140);  // blue

    // (3) textured "glyph atlas" quad (top-left area). White color = show texel.
    gVK.bindTexture(tex);
    gVK.color4ub(255, 255, 255, 255);
    texQuad(80, 500, 220, 200);
    gVK.bindTexture(0);

    // (4) two overlapping alpha-blended translucent panels. Draw order matters:
    //     cyan first, then magenta on top (magenta should tint the overlap).
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    gVK.color4ub(0, 200, 220, 128);   // translucent cyan
    solidQuad(440, 420, 260, 200);
    gVK.color4ub(230, 30, 200, 128);  // translucent magenta over it
    solidQuad(560, 500, 260, 200);

    // (5) lines: an X through a box + a border, opaque.
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.color4ub(255, 255, 0, 255);
    gVK.begin(llrvk::LINES);
    // X
    gVK.vertex2f(800, 120); gVK.vertex2f(960, 260);
    gVK.vertex2f(800, 260); gVK.vertex2f(960, 120);
    // border box
    gVK.vertex2f(800, 120); gVK.vertex2f(960, 120);
    gVK.vertex2f(960, 120); gVK.vertex2f(960, 260);
    gVK.vertex2f(960, 260); gVK.vertex2f(800, 260);
    gVK.vertex2f(800, 260); gVK.vertex2f(800, 120);
    gVK.end();

    // (6) nested transform: push MODELVIEW, translate to (860,560), scale 2x,
    //     draw a unit-ish white quad -> should appear as a 80x80 quad at that
    //     spot (40x40 authored, scaled 2x). Then pop and confirm we can draw at
    //     origin space again (a small marker) to prove pop restored the matrix.
    gVK.matrixMode(LLRenderVK::MM_MODELVIEW);
    gVK.pushMatrix();
    gVK.translatef(860.0f, 560.0f, 0.0f);
    gVK.scalef(2.0f, 2.0f, 1.0f);
    gVK.color4ub(255, 160, 0, 255);      // orange
    solidQuad(0, 0, 40, 40);             // -> 80x80 at (860,560)
    gVK.popMatrix();

    // after pop: a small marker at absolute (300,560) proves transform was undone
    gVK.color4ub(255, 255, 255, 255);
    solidQuad(300, 560, 24, 24);

    gVK.endFrame();

    bool ok = target.readbackToPng(ctx, out);

    uint32_t vErr = ctx.validationErrorCount();
    uint32_t vWarn = ctx.validationWarnCount();
    std::printf("[llrendervk_test] validation: %u error(s), %u warning(s)\n",
                vErr, vWarn);

    gVK.shutdown();
    target.destroy(ctx);
    ctx.shutdown();

    if (!ok) { std::fprintf(stderr, "readback/PNG failed\n"); return 1; }
    if (vErr > 0) { std::fprintf(stderr, "FAIL: validation errors\n"); return 1; }
    std::printf("[llrendervk_test] OK -> %s\n", out.c_str());
    return 0;
}
