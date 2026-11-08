// Pinhole camera with radial distortion, a posed (optionally cylindrically
// bent) symbol surface, and time-dependent pose for motion blur and rolling
// shutter. Units: surface coordinates in millimetres centred on the symbol
// canvas; camera coordinates in millimetres, +z forward; image in pixels
// with pixel centres at integer + 0.5.
#ifndef CKT_CAMERA_H
#define CKT_CAMERA_H

#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace ckt {

inline constexpr double kPi = 3.14159265358979323846;
inline double deg2rad(double d) { return d * kPi / 180.0; }

struct Vec3 { double x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(double s, Vec3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Mat3 {
    double m[3][3]{};
    Vec3 operator*(Vec3 v) const { return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z}; }
    Vec3 tmul(Vec3 v) const { return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z, m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z}; }
};
inline Mat3 mul(const Mat3 &a, const Mat3 &b) { Mat3 r; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j]; return r; }
// R = Rz(roll) * Ry(yaw) * Rx(pitch), angles in degrees.
inline Mat3 rotation(double roll, double yaw, double pitch) {
    const double a = deg2rad(roll), b = deg2rad(yaw), c = deg2rad(pitch);
    Mat3 z{{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}};
    Mat3 y{{{std::cos(b), 0, std::sin(b)}, {0, 1, 0}, {-std::sin(b), 0, std::cos(b)}}};
    Mat3 x{{{1, 0, 0}, {0, std::cos(c), -std::sin(c)}, {0, std::sin(c), std::cos(c)}}};
    return mul(z, mul(y, x));
}

struct Intrinsics {
    int width = 2160, height = 3840;
    double f = 2360, cx = 1080, cy = 1920;
    double k1 = 0, k2 = 0;  // radial distortion on normalized coordinates (Brown model)
};

struct Pose {
    double roll = 0, yaw = 0, pitch = 0;  // degrees
    double tx = 0, ty = 0, tz = 300;       // mm, camera frame
};

struct Surface {
    double bend_radius = 0;  // mm; 0 = planar. Positive bulges toward the camera.
    double bend_axis = 0;    // degrees, direction of the bend's straight generator in surface coords
    double ca = 1, sa = 0;   // cached cos/sin of bend_axis (Camera::prepare)
};

class Camera {
public:
    Intrinsics K;
    Surface S;

    // Surface point (u,v) mm -> 3D point in the surface's local frame.
    void prepare() { S.ca = std::cos(deg2rad(S.bend_axis)); S.sa = std::sin(deg2rad(S.bend_axis)); }

    Vec3 surface_local(double u, double v) const {
        if (S.bend_radius == 0) return {u, v, 0};
        const double ca = S.ca, sa = S.sa;
        // along: the straight generator direction; across: the curved direction.
        // Positive radius: convex toward the camera (edges recede, +z).
        const double across = -u * sa + v * ca, along = u * ca + v * sa;
        const double R = S.bend_radius;
        const double ac = R * std::sin(across / R), z = R * (1 - std::cos(across / R));
        return {along * ca - ac * sa, along * sa + ac * ca, z};
    }

    // Forward: surface (u,v) under pose -> distorted pixel. Returns false behind camera.
    bool project(const Pose &p, double u, double v, double &px, double &py) const {
        const Mat3 R = rotation(p.roll, p.yaw, p.pitch);
        const Vec3 X = R * surface_local(u, v) + Vec3{p.tx, p.ty, p.tz};
        if (X.z <= 1e-6) return false;
        double xn = X.x / X.z, yn = X.y / X.z;
        distort(xn, yn);
        px = K.f * xn + K.cx; py = K.f * yn + K.cy;
        return true;
    }

    // Inverse: distorted pixel -> surface (u,v) under pose. Returns false if
    // the ray misses the (front of the) surface.
    bool unproject(const Pose &p, double px, double py, double &u, double &v) const {
        return unproject(rotation(p.roll, p.yaw, p.pitch), Vec3{p.tx, p.ty, p.tz}, px, py, u, v);
    }
    // `guess` (optional) warm-starts the bent-surface Newton solve, e.g. from a neighbouring pixel.
    bool unproject(const Mat3 &R, const Vec3 &T, double px, double py, double &u, double &v, const double *guess = nullptr) const {
        double xn = (px - K.cx) / K.f, yn = (py - K.cy) / K.f;
        undistort(xn, yn);
        const Vec3 ray{xn, yn, 1};
        const Vec3 n = R * Vec3{0, 0, 1};
        const double denom = dot(n, ray);
        if (std::abs(denom) < 1e-9) return false;
        const double t = dot(n, T) / denom;
        if (t <= 0) return false;
        const Vec3 local = R.tmul(t * ray - T);
        u = local.x; v = local.y;
        if (S.bend_radius == 0) return true;
        (void)guess;
        // Closed-form ray / cylinder intersection in the surface's local frame.
        // The bent surface is the cylinder of radius |R| whose axis passes through
        // (0,0,R) along the generator g; S(theta) - A = along*g + R sin(theta) n - R cos(theta) e_z.
        const double Rb = S.bend_radius;
        const Vec3 g{S.ca, S.sa, 0}, nrm{-S.sa, S.ca, 0};
        const Vec3 O = R.tmul(Vec3{-T.x, -T.y, -T.z}), D = R.tmul(ray);
        const Vec3 o = O - Vec3{0, 0, Rb};
        const Vec3 op = o - dot(o, g) * g, dp = D - dot(D, g) * g;
        const double qa = dot(dp, dp), qb = dot(op, dp), qc = dot(op, op) - Rb * Rb;
        const double disc = qb * qb - qa * qc;
        if (qa < 1e-18 || disc < 0) return false;
        const double sq = std::sqrt(disc);
        // Convex (R > 0): the near intersection; concave (R < 0): the far one.
        const double tt = Rb > 0 ? (-qb - sq) / qa : (-qb + sq) / qa;
        if (tt <= 0) return false;
        const Vec3 w = o + tt * D;
        const double along = dot(w, g);
        const Vec3 wp = w - along * g;
        const double theta = std::atan2(dot(wp, nrm) / Rb, -wp.z / Rb);
        const double across = Rb * theta;
        u = along * S.ca - across * S.sa;
        v = along * S.sa + across * S.ca;
        return true;
    }

    void distort(double &x, double &y) const {
        const double r2 = x * x + y * y, d = 1 + K.k1 * r2 + K.k2 * r2 * r2;
        x *= d; y *= d;
    }
    void undistort(double &x, double &y) const {
        if (K.k1 == 0 && K.k2 == 0) return;
        if (!grid_.empty()) {  // normalized -> pixel -> grid cell
            const double gx = (x * K.f + K.cx) / kGrid, gy = (y * K.f + K.cy) / kGrid;
            if (gx >= 0 && gy >= 0 && gx < gw_ - 1 && gy < gh_ - 1) {
                const int ix = static_cast<int>(gx), iy = static_cast<int>(gy); const double fx = gx - ix, fy = gy - iy;
                const auto &a = grid_[static_cast<size_t>(iy) * gw_ + ix], &b = grid_[static_cast<size_t>(iy) * gw_ + ix + 1];
                const auto &c = grid_[static_cast<size_t>(iy + 1) * gw_ + ix], &d = grid_[static_cast<size_t>(iy + 1) * gw_ + ix + 1];
                x = (a.first * (1 - fx) + b.first * fx) * (1 - fy) + (c.first * (1 - fx) + d.first * fx) * fy;
                y = (a.second * (1 - fx) + b.second * fx) * (1 - fy) + (c.second * (1 - fx) + d.second * fx) * fy;
                return;
            }
        }
        undistort_exact(x, y);
    }
    // Precompute the undistortion on a kGrid-pixel lattice (call after setting K).
    void build_undistort_grid() {
        grid_.clear();
        if (K.k1 == 0 && K.k2 == 0) return;
        gw_ = K.width / kGrid + 3; gh_ = K.height / kGrid + 3;
        grid_.resize(static_cast<size_t>(gw_) * gh_);
        for (int j = 0; j < gh_; ++j) for (int i = 0; i < gw_; ++i) {
            double x = (i * kGrid - K.cx) / K.f, y = (j * kGrid - K.cy) / K.f; undistort_exact(x, y);
            grid_[static_cast<size_t>(j) * gw_ + i] = {x, y};
        }
    }
    void undistort_exact(double &x, double &y) const {
        const double xd = x, yd = y;
        for (int i = 0; i < 8; ++i) { const double r2 = x * x + y * y, d = 1 + K.k1 * r2 + K.k2 * r2 * r2; x = xd / d; y = yd / d; }
    }

private:
    static constexpr int kGrid = 8;
    std::vector<std::pair<double, double>> grid_;
    int gw_ = 0, gh_ = 0;
    bool proj_norm(const Mat3 &R, const Vec3 &T, double u, double v, double &xn, double &yn) const {
        const Vec3 X = R * surface_local(u, v) + T;
        if (X.z <= 1e-6) return false;
        xn = X.x / X.z; yn = X.y / X.z; return true;
    }
};

// 4-point homography (src -> dst), 8 parameters, h33 = 1.
inline bool solve_homography(const double src[8], const double dst[8], double h[8]) {
    double a[8][9]{};
    for (int i = 0; i < 4; ++i) {
        const double x = src[2 * i], y = src[2 * i + 1], u = dst[2 * i], v = dst[2 * i + 1];
        const double r0[9] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u}, r1[9] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
        for (int j = 0; j < 9; ++j) { a[2 * i][j] = r0[j]; a[2 * i + 1][j] = r1[j]; }
    }
    for (int c = 0; c < 8; ++c) {
        int p = c; for (int r = c + 1; r < 8; ++r) if (std::abs(a[r][c]) > std::abs(a[p][c])) p = r;
        if (std::abs(a[p][c]) < 1e-12) return false;
        for (int j = 0; j < 9; ++j) std::swap(a[p][j], a[c][j]);
        for (int j = 8; j >= c; --j) a[c][j] /= a[c][c];
        for (int r = 0; r < 8; ++r) if (r != c) { const double f = a[r][c]; for (int j = c; j < 9; ++j) a[r][j] -= f * a[c][j]; }
    }
    for (int i = 0; i < 8; ++i) h[i] = a[i][8];
    return true;
}

// Max corner distance between two quads, minimised over the 8 cyclic
// correspondences (4 starting corners x 2 windings).
inline double quad_distance(const double a[8], const double b[8]) {
    double best = 1e300;
    for (int w = 0; w < 2; ++w) for (int s = 0; s < 4; ++s) {
        double worst = 0;
        for (int i = 0; i < 4; ++i) {
            const int j = w == 0 ? (s + i) % 4 : (s - i + 4) % 4;
            worst = std::max(worst, std::hypot(a[2 * i] - b[2 * j], a[2 * i + 1] - b[2 * j + 1]));
        }
        best = std::min(best, worst);
    }
    return best;
}

}  // namespace ckt
#endif
