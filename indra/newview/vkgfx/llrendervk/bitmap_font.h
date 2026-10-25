// bitmap_font.h — a tiny, self-contained bitmap font + glyph-quad layout that
// mirrors LLFontGL's RENDER path (not its FreeType layout). This exists so the
// LLRenderVK offscreen test can prove the GLYPH-ATLAS -> textured-alpha-blended-
// quad pipeline works through the facade WITHOUT linking the whole viewer
// (FreeType, llcommon, LLSD, the font registry XML, image codecs).
//
// Fidelity to the real path:
//   * The atlas is a GRAYSCALE coverage bitmap (stored RGBA8 with R=G=B=A=cov),
//     exactly like LLFontGL's Grayscale font_image — the glyph shape lives in
//     the ALPHA channel and the vertex color tints it. This is what makes text
//     alpha-blend correctly over translucent panels.
//   * Layout emits, per glyph, a screen_rect (mLeft/mTop/mRight/mBottom) and a
//     uv_rect into the atlas, then expands to 6 vertices in the SAME order as
//     LLFontGL::renderTriangle:  (R,T)(L,T)(L,B) (R,T)(L,B)(R,B).
//   * Advance + baseline handling mirrors gGL UI text: y-up UI ortho, mTop above
//     mBottom, glyphs advance in +x.
//
// The real LLFontGL integration replaces buildAtlas()/layout() with
// mFontFreetype->getFontBitmapCache() and getGlyphInfo(); the emit code
// (renderTriangle-equivalent) is identical.
#pragma once

#include "llrendervk.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bmfont {

// 5x7 glyph cell (classic terminal font). 1 bit per pixel, 7 rows, low 5 bits
// used per row, MSB-first (bit 4 = leftmost column). Covers the characters the
// test needs; unknown chars render as a filled box so mistakes are visible.
struct Glyph5x7 { char ch; uint8_t rows[7]; };

// A compact legible 5x7 set. Rows top-to-bottom.
static const Glyph5x7 kGlyphs[] = {
    {' ', {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'A', {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'B', {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
    {'C', {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'D', {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
    {'E', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    {'F', {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    {'G', {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}},
    {'H', {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
    {'I', {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}},
    {'J', {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}},
    {'K', {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    {'L', {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
    {'M', {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}},
    {'N', {0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'O', {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'P', {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'Q', {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}},
    {'R', {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    {'S', {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}},
    {'T', {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    {'U', {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    {'V', {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
    {'W', {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}},
    {'X', {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
    {'Y', {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}},
    {'Z', {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    {'0', {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1', {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2', {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    {'3', {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    {'4', {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5', {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6', {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7', {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8', {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9', {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'.', {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}},
    {',', {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}},
    {'!', {0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
    {'?', {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}},
    {':', {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}},
    {'-', {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
    {'+', {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}},
    {'/', {0x01,0x01,0x02,0x04,0x08,0x10,0x10}},
    {'(', {0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')', {0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
};

// Atlas geometry: 16 columns of glyph cells, each cell CELL x CELL, glyph drawn
// GW x GH at (PADX,PADY) inside its cell so bilinear sampling doesn't bleed
// across cells.
static const int GW = 5, GH = 7;         // glyph pixel extent
static const int PADX = 1, PADY = 1;     // padding inside cell
static const int CELL = 8;               // cell size (5+1 padding, rounded to 8)
static const int COLS = 16;

struct GlyphUV { float u0, v0, u1, v1; bool valid; };

// A built font: the RGBA8 atlas plus per-ASCII UV lookups (top-left origin UVs
// suitable for a y-up UI ortho: v0=top of glyph, v1=bottom).
struct Font {
    std::vector<uint8_t> atlas;   // RGBA8
    int atlasW = 0, atlasH = 0;
    GlyphUV uv[128];              // indexed by ASCII
    uint32_t texId = 0;          // filled in by uploadAtlas()

    // Build the grayscale coverage atlas + UV table.
    void build() {
        const int n = (int)(sizeof(kGlyphs) / sizeof(kGlyphs[0]));
        const int rows = (n + COLS - 1) / COLS;
        atlasW = COLS * CELL;
        atlasH = rows * CELL;
        atlas.assign((size_t)atlasW * atlasH * 4, 0);
        for (int i = 0; i < 128; ++i) uv[i].valid = false;

        for (int g = 0; g < n; ++g) {
            const Glyph5x7& gl = kGlyphs[g];
            int cx = (g % COLS) * CELL;
            int cy = (g / COLS) * CELL;
            for (int ry = 0; ry < GH; ++ry) {
                uint8_t bits = gl.rows[ry];
                for (int rx = 0; rx < GW; ++rx) {
                    bool on = (bits >> (GW - 1 - rx)) & 1;
                    if (!on) continue;
                    int px = cx + PADX + rx;
                    int py = cy + PADY + ry;
                    uint8_t* p = &atlas[((size_t)py * atlasW + px) * 4];
                    p[0] = p[1] = p[2] = p[3] = 255; // full coverage in ALL chans
                }
            }
            int c = (unsigned char)gl.ch;
            if (c < 128) {
                GlyphUV u;
                u.u0 = (float)(cx + PADX) / atlasW;
                u.v0 = (float)(cy + PADY) / atlasH;             // top
                u.u1 = (float)(cx + PADX + GW) / atlasW;
                u.v1 = (float)(cy + PADY + GH) / atlasH;        // bottom
                u.valid = true;
                uv[c] = u;
            }
        }
    }

    // Upload the atlas as a linear-filtered VK texture via the facade.
    void uploadAtlas(LLRenderVK& vk) {
        texId = vk.createTexture((uint32_t)atlasW, (uint32_t)atlasH,
                                 atlas.data(), /*nearest=*/false);
    }
};

// Lay out a string into glyph quads and submit them through the facade EXACTLY
// like LLFontGL: bind the atlas, begin(TRIANGLES), emit 6 verts/glyph via
// vertexBatchPreTransformed, end(). `pxSize` is the target glyph HEIGHT in px;
// the string is drawn with its baseline-left at (x,y) in a y-up UI ortho
// (mTop = y + pxSize, mBottom = y). Color r,g,b,a tints the coverage.
//
// Caller is responsible for setting BT_ALPHA blend before this (as gGL does).
inline void drawString(LLRenderVK& vk, const Font& font, const std::string& s,
                       float x, float y, float pxSize,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const float scale = pxSize / (float)GH;
    const float glyphW = GW * scale;
    const float glyphH = GH * scale;
    const float advance = (GW + 1) * scale;   // 1px tracking between glyphs

    // Bind atlas on unit 0, linear filter, clamp (default), alpha blend assumed.
    vk.setActiveTextureUnit(0);
    vk.setTextureFilter(llrvk::TF_BILINEAR);
    vk.setTextureAddressMode(llrvk::TAM_CLAMP);
    vk.bindTexture(font.texId);

    std::vector<LLRenderVK::PreVert> verts;
    verts.reserve(s.size() * 6);

    float cx = x;
    for (char rawc : s) {
        int c = (unsigned char)rawc;
        if (c == ' ' || c >= 128 || !font.uv[c].valid) { cx += advance; continue; }
        const GlyphUV& u = font.uv[c];

        // screen_rect in y-up UI space (mTop above mBottom), mirrors LLFontGL.
        float L = cx, R = cx + glyphW;
        float B = y,  T = y + glyphH;
        // uv_rect: v0 is the atlas TOP of the glyph, v1 the atlas BOTTOM. In a
        // y-up ortho the screen TOP samples the atlas TOP, so map T->v0, B->v1.
        float uL = u.u0, uR = u.u1, uTop = u.v0, uBot = u.v1;

        auto push = [&](float px, float py, float pu, float pv) {
            LLRenderVK::PreVert v{px, py, 0.f, pu, pv, r, g, b, a};
            verts.push_back(v);
        };
        // Same 6-vertex order as LLFontGL::renderTriangle:
        //   (R,T)(L,T)(L,B)  (R,T)(L,B)(R,B)
        push(R, T, uR, uTop);
        push(L, T, uL, uTop);
        push(L, B, uL, uBot);
        push(R, T, uR, uTop);
        push(L, B, uL, uBot);
        push(R, B, uR, uBot);

        cx += advance;
    }

    if (verts.empty()) return;
    vk.begin(llrvk::TRIANGLES);
    vk.vertexBatchPreTransformed(verts.data(), (int)verts.size());
    vk.end();
    vk.bindTexture(0);
}

// Pixel width a string will occupy (for centering / right-align).
inline float stringWidth(const std::string& s, float pxSize) {
    const float scale = pxSize / (float)GH;
    return (float)s.size() * (GW + 1) * scale;
}

} // namespace bmfont
