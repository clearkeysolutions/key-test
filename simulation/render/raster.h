// Symbol rasterization: SVG text from the production renderer -> mark
// coverage in [0,1] over the whole SVG canvas (viewBox).
//
// Two implementations, for oracle independence (SPEC §5):
//  * reference: ckey's own svg_raster.h (the rasterizer ckey CI uses);
//  * independent: written for this harness from the SVG 1.1 spec only
//    (endpoint->centre arc conversion F.6.5, even-odd polygons, rounded-rect
//    SDF), with a different (rotated-grid) supersampling pattern.
// The selftest and the S01 cross-check compare them; Profile 0 is also
// compared with the production native raster (ckey_render_scalable_raster).
#ifndef CKT_RASTER_H
#define CKT_RASTER_H

#include "simulation/common/image.h"
#include "svg_raster.h"  // from the pinned ckey checkout (src/)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ckt {

inline Plane raster_reference(const std::string &svg, int n, int ss) {
    const auto parsed = svgtest::parse(svg);
    const auto px = svgtest::rasterize(parsed, n, n, ss, 0, 255);
    Plane p(n, n);
    for (size_t i = 0; i < px.size(); ++i) p.v[i] = px[i] / 255.f;
    return p;
}

namespace indep {

struct Xf { double sx = 1, sy = 1, tx = 0, ty = 0; void apply(double &x, double &y) const { x = sx * x + tx; y = sy * y + ty; } };

struct Seg { bool arc = false; double x0, y0, x1, y1; double cx = 0, cy = 0, r = 0, a0 = 0, da = 0; };

struct Shape {
    enum K { RectFill, RectStroke, Disc, Stroke, Poly } k;
    double x = 0, y = 0, w = 0, h = 0, rx = 0, hw = 0; bool round = false;
    std::vector<Seg> segs; std::vector<double> pts;
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool erase = false;
};

inline std::string attr(const std::string &tag, const char *name) {
    const std::string key = std::string(" ") + name + "=\"";
    const size_t p = tag.find(key);
    if (p == std::string::npos) return {};
    const size_t s = p + key.size(), e = tag.find('"', s);
    return tag.substr(s, e - s);
}

// SVG 1.1 F.6.5 for a circular arc (rx = ry, no rotation).
inline Seg arc_seg(double x1, double y1, double r, bool large, bool sweep, double x2, double y2) {
    Seg s; s.arc = true; s.x0 = x1; s.y0 = y1; s.x1 = x2; s.y1 = y2;
    const double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    const double d2 = dx * dx + dy * dy;
    if (r * r < d2) r = std::sqrt(d2);
    double coef = std::sqrt(std::max(0.0, (r * r - d2) / d2));
    if (large == sweep) coef = -coef;
    const double cxp = coef * dy, cyp = -coef * dx;
    s.cx = cxp + (x1 + x2) / 2; s.cy = cyp + (y1 + y2) / 2; s.r = r;
    const double t1 = std::atan2((y1 - s.cy) / r, (x1 - s.cx) / r), t2 = std::atan2((y2 - s.cy) / r, (x2 - s.cx) / r);
    double dt = t2 - t1;
    if (sweep && dt < 0) dt += 2 * M_PI;
    if (!sweep && dt > 0) dt -= 2 * M_PI;
    s.a0 = t1; s.da = dt;
    return s;
}

inline bool on_arc(const Seg &s, double ang) {
    // Is angle within the swept range starting at a0 and spanning da?
    double rel = ang - s.a0;
    if (s.da >= 0) { while (rel < 0) rel += 2 * M_PI; while (rel >= 2 * M_PI) rel -= 2 * M_PI; return rel <= s.da; }
    while (rel > 0) rel -= 2 * M_PI; while (rel <= -2 * M_PI) rel += 2 * M_PI; return rel >= s.da;
}

inline bool hit(const Shape &s, double x, double y) {
    switch (s.k) {
    case Shape::Disc: return (x - s.x) * (x - s.x) + (y - s.y) * (y - s.y) <= s.rx * s.rx;
    case Shape::RectFill: case Shape::RectStroke: {
        const double cx = s.x + s.w / 2, cy = s.y + s.h / 2, r = std::min(s.rx, std::min(s.w, s.h) / 2);
        const double qx = std::abs(x - cx) - (s.w / 2 - r), qy = std::abs(y - cy) - (s.h / 2 - r);
        const double sd = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
        return s.k == Shape::RectFill ? sd <= 0 : std::abs(sd) <= s.hw;
    }
    case Shape::Poly: {
        bool in = false; const size_t n = s.pts.size() / 2;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double xi = s.pts[2 * i], yi = s.pts[2 * i + 1], xj = s.pts[2 * j], yj = s.pts[2 * j + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
    }
    case Shape::Stroke:
        for (const auto &g : s.segs) {
            if (s.round && (std::hypot(x - g.x0, y - g.y0) <= s.hw || std::hypot(x - g.x1, y - g.y1) <= s.hw)) return true;
            if (g.arc) {
                if (std::abs(std::hypot(x - g.cx, y - g.cy) - g.r) <= s.hw && on_arc(g, std::atan2(y - g.cy, x - g.cx))) return true;
            } else {
                const double vx = g.x1 - g.x0, vy = g.y1 - g.y0, L2 = vx * vx + vy * vy;
                const double t = L2 > 0 ? ((x - g.x0) * vx + (y - g.y0) * vy) / L2 : 0;
                if (t >= 0 && t <= 1 && std::hypot(x - (g.x0 + t * vx), y - (g.y0 + t * vy)) <= s.hw) return true;
            }
        }
        return false;
    }
    return false;
}

struct Doc { double vx = 0, vy = 0, vw = 1, vh = 1; std::vector<Shape> shapes; };

inline Doc parse(const std::string &text) {
    Doc d;
    struct St { Xf xf; std::string fill, stroke, cap, cls; };
    std::vector<St> st(1);
    size_t i = 0;
    while (true) {
        const size_t a = text.find('<', i);
        if (a == std::string::npos) break;
        const size_t b = text.find('>', a);
        const std::string tag = " " + text.substr(a + 1, b - a - 1);
        i = b + 1;
        if (tag.compare(1, 4, "defs") == 0) { const size_t e = text.find("</defs>", i); i = e == std::string::npos ? text.size() : e + 7; continue; }
        if (tag.compare(1, 2, "/g") == 0) { if (st.size() > 1) st.pop_back(); continue; }
        if (tag[1] == '/' || tag[1] == '?' || tag[1] == '!') continue;
        size_t ne = 1; while (ne < tag.size() && tag[ne] != ' ' && tag[ne] != '/') ++ne;
        const std::string name = tag.substr(1, ne - 1);
        const bool closed = tag.size() > 1 && tag[tag.size() - 1] == '/';
        St cur = st.back();
        for (auto [key, dst] : {std::pair<const char *, std::string *>{"fill", &cur.fill}, {"stroke", &cur.stroke}, {"stroke-linecap", &cur.cap}, {"class", &cur.cls}}) {
            const std::string v = attr(tag, key); if (!v.empty()) *dst = v;
        }
        if (name == "svg") { std::sscanf(attr(tag, "viewBox").c_str(), "%lf %lf %lf %lf", &d.vx, &d.vy, &d.vw, &d.vh); continue; }
        if (name == "g") {
            const std::string t = attr(tag, "transform");
            double tx = 0, ty = 0, sx = 1, sy = 1;
            if (const size_t p = t.find("translate("); p != std::string::npos) std::sscanf(t.c_str() + p + 10, "%lf %lf", &tx, &ty);
            if (const size_t p = t.find("scale("); p != std::string::npos) std::sscanf(t.c_str() + p + 6, "%lf %lf", &sx, &sy);
            const Xf q = st.back().xf;
            cur.xf = {q.sx * sx, q.sy * sy, q.sx * tx + q.tx, q.sy * ty + q.ty};
            if (!closed) st.push_back(cur);
            continue;
        }
        if (cur.fill == "#050505") continue;
        Shape s; s.erase = cur.cls == "ckey-logo-plate";
        const auto num = [&](const char *k) { const std::string v = attr(tag, k); return v.empty() ? 0.0 : std::atof(v.c_str()); };
        if (name == "rect") {
            s.x = num("x"); s.y = num("y"); s.w = num("width"); s.h = num("height"); s.rx = num("rx");
            if (s.x == d.vx && s.w == d.vw) continue;  // full-canvas background
            if (cur.fill == "none") { s.k = Shape::RectStroke; s.hw = num("stroke-width") / 2; } else s.k = Shape::RectFill;
            // Transform into user space (axis-aligned scale/translate only).
            double x0 = s.x, y0 = s.y, x1 = s.x + s.w, y1 = s.y + s.h;
            cur.xf.apply(x0, y0); cur.xf.apply(x1, y1);
            const double k = std::abs(cur.xf.sx);
            s.x = std::min(x0, x1); s.y = std::min(y0, y1); s.w = std::abs(x1 - x0); s.h = std::abs(y1 - y0); s.rx *= k; s.hw *= k;
            s.bx0 = s.x - s.hw; s.by0 = s.y - s.hw; s.bx1 = s.x + s.w + s.hw; s.by1 = s.y + s.h + s.hw;
        } else if (name == "circle") {
            s.k = Shape::Disc; s.x = num("cx"); s.y = num("cy"); s.rx = num("r") * std::abs(cur.xf.sx); cur.xf.apply(s.x, s.y);
            s.bx0 = s.x - s.rx; s.by0 = s.y - s.rx; s.bx1 = s.x + s.rx; s.by1 = s.y + s.rx;
        } else if (name == "polygon") {
            s.k = Shape::Poly; std::string p = attr(tag, "points"); for (char &c : p) if (c == ',') c = ' ';
            std::istringstream in(p); double x, y; s.bx0 = s.by0 = 1e300; s.bx1 = s.by1 = -1e300;
            while (in >> x >> y) { cur.xf.apply(x, y); s.pts.push_back(x); s.pts.push_back(y); s.bx0 = std::min(s.bx0, x); s.bx1 = std::max(s.bx1, x); s.by0 = std::min(s.by0, y); s.by1 = std::max(s.by1, y); }
        } else if (name == "path") {
            s.k = Shape::Stroke; s.hw = num("stroke-width") / 2 * std::abs(cur.xf.sx); s.round = cur.cap == "round";
            std::istringstream in(attr(tag, "d")); std::string c; double px = 0, py = 0;
            while (in >> c) {
                if (c == "M") { in >> px >> py; }
                else if (c == "L") { double x, y; in >> x >> y; Seg g; g.x0 = px; g.y0 = py; g.x1 = x; g.y1 = y; s.segs.push_back(g); px = x; py = y; }
                else if (c == "A") { double r, ry, rot, x, y; int la, sw; in >> r >> ry >> rot >> la >> sw >> x >> y; s.segs.push_back(arc_seg(px, py, r, la, sw, x, y)); px = x; py = y; }
            }
            // Apply the transform to every segment (mirror scales flip arc direction).
            for (auto &g : s.segs) {
                cur.xf.apply(g.x0, g.y0); cur.xf.apply(g.x1, g.y1);
                if (g.arc) {
                    cur.xf.apply(g.cx, g.cy); g.r *= std::abs(cur.xf.sx);
                    g.a0 = std::atan2((g.y0 - g.cy), (g.x0 - g.cx));
                    if (cur.xf.sx * cur.xf.sy < 0) g.da = -g.da;
                }
            }
            s.bx0 = s.by0 = 1e300; s.bx1 = s.by1 = -1e300;
            for (const auto &g : s.segs) {
                const double ext = g.arc ? g.r + s.hw : s.hw;
                const double cx = g.arc ? g.cx : 0, cy = g.arc ? g.cy : 0;
                if (g.arc) { s.bx0 = std::min(s.bx0, cx - ext); s.bx1 = std::max(s.bx1, cx + ext); s.by0 = std::min(s.by0, cy - ext); s.by1 = std::max(s.by1, cy + ext); }
                for (auto [x, y] : {std::pair{g.x0, g.y0}, std::pair{g.x1, g.y1}}) { s.bx0 = std::min(s.bx0, x - ext); s.bx1 = std::max(s.bx1, x + ext); s.by0 = std::min(s.by0, y - ext); s.by1 = std::max(s.by1, y + ext); }
            }
        } else continue;
        d.shapes.push_back(std::move(s));
    }
    return d;
}

}  // namespace indep

inline Plane raster_independent(const std::string &svg, int n, int ss) {
    const auto doc = indep::parse(svg);
    const int S = ss * ss;
    std::vector<double> ox(S), oy(S);  // rotated-grid supersampling offsets
    const double ang = std::atan(0.5);
    for (int j = 0; j < ss; ++j) for (int i = 0; i < ss; ++i) {
        const double u = (i + 0.5) / ss - 0.5, v = (j + 0.5) / ss - 0.5;
        ox[j * ss + i] = 0.5 + u * std::cos(ang) - v * std::sin(ang);
        oy[j * ss + i] = 0.5 + u * std::sin(ang) + v * std::cos(ang);
    }
    std::vector<uint64_t> mask(static_cast<size_t>(n) * n, 0);
    const double kx = doc.vw / n, ky = doc.vh / n;
    for (const auto &s : doc.shapes) {
        const int x0 = std::max(0, static_cast<int>((s.bx0 - doc.vx) / kx) - 1), x1 = std::min(n - 1, static_cast<int>((s.bx1 - doc.vx) / kx) + 1);
        const int y0 = std::max(0, static_cast<int>((s.by0 - doc.vy) / ky) - 1), y1 = std::min(n - 1, static_cast<int>((s.by1 - doc.vy) / ky) + 1);
        for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
            uint64_t &m = mask[static_cast<size_t>(y) * n + x];
            for (int k = 0; k < S && k < 64; ++k) {
                const bool in = indep::hit(s, doc.vx + (x + ox[k]) * kx, doc.vy + (y + oy[k]) * ky);
                if (!in) continue;
                if (s.erase) m &= ~(1ull << k); else m |= 1ull << k;
            }
        }
    }
    Plane p(n, n);
    for (size_t i = 0; i < mask.size(); ++i) p.v[i] = static_cast<float>(__builtin_popcountll(mask[i])) / std::min(S, 64);
    return p;
}

// Agreement metric between two coverage rasters: fraction of pixels whose
// binarized value differs, ignoring pixels that are partial in either
// (edge pixels legitimately differ between antialiasing patterns).
inline double raster_disagreement(const Plane &a, const Plane &b) {
    size_t n = 0, bad = 0;
    for (size_t i = 0; i < a.v.size() && i < b.v.size(); ++i) {
        const bool ia = a.v[i] > 0.85f || a.v[i] < 0.15f, ib = b.v[i] > 0.85f || b.v[i] < 0.15f;
        if (!ia || !ib) continue;
        ++n; bad += (a.v[i] > 0.5f) != (b.v[i] > 0.5f);
    }
    return n ? static_cast<double>(bad) / n : 1.0;
}

}  // namespace ckt
#endif
