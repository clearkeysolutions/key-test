// Scene content around and on the symbol: background clutter and
// distractors (image space), illumination field / shadows / specular glare
// (surface space, they move with the object), surface damage (scratches,
// tears, stickers, missing corners, finder covers, smudges) and image-space
// occluders (a finger near the lens).
#ifndef CKT_SCENE_H
#define CKT_SCENE_H

#include "simulation/common/image.h"
#include "simulation/common/prng.h"

#include <cmath>
#include <vector>

namespace ckt {

struct SceneModel {
    double clutter = 0.3;       // 0..1 density of background objects
    int distractors = 0;        // code-like distractor patterns (grids, dot rings, fake CKey frames)
    double bg_level = 0.35;     // mean background reflectance
    double bg_gradient = 0.3;   // background illumination gradient across the frame
};

inline void fill_rect(Image &img, double x0, double y0, double x1, double y1, const float *col) {
    const int W = img.w(), H = img.h();
    for (int y = std::max(0, static_cast<int>(y0)); y < std::min(H, static_cast<int>(y1)); ++y)
        for (int x = std::max(0, static_cast<int>(x0)); x < std::min(W, static_cast<int>(x1)); ++x)
            for (int c = 0; c < img.channels(); ++c) img.c[c].at(x, y) = col[c];
}

inline void fill_disc(Image &img, double cx, double cy, double r, const float *col) {
    const int W = img.w(), H = img.h();
    for (int y = std::max(0, static_cast<int>(cy - r)); y < std::min(H, static_cast<int>(cy + r + 1)); ++y)
        for (int x = std::max(0, static_cast<int>(cx - r)); x < std::min(W, static_cast<int>(cx + r + 1)); ++x)
            if ((x + .5 - cx) * (x + .5 - cx) + (y + .5 - cy) * (y + .5 - cy) <= r * r)
                for (int c = 0; c < img.channels(); ++c) img.c[c].at(x, y) = col[c];
}

inline void stroke_line(Image &img, double x0, double y0, double x1, double y1, double hw, const float *col) {
    const int W = img.w(), H = img.h();
    const int bx0 = std::max(0, static_cast<int>(std::min(x0, x1) - hw - 1)), bx1 = std::min(W, static_cast<int>(std::max(x0, x1) + hw + 2));
    const int by0 = std::max(0, static_cast<int>(std::min(y0, y1) - hw - 1)), by1 = std::min(H, static_cast<int>(std::max(y0, y1) + hw + 2));
    const double vx = x1 - x0, vy = y1 - y0, L2 = std::max(1e-9, vx * vx + vy * vy);
    for (int y = by0; y < by1; ++y) for (int x = bx0; x < bx1; ++x) {
        const double t = std::clamp(((x + .5 - x0) * vx + (y + .5 - y0) * vy) / L2, 0.0, 1.0);
        if (std::hypot(x + .5 - (x0 + t * vx), y + .5 - (y0 + t * vy)) <= hw) for (int c = 0; c < img.channels(); ++c) img.c[c].at(x, y) = col[c];
    }
}

inline void random_colour(Rng &r, int channels, float *col, double level) {
    const double base = std::clamp(level * std::exp(1.2 * r.normal()), 0.01, 0.95);
    for (int c = 0; c < 3; ++c) col[c] = static_cast<float>(channels == 3 ? std::clamp(base * (0.6 + 0.8 * r.unit()), 0.005, 0.97) : base);
}

// Background radiance (reflectance x nominal light) for the whole frame.
inline Image make_background(int channels, int W, int H, const SceneModel &m, Rng &r) {
    Image img(channels, W, H);
    const double ang = r.uniform(0, 2 * kPiD), gx = std::cos(ang), gy = std::sin(ang);
    float base[3]; random_colour(r, channels, base, m.bg_level);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const double t = ((x - W / 2.0) * gx + (y - H / 2.0) * gy) / std::max(W, H);
        for (int c = 0; c < channels; ++c) img.c[c].at(x, y) = static_cast<float>(base[c] * std::max(0.05, 1 + m.bg_gradient * 2 * t));
    }
    const int scale = std::max(W, H);
    const int rects = static_cast<int>(3 + 40 * m.clutter), strokes = static_cast<int>(60 * m.clutter), discs = static_cast<int>(10 * m.clutter);
    float col[3];
    for (int i = 0; i < rects; ++i) {
        random_colour(r, channels, col, m.bg_level);
        const double w = scale * r.uniform(0.02, 0.35), h = scale * r.uniform(0.02, 0.35), x = r.uniform(-0.1, 1.0) * W, y = r.uniform(-0.1, 1.0) * H;
        fill_rect(img, x, y, x + w, y + h, col);
        if (r.unit() < 0.3) {  // striped / textured surfaces (fabric, wood, blinds)
            float c2[3]; random_colour(r, channels, c2, m.bg_level);
            const double period = r.uniform(3, 40);
            for (double s = x; s < x + w; s += period) fill_rect(img, s, y, s + period * 0.4, y + h, c2);
        }
    }
    for (int i = 0; i < strokes; ++i) {  // text-like strokes: short dark lines in clusters
        random_colour(r, channels, col, 0.06);
        const double cx = r.unit() * W, cy = r.unit() * H, len = r.uniform(4, 40), a = r.uniform(0, 2 * kPiD), hw = r.uniform(0.6, 3);
        for (int k = 0; k < 12; ++k) {
            const double ox = cx + k * len * 0.9 * std::cos(a), oy = cy + k * len * 0.9 * std::sin(a);
            stroke_line(img, ox, oy, ox + len * 0.6 * std::cos(a + r.normal() * 0.6), oy + len * 0.6 * std::sin(a + r.normal() * 0.6), hw, col);
        }
    }
    for (int i = 0; i < discs; ++i) { random_colour(r, channels, col, m.bg_level); fill_disc(img, r.unit() * W, r.unit() * H, scale * r.uniform(0.005, 0.06), col); }
    // Code-like distractors.
    for (int i = 0; i < m.distractors; ++i) {
        const double side = scale * r.uniform(0.05, 0.22), x = r.uniform(0, 1) * (W - side), y = r.uniform(0, 1) * (H - side);
        const float dark[3] = {0.03f, 0.03f, 0.03f}, light[3] = {0.85f, 0.85f, 0.85f};
        const int kind = static_cast<int>(r.below(4));
        fill_rect(img, x, y, x + side, y + side, light);
        if (kind == 0) {  // QR/Data-Matrix-like module grid with three square finders
            const int n = 21 + 4 * static_cast<int>(r.below(5)); const double m1 = side / n;
            for (int j = 0; j < n; ++j) for (int k = 0; k < n; ++k) if (r.unit() < 0.5) fill_rect(img, x + k * m1, y + j * m1, x + (k + 1) * m1, y + (j + 1) * m1, dark);
            for (auto [fx, fy] : {std::pair{0, 0}, std::pair{n - 7, 0}, std::pair{0, n - 7}}) {
                fill_rect(img, x + fx * m1, y + fy * m1, x + (fx + 7) * m1, y + (fy + 7) * m1, dark);
                fill_rect(img, x + (fx + 1) * m1, y + (fy + 1) * m1, x + (fx + 6) * m1, y + (fy + 6) * m1, light);
                fill_rect(img, x + (fx + 2) * m1, y + (fy + 2) * m1, x + (fx + 5) * m1, y + (fy + 5) * m1, dark);
            }
        } else if (kind == 1) {  // polar dot rings (resembles the CKey ring matrix)
            const double cx = x + side / 2, cy = y + side / 2;
            for (int ring = 0; ring < 8; ++ring) {
                const double rr = side * (0.12 + 0.045 * ring); const int slots = 48 + 8 * ring;
                for (int s = 0; s < slots; ++s) if (r.unit() < 0.5) fill_disc(img, cx + rr * std::cos(2 * kPiD * s / slots), cy + rr * std::sin(2 * kPiD * s / slots), side * 0.008, dark);
            }
        } else if (kind == 2) {  // fake CKey frame: rounded border, corner arcs, random interior
            const double t = side * 0.022;
            fill_rect(img, x, y, x + side, y + t, dark); fill_rect(img, x, y + side - t, x + side, y + side, dark);
            fill_rect(img, x, y, x + t, y + side, dark); fill_rect(img, x + side - t, y, x + side, y + side, dark);
            for (int k = 0; k < 400; ++k) fill_disc(img, x + r.uniform(0.1, 0.9) * side, y + r.uniform(0.1, 0.9) * side, side * 0.01, dark);
        } else {  // dark square with light border (polarity-inverted look)
            fill_rect(img, x + side * 0.05, y + side * 0.05, x + side * 0.95, y + side * 0.95, dark);
            for (int k = 0; k < 300; ++k) fill_disc(img, x + r.uniform(0.15, 0.85) * side, y + r.uniform(0.15, 0.85) * side, side * 0.012, light);
        }
    }
    return img;
}

// ---- Surface-space lighting --------------------------------------------------
struct Lighting {
    double gradient = 0;        // illumination slope across the symbol (fraction per half-width)
    double gradient_angle = 0;  // degrees
    struct Shadow { double nx, ny, offset, softness, depth; };
    std::vector<Shadow> shadows;  // half-plane shadows with soft edges (surface mm)
    struct Glare { double u, v, radius, peak; };
    std::vector<Glare> glare;     // specular highlights (additive radiance, relative to white)
    double veil = 0;              // uniform additive veiling glare
    double half_mm = 20;
    double gx = 0, gy = 0;   // precomputed gradient direction / half_mm
    bool flat = true;        // no gradient, shadows, glare or veil: illum = 1, specular = 0
    void prepare() {
        const double a = gradient_angle * kPiD / 180;
        gx = gradient * std::cos(a) / half_mm; gy = gradient * std::sin(a) / half_mm;
        flat = gradient == 0 && shadows.empty() && glare.empty() && veil == 0;
    }
    float illum(double u, double v) const {
        if (flat) return 1.f;
        double f = 1 + u * gx + v * gy;
        for (const auto &s : shadows) {
            const double d = (u * s.nx + v * s.ny - s.offset) / std::max(1e-3, s.softness);
            f *= 1 - s.depth / (1 + std::exp(-d));
        }
        return static_cast<float>(std::max(0.02, f));
    }
    float specular(double u, double v) const {
        if (flat) return 0.f;
        double s = veil;
        for (const auto &g : glare) { const double d2 = ((u - g.u) * (u - g.u) + (v - g.v) * (v - g.v)) / (g.radius * g.radius); if (d2 < 9) s += g.peak * std::exp(-0.5 * d2); }
        return static_cast<float>(s);
    }
};

// ---- Surface damage ----------------------------------------------------------------
struct Damage {
    enum Kind { Scratch, Hole, Sticker, Smudge } kind;
    double x0, y0, x1, y1, width;  // Scratch: segment; others: axis-aligned box in canvas mm, or polygon via (x0..y1) triangle
    double tri[6]{};               // Hole as triangle when width < 0
    float value[3]{};              // replacement reflectance (Scratch/Sticker); Hole shows hole_refl
    double strength = 1;           // Smudge: blend toward mid-grey
    bool triangle = false;
};

inline bool point_in_tri(const double *t, double x, double y) {
    const auto s = [](double ax, double ay, double bx, double by, double px, double py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
    const double d1 = s(t[0], t[1], t[2], t[3], x, y), d2 = s(t[2], t[3], t[4], t[5], x, y), d3 = s(t[4], t[5], t[0], t[1], x, y);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

// Returns true if the damage covers (x,y) (canvas mm); writes the replacement.
inline bool damage_at(const std::vector<Damage> &ds, double x, double y, int ch, float &value, bool &hole) {
    bool any = false;
    for (const auto &d : ds) {
        bool in = false;
        if (d.kind == Damage::Scratch) {
            const double vx = d.x1 - d.x0, vy = d.y1 - d.y0, L2 = std::max(1e-9, vx * vx + vy * vy);
            const double t = std::clamp(((x - d.x0) * vx + (y - d.y0) * vy) / L2, 0.0, 1.0);
            in = std::hypot(x - (d.x0 + t * vx), y - (d.y0 + t * vy)) <= d.width / 2;
        } else if (d.triangle) in = point_in_tri(d.tri, x, y);
        else in = x >= d.x0 && x <= d.x1 && y >= d.y0 && y <= d.y1;
        if (!in) continue;
        any = true;
        if (d.kind == Damage::Hole) { hole = true; }
        else if (d.kind == Damage::Smudge) value = static_cast<float>((1 - d.strength) * value + d.strength * 0.3);
        else value = d.value[ch];
    }
    return any;
}

// Image-space occluder (finger near the lens): a soft-edged ellipse.
struct Finger { bool on = false; double cx, cy, rx, ry, angle, edge_px; float col[3]; };
inline float finger_alpha(const Finger &f, double x, double y) {
    if (!f.on) return 0;
    const double a = f.angle * kPiD / 180, dx = x - f.cx, dy = y - f.cy;
    const double u = (dx * std::cos(a) + dy * std::sin(a)) / f.rx, v = (-dx * std::sin(a) + dy * std::cos(a)) / f.ry;
    const double d = (std::sqrt(u * u + v * v) - 1) * std::min(f.rx, f.ry);  // approx signed px distance
    return static_cast<float>(std::clamp(0.5 - d / std::max(1.0, f.edge_px), 0.0, 1.0));
}

}  // namespace ckt
#endif
