// Builds the symbol under test with the PRODUCTION encoder and renderer
// (ckey_encode_* / ckey_render_*_svg), and the hard negatives derived from
// it. Also hosts an independent Reed-Solomon encoder (GF(256), 0x11D,
// roots alpha^0..alpha^(np-1), systematic) used to (a) verify ckey's parity
// for Profiles 1 and 2 on every case that touches FEC, and (b) forge
// RS-consistent codewords with a broken CRC (crc_forge negatives).
#ifndef CKT_SYMBOL_FACTORY_H
#define CKT_SYMBOL_FACTORY_H

#include "ckey/ckey.h"
#include "simulation/case_spec.h"
#include "simulation/common/prng.h"

#include <algorithm>
#include <cmath>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ckt {

// ---- independent RS (oracle) ------------------------------------------------------
struct Gf256 {
    uint8_t exp[512], log[256];
    Gf256() {
        int x = 1;
        for (int i = 0; i < 255; ++i) { exp[i] = static_cast<uint8_t>(x); log[x] = static_cast<uint8_t>(i); x <<= 1; if (x & 0x100) x ^= 0x11D; }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
};
inline const Gf256 &gf() { static const Gf256 g; return g; }

inline std::vector<uint8_t> rs_parity(const std::vector<uint8_t> &data, int np) {
    const auto &G = gf();
    std::vector<uint8_t> gen{1};
    for (int i = 0; i < np; ++i) {
        std::vector<uint8_t> next(gen.size() + 1, 0);
        for (size_t j = 0; j < gen.size(); ++j) { next[j] ^= gen[j]; next[j + 1] ^= G.mul(gen[j], G.exp[i]); }
        gen = next;
    }
    std::vector<uint8_t> rem(data); rem.resize(data.size() + np, 0);
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t f = rem[i];
        if (f) for (size_t j = 0; j < gen.size(); ++j) rem[i + j] ^= G.mul(f, gen[j]);
    }
    return std::vector<uint8_t>(rem.end() - np, rem.end());
}

// ---- payloads -----------------------------------------------------------------------
inline std::vector<uint8_t> make_payload(const std::string &pattern, size_t n, uint64_t seed) {
    Rng r(seed, "payload");
    std::vector<uint8_t> p(n);
    if (pattern == "zeros") for (auto &b : p) b = 0;
    else if (pattern == "ones") for (auto &b : p) b = 0xFF;
    else if (pattern == "alt55") for (auto &b : p) b = 0x55;
    else if (pattern == "altAA") for (auto &b : p) b = 0xAA;
    else if (pattern == "incrementing") for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(i + r.below(1));
    else if (pattern == "single_bit") { for (auto &b : p) b = 0; if (n) p[r.below(static_cast<uint32_t>(n))] = static_cast<uint8_t>(1u << r.below(8)); }
    else if (pattern == "runs") { uint8_t v = static_cast<uint8_t>(r.next()); for (size_t i = 0; i < n; ++i) { if (r.unit() < 0.04) v = static_cast<uint8_t>(r.next()); p[i] = v; } }
    else if (pattern == "ascii") { static const char a[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,-_/:"; for (auto &b : p) b = static_cast<uint8_t>(a[r.below(sizeof a - 1)]); }
    else if (pattern == "url") {
        const std::string head = "https://ckey.example/";
        static const char a[] = "abcdefghijklmnopqrstuvwxyz0123456789-_";
        for (size_t i = 0; i < n; ++i) p[i] = i < head.size() ? static_cast<uint8_t>(head[i]) : static_cast<uint8_t>(a[r.below(sizeof a - 1)]);
    } else if (pattern == "utf8") {  // valid UTF-8 mixing 1-4 byte sequences, padded with ASCII to exactly n
        size_t i = 0;
        while (i < n) {
            const uint32_t k = r.below(4);
            uint32_t cp = k == 0 ? 0x20 + r.below(0x5F) : k == 1 ? 0x80 + r.below(0x780) : k == 2 ? 0x800 + r.below(0xD000 - 0x800) : 0x10000 + r.below(0x10000);
            uint8_t enc[4]; int len;
            if (cp < 0x80) { enc[0] = static_cast<uint8_t>(cp); len = 1; }
            else if (cp < 0x800) { enc[0] = static_cast<uint8_t>(0xC0 | cp >> 6); enc[1] = static_cast<uint8_t>(0x80 | (cp & 63)); len = 2; }
            else if (cp < 0x10000) { enc[0] = static_cast<uint8_t>(0xE0 | cp >> 12); enc[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 63)); enc[2] = static_cast<uint8_t>(0x80 | (cp & 63)); len = 3; }
            else { enc[0] = static_cast<uint8_t>(0xF0 | cp >> 18); enc[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 63)); enc[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 63)); enc[3] = static_cast<uint8_t>(0x80 | (cp & 63)); len = 4; }
            if (i + static_cast<size_t>(len) > n) { p[i++] = 'a'; continue; }
            for (int j = 0; j < len; ++j) p[i++] = enc[j];
        }
    } else for (auto &b : p) b = static_cast<uint8_t>(r.next() >> 56);  // random
    return p;
}

inline uint64_t parse_u64(const std::string &s) { return std::strtoull(s.c_str(), nullptr, 10); }

inline int profile_index(const std::string &p) { return p == "P0" ? 0 : p == "P1" ? 1 : p == "P2" ? 2 : p == "P3" ? 3 : -1; }

inline ckey_ecc_level level_of(const std::string &l) { return l == "L" ? CKEY_ECC_L : l == "Q" ? CKEY_ECC_Q : l == "H" ? CKEY_ECC_H : CKEY_ECC_M; }

// ---- the built symbol ------------------------------------------------------------------
struct SymbolArt {
    int profile = 1;
    std::vector<uint8_t> payload;                 // the true payload (A)
    std::vector<std::vector<uint8_t>> allowed;    // acceptable reader outputs
    bool must_accept = true;                      // DECODE / DECODE_ANY_OF
    std::string svg, svg_b;                       // svg_b: second half of a splice
    double splice = 0;                            // fraction of the canvas taken from svg_b (left->right)
    bool mirror = false, blank = false;
    double canvas_units = 31, field_units = 29, canvas_origin = 1;
    int modules = 29;                             // Cartesian modules across the field
    double min_mark_modules = 0.2;                // thinnest mark, in module pitches (informative)
    double ecc_fraction = 0.1;                    // correctable byte-error fraction (estimate)
    std::string format;                           // human-readable format descriptor
    std::string error;                            // non-empty: invalid generation
    int rs_verified = -1;                         // 1 harness RS == ckey parity, 0 mismatch, -1 not checked
    bool ckey_raster = false;                     // texture from ckey's svg_raster.h instead of the harness rasterizer (holdout split)
    bool has_p0 = false;
    ckey_scalable_symbol p0{};
    uint32_t p0_version = 0;
};

template <class Sym, class F> std::string render_with(const Sym &s, F fn) {
    size_t n = 0; fn(&s, 8u, nullptr, 0, &n);
    std::string out(n, '\0'); fn(&s, 8u, out.data(), n, &n);
    if (n && out[n - 1] == '\0') out.resize(n - 1);
    return out;
}

// A QR/Data-Matrix-like square symbol with three nested-square finders,
// drawn as an SVG on the same 31-unit canvas (foreign_grid negatives).
inline std::string foreign_grid_svg(Rng &r) {
    std::string s = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-1 -1 31 31\"><rect x=\"-1\" y=\"-1\" width=\"31\" height=\"31\" fill=\"#ffffff\"/><g fill=\"#000000\">";
    const int n = 25; const double m = 29.0 / n;
    auto rect = [&](double x, double y, double w, double h) { char b[160]; std::snprintf(b, sizeof b, "<rect x=\"%.4f\" y=\"%.4f\" width=\"%.4f\" height=\"%.4f\"/>", x, y, w, h); s += b; };
    auto finder = [&](int fx, int fy) {
        rect(fx * m, fy * m, 7 * m, m); rect(fx * m, (fy + 6) * m, 7 * m, m); rect(fx * m, fy * m, m, 7 * m); rect((fx + 6) * m, fy * m, m, 7 * m);
        rect((fx + 2) * m, (fy + 2) * m, 3 * m, 3 * m);
    };
    finder(0, 0); finder(n - 7, 0); finder(0, n - 7);
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const bool in_finder = (x < 8 && y < 8) || (x >= n - 8 && y < 8) || (x < 8 && y >= n - 8);
        if (!in_finder && r.unit() < 0.5) rect(x * m, y * m, m, m);
    }
    return s + "</g></svg>";
}

inline SymbolArt build_symbol(const CaseSpec &c, uint64_t salt = 0) {
    SymbolArt a;
    a.ckey_raster = c.rasterizer == "ckey";
    if (c.rasterizer != "harness" && c.rasterizer != "ckey") { a.error = "unknown rasterizer " + c.rasterizer; return a; }
    const std::string profile = salt && !c.second_profile.empty() ? c.second_profile : c.profile;
    a.profile = profile_index(profile);
    if (a.profile < 0) { a.error = "unknown profile " + profile; return a; }
    const uint64_t pseed = parse_u64(c.payload_seed) ^ (salt * 0x9E3779B97F4A7C15ull);
    Rng neg(parse_u64(c.seed) ^ salt, "negative");
    const std::string kind = salt ? "none" : c.neg_kind;
    ckey_diagnostics d{};
    size_t len = static_cast<size_t>(c.payload_len);
    if (a.profile == 1) len = CKEY_LOCATOR_PAYLOAD_BYTES;
    a.payload = make_payload(c.payload_pattern, len, pseed);

    // Encode with the production encoder; keep the codeword so negatives can corrupt it.
    std::vector<uint8_t> *cw = nullptr; size_t cw_len = 0;
    ckey_symbol p1{}; ckey_text_symbol p2{}; ckey_extended_symbol p3{};
    ckey_status st = CKEY_OK;
    if (a.profile == 0) {
        const ckey_scalable_layout layout = c.p0_layout == "print" ? CKEY_LAYOUT_PRINT : CKEY_LAYOUT_STANDARD;
        st = ckey_encode_scalable(a.payload.data(), a.payload.size(), static_cast<uint32_t>(c.p0_version), level_of(c.p0_level), layout, &a.p0, &d);
        a.has_p0 = st == CKEY_OK; a.p0_version = a.p0.version;
        a.modules = static_cast<int>(ckey_scalable_modules_across(a.p0.version));
        a.min_mark_modules = layout == CKEY_LAYOUT_PRINT ? 0.35 : 0.2;
        const double lv[4] = {0.07, 0.14, 0.22, 0.30};
        a.ecc_fraction = lv[level_of(c.p0_level)];
        a.format = "P0" + std::string(layout == CKEY_LAYOUT_PRINT ? ".1" : "") + " v" + std::to_string(a.p0.version) + " " + c.p0_level + " fmt" + std::to_string(layout == CKEY_LAYOUT_PRINT ? 28 : 27);
    } else if (a.profile == 1) {
        st = ckey_encode_locator(a.payload.data(), a.payload.size(), &p1, &d);
        a.modules = 29; a.ecc_fraction = 25.0 / 77; a.min_mark_modules = 0.3; a.format = "P1 fmt14 RS(77,26)";
    } else if (a.profile == 2) {
        st = ckey_encode_text(a.payload.data(), a.payload.size(), &p2, &d);
        a.modules = 58; a.ecc_fraction = 22.0 / 255; a.min_mark_modules = 0.44; a.format = "P2 fmt25 RS(255,210)";
    } else {
        st = ckey_encode_extended_tiered(a.payload.data(), a.payload.size(), static_cast<ckey_extended_tier>(c.p3_tier), &p3, &d);
        a.canvas_units = 124; a.field_units = 116; a.canvas_origin = 4; a.modules = 232; a.min_mark_modules = 0.56;
        const double tier = c.p3_tier ? c.p3_tier : 2048;
        const double k = std::ceil((tier + 10) / 10.0);
        a.ecc_fraction = (255 - k) / 2 / 255;
        a.format = "P3 fmt26 tier" + std::to_string(c.p3_tier);
    }
    if (st != CKEY_OK) { a.error = "encoder refused the case (status " + std::to_string(st) + "): " + d.message; return a; }

    uint8_t *code = a.profile == 0 ? a.p0.codeword : a.profile == 1 ? p1.codeword : a.profile == 2 ? p2.codeword : p3.codeword;
    cw_len = a.profile == 0 ? a.p0.codeword_length : a.profile == 1 ? 77 : a.profile == 2 ? 255 : CKEY_EXTENDED_CODEWORD_BYTES;
    (void)cw;

    // Independent RS check of the production parity (single-block profiles).
    if (a.profile == 1 || a.profile == 2) {
        const int k = a.profile == 1 ? 26 : 210;
        const std::vector<uint8_t> data(code, code + k), parity(code + k, code + cw_len);
        a.rs_verified = rs_parity(data, static_cast<int>(cw_len) - k) == parity ? 1 : 0;
    }

    a.allowed = {a.payload};
    a.must_accept = c.expected == "DECODE" || c.expected == "DECODE_ANY_OF";
    const auto expect = [&](const char *want) { if (c.expected != want) a.error = "neg_kind " + kind + " requires expected=" + want + ", got " + c.expected; };
    if (kind == "none") {
        if (c.expected != "DECODE" && c.expected != "DECODE_ANY_OF") a.error = "valid symbol requires expected DECODE or DECODE_ANY_OF";
    } else if (kind == "random_codeword") {
        for (size_t i = 0; i < cw_len; ++i) code[i] = static_cast<uint8_t>(neg.next() >> 56);
        a.allowed.clear(); expect("REJECT");
    } else if (kind == "heavy_corruption") {
        const double f = std::clamp(c.neg_param, 0.0, 1.0);
        for (size_t i = 0; i < cw_len; ++i) if (neg.unit() < f) code[i] ^= static_cast<uint8_t>(1 + neg.below(255));
        expect("NO_WRONG");
    } else if (kind == "t_plus_one") {
        if (a.profile != 1 && a.profile != 2) { a.error = "t_plus_one needs P1 or P2"; return a; }
        const int t = a.profile == 1 ? 25 : 22;
        std::vector<int> idx(cw_len); for (size_t i = 0; i < cw_len; ++i) idx[i] = static_cast<int>(i);
        for (int i = 0; i < t + 1; ++i) { const int j = i + static_cast<int>(neg.below(static_cast<uint32_t>(cw_len - i))); std::swap(idx[i], idx[j]); code[idx[i]] ^= static_cast<uint8_t>(1 + neg.below(255)); }
        expect("NO_WRONG");
    } else if (kind == "crc_forge") {
        if (a.profile != 1 && a.profile != 2) { a.error = "crc_forge needs P1 or P2"; return a; }
        if (a.rs_verified != 1) { a.error = "independent RS did not reproduce production parity; cannot forge"; return a; }
        const int k = a.profile == 1 ? 26 : 210;
        code[6] ^= static_cast<uint8_t>(1 + neg.below(255));  // first payload byte; CRC now stale
        const auto parity = rs_parity(std::vector<uint8_t>(code, code + k), static_cast<int>(cw_len) - k);
        for (size_t i = 0; i < parity.size(); ++i) code[k + i] = parity[i];
        expect("NO_WRONG");
    } else if (kind == "splice") {
        SymbolArt b = build_symbol([&] { CaseSpec cb = c; cb.rasterizer = c.rasterizer; cb.neg_kind = "none"; cb.expected = "DECODE"; return cb; }(), 0x51ull);
        if (!b.error.empty()) { a.error = "splice partner: " + b.error; return a; }
        a.svg_b = b.svg; a.splice = std::clamp(c.neg_param, 0.05, 0.95);
        a.allowed.push_back(b.payload);
        expect("NO_WRONG");
    } else if (kind == "mirror") {
        a.mirror = true; expect("NO_WRONG");
    } else if (kind == "foreign_grid") {
        a.svg = foreign_grid_svg(neg); a.allowed.clear(); a.format = "foreign QR-like grid"; expect("REJECT");
        return a;
    } else if (kind == "blank") {
        a.blank = true; a.allowed.clear(); a.format = "no symbol"; expect("REJECT");
        return a;
    } else { a.error = "unknown neg_kind " + kind; return a; }

    if (a.profile == 0) a.svg = render_with(a.p0, ckey_render_scalable_svg);
    else if (a.profile == 1) a.svg = render_with(p1, ckey_render_svg);
    else if (a.profile == 2) a.svg = render_with(p2, ckey_render_text_svg);
    else a.svg = render_with(p3, ckey_render_extended_svg);
    if (a.svg.size() < 64) a.error = "renderer produced no SVG";
    return a;
}

}  // namespace ckt
#endif
