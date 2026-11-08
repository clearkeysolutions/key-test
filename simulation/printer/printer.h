// Print substrate: binary device dots at the printer's resolution, dot gain
// (ink spread, signed), optional clustered-dot halftone for tinted ink,
// paper wicking blur, and multiplicative paper texture. Output is a linear
// reflectance texture over the symbol canvas.
#ifndef CKT_PRINTER_H
#define CKT_PRINTER_H

#include "simulation/common/image.h"
#include "simulation/common/prng.h"
#include "simulation/optics/psf.h"

#include <cmath>
#include <vector>

namespace ckt {

// Exact Euclidean distance transform (Felzenszwalb & Huttenlocher 2012):
// distance in pixels from each pixel to the nearest pixel with mask != 0.
inline std::vector<float> edt(const std::vector<uint8_t> &mask, int w, int h) {
    const float INF = 1e20f;
    std::vector<float> f(mask.size());
    for (size_t i = 0; i < f.size(); ++i) f[i] = mask[i] ? 0.f : INF;
    std::vector<float> d, z; std::vector<int> v;
    const auto pass = [&](float *p, int n, int stride) {
        d.assign(n, 0); z.assign(n + 1, 0); v.assign(n, 0);
        int k = 0; v[0] = 0; z[0] = -INF; z[1] = INF;
        for (int q = 1; q < n; ++q) {
            float s;
            while (true) {
                const int r = v[k];
                s = ((p[q * stride] + float(q) * q) - (p[r * stride] + float(r) * r)) / (2.f * q - 2.f * r);
                if (s <= z[k] && k > 0) --k; else break;
            }
            if (s <= z[k]) { k = 0; v[0] = q; z[0] = -INF; z[1] = INF; continue; }
            ++k; v[k] = q; z[k] = s; z[k + 1] = INF;
        }
        k = 0;
        for (int q = 0; q < n; ++q) { while (z[k + 1] < q) ++k; d[q] = float(q - v[k]) * (q - v[k]) + p[v[k] * stride]; }
        for (int q = 0; q < n; ++q) p[q * stride] = d[q];
    };
    for (int x = 0; x < w; ++x) pass(&f[x], h, w);
    for (int y = 0; y < h; ++y) pass(&f[static_cast<size_t>(y) * w], w, 1);
    for (auto &x : f) x = std::sqrt(x);
    return f;
}

struct PrintModel {
    double dpi = 600;            // device dots per inch (binary)
    double dot_gain_mm = 0.02;   // ink spread radius (negative = under-inking / erosion)
    double paper_blur_mm = 0.01; // wicking / optical scatter in paper
    double halftone_lpi = 0;     // 0: solid ink; else clustered-dot screen at 45 deg
    double ink_density = 1.0;    // tint for halftoned ink (1 = solid)
    double texture = 0.02;       // paper texture RMS (multiplicative)
    double texture_mm = 0.15;    // texture correlation length
    float ink[3] = {0.04f, 0.04f, 0.04f};    // ink reflectance (linear)
    float paper[3] = {0.85f, 0.85f, 0.85f};  // paper reflectance (linear)
    bool marks_are_ink = true;   // dark-on-light print; false inks the background
};

// coverage: mark coverage over the canvas, rasterized at the device size.
inline Image print_texture(const Plane &coverage, double canvas_mm, const PrintModel &m, int channels, Rng &rng, double &dpi_effective) {
    const int n = coverage.w;
    dpi_effective = n / (canvas_mm / 25.4);
    const double px_per_mm = n / canvas_mm;
    std::vector<uint8_t> ink(coverage.v.size());
    for (size_t i = 0; i < ink.size(); ++i) ink[i] = m.marks_are_ink ? coverage.v[i] >= 0.5f : coverage.v[i] < 0.5f;
    const double g = m.dot_gain_mm * px_per_mm;
    if (g > 0.05) {
        const auto dist = edt(ink, n, n);
        for (size_t i = 0; i < ink.size(); ++i) ink[i] = dist[i] <= g;
    } else if (g < -0.05) {
        std::vector<uint8_t> paper(ink.size()); for (size_t i = 0; i < ink.size(); ++i) paper[i] = !ink[i];
        const auto dist = edt(paper, n, n);
        for (size_t i = 0; i < ink.size(); ++i) if (ink[i] && dist[i] <= -g) ink[i] = 0;
    }
    Plane cover(n, n);
    const bool halftone = m.halftone_lpi > 0 && m.ink_density < 0.999;
    const double cell_px = halftone ? px_per_mm * 25.4 / m.halftone_lpi : 1;
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        float c = ink[static_cast<size_t>(y) * n + x];
        if (c > 0 && halftone) {
            const double a = 0.70710678, xr = (x * a - y * a) / cell_px, yr = (x * a + y * a) / cell_px;
            const double t = 0.5 + 0.25 * (std::cos(2 * kPiD * xr) + std::cos(2 * kPiD * yr));  // clustered-dot threshold in [0,1]
            c = m.ink_density > t ? 1.f : 0.f;
        }
        cover.at(x, y) = c;
    }
    gaussian_blur(cover, m.paper_blur_mm * px_per_mm);
    // Low-frequency paper texture: a coarse noise grid upsampled bilinearly.
    const int tg = std::max(2, static_cast<int>(canvas_mm / std::max(0.02, m.texture_mm)));
    Plane tex(tg + 2, tg + 2);
    for (auto &v : tex.v) v = static_cast<float>(1 + m.texture * rng.normal());
    Image out(channels, n, n);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const float t = tex.bilinear((x + .5) / n * tg + 1, (y + .5) / n * tg + 1);
        const float c = cover.at(x, y);
        for (int ch = 0; ch < channels; ++ch) {
            const int k = channels == 3 ? ch : 1;
            out.c[ch].at(x, y) = std::max(0.f, (m.paper[k] * (1 - c) + m.ink[k] * c) * t);
        }
    }
    return out;
}

}  // namespace ckt
#endif
