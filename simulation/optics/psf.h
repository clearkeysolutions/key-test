// Lens point-spread functions applied in linear light.
//  * Gaussian defocus / residual aberration: separable, exact kernel for
//    sigma <= 3 px, three-pass box approximation above (Wells 1986).
//  * Pillbox (geometric defocus disc of radius R px): 2D convolution limited
//    to a region of interest (the symbol and a margin); outside it the
//    variance-matched Gaussian (sigma = R/2) is used. Documented in
//    simulation/README.md.
//  * Field curvature: blur grows with radius; implemented by blending a
//    centre and an edge Gaussian by normalized r^2.
#ifndef CKT_PSF_H
#define CKT_PSF_H

#include "simulation/common/image.h"

#include <cmath>
#include <vector>

namespace ckt {

inline void box_blur_1d(const float *in, float *out, int n, int stride, int r) {
    if (r <= 0) { for (int i = 0; i < n; ++i) out[i * stride] = in[i * stride]; return; }
    double acc = 0; const double norm = 1.0 / (2 * r + 1);
    for (int i = -r; i <= r; ++i) acc += in[std::clamp(i, 0, n - 1) * stride];
    for (int i = 0; i < n; ++i) {
        out[i * stride] = static_cast<float>(acc * norm);
        acc += in[std::min(i + r + 1, n - 1) * stride] - in[std::max(i - r, 0) * stride];
    }
}

inline void gaussian_blur(Plane &p, double sigma) {
    if (sigma < 0.05 || p.empty()) return;
    const int W = p.w, H = p.h;
    if (sigma > 3.0) {
        // Three box passes of width 2r+1 approximate a Gaussian with variance 3*((2r+1)^2-1)/12.
        std::vector<float> tmp(p.v.size());
        const int r = std::max(1, static_cast<int>(std::round((std::sqrt(4.0 * sigma * sigma + 1) - 1) / 2)));
        for (int pass = 0; pass < 3; ++pass) {
            for (int y = 0; y < H; ++y) box_blur_1d(&p.v[static_cast<size_t>(y) * W], &tmp[static_cast<size_t>(y) * W], W, 1, r);
            for (int x = 0; x < W; ++x) box_blur_1d(&tmp[x], &p.v[x], H, W, r);
        }
        return;
    }
    const int r = static_cast<int>(std::ceil(3 * sigma));
    std::vector<float> k(2 * r + 1); double s = 0;
    for (int i = -r; i <= r; ++i) s += k[i + r] = static_cast<float>(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (auto &x : k) x = static_cast<float>(x / s);
    // Horizontal: pad each row by r (edge-replicated), then accumulate tap by tap.
    std::vector<float> row(static_cast<size_t>(W) + 2 * r), acc(W), tmp(p.v.size());
    for (int y = 0; y < H; ++y) {
        const float *src = &p.v[static_cast<size_t>(y) * W];
        for (int i = 0; i < r; ++i) { row[i] = src[0]; row[r + W + i] = src[W - 1]; }
        std::copy(src, src + W, row.begin() + r);
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int t = 0; t <= 2 * r; ++t) { const float kt = k[t]; const float *in = &row[t]; for (int x = 0; x < W; ++x) acc[x] += kt * in[x]; }
        std::copy(acc.begin(), acc.end(), tmp.begin() + static_cast<std::ptrdiff_t>(y) * W);
    }
    // Vertical: each output row is a weighted sum of 2r+1 input rows (clamped row index).
    for (int y = 0; y < H; ++y) {
        std::fill(acc.begin(), acc.end(), 0.f);
        for (int t = -r; t <= r; ++t) {
            const float kt = k[t + r]; const float *in = &tmp[static_cast<size_t>(std::clamp(y + t, 0, H - 1)) * W];
            for (int x = 0; x < W; ++x) acc[x] += kt * in[x];
        }
        std::copy(acc.begin(), acc.end(), p.v.begin() + static_cast<std::ptrdiff_t>(y) * W);
    }
}

// Pillbox convolution of radius R (px) inside roi; Gaussian outside.
inline void disc_blur(Plane &p, double R, Rect roi) {
    if (R < 0.3) return;
    Plane outside = p;
    gaussian_blur(outside, R / 2);
    roi = clip_rect(roi, p.w, p.h);
    const int r = static_cast<int>(std::ceil(R));
    std::vector<std::pair<int, float>> taps;  // offset index pairs computed per row
    std::vector<int> dx, dy; std::vector<float> wt; double sum = 0;
    for (int j = -r; j <= r; ++j) for (int i = -r; i <= r; ++i) {
        // Area of the unit pixel inside the disc, estimated with 4x4 samples.
        int in = 0; for (int a = 0; a < 4; ++a) for (int b = 0; b < 4; ++b) { const double x = i - 0.375 + 0.25 * a, y = j - 0.375 + 0.25 * b; in += x * x + y * y <= R * R; }
        if (!in) continue;
        dx.push_back(i); dy.push_back(j); wt.push_back(in / 16.f); sum += in / 16.0;
    }
    for (auto &w : wt) w = static_cast<float>(w / sum);
    Plane out = outside;
    for (int y = roi.y0; y < roi.y1; ++y) for (int x = roi.x0; x < roi.x1; ++x) {
        float a = 0;
        for (size_t t = 0; t < wt.size(); ++t) a += wt[t] * p.clamped(x + dx[t], y + dy[t]);
        out.at(x, y) = a;
    }
    // Feather the ROI boundary over r pixels so no seam forms.
    for (int y = roi.y0; y < roi.y1; ++y) for (int x = roi.x0; x < roi.x1; ++x) {
        const int d = std::min({x - roi.x0, roi.x1 - 1 - x, y - roi.y0, roi.y1 - 1 - y});
        if (d < r) { const float f = static_cast<float>(d) / r; out.at(x, y) = f * out.at(x, y) + (1 - f) * outside.at(x, y); }
    }
    p = std::move(out);
}

// Radially varying blur: sigma_centre at the optical axis, sigma_edge at the
// frame corner, blended by normalized r^2.
inline void field_blur(Plane &p, double sigma_centre, double sigma_edge) {
    if (std::abs(sigma_edge - sigma_centre) < 0.05) { gaussian_blur(p, sigma_centre); return; }
    Plane a = p, b = p;
    gaussian_blur(a, sigma_centre); gaussian_blur(b, sigma_edge);
    const double cx = p.w / 2.0, cy = p.h / 2.0, rmax2 = cx * cx + cy * cy;
    for (int y = 0; y < p.h; ++y) for (int x = 0; x < p.w; ++x) {
        const float t = static_cast<float>(((x + .5 - cx) * (x + .5 - cx) + (y + .5 - cy) * (y + .5 - cy)) / rmax2);
        p.at(x, y) = (1 - t) * a.at(x, y) + t * b.at(x, y);
    }
}

}  // namespace ckt
#endif
