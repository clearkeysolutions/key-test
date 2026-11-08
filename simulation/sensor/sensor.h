// Sensor and ISP stages (linear light in, 8-bit luma out).
//
// Sensor: scene radiance (relative, 1.0 = white diffuse reflector under the
// nominal illuminant) -> photoelectrons with an auto-exposure model ->
// Poisson shot noise + Gaussian read noise + PRNU -> full-well clip -> ADC.
// Optional RGGB Bayer mosaic with bilinear demosaic.
// ISP: digital gain, denoise, tone curve, unsharp-mask sharpening, luma,
// JPEG-like 8x8 DCT quantization, area rescale. iOS delivers the luma plane
// of a 4:2:0 frame; the reader decodes that plane directly.
#ifndef CKT_SENSOR_H
#define CKT_SENSOR_H

#include "simulation/common/image.h"
#include "simulation/common/prng.h"
#include "simulation/optics/psf.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ckt {

struct SensorModel {
    double full_well_e = 6000;   // electrons at clip (output-pixel equivalent)
    double read_noise_e = 3.0;   // electrons RMS
    double prnu = 0.006;         // photo-response non-uniformity (fraction, fixed pattern per device)
    int adc_bits = 10;
    double ae_target = 0.18;     // auto-exposure: mean metered radiance -> this fraction of full well
    double max_signal_e = 4000;  // electrons a white reflector yields at max exposure & analog gain, at illum 1
    double illum = 1.0;          // scene illuminance relative to the nominal (S08 sweeps this down)
    double ae_bias = 0.0;        // exposure compensation in stops (+ brighter)
    bool bayer = false;
    uint64_t device_seed = 0;    // fixed-pattern seed (per device, not per case)
};

struct SensorStats { double electrons_white = 0, digital_gain = 1, metered_mean = 0; };

// Fixed-pattern PRNU gain map for a device, cached per (device, size, channel).
inline const std::vector<float> &prnu_map(uint64_t device_seed, int W, int H, int channel, double prnu) {
    struct Key { uint64_t s; int w, h, c; double p; };
    static std::vector<std::pair<Key, std::vector<float>>> cache;
    for (auto &e : cache) if (e.first.s == device_seed && e.first.w == W && e.first.h == H && e.first.c == channel && e.first.p == prnu) return e.second;
    if (cache.size() > 6) cache.erase(cache.begin());
    Rng fp(device_seed, "prnu" + std::to_string(channel));
    std::vector<float> m(static_cast<size_t>(W) * H);
    for (auto &v : m) v = static_cast<float>(1 + prnu * fp.fast_normal());
    cache.push_back({{device_seed, W, H, channel, prnu}, std::move(m)});
    return cache.back().second;
}

// Photoelectrons -> clipped electrons: exact Poisson + read noise for small
// means, a single Gaussian with the combined variance above 40 e-.
inline float expose_pixel(float mean_e, float read_e, float full_well, Rng &rng) {
    float e;
    if (mean_e > 40.f) e = mean_e + std::sqrt(mean_e + read_e * read_e) * static_cast<float>(rng.fast_normal());
    else e = static_cast<float>(rng.poisson(std::max(0.f, mean_e))) + read_e * static_cast<float>(rng.fast_normal());
    return std::clamp(e, 0.f, full_well);
}

// Returns normalized code values in [0,1] (post-ADC, pre-ISP), one plane
// per channel (mono or demosaiced RGB).
inline Image sensor_capture(const Image &radiance, const SensorModel &m, Rng &rng, Rect meter, SensorStats &st) {
    const int W = radiance.w(), H = radiance.h(), C = radiance.channels();
    // Metering: mean luminance over the metering window (centre-weighted region).
    meter = clip_rect(meter, W, H);
    double sum = 0; size_t n = 0;
    for (int y = meter.y0; y < meter.y1; y += 2) for (int x = meter.x0; x < meter.x1; x += 2) {
        const float l = C == 3 ? luma709(radiance.c[0].at(x, y), radiance.c[1].at(x, y), radiance.c[2].at(x, y)) : radiance.c[0].at(x, y);
        sum += l; ++n;
    }
    const double mean = n ? std::max(1e-6, sum / n) : 0.18;
    st.metered_mean = mean;
    const double want_e_per_unit = m.ae_target * std::pow(2.0, m.ae_bias) * m.full_well_e / mean;  // electrons per unit radiance
    const double avail_e_per_unit = m.max_signal_e * m.illum;
    const double k = std::min(want_e_per_unit, avail_e_per_unit);
    st.electrons_white = k;
    st.digital_gain = want_e_per_unit / k;  // > 1 in low light: ISO beyond analog range
    const double levels = std::pow(2.0, m.adc_bits) - 1;
    Image out(C == 3 && !m.bayer ? 3 : (m.bayer ? 3 : 1), W, H);
    if (!m.bayer) {
        for (int c = 0; c < C; ++c) {
            const auto &gain = prnu_map(m.device_seed, W, H, c, m.prnu);
            const float kf = static_cast<float>(k), rn = static_cast<float>(m.read_noise_e), fw = static_cast<float>(m.full_well_e);
            const float to_code = static_cast<float>(levels / m.full_well_e), inv_levels = static_cast<float>(1.0 / levels);
            const float *src = radiance.c[c].v.data(); float *dst = out.c[c].v.data();
            for (size_t i = 0; i < radiance.c[c].v.size(); ++i) {
                const float e = expose_pixel(src[i] * kf * gain[i], rn, fw, rng);
                dst[i] = std::nearbyint(e * to_code) * inv_levels;
            }
        }
        return out;
    }
    // RGGB mosaic: each photosite sees one channel; then bilinear demosaic.
    Plane mosaic(W, H);
    const auto &gain = prnu_map(m.device_seed, W, H, 3, m.prnu);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const int ch = (y & 1) == 0 ? ((x & 1) == 0 ? 0 : 1) : ((x & 1) == 0 ? 1 : 2);
        const float r = C == 3 ? radiance.c[ch].at(x, y) : radiance.c[0].at(x, y);
        const float e = expose_pixel(static_cast<float>(r * k) * gain[static_cast<size_t>(y) * W + x], static_cast<float>(m.read_noise_e), static_cast<float>(m.full_well_e), rng);
        mosaic.at(x, y) = static_cast<float>(std::round(e / m.full_well_e * levels) / levels);
    }
    // Bilinear demosaic: the photosite's own channel, plus the mean of the
    // nearest same-channel neighbours for the other two (classic RGGB rules).
    const auto M = [&](int x, int y) { return mosaic.at(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1)); };
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const float c0 = M(x, y);
        const float cross = 0.25f * (M(x - 1, y) + M(x + 1, y) + M(x, y - 1) + M(x, y + 1));
        const float diag = 0.25f * (M(x - 1, y - 1) + M(x + 1, y - 1) + M(x - 1, y + 1) + M(x + 1, y + 1));
        const float horiz = 0.5f * (M(x - 1, y) + M(x + 1, y)), vert = 0.5f * (M(x, y - 1) + M(x, y + 1));
        float r, g, b;
        const bool ey = (y & 1) == 0, ex = (x & 1) == 0;
        if (ey && ex) { r = c0; g = cross; b = diag; }          // R site
        else if (!ey && !ex) { b = c0; g = cross; r = diag; }   // B site
        else if (ey) { g = c0; r = horiz; b = vert; }           // G on an R row
        else { g = c0; b = horiz; r = vert; }                   // G on a B row
        out.c[0].at(x, y) = r; out.c[1].at(x, y) = g; out.c[2].at(x, y) = b;
    }
    return out;
}

struct IspModel {
    int tone = 0;            // 0 = sRGB OETF, 1 = filmic S-curve, 2 = linear (gamma 1)
    double denoise = 0;      // 0..1 blend toward a 3x3 (strength>0.5: 5x5) box, edge-preserving by luminance gate
    double sharpen = 0.3;    // unsharp-mask amount
    double sharpen_sigma = 1.0;
    int jpeg_quality = 0;    // 0 = none; else 1..100 (IJG table scaling)
    double rescale = 1.0;    // output scale factor (area resample), e.g. 0.5 for a 4K->1080p preview path
    bool video_range = false;  // 16..235 luma
};

inline float tone_curve(float x, int tone) {
    x = std::max(0.f, x);
    if (tone == 2) return std::min(1.f, x);
    if (tone == 1) { const float s = srgb_encode_fast(x); return std::clamp(s * s * (3 - 2 * s) * 0.6f + s * 0.4f, 0.f, 1.f); }
    return srgb_encode_fast(x);
}

inline void denoise_plane(Plane &p, double strength) {
    if (strength <= 0) return;
    Plane s = p; gaussian_blur(s, strength > 0.5 ? 1.4 : 0.8);
    const float k = static_cast<float>(std::min(1.0, strength));
    // Edge gate: smooth less where the local difference is large relative to 0.08.
    for (size_t i = 0; i < p.v.size(); ++i) {
        const float d = std::abs(p.v[i] - s.v[i]);
        const float g = k * std::exp(-d / 0.08f);
        p.v[i] = (1 - g) * p.v[i] + g * s.v[i];
    }
}

inline void jpeg_like(Plane &y, int quality) {
    if (quality <= 0 || quality > 100) return;
    static const int base[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                                 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    const int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    double q[64]; for (int i = 0; i < 64; ++i) q[i] = std::clamp((base[i] * scale + 50) / 100, 1, 255);
    double cosv[8][8]; for (int x = 0; x < 8; ++x) for (int u = 0; u < 8; ++u) cosv[x][u] = std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16);
    const auto c = [](int u) { return u == 0 ? std::sqrt(0.125) : 0.5; };
    // Orthonormal DCT-II basis: B[u][x] = c(u) cos((2x+1) u pi / 16).
    float B[8][8];
    for (int u = 0; u < 8; ++u) for (int x = 0; x < 8; ++x) B[u][x] = static_cast<float>(c(u) * cosv[x][u]);
    float qf[64]; for (int i = 0; i < 64; ++i) qf[i] = static_cast<float>(q[i]);
    for (int by = 0; by + 8 <= y.h; by += 8) for (int bx = 0; bx + 8 <= y.w; bx += 8) {
        float blk[8][8], tmp[8][8], F[8][8];
        for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) blk[j][i] = y.at(bx + i, by + j) * 255.f - 128.f;
        for (int j = 0; j < 8; ++j) for (int u = 0; u < 8; ++u) { float a = 0; for (int i = 0; i < 8; ++i) a += B[u][i] * blk[j][i]; tmp[j][u] = a; }   // rows
        for (int v = 0; v < 8; ++v) for (int u = 0; u < 8; ++u) { float a = 0; for (int j = 0; j < 8; ++j) a += B[v][j] * tmp[j][u]; F[v][u] = std::nearbyint(a / qf[v * 8 + u]) * qf[v * 8 + u]; }
        for (int v = 0; v < 8; ++v) for (int i = 0; i < 8; ++i) { float a = 0; for (int u = 0; u < 8; ++u) a += B[u][i] * F[v][u]; tmp[v][i] = a; }     // inverse rows
        for (int j = 0; j < 8; ++j) for (int i = 0; i < 8; ++i) { float a = 0; for (int v = 0; v < 8; ++v) a += B[v][j] * tmp[v][i]; y.at(bx + i, by + j) = std::clamp((a + 128.f) / 255.f, 0.f, 1.f); }
    }
}

inline Plane area_rescale(const Plane &p, double f) {
    if (std::abs(f - 1.0) < 1e-9) return p;
    const int W = std::max(16, static_cast<int>(std::lround(p.w * f))), H = std::max(16, static_cast<int>(std::lround(p.h * f)));
    Plane o(W, H);
    const double sx = static_cast<double>(p.w) / W, sy = static_cast<double>(p.h) / H;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        if (sx <= 1 && sy <= 1) { o.at(x, y) = p.bilinear((x + .5) * sx, (y + .5) * sy); continue; }
        const double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
        double a = 0, wsum = 0;
        for (int yy = static_cast<int>(y0); yy < std::min(p.h, static_cast<int>(std::ceil(y1))); ++yy)
            for (int xx = static_cast<int>(x0); xx < std::min(p.w, static_cast<int>(std::ceil(x1))); ++xx) {
                const double w = (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0)) * (std::min<double>(yy + 1, y1) - std::max<double>(yy, y0));
                a += w * p.at(xx, yy); wsum += w;
            }
        o.at(x, y) = static_cast<float>(a / wsum);
    }
    return o;
}

// Full ISP to an 8-bit luma frame.
inline std::vector<uint8_t> isp_to_luma8(Image img, const IspModel &m, double digital_gain, int &out_w, int &out_h) {
    for (auto &p : img.c) for (auto &v : p.v) v = static_cast<float>(v * digital_gain);
    Plane y(img.w(), img.h());
    for (size_t i = 0; i < y.v.size(); ++i) {
        if (img.channels() == 3) {
            const float r = tone_curve(img.c[0].v[i], m.tone), g = tone_curve(img.c[1].v[i], m.tone), b = tone_curve(img.c[2].v[i], m.tone);
            y.v[i] = luma709(r, g, b);  // luma from gamma-encoded R'G'B' (Y'), as video pipelines do
        } else y.v[i] = tone_curve(img.c[0].v[i], m.tone);
    }
    // Luma noise reduction on the delivered Y' plane (as YUV-domain ISP NR does).
    denoise_plane(y, m.denoise);
    if (m.sharpen > 0) {
        Plane bl = y; gaussian_blur(bl, m.sharpen_sigma);
        for (size_t i = 0; i < y.v.size(); ++i) y.v[i] = std::clamp(y.v[i] + static_cast<float>(m.sharpen) * (y.v[i] - bl.v[i]), 0.f, 1.f);
    }
    y = area_rescale(y, m.rescale);
    jpeg_like(y, m.jpeg_quality);
    out_w = y.w; out_h = y.h;
    std::vector<uint8_t> out(y.v.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const double v = std::clamp<double>(y.v[i], 0, 1);
        out[i] = static_cast<uint8_t>(std::lround(m.video_range ? 16 + 219 * v : 255 * v));
    }
    return out;
}

}  // namespace ckt
#endif
