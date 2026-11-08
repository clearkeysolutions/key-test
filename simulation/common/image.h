// Float image planes (linear light unless stated) used by every simulation stage.
#ifndef CKT_IMAGE_H
#define CKT_IMAGE_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ckt {

struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    Plane() = default;
    Plane(int width, int height, float fill = 0.f) : w(width), h(height), v(static_cast<size_t>(width) * height, fill) {}
    float &at(int x, int y) { return v[static_cast<size_t>(y) * w + x]; }
    float at(int x, int y) const { return v[static_cast<size_t>(y) * w + x]; }
    float clamped(int x, int y) const { return at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1)); }
    // Bilinear sample with pixel centres at integer + 0.5; clamps at edges.
    float bilinear(double x, double y) const {
        x -= 0.5; y -= 0.5;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = static_cast<float>(x - x0), fy = static_cast<float>(y - y0);
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < w && y0 + 1 < h) {  // interior fast path
            const float *r0 = &v[static_cast<size_t>(y0) * w + x0], *r1 = r0 + w;
            return (r0[0] * (1 - fx) + r0[1] * fx) * (1 - fy) + (r1[0] * (1 - fx) + r1[1] * fx) * fy;
        }
        const float a = clamped(x0, y0), b = clamped(x0 + 1, y0), c = clamped(x0, y0 + 1), d = clamped(x0 + 1, y0 + 1);
        return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
    }
    bool empty() const { return v.empty(); }
};

// An image of 1 (mono luminance) or 3 (linear RGB) planes.
struct Image {
    std::vector<Plane> c;
    Image() = default;
    Image(int channels, int w, int h, float fill = 0.f) : c(static_cast<size_t>(channels), Plane(w, h, fill)) {}
    int channels() const { return static_cast<int>(c.size()); }
    int w() const { return c.empty() ? 0 : c[0].w; }
    int h() const { return c.empty() ? 0 : c[0].h; }
};

// Rec. 709 luma weights (the iOS reader converts BGRA with 54/183/19 >> 8).
inline float luma709(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }

inline float srgb_to_linear(float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); }
inline float linear_to_srgb(float l) {
    l = std::clamp(l, 0.f, 1.f);
    return l <= 0.0031308f ? 12.92f * l : 1.055f * std::pow(l, 1.f / 2.4f) - 0.055f;
}

// LUT versions (4096 entries, linear interpolation; max abs error < 2e-4).
inline float srgb_encode_fast(float l) {
    static const std::vector<float> t = [] { std::vector<float> v(4097); for (int i = 0; i <= 4096; ++i) v[i] = linear_to_srgb(i / 4096.f); return v; }();
    const float x = std::clamp(l, 0.f, 1.f) * 4096.f; const int i = std::min(4095, static_cast<int>(x)); const float f = x - i;
    return t[i] + f * (t[i + 1] - t[i]);
}
inline float srgb_decode_fast(float s) {
    static const std::vector<float> t = [] { std::vector<float> v(4097); for (int i = 0; i <= 4096; ++i) v[i] = srgb_to_linear(i / 4096.f); return v; }();
    const float x = std::clamp(s, 0.f, 1.f) * 4096.f; const int i = std::min(4095, static_cast<int>(x)); const float f = x - i;
    return t[i] + f * (t[i + 1] - t[i]);
}

inline constexpr double kPiD = 3.14159265358979323846;

struct Rect { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; };  // half-open [x0,x1) x [y0,y1)
inline Rect clip_rect(Rect r, int w, int h) { return {std::max(0, r.x0), std::max(0, r.y0), std::min(w, r.x1), std::min(h, r.y1)}; }

}  // namespace ckt
#endif
