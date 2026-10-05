#ifndef FALLOUT_PLIB_GNW_LIGHTING_FX_H_
#define FALLOUT_PLIB_GNW_LIGHTING_FX_H_

// A post-process "lighting" pass over a finished 0x00RRGGBB picture: a soft glow
// around bright things, extra local contrast so shapes stand out from what is
// behind them, a gentle vignette and a warmer, slightly richer colour grade.
// Works on the picture as a whole (it knows nothing about the game's lights).

#include "plib/gnw/xbrz2x.h"

#include <math.h>
#include <stdint.h>
#include <vector>

namespace fallout {
namespace lightingfx {

// Strengths (the first version had 0.9 / 0.42 / 1.5 / 0.85 / 0.10 -- too strong).
static const float kLocalContrast = 0.35f; // how much a thing stands out from its surroundings
static const float kGlowThreshold = 0.52f; // brightness above which a glow appears
static const float kGlowGain = 1.0f;
static const float kGlowAmount = 0.40f; // how much of the glow is added back
static const float kSCurve = 0.04f; // overall contrast curve

// Colour enrichment strengths.
static const float kVibrance = 0.30f; // extra saturation for dull colours
static const float kShadowCool = 0.006f; // blue/teal tint in the shadows
static const float kHighlightWarm = 0.014f; // warm tint in the highlights

struct Scratch {
    std::vector<float> lowRaw; // 4x4 block averages, rgb
    std::vector<float> glow; // small blur
    std::vector<float> base; // large blur
    std::vector<float> tmp;
};

// In-place box blur (radius in samples) of a 3-channel float image.
static void boxBlur(const std::vector<float>& in, std::vector<float>& out, std::vector<float>& tmp, int w, int h, int radius)
{
    tmp.assign(in.size(), 0.0f);
    out.assign(in.size(), 0.0f);
    const float norm = 1.0f / static_cast<float>(radius * 2 + 1);

    // horizontal
    for (int y = 0; y < h; y++) {
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; k++) {
                int xx = k < 0 ? 0 : (k >= w ? w - 1 : k);
                sum += in[(static_cast<size_t>(y) * w + xx) * 3 + c];
            }
            for (int x = 0; x < w; x++) {
                tmp[(static_cast<size_t>(y) * w + x) * 3 + c] = sum * norm;
                int addX = x + radius + 1;
                int subX = x - radius;
                addX = addX >= w ? w - 1 : addX;
                subX = subX < 0 ? 0 : subX;
                sum += in[(static_cast<size_t>(y) * w + addX) * 3 + c] - in[(static_cast<size_t>(y) * w + subX) * 3 + c];
            }
        }
    }

    // vertical
    for (int x = 0; x < w; x++) {
        for (int c = 0; c < 3; c++) {
            float sum = 0.0f;
            for (int k = -radius; k <= radius; k++) {
                int yy = k < 0 ? 0 : (k >= h ? h - 1 : k);
                sum += tmp[(static_cast<size_t>(yy) * w + x) * 3 + c];
            }
            for (int y = 0; y < h; y++) {
                out[(static_cast<size_t>(y) * w + x) * 3 + c] = sum * norm;
                int addY = y + radius + 1;
                int subY = y - radius;
                addY = addY >= h ? h - 1 : addY;
                subY = subY < 0 ? 0 : subY;
                sum += tmp[(static_cast<size_t>(addY) * w + x) * 3 + c] - tmp[(static_cast<size_t>(subY) * w + x) * 3 + c];
            }
        }
    }
}

static inline float luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Bilinear sample of a 3-channel low-resolution image at full-resolution (x, y).
static inline void sampleLow(const std::vector<float>& img, int lw, int lh, int x, int y, int scale, float* rgb)
{
    float fx = (static_cast<float>(x) + 0.5f) / scale - 0.5f;
    float fy = (static_cast<float>(y) + 0.5f) / scale - 0.5f;
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    int x1 = x0 + 1 < lw ? x0 + 1 : lw - 1;
    int y1 = y0 + 1 < lh ? y0 + 1 : lh - 1;
    if (x0 >= lw) x0 = lw - 1;
    if (y0 >= lh) y0 = lh - 1;
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    for (int c = 0; c < 3; c++) {
        const float a = img[(static_cast<size_t>(y0) * lw + x0) * 3 + c];
        const float b = img[(static_cast<size_t>(y0) * lw + x1) * 3 + c];
        const float d = img[(static_cast<size_t>(y1) * lw + x0) * 3 + c];
        const float e = img[(static_cast<size_t>(y1) * lw + x1) * 3 + c];
        rgb[c] = (a * (1.0f - tx) + b * tx) * (1.0f - ty) + (d * (1.0f - tx) + e * tx) * ty;
    }
}

// `enrich` adds the colour enrichment (see below).
static void apply(uint32_t* pixels, int w, int h, int pitch, Scratch& s, bool enrich = false)
{
    const int scale = 4;
    const int lw = w / scale;
    const int lh = h / scale;
    if (lw < 8 || lh < 8) {
        return;
    }

    // 4x4 block averages.
    s.lowRaw.assign(static_cast<size_t>(lw) * lh * 3, 0.0f);
    xbrz2x::parallelRows(lh, [&](int firstRow, int endRow) {
        for (int ly = firstRow; ly < endRow; ly++) {
            for (int lx = 0; lx < lw; lx++) {
                float r = 0.0f, g = 0.0f, b = 0.0f;
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        const uint32_t p = pixels[static_cast<size_t>(ly * scale + dy) * pitch + lx * scale + dx];
                        r += static_cast<float>((p >> 16) & 255);
                        g += static_cast<float>((p >> 8) & 255);
                        b += static_cast<float>(p & 255);
                    }
                }
                const float k = 1.0f / (255.0f * scale * scale);
                float* o = &s.lowRaw[(static_cast<size_t>(ly) * lw + lx) * 3];
                o[0] = r * k;
                o[1] = g * k;
                o[2] = b * k;
            }
        }
    });

    boxBlur(s.lowRaw, s.glow, s.tmp, lw, lh, 3);
    boxBlur(s.lowRaw, s.base, s.tmp, lw, lh, 12);

    const float halfW = static_cast<float>(w) * 0.5f;
    const float halfH = static_cast<float>(h) * 0.5f;

    xbrz2x::parallelRows(h, [&](int firstRow, int endRow) {
        for (int y = firstRow; y < endRow; y++) {
            const float ny = (static_cast<float>(y) - halfH) / halfH;
            for (int x = 0; x < w; x++) {
                const uint32_t p = pixels[static_cast<size_t>(y) * pitch + x];
                float c[3] = {
                    static_cast<float>((p >> 16) & 255) * (1.0f / 255.0f),
                    static_cast<float>((p >> 8) & 255) * (1.0f / 255.0f),
                    static_cast<float>(p & 255) * (1.0f / 255.0f)
                };

                float glow[3];
                float base[3];
                sampleLow(s.glow, lw, lh, x, y, scale, glow);
                sampleLow(s.base, lw, lh, x, y, scale, base);

                const float lc = luma(c[0], c[1], c[2]);
                const float lb = luma(base[0], base[1], base[2]);

                // Local contrast: things brighter than their surroundings get
                // brighter, darker ones darker -- gives shapes some depth.
                const float d = lc - lb;
                const float boost = 1.0f + kLocalContrast * d;

                // Glow around bright areas.
                const float lg = luma(glow[0], glow[1], glow[2]);
                float bright = lg - kGlowThreshold;
                bright = bright < 0.0f ? 0.0f : bright * kGlowGain;

                const float nx = (static_cast<float>(x) - halfW) / halfW;
                const float vignette = 1.0f - 0.16f * (nx * nx + ny * ny) * 0.5f;

                float out[3];
                for (int k = 0; k < 3; k++) {
                    float v = c[k] * boost + glow[k] * bright * kGlowAmount;
                    out[k] = v;
                }

                if (enrich) {
                    // Colour enrichment, per pixel (nothing is blurred, so colour
                    // cannot smear): dull colours gain more saturation than ones
                    // that are already vivid, which widens the range of colours
                    // without turning everything neon.
                    float mx = out[0] > out[1] ? out[0] : out[1];
                    mx = mx > out[2] ? mx : out[2];
                    float mn = out[0] < out[1] ? out[0] : out[1];
                    mn = mn < out[2] ? mn : out[2];
                    const float sat = mx > 0.001f ? (mx - mn) / mx : 0.0f;
                    const float vib = 1.0f + kVibrance * (1.0f - sat);
                    const float lo0 = luma(out[0], out[1], out[2]);
                    for (int k = 0; k < 3; k++) {
                        out[k] = lo0 + (out[k] - lo0) * vib;
                    }

                    // Split toning: cool shadows, warm highlights.
                    float shadow = 1.0f - lo0 * 2.5f;
                    shadow = shadow < 0.0f ? 0.0f : shadow;
                    float highlight = lo0 * 2.0f - 1.0f;
                    highlight = highlight < 0.0f ? 0.0f : highlight;
                    out[0] += kHighlightWarm * highlight - kShadowCool * shadow;
                    out[1] += 0.01f * highlight + 0.004f * shadow;
                    out[2] += kShadowCool * 1.6f * shadow - kHighlightWarm * highlight;
                }

                // Colour grade: a touch warmer, a touch richer, a gentle S-curve.
                float lo = luma(out[0], out[1], out[2]);
                out[0] = lo + (out[0] - lo) * 1.12f;
                out[1] = lo + (out[1] - lo) * 1.12f;
                out[2] = lo + (out[2] - lo) * 1.12f;
                out[0] *= 1.035f;
                out[2] *= 0.965f;

                uint32_t packed = 0;
                for (int k = 0; k < 3; k++) {
                    float v = out[k] * vignette;
                    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    v = v + kSCurve * v * (1.0f - v) * (v - 0.5f) * 4.0f; // mild S-curve
                    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                    packed = (packed << 8) | static_cast<uint32_t>(v * 255.0f + 0.5f);
                }
                pixels[static_cast<size_t>(y) * pitch + x] = packed;
            }
        }
    });
}

} // namespace lightingfx
} // namespace fallout

#endif /* FALLOUT_PLIB_GNW_LIGHTING_FX_H_ */
