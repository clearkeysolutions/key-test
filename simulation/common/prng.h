// ckt-prng-v1: deterministic pseudorandom streams for the CKey test harness.
//
// Every random draw in the simulator comes from an Rng constructed from the
// case seed (a 64-bit value derived in Python by generator/seeds.py) and a
// named stream tag, so adding a new stream never perturbs existing ones.
// Generator: xoshiro256** seeded through SplitMix64. Changing anything in
// this file changes PRNG_VERSION and invalidates image-hash reproduction.
#ifndef CKT_PRNG_H
#define CKT_PRNG_H

#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ckt {

inline constexpr const char *PRNG_VERSION = "ckt-prng-v1(xoshiro256**/splitmix64/fnv1a-stream)";

inline uint64_t splitmix64(uint64_t &x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

class Rng {
public:
    explicit Rng(uint64_t seed, std::string_view stream = "") {
        uint64_t x = seed ^ fnv1a64(stream);
        for (auto &v : s_) v = splitmix64(x);
    }
    uint64_t next() {
        const uint64_t r = rotl(s_[1] * 5, 7) * 9, t = s_[1] << 17;
        s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3]; s_[2] ^= t; s_[3] = rotl(s_[3], 45);
        return r;
    }
    double unit() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }  // [0,1)
    double uniform(double a, double b) { return a + (b - a) * unit(); }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(((next() >> 32) * static_cast<uint64_t>(n)) >> 32); }
    double normal() {  // Box-Muller; no cached second value, so draws are position-independent
        double u1 = unit();
        if (u1 < 1e-300) u1 = 1e-300;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * unit());
    }
    // Fast standard normal via a 65,536-entry inverse-CDF table (Acklam's
    // approximation, |rel err| < 1.2e-9); tails are truncated at about 4.3 sigma,
    // which is immaterial for sensor noise. Used for per-pixel noise only.
    double fast_normal() { return table()[next() >> 48]; }
    // Poisson: exact (multiplication method) for small means, Gaussian for large.
    double poisson(double mean) {
        if (mean <= 0) return 0;
        if (mean > 40) { const double v = mean + std::sqrt(mean) * normal(); return v < 0 ? 0 : std::round(v); }
        const double L = std::exp(-mean);
        double p = 1; int k = 0;
        do { ++k; p *= unit(); } while (p > L);
        return k - 1;
    }

private:
    static double inv_norm_cdf(double p) {
        static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02, 1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
        static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02, 6.680131188771972e+01, -1.328068155288572e+01};
        static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00, -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
        static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
        const double lo = 0.02425;
        if (p < lo) { const double q = std::sqrt(-2 * std::log(p)); return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
        if (p > 1 - lo) { const double q = std::sqrt(-2 * std::log(1 - p)); return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1); }
        const double q = p - 0.5, r = q * q;
        return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    }
    static const double *table() {
        static const std::vector<double> t = [] { std::vector<double> v(65536); for (int i = 0; i < 65536; ++i) v[i] = inv_norm_cdf((i + 0.5) / 65536.0); return v; }();
        return t.data();
    }
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t s_[4];
};

}  // namespace ckt
#endif
