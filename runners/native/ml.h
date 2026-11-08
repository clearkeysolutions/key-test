// ML localization option (SPEC §6 "ML option"), harness side.
//
// Task: from the production reader's own border candidate (ckey_find_borders_gray8),
// predict the true Profile 0 field corners. Each corner is described by a
// 24x24 patch sampled in the candidate's local edge frame (axes along the two
// adjacent border edges, extent +-12% of the side), contrast-normalised. The
// target is the true corner in the same local frame. A small MLP (weights from
// ml/train.py) predicts it; the decoder still accepts a payload only after
// RS + CRC, so the model can only ever propose geometry.
//
// Records (ml-export) are fixed-size binary so datasets stay compact and need
// no image storage: frames are regenerable from the case lines.
#ifndef CKT_ML_H
#define CKT_ML_H

#include "simulation/optics/camera.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace ckt {

inline constexpr int kPatch = 24;
inline constexpr double kPatchExtent = 0.12;  // fraction of the mean side
inline constexpr uint32_t kRecordMagic = 0x4C4D4B43;  // "CKML"

#pragma pack(push, 1)
struct MlRecord {
    uint32_t magic = kRecordMagic, version = 1;
    uint8_t case_hash[16]{};
    uint8_t cand_idx = 0, matched = 0, reader_ok = 0, gtq_ok = 0, layout = 0, p0_version = 0, stratum = 0, pad = 0;
    float side_px = 0, ppm = 0;
    uint8_t patch[4][kPatch][kPatch]{};
    float label[4][2]{};    // true corner in the local frame (NaN if the candidate is not the symbol)
    float refined[4][2]{};  // production refined quad corner in the local frame (NaN if refine failed)
};
#pragma pack(pop)
static_assert(sizeof(MlRecord) == 2408, "record layout");

struct LocalFrame { double ox, oy, ux, uy, vx, vy, side; };

inline LocalFrame corner_frame(const double q[8], int k) {
    const int n = (k + 1) % 4, p = (k + 3) % 4;
    double side = 0;
    for (int i = 0; i < 4; ++i) side += std::hypot(q[2 * ((i + 1) % 4)] - q[2 * i], q[2 * ((i + 1) % 4) + 1] - q[2 * i + 1]) / 4;
    LocalFrame f{q[2 * k], q[2 * k + 1], q[2 * n] - q[2 * k], q[2 * n + 1] - q[2 * k + 1], q[2 * p] - q[2 * k], q[2 * p + 1] - q[2 * k + 1], side};
    const double lu = std::hypot(f.ux, f.uy), lv = std::hypot(f.vx, f.vy);
    f.ux /= lu; f.uy /= lu; f.vx /= lv; f.vy /= lv;
    return f;
}

// Image point -> local (alpha, beta) in side units; and back.
inline bool to_local(const LocalFrame &f, double x, double y, float out[2]) {
    const double dx = (x - f.ox) / f.side, dy = (y - f.oy) / f.side, det = f.ux * f.vy - f.vx * f.uy;
    if (std::abs(det) < 1e-9) return false;
    out[0] = static_cast<float>((dx * f.vy - dy * f.vx) / det);
    out[1] = static_cast<float>((f.ux * dy - f.uy * dx) / det);
    return true;
}
inline void from_local(const LocalFrame &f, double a, double b, double &x, double &y) {
    x = f.ox + f.side * (a * f.ux + b * f.vx);
    y = f.oy + f.side * (a * f.uy + b * f.vy);
}

inline void sample_patch(const uint8_t *g, int w, int h, const LocalFrame &f, uint8_t out[kPatch][kPatch]) {
    for (int j = 0; j < kPatch; ++j) for (int i = 0; i < kPatch; ++i) {
        const double a = -kPatchExtent + 2 * kPatchExtent * (i + 0.5) / kPatch, b = -kPatchExtent + 2 * kPatchExtent * (j + 0.5) / kPatch;
        double x, y; from_local(f, a, b, x, y);
        // ckey pixel convention: pixel centres at integers.
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const double fx = x - x0, fy = y - y0;
        const auto at = [&](int xx, int yy) { return static_cast<double>(g[static_cast<size_t>(std::clamp(yy, 0, h - 1)) * w + std::clamp(xx, 0, w - 1)]); };
        const double v = (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
        out[j][i] = static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
    }
}

// Best cyclic correspondence of quad a's corners to quad b's (perm[k] = index in b).
inline void quad_perm(const double a[8], const double b[8], int perm[4]) {
    double best = 1e300;
    for (int w = 0; w < 2; ++w) for (int s = 0; s < 4; ++s) {
        double worst = 0; int pm[4];
        for (int i = 0; i < 4; ++i) { pm[i] = w == 0 ? (s + i) % 4 : (s - i + 4) % 4; worst = std::max(worst, std::hypot(a[2 * i] - b[2 * pm[i]], a[2 * i + 1] - b[2 * pm[i] + 1])); }
        if (worst < best) { best = worst; for (int i = 0; i < 4; ++i) perm[i] = pm[i]; }
    }
}

// Contrast normalisation shared bit-for-bit with ml/train.py (float32 arithmetic).
inline void normalise(const uint8_t p[kPatch][kPatch], float out[kPatch * kPatch]) {
    double mean = 0, sq = 0;
    for (int j = 0; j < kPatch; ++j) for (int i = 0; i < kPatch; ++i) { mean += p[j][i]; sq += static_cast<double>(p[j][i]) * p[j][i]; }
    const int n = kPatch * kPatch;
    mean /= n;
    const double sd = std::sqrt(std::max(1.0, sq / n - mean * mean));
    for (int j = 0; j < kPatch; ++j) for (int i = 0; i < kPatch; ++i) out[j * kPatch + i] = static_cast<float>((p[j][i] - mean) / sd);
}

// Dense ReLU MLP. File format (little endian): "CKTMLP1\0", uint32 layers, then per
// layer uint32 in, uint32 out, float32 W[out][in], float32 b[out]. Output is linear.
class Mlp {
public:
    bool load(const std::string &path, std::string &err) {
        std::ifstream in(path, std::ios::binary);
        char magic[8]{};
        if (!in.read(magic, 8) || std::memcmp(magic, "CKTMLP1", 7) != 0) { err = "bad model magic"; return false; }
        uint32_t layers = 0; in.read(reinterpret_cast<char *>(&layers), 4);
        if (!in || layers == 0 || layers > 16) { err = "bad layer count"; return false; }
        L_.resize(layers);
        for (auto &l : L_) {
            in.read(reinterpret_cast<char *>(&l.in), 4); in.read(reinterpret_cast<char *>(&l.out), 4);
            if (!in || l.in == 0 || l.out == 0 || l.in > 100000 || l.out > 100000) { err = "bad layer shape"; return false; }
            l.W.resize(static_cast<size_t>(l.in) * l.out); l.b.resize(l.out);
            in.read(reinterpret_cast<char *>(l.W.data()), static_cast<std::streamsize>(l.W.size() * 4));
            in.read(reinterpret_cast<char *>(l.b.data()), static_cast<std::streamsize>(l.b.size() * 4));
            if (!in) { err = "truncated model"; return false; }
        }
        if (L_.front().in != kPatch * kPatch || L_.back().out != 2) { err = "model must map 576 -> 2"; return false; }
        return true;
    }
    void predict(const float *x, float out[2]) const {
        std::vector<float> a(x, x + L_.front().in), b;
        for (size_t li = 0; li < L_.size(); ++li) {
            const auto &l = L_[li];
            b.assign(l.out, 0.f);
            for (uint32_t o = 0; o < l.out; ++o) {
                float s = l.b[o]; const float *w = &l.W[static_cast<size_t>(o) * l.in];
                for (uint32_t i = 0; i < l.in; ++i) s += w[i] * a[i];
                b[o] = li + 1 < L_.size() ? std::max(0.f, s) : s;
            }
            a.swap(b);
        }
        out[0] = a[0]; out[1] = a[1];
    }
    size_t parameters() const { size_t n = 0; for (const auto &l : L_) n += l.W.size() + l.b.size(); return n; }

private:
    struct Layer { uint32_t in = 0, out = 0; std::vector<float> W, b; };
    std::vector<Layer> L_;
};

}  // namespace ckt
#endif
