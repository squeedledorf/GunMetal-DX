// ui_text_test — offscreen verification of LLRenderVK's FULL render-state parity
// + the FONT/TEXT glyph-quad path. Renders a representative "UI + text" frame to
// a 1024x768 RGBA8 PNG using ONLY gGL-style facade calls (no direct Vulkan).
//
// This is the Phase-3 UI gate. The frame exercises, and the PNG proves:
//   * gl_rect_2d-style panels/buttons (solid + textured).
//   * A bordered box via LINE_LOOP with a real (wide) setLineWidth.
//   * A DEPTH-tested region: two overlapping quads where the nearer one wins by
//     depth compare (not by draw order) — proves depth test + write + func.
//   * A SCISSORED region: a quad clipped to a sub-rect — proves scissor + the
//     GL->Vulkan origin flip.
//   * A COLOR-MASKed draw: writing only the R+G channels over a blue field.
//   * SEVERAL text strings through the glyph-atlas path: different sizes, colors,
//     and alpha; one string drawn OVER a translucent panel to prove text
//     alpha-blends correctly (coverage in the atlas alpha, tinted by vtx color).
//
//   ui_text_test --out <file.png> [--shaders <dir>]
#include "llrendervk.h"
#include "bitmap_font.h"

#include <cstdio>
#include <string>
#include <vector>

static const int W = 1024;
static const int H = 768;

static void solidQuad(float x, float y, float w, float h) {
    gVK.begin(llrvk::QUADS);
    gVK.vertex2f(x,     y);
    gVK.vertex2f(x + w, y);
    gVK.vertex2f(x + w, y + h);
    gVK.vertex2f(x,     y + h);
    gVK.end();
}

// Solid quad at an explicit depth z (for the depth-test region).
static void solidQuadZ(float x, float y, float w, float h, float z) {
    gVK.begin(llrvk::QUADS);
    gVK.vertex3f(x,     y,     z);
    gVK.vertex3f(x + w, y,     z);
    gVK.vertex3f(x + w, y + h, z);
    gVK.vertex3f(x,     y + h, z);
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
    target.init(ctx, W, H, /*withDepth=*/true);   // depth needed for depth region

    gVK.init(ctx, shaderDir);

    // Build + upload the bitmap-font glyph atlas (mirrors LLFontGL's grayscale
    // font_image + getGlyphInfo UVs; see bitmap_font.h header comment).
    bmfont::Font font;
    font.build();
    font.uploadAtlas(gVK);

    gVK.beginFrame(target);

    // UI ortho: y-up, origin bottom-left (matches gGL 2D UI space). Z in [-1,1]
    // ortho maps to Vulkan [0,1] via GLM_FORCE_DEPTH_ZERO_TO_ONE; larger caller z
    // => farther. We drive an explicit depth region below.
    gVK.matrixMode(LLRenderVK::MM_PROJECTION);
    gVK.loadIdentity();
    gVK.ortho(0.0f, (float)W, 0.0f, (float)H, -1.0f, 1.0f);
    gVK.matrixMode(LLRenderVK::MM_MODELVIEW);
    gVK.loadIdentity();

    // (1) Background: opaque dark panel, depth OFF.
    gVK.disableDepthTest();
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.setColorMask(true, true);
    gVK.bindTexture(0);
    gVK.color4ub(28, 32, 46, 255);
    solidQuad(0, 0, (float)W, (float)H);

    // (2) A "title bar" panel + a couple "buttons" (gl_rect_2d style), opaque.
    gVK.color4ub(52, 60, 84, 255);
    solidQuad(40, 690, 944, 48);                 // title bar
    gVK.color4ub(90, 120, 200, 255);
    solidQuad(60, 620, 150, 46);                 // button A
    gVK.color4ub(90, 200, 130, 255);
    solidQuad(230, 620, 150, 46);                // button B

    // Title + button labels (white / dark text). BT_ALPHA for glyph coverage.
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    bmfont::drawString(gVK, font, "ALCHEMY VULKAN UI", 60, 702, 24,
                       235, 238, 245, 255);
    bmfont::drawString(gVK, font, "OPEN",  95, 634, 18, 20, 24, 40, 255);
    bmfont::drawString(gVK, font, "SAVE", 268, 634, 18, 20, 24, 40, 255);

    // (3) DEPTH-tested region (bottom-left box). Enable depth test+write, LEQUAL.
    //     NOTE gGL/glm UI convention: a LARGER caller z maps to a SMALLER (nearer)
    //     depth value (glm ortho looks down -Z; LLRender's UI increments sCurDepth
    //     to push newer widgets nearer). So "nearest" == largest z here.
    //     Draw a FAR red quad first (z=0.2), then a NEAR green quad (z=0.8) that
    //     overlaps — green must win in the overlap. Then draw a FAR blue quad
    //     (z=0.1) LAST that overlaps green: with LEQUAL depth it must be REJECTED
    //     in the overlap (proving depth test, not draw order, decides).
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.enableDepthTest(true, llrvk::CF_LESS_EQUAL);
    gVK.color4ub(210, 60, 60, 255);   solidQuadZ(60, 90, 180, 150, 0.2f);  // far red
    gVK.color4ub(60, 200, 90, 255);   solidQuadZ(150, 150, 180, 150, 0.8f);// near green
    gVK.color4ub(70, 90, 220, 255);   solidQuadZ(240, 90, 150, 150, 0.1f); // far blue (rejected under green)
    gVK.disableDepthTest();

    // Label under the depth box.
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    bmfont::drawString(gVK, font, "DEPTH TEST LEQUAL", 60, 60, 14,
                       200, 205, 215, 255);

    // (4) SCISSORED region (bottom-middle). A big yellow quad clipped to a
    //     smaller window; only the intersection should appear.
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.setScissor(true, 470, 90, 180, 130);      // GL bottom-left origin rect
    gVK.color4ub(240, 210, 60, 255);
    solidQuad(430, 60, 300, 260);                 // larger than scissor -> clipped
    gVK.setScissor(false);
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    bmfont::drawString(gVK, font, "SCISSOR CLIP", 470, 60, 14,
                       200, 205, 215, 255);

    // (5) COLOR-MASK draw (bottom-right). Fill a blue field, then draw a white
    //     quad with only R+G writable => it should appear YELLOW (B masked out,
    //     keeps the field's blue channel... here field over dark bg, so result is
    //     R=1,G=1,B=field.b). Draw the field opaque first.
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.setColorMask(true, true, true, true);
    gVK.color4ub(30, 40, 200, 255);
    solidQuad(760, 90, 200, 150);                 // blue field
    // Now mask B off, draw white on top: R,G written to 255, B untouched (stays
    // ~200/255), A untouched. Expected ~ (255,255,200).
    gVK.setColorMask(true, true, false, true);
    gVK.color4ub(255, 255, 255, 255);
    solidQuad(800, 120, 120, 90);
    gVK.setColorMask(true, true, true, true);
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    bmfont::drawString(gVK, font, "COLOR MASK RG", 760, 60, 14,
                       200, 205, 215, 255);

    // (6) Textured "atlas preview" quad (top-right) — shows the raw glyph atlas
    //     so the sampler/UV orientation is directly verifiable (text is right
    //     side up => atlas preview is right side up).
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    gVK.setActiveTextureUnit(0);
    gVK.bindTexture(font.texId);
    gVK.setTextureFilter(llrvk::TF_POINT);  // after bind (bind adopts tex default)
    gVK.color4ub(255, 255, 255, 255);
    {
        float x = 720, y = 470, w = (float)font.atlasW * 2.f, h = (float)font.atlasH * 2.f;
        gVK.begin(llrvk::TRIANGLES);
        // atlas top-left origin; y-up ortho: screen top samples atlas v=0.
        gVK.texCoord2f(0, 0); gVK.vertex2f(x,     y + h);
        gVK.texCoord2f(1, 0); gVK.vertex2f(x + w, y + h);
        gVK.texCoord2f(1, 1); gVK.vertex2f(x + w, y);
        gVK.texCoord2f(0, 0); gVK.vertex2f(x,     y + h);
        gVK.texCoord2f(1, 1); gVK.vertex2f(x + w, y);
        gVK.texCoord2f(0, 1); gVK.vertex2f(x,     y);
        gVK.end();
    }
    gVK.bindTexture(0);

    // (7) A bordered box drawn with LINE_LOOP + a WIDE line (real setLineWidth).
    //     Center-left. Then a label inside.
    gVK.setSceneBlendType(llrvk::BT_REPLACE);
    gVK.setLineWidth(3.0f);
    gVK.color4ub(255, 200, 60, 255);
    gVK.begin(llrvk::LINE_LOOP);
    gVK.vertex2f(60,  360);
    gVK.vertex2f(360, 360);
    gVK.vertex2f(360, 470);
    gVK.vertex2f(60,  470);
    gVK.end();
    gVK.setLineWidth(1.0f);
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    bmfont::drawString(gVK, font, "LINE LOOP BORDER 3PX", 80, 405, 14,
                       255, 220, 120, 255);

    // (8) TEXT-OVER-TRANSLUCENT-PANEL (the key alpha-blend proof). A translucent
    //     magenta panel, then a solid white string over it — the text must be
    //     legible AND the panel must show through where glyph coverage is 0.
    gVK.setSceneBlendType(llrvk::BT_ALPHA);
    gVK.color4ub(220, 40, 200, 140);              // translucent magenta panel
    solidQuad(430, 400, 400, 90);
    bmfont::drawString(gVK, font, "TEXT ON GLASS", 450, 430, 22,
                       255, 255, 255, 255);
    // A second, half-alpha string beneath it to prove per-string text alpha.
    bmfont::drawString(gVK, font, "HALF ALPHA LABEL", 450, 408, 14,
                       255, 255, 255, 128);

    // (9) A size ramp of a numeric string (proves multiple sizes cleanly).
    bmfont::drawString(gVK, font, "SIZE 12", 720, 300, 12, 180, 220, 255, 255);
    bmfont::drawString(gVK, font, "SIZE 18", 720, 270, 18, 180, 220, 255, 255);
    bmfont::drawString(gVK, font, "SIZE 28", 720, 232, 28, 180, 220, 255, 255);

    gVK.endFrame();

    bool ok = target.readbackToPng(ctx, out);

    uint32_t vErr = ctx.validationErrorCount();
    uint32_t vWarn = ctx.validationWarnCount();
    std::printf("[ui_text_test] validation: %u error(s), %u warning(s)\n",
                vErr, vWarn);

    gVK.shutdown();
    target.destroy(ctx);
    ctx.shutdown();

    if (!ok) { std::fprintf(stderr, "readback/PNG failed\n"); return 1; }
    if (vErr > 0) { std::fprintf(stderr, "FAIL: validation errors\n"); return 1; }
    std::printf("[ui_text_test] OK -> %s\n", out.c_str());
    return 0;
}
