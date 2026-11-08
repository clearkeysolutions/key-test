// Staged image formation (SPEC §5):
//   ideal vector symbol (production SVG) -> coverage raster
//   -> substrate (ideal paint | print | display) -> surface texture
//   -> 3D scene: posed/bent surface, sheet, background clutter, lighting,
//      damage, a second code, image-space finger
//   -> camera sampling with exposure-time integration (motion blur, rolling
//      shutter, rotation/zoom blur) and lens distortion
//   -> lens PSF (field-dependent Gaussian, defocus disc), CA, vignetting
//   -> sensor (AE, shot/read noise, PRNU, ADC, optional Bayer)
//   -> ISP (gain, denoise, tone, sharpen, luma, rescale, JPEG-like DCT)
//   -> 8-bit luma frame handed to the scanner.
// All radiometry before the tone curve is linear light. The "ideal" path
// (S01) skips the camera entirely and writes an exact raster.
#ifndef CKT_PIPELINE_H
#define CKT_PIPELINE_H

#include "runners/native/symbol_factory.h"
#include "simulation/case_spec.h"
#include "simulation/display/display.h"
#include "simulation/optics/camera.h"
#include "simulation/optics/psf.h"
#include "simulation/printer/printer.h"
#include "simulation/render/raster.h"
#include "simulation/scene/scene.h"
#include "simulation/sensor/sensor.h"

#include <chrono>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace ckt {

inline constexpr const char *SIM_VERSION = "ckt-sim-v1";
inline constexpr const char *RECOVERABILITY_VERSION = "recov-v1";

struct Phys {
    double ppm_min = 0, ppm_mean = 0;   // output-frame pixels per module (worst local direction / mean)
    double blur_sigma_px = 0;           // combined Gaussian-equivalent blur (output px)
    double mtf = 1;                     // modulation transfer at 0.5 cycles/module
    double dprime = 0;                  // module modulation / noise (detectability)
    double contrast = 0;                // nominal Michelson contrast of marks vs background
    double occluded = 0;                // field fraction covered by damage / occluders
    double byte_err_est = 0;            // byte-error fraction implied by occlusion + saturation
    double saturated = 0;               // field fraction clipped
    bool in_frame = true;
    std::string cls = "RECOVERABLE";    // RECOVERABLE | MARGINAL | UNRECOVERABLE
    std::string reason;
};

struct Frame {
    std::vector<uint8_t> gray;
    int w = 0, h = 0;
    double gt[8]{};             // field corners in output pixels: (0,0),(F,0),(F,F),(0,F) canonical
    bool gt_valid = false;
    double gt2[8]{};            // second code's field corners
    bool gt2_valid = false;
    Phys phys;
    double sim_ms = 0;
    double electrons_white = 0, digital_gain = 1;
    double texture_px = 0, dpi_effective = 0;
    double crosscheck = -1;     // renderer cross-check disagreement (S01 only)
};

// ---- helpers ------------------------------------------------------------------------
inline void rgb_lin(const std::vector<double> &rgb8, float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = srgb_to_linear(static_cast<float>(std::clamp(i < static_cast<int>(rgb8.size()) ? rgb8[i] : 0.0, 0.0, 255.0) / 255.0));
}
inline float lin_luma(const float c[3]) { return luma709(c[0], c[1], c[2]); }

struct Instance {
    const SymbolArt *art = nullptr;
    Image tex;                  // reflectance texture over the canvas (ideal/print)
    bool display = false;
    Screen screen;
    Pose pose;
    Camera cam;
    double canvas_mm = 30, sheet_half = 20;
    float sheet[3]{}, under[3]{};
    double bg_alpha = 1;
    std::vector<Damage> damage;
    Lighting light;
    bool mirror = false;
};

// Coverage over the canvas at n x n, with splice composition.
inline std::shared_ptr<const Plane> coverage_for(const SymbolArt &art, int n, int ss_override = 0) {
    if (art.blank) return std::make_shared<const Plane>(n, n, 0.f);
    // Coverage is cached per (symbol, size): video frames reuse the same texture.
    // Keyed by content (never by address: a later case's symbol may reuse it).
    struct Entry { uint64_t h; int n, ss; double splice; bool indep; std::shared_ptr<const Plane> p; };
    static std::vector<Entry> cache;
    const int ss = ss_override ? ss_override : n <= 1024 ? 3 : 2;
    const uint64_t h = fnv1a64(art.svg) ^ (fnv1a64(art.svg_b) * 0x9E3779B97F4A7C15ull);
    for (const auto &e : cache) if (e.h == h && e.n == n && e.ss == ss && e.splice == art.splice && e.indep == art.ckey_raster) return e.p;
    const auto rast = [&](const std::string &svg) { return art.ckey_raster ? raster_reference(svg, n, ss) : raster_independent(svg, n, ss); };
    Plane a = rast(art.svg);
    if (!art.svg_b.empty()) {
        const Plane b = rast(art.svg_b);
        const int split = static_cast<int>(std::lround(art.splice * n));
        for (int y = 0; y < n; ++y) for (int x = n - split; x < n; ++x) a.at(x, y) = b.at(x, y);
    }
    auto shared = std::make_shared<const Plane>(std::move(a));
    if (cache.size() >= 2) cache.erase(cache.begin());
    cache.push_back({h, n, ss, art.splice, art.ckey_raster, shared});
    return shared;
}

inline Image paint_texture(const Plane &cov, const CaseSpec &c, int channels, const float fg[3], const float bg[3], const float under[3], double canvas_mm) {
    Image t(channels, cov.w, cov.h);
    const double period = canvas_mm / 7;
    for (int ch = 0; ch < channels; ++ch) {
        const int i = channels == 3 ? ch : 1;
        const float f = channels == 3 ? fg[i] : lin_luma(fg), b0 = channels == 3 ? bg[i] : lin_luma(bg), u0 = channels == 3 ? under[i] : lin_luma(under);
        const bool opaque = c.bg_alpha >= 1;
        const float fs = linear_to_srgb(f), bs_opaque = linear_to_srgb(b0);
        Plane &out = t.c[ch];
        for (int y = 0; y < cov.h; ++y) for (int x = 0; x < cov.w; ++x) {
            const float k = cov.at(x, y);
            float bs = bs_opaque, b = b0;
            if (!opaque) {  // transparent background over a striped (wood/fabric-like) surface
                const float pat = static_cast<float>(1 + 0.25 * std::sin(2 * kPiD * (x + 0.3 * y) / cov.w * canvas_mm / period));
                b = static_cast<float>(c.bg_alpha * b0 + (1 - c.bg_alpha) * u0 * pat);
                bs = linear_to_srgb(b);
            }
            out.at(x, y) = c.mix_srgb ? srgb_decode_fast(k * fs + (1 - k) * bs) : k * f + (1 - k) * b;
        }
    }
    return t;
}

// Surface damage generated to cover roughly `frac` of the field (measured afterwards).
inline std::vector<Damage> make_damage(const CaseSpec &c, const SymbolArt &art, double canvas_mm, Rng &r, Finger &finger) {
    std::vector<Damage> out;
    const double mm_per_unit = canvas_mm / art.canvas_units, f0 = art.canvas_origin * mm_per_unit, F = art.field_units * mm_per_unit;
    const double frac = std::clamp(c.occl_frac, 0.0, 0.95);
    const int count = std::max(1, c.occl_count);
    if (c.occl_kind == "none" || frac <= 0) return out;
    if (c.occl_kind == "scratch") {
        const double total_len_est = count * F * 1.1;
        const double width = std::max(0.02, frac * F * F / total_len_est);
        for (int i = 0; i < count; ++i) {
            Damage d; d.kind = Damage::Scratch; d.width = width;
            const double a = r.uniform(0, kPiD), cx = f0 + r.uniform(0.2, 0.8) * F, cy = f0 + r.uniform(0.2, 0.8) * F;
            d.x0 = cx - 0.75 * F * std::cos(a); d.y0 = cy - 0.75 * F * std::sin(a); d.x1 = cx + 0.75 * F * std::cos(a); d.y1 = cy + 0.75 * F * std::sin(a);
            const float v = r.unit() < 0.5 ? 0.9f : 0.05f; d.value[0] = d.value[1] = d.value[2] = v;
            out.push_back(d);
        }
    } else if (c.occl_kind == "tear" || c.occl_kind == "missing_corner") {
        // A right triangle with legs L covers L^2/2 of the field.
        const double L = std::min(1.4, std::sqrt(2 * frac)) * F;
        const int corner = static_cast<int>(r.below(4));
        const double cx = corner & 1 ? f0 + F : f0, cy = corner & 2 ? f0 + F : f0, sx = corner & 1 ? -1 : 1, sy = corner & 2 ? -1 : 1;
        Damage d; d.kind = Damage::Hole; d.triangle = true;
        if (c.occl_kind == "tear") {  // jagged: tilt the hypotenuse
            const double j = r.uniform(0.7, 1.3);
            d.tri[0] = cx - sx * 0.02 * F; d.tri[1] = cy - sy * 0.02 * F; d.tri[2] = cx + sx * L * j; d.tri[3] = cy; d.tri[4] = cx; d.tri[5] = cy + sy * L / j;
        } else { d.tri[0] = cx - sx * 0.02 * F; d.tri[1] = cy - sy * 0.02 * F; d.tri[2] = cx + sx * L; d.tri[3] = cy; d.tri[4] = cx; d.tri[5] = cy + sy * L; }
        out.push_back(d);
    } else if (c.occl_kind == "sticker" || c.occl_kind == "smudge") {
        for (int i = 0; i < count; ++i) {
            const double area = frac * F * F / count, aspect = r.uniform(0.5, 2.0), w = std::sqrt(area * aspect), h = area / w;
            Damage d; d.kind = c.occl_kind == "sticker" ? Damage::Sticker : Damage::Smudge; d.strength = r.uniform(0.5, 0.9);
            d.x0 = f0 + r.uniform(0, std::max(0.0, F - w)); d.y0 = f0 + r.uniform(0, std::max(0.0, F - h)); d.x1 = d.x0 + w; d.y1 = d.y0 + h;
            for (auto &v : d.value) v = static_cast<float>(r.uniform(0.05, 0.9));
            out.push_back(d);
        }
    } else if (c.occl_kind == "finder_cover") {
        // Cover one corner finder (index from neg/occl seed; 0 = the split top-left orientation finder).
        const int k = static_cast<int>(r.below(4));
        const double fc = (art.profile == 3 ? 16.0 : 4.0) * mm_per_unit;  // finder centre offset from field origin
        const double cx = f0 + (k & 1 ? F - fc : fc), cy = f0 + (k & 2 ? F - fc : fc);
        const double side = std::sqrt(frac) * F;
        Damage d; d.kind = Damage::Sticker; d.x0 = cx - side / 2; d.x1 = cx + side / 2; d.y0 = cy - side / 2; d.y1 = cy + side / 2;
        for (auto &v : d.value) v = static_cast<float>(r.uniform(0.1, 0.8));
        out.push_back(d);
    } else if (c.occl_kind == "finger") {
        finger.on = true;  // placed in image space by the pipeline once the symbol's projection is known
    }
    return out;
}

// ---- the simulator ------------------------------------------------------------------
class Simulator {
public:
    Frame run(const CaseSpec &c, const SymbolArt &art, const SymbolArt *second, int frame_index, bool crosscheck) {
        const auto t0 = std::chrono::steady_clock::now();
        Frame out = c.ideal ? ideal(c, art) : physical(c, art, second, frame_index);
        if (crosscheck && !art.blank) out.crosscheck = renderer_crosscheck(art);
        out.sim_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return out;
    }

    // Disagreement between the reference and independent rasterizers (and,
    // for Profile 0, the production native raster) on the clean symbol.
    static double renderer_crosscheck(const SymbolArt &art) {
        const int n = art.profile == 3 ? 768 : 384;
        const Plane a = raster_reference(art.svg, n, 3), b = raster_independent(art.svg, n, 3);
        double worst = raster_disagreement(a, b);
        if (art.has_p0 && art.svg_b.empty()) {
            ckey_raster_options o; ckey_raster_options_default(&o);
            o.size_px = n; o.format = CKEY_PIXELS_GRAY8; o.materials = 0; o.policy = CKEY_RASTER_COVERAGE;
            std::vector<uint8_t> px(static_cast<size_t>(n) * n); size_t req = 0;
            if (ckey_render_scalable_raster(&art.p0, nullptr, &o, px.data(), px.size(), &req) == CKEY_OK) {
                // Native raster is styled (dark background, light ink by default): normalize to coverage.
                uint8_t lo = 255, hi = 0; for (auto v : px) { lo = std::min(lo, v); hi = std::max(hi, v); }
                Plane c(n, n);
                const bool ink_light = true;
                for (size_t i = 0; i < px.size(); ++i) { float k = hi > lo ? float(px[i] - lo) / float(hi - lo) : 0.f; c.v[i] = ink_light ? k : 1 - k; }
                // Polarity is style-dependent: take the better of both readings.
                Plane ci = c; for (auto &v : ci.v) v = 1 - v;
                worst = std::max(worst, std::min(raster_disagreement(a, c), raster_disagreement(a, ci)));
            }
        }
        return worst;
    }

private:
    // Field corners in canvas mm (top-left origin).
    static void field_corners_mm(const SymbolArt &art, double canvas_mm, double out[8]) {
        const double k = canvas_mm / art.canvas_units, o = art.canvas_origin * k, F = art.field_units * k;
        const double c[8] = {o, o, o + F, o, o + F, o + F, o, o + F};
        for (int i = 0; i < 8; ++i) out[i] = c[i];
    }

    Frame ideal(const CaseSpec &c, const SymbolArt &art) {
        Frame f; f.w = c.frame_w; f.h = c.frame_h;
        const double ppm = c.ideal_ppm > 0 ? c.ideal_ppm : 8;
        const int n = std::max(16, static_cast<int>(std::lround(ppm * art.modules * art.canvas_units / art.field_units)));
        const Plane &cov = *coverage_for(art, n);
        const int rot = ((static_cast<int>(std::lround(c.roll / 90.0)) % 4) + 4) % 4;
        float fg[3], bg[3], sh[3]; rgb_lin(c.fg_rgb, fg); rgb_lin(c.bg_rgb, bg); rgb_lin(c.sheet_rgb, sh);
        const float fgs = linear_to_srgb(lin_luma(fg)), bgs = linear_to_srgb(lin_luma(bg)), shs = linear_to_srgb(lin_luma(sh));
        f.gray.assign(static_cast<size_t>(f.w) * f.h, static_cast<uint8_t>(std::lround(255 * shs)));
        const int ox = static_cast<int>(std::lround(c.center_x * f.w - n / 2.0)), oy = static_cast<int>(std::lround(c.center_y * f.h - n / 2.0));
        for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
            int sx = x, sy = y;  // rotate by rot * 90 degrees (clockwise in image coordinates)
            if (rot == 1) { sx = y; sy = n - 1 - x; } else if (rot == 2) { sx = n - 1 - x; sy = n - 1 - y; } else if (rot == 3) { sx = n - 1 - y; sy = x; }
            if (art.mirror) sx = n - 1 - sx;
            const int X = ox + x, Y = oy + y;
            if (X < 0 || Y < 0 || X >= f.w || Y >= f.h) continue;
            const float k = cov.at(sx, sy);
            f.gray[static_cast<size_t>(Y) * f.w + X] = static_cast<uint8_t>(std::lround(255 * std::clamp(k * fgs + (1 - k) * bgs, 0.f, 1.f)));
        }
        // Ground truth: field corners rotated with the raster.
        const double s = static_cast<double>(n) / art.canvas_units, o = art.canvas_origin * s, F = art.field_units * s;
        double q[8] = {o, o, o + F, o, o + F, o + F, o, o + F};
        for (int i = 0; i < 4; ++i) {
            double x = q[2 * i], y = q[2 * i + 1];
            if (art.mirror) x = n - x;
            double rx = x, ry = y;  // inverse of the sampling map above, in continuous coordinates
            if (rot == 1) { rx = n - y; ry = x; } else if (rot == 2) { rx = n - x; ry = n - y; } else if (rot == 3) { rx = y; ry = n - x; }
            // ckey pixel convention: pixel (i,j)'s centre is at (i,j) (core_internal.inc gray_sample).
            f.gt[2 * i] = ox + rx - 0.5; f.gt[2 * i + 1] = oy + ry - 0.5;
        }
        f.gt_valid = !art.blank && art.allowed.size() > 0 && art.svg.find("foreign") == std::string::npos;
        f.phys.ppm_min = f.phys.ppm_mean = ppm; f.phys.dprime = 1e9; f.phys.mtf = 1;
        f.phys.contrast = std::abs(lin_luma(fg) - lin_luma(bg)) / std::max(1e-6f, lin_luma(fg) + lin_luma(bg));
        f.phys.in_frame = ox >= 0 && oy >= 0 && ox + n <= f.w && oy + n <= f.h;
        classify(f.phys, art);
        f.texture_px = n;
        return f;
    }

    Frame physical(const CaseSpec &c0, const SymbolArt &art, const SymbolArt *second, int fi) {
        static const bool timing = std::getenv("CKT_STAGE_TIMING") != nullptr;
        auto tp = std::chrono::steady_clock::now();
        const auto lap = [&](const char *what) {
            if (!timing) return;
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[stage] %-12s %8.1f ms\n", what, std::chrono::duration<double, std::milli>(now - tp).count()); tp = now;
        };
        CaseSpec c = c0;
        Rng seq(parse_u64(c.seed), "sequence" + std::to_string(fi));
        if (c.frames > 1) perturb_frame(c, seq, fi);
        const uint64_t seed = parse_u64(c.seed) ^ (static_cast<uint64_t>(fi) * 0x632BE59BD9B4E019ull);
        Rng rbg(seed, "background"), rlight(seed, "lighting"), rdmg(seed, "damage"), rsens(seed, "sensor"), rtex(seed, "texture"), rsec(seed, "second");
        const int W = c.frame_w, H = c.frame_h;
        const bool rgb = c.color_mode == "rgb" || c.substrate == "display" || c.bayer;
        const int C = rgb ? 3 : 1;
        Frame f;

        SceneModel sm; sm.clutter = c.clutter; sm.distractors = c.distractors; sm.bg_level = c.bg_level;
        Image rad = make_background(C, W, H, sm, rbg);
        lap("background");

        Instance primary = make_instance(c, art, rtex, rlight, rdmg, f, false, c0.case_id);
        Finger finger;
        primary.damage = make_damage(c, art, primary.canvas_mm, rdmg, finger);
        lap("texture");
        Instance sec;
        if (second) {
            CaseSpec cs = c;
            // Place the second code beside the first, never overlapping it.
            cs.center_x = c.center_x < 0.5 ? std::min(0.95, c.center_x + 0.45) : std::max(0.05, c.center_x - 0.45);
            cs.center_y = std::clamp(c.center_y + rsec.uniform(-0.15, 0.15), 0.1, 0.9);
            cs.roll = c.roll + rsec.uniform(-40, 40); cs.occl_kind = "none"; cs.glare_spots = 0;
            sec = make_instance(cs, *second, rtex, rlight, rdmg, f, true);
            draw_instance(rad, sec, c, rsens);
        }
        const Rect roi = draw_instance(rad, primary, c, rsens);
        lap("sample");
        exposure_blur(rad, roi, primary, c);
        lap("exp_blur");

        // Ground truth and finger placement need the projected field.
        double fq[8]; field_corners_mm(art, primary.canvas_mm, fq);
        f.gt_valid = project_quad(primary, fq, f.gt);
        if (second) { double q2[8]; field_corners_mm(*second, sec.canvas_mm, q2); f.gt2_valid = project_quad(sec, q2, f.gt2); }
        if (finger.on && f.gt_valid) place_finger(finger, c, f.gt, rdmg, W, H);
        if (finger.on) for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float a = finger_alpha(finger, x + .5, y + .5);
            if (a <= 0) continue;
            for (int ch = 0; ch < C; ++ch) rad.c[ch].at(x, y) = (1 - a) * rad.c[ch].at(x, y) + a * finger.col[ch] * static_cast<float>(c.illum > 0 ? 1 : 1);
        }

        // Lens: field-dependent Gaussian (residual + defocus), defocus disc, CA, vignetting.
        Rect broi = roi; const int margin = static_cast<int>(std::ceil(3 * c.defocus_disc_px + 8));
        broi.x0 -= margin; broi.y0 -= margin; broi.x1 += margin; broi.y1 += margin;
        // Field curvature: the residual lens blur is evaluated at the symbol's field
        // position (normalized r^2 of the ROI centre) and applied frame-wide.
        const double rcx = (roi.x0 + roi.x1) / 2.0 - W / 2.0, rcy = (roi.y0 + roi.y1) / 2.0 - H / 2.0;
        const double rr2 = std::min(1.0, (rcx * rcx + rcy * rcy) / (W * W / 4.0 + H * H / 4.0));
        const double lens = c.lens_sigma_px + (c.lens_sigma_edge_px - c.lens_sigma_px) * rr2;
        const double small_motion = c.motion_px < 1.0 ? c.motion_px * c.motion_px / 12 : 0;  // uniform-motion variance
        const double sg = std::sqrt(lens * lens + c.defocus_gauss_px * c.defocus_gauss_px + small_motion);
        for (auto &p : rad.c) { gaussian_blur(p, sg); if (c.defocus_disc_px > 0.3) disc_blur(p, c.defocus_disc_px, broi); }
        if (C == 3 && c.ca_px > 0) chromatic(rad, c.ca_px);
        if (c.vignette > 0) {
            const double cx = W / 2.0, cy = H / 2.0, r2 = cx * cx + cy * cy;
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                const float v = static_cast<float>(1 - c.vignette * ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r2);
                for (auto &p : rad.c) p.at(x, y) *= v;
            }
        }

        lap("lens");
        // Sensor + ISP.
        SensorModel sens; sens.full_well_e = c.full_well_e; sens.read_noise_e = c.read_noise_e; sens.prnu = c.prnu; sens.adc_bits = c.adc_bits;
        sens.max_signal_e = c.max_signal_e; sens.illum = c.illum; sens.ae_bias = c.ae_bias; sens.bayer = c.bayer != 0; sens.device_seed = parse_u64(c.device_seed);
        SensorStats st;
        // Centre-weighted metering over the middle half of the frame (continuous AE).
        Image raw = sensor_capture(rad, sens, rsens, Rect{W / 4, H / 4, 3 * W / 4, 3 * H / 4}, st);
        IspModel isp; isp.tone = c.tone; isp.denoise = c.denoise; isp.sharpen = c.sharpen; isp.sharpen_sigma = c.sharpen_sigma;
        isp.jpeg_quality = c.jpeg_q; isp.rescale = c.rescale; isp.video_range = c.video_range != 0;
        f.gray = isp_to_luma8(std::move(raw), isp, st.digital_gain, f.w, f.h);
        f.electrons_white = st.electrons_white; f.digital_gain = st.digital_gain;
        lap("sensor+isp");
        const double rs = static_cast<double>(f.w) / W;
        // Continuous image coordinates -> output frame -> ckey's pixel-centre convention.
        for (double &v : f.gt) v = v * rs - 0.5;
        for (double &v : f.gt2) v = v * rs - 0.5;

        physics(f, c, art, primary, finger, st, rs);
        lap("physics");
        return f;
    }

    static void perturb_frame(CaseSpec &c, Rng &r, int fi) {
        // Hand-held sequence: shake (translation, roll), motion, focus hunting, AE drift.
        c.center_x += c.shake_px * r.normal() / c.frame_w;
        c.center_y += c.shake_px * r.normal() / c.frame_h;
        c.roll += c.shake_roll * r.normal();
        c.motion_px = std::abs(c.motion_px + c.frame_motion_px * r.normal());
        c.motion_angle += 30 * r.normal();
        const double focus = 1 + c.focus_drift * std::sin(0.9 * fi + r.uniform(0, 0.5));
        c.defocus_gauss_px = std::max(0.0, c.defocus_gauss_px * focus + c.focus_drift * std::abs(r.normal()));
        c.illum *= std::exp(c.exposure_drift * r.normal());
    }

    // The surface texture is invariant across a sequence's frames (only pose,
    // focus, exposure and motion change), so it is built once per case.
    std::string tex_key_;
    Image tex_cache_; Screen screen_cache_; double tex_px_cache_ = 0, dpi_cache_ = 0;

    Instance make_instance(const CaseSpec &c, const SymbolArt &art, Rng &rtex, Rng &rl, Rng &rd, Frame &f, bool is_second, const std::string &cache_id = "") {
        (void)rd;
        Instance in; in.art = &art; in.mirror = art.mirror;
        in.canvas_mm = c.symbol_mm; in.sheet_half = c.symbol_mm * (0.5 + std::max(0.0, c.sheet_margin));
        rgb_lin(c.sheet_rgb, in.sheet); rgb_lin(c.sheet_rgb, in.under); in.bg_alpha = c.bg_alpha;
        in.cam.K.width = c.frame_w; in.cam.K.height = c.frame_h; in.cam.K.f = c.focal_px; in.cam.K.cx = c.frame_w / 2.0; in.cam.K.cy = c.frame_h / 2.0;
        in.cam.K.k1 = c.k1; in.cam.K.k2 = c.k2; in.cam.S.bend_radius = c.bend_radius_mm; in.cam.S.bend_axis = c.bend_axis;
        in.cam.prepare();
        in.cam.build_undistort_grid();
        in.pose.roll = c.roll; in.pose.yaw = c.yaw; in.pose.pitch = c.pitch; in.pose.tz = c.distance_mm;
        in.pose.tx = c.distance_mm * (c.center_x * c.frame_w - in.cam.K.cx) / c.focal_px;
        in.pose.ty = c.distance_mm * (c.center_y * c.frame_h - in.cam.K.cy) / c.focal_px;
        float fg[3], bg[3]; rgb_lin(c.fg_rgb, fg); rgb_lin(c.bg_rgb, bg);
        const bool rgb = c.color_mode == "rgb" || c.substrate == "display" || c.bayer;
        const int C = rgb ? 3 : 1;
        const double cam_px = c.focal_px * c.symbol_mm / std::max(1.0, c.distance_mm);  // frontal canvas size in camera px
        const std::string key = cache_id.empty() || is_second ? "" : cache_id + "|" + std::to_string(fnv1a64(art.svg)) + "|" + c.substrate;
        if (!key.empty() && key == tex_key_) {
            in.display = c.substrate == "display";
            if (in.display) in.screen = screen_cache_; else in.tex = tex_cache_;
            f.texture_px = tex_px_cache_; f.dpi_effective = dpi_cache_;
        } else if (c.substrate == "display") {
            in.display = true;
            DisplayModel dm; dm.ppi = c.disp_ppi; dm.fill = c.disp_fill; dm.layout = c.disp_layout; dm.brightness = c.disp_brightness;
            dm.glass_reflect = c.disp_glass; dm.refresh_amp = c.disp_refresh_amp; dm.refresh_period_rows = c.disp_refresh_period; dm.refresh_phase = rl.unit();
            const int sp = std::clamp(static_cast<int>(std::lround(c.symbol_mm / 25.4 * c.disp_ppi)), 16, 4096);
            const auto covp = coverage_for(art, sp); const Plane &cov = *covp;
            // Screen pixel values: the display's own renderer mixes in sRGB, then the panel emits linear light.
            in.screen.px = Image(3, sp, sp);
            for (int y = 0; y < sp; ++y) for (int x = 0; x < sp; ++x) for (int ch = 0; ch < 3; ++ch) {
                const float k = cov.at(x, y), v = k * linear_to_srgb(fg[ch]) + (1 - k) * linear_to_srgb(bg[ch]);
                in.screen.px.c[ch].at(x, y) = srgb_to_linear(std::round(v * 255) / 255);
            }
            in.screen.pitch_mm = c.symbol_mm / sp; in.screen.canvas_mm = c.symbol_mm; in.screen.m = dm;
            if (!is_second) { f.texture_px = sp; f.dpi_effective = c.disp_ppi; }
        } else if (c.substrate == "print") {
            PrintModel pm; pm.dpi = c.print_dpi; pm.dot_gain_mm = c.dot_gain_mm; pm.paper_blur_mm = c.paper_blur_mm;
            pm.halftone_lpi = c.halftone_lpi; pm.ink_density = c.ink_density; pm.texture = c.paper_texture;
            const bool marks_dark = lin_luma(fg) < lin_luma(bg);
            pm.marks_are_ink = marks_dark;
            const float *ink = marks_dark ? fg : bg, *paper = marks_dark ? bg : fg;
            for (int i = 0; i < 3; ++i) { pm.ink[i] = ink[i]; pm.paper[i] = paper[i]; }
            const int dev = std::clamp(static_cast<int>(std::lround(c.symbol_mm / 25.4 * c.print_dpi)), 32, 4096);
            const int n = std::clamp(std::max(dev, static_cast<int>(2 * cam_px)), 64, 4096);
            // Binary device dots at the printer's own resolution, replicated to the texture grid.
            const auto devp = coverage_for(art, dev, 2); const Plane &devcov = *devp;  // thresholded to binary dots: 2x2 suffices
            Plane cov(n, n);
            for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) cov.at(x, y) = devcov.at(std::min(dev - 1, x * dev / n), std::min(dev - 1, y * dev / n)) >= 0.5f ? 1.f : 0.f;
            double dpi_eff = 0;
            in.tex = print_texture(cov, c.symbol_mm, pm, C, rtex, dpi_eff);
            if (!is_second) { f.texture_px = n; f.dpi_effective = std::min(c.print_dpi, dev / (c.symbol_mm / 25.4)); }
        } else {
            // 2 texels per camera pixel: each camera pixel takes 2x2 bilinear samples of it.
            const int n = std::clamp(static_cast<int>(std::ceil(2 * cam_px)), 256, 4096);
            const auto covp = coverage_for(art, n);
            in.tex = paint_texture(*covp, c, C, fg, bg, in.under, c.symbol_mm);
            if (!is_second) f.texture_px = n;
        }
        if (!key.empty() && key != tex_key_) {
            tex_key_ = key; tex_cache_ = in.tex; screen_cache_ = in.screen; tex_px_cache_ = f.texture_px; dpi_cache_ = f.dpi_effective;
        }
        // Lighting in surface coordinates.
        in.light.half_mm = c.symbol_mm / 2; in.light.gradient = c.grad; in.light.gradient_angle = c.grad_angle;
        for (int i = 0; i < c.shadows; ++i) {
            const double a = rl.uniform(0, 2 * kPiD);
            in.light.shadows.push_back({std::cos(a), std::sin(a), rl.uniform(-0.3, 0.3) * c.symbol_mm, std::max(0.01, c.shadow_soft_mm), std::clamp(c.shadow_depth, 0.0, 0.98)});
        }
        for (int i = 0; i < c.glare_spots; ++i)
            in.light.glare.push_back({rl.uniform(-0.35, 0.35) * c.symbol_mm, rl.uniform(-0.35, 0.35) * c.symbol_mm, std::max(0.05, c.glare_radius_frac * c.symbol_mm * rl.uniform(0.7, 1.3)), c.glare_peak});
        in.light.veil = c.veil;
        in.light.prepare();
        return in;
    }

    // Radiance at a surface point (u,v mm, centred), channel ch; returns
    // false when the point is off the sheet (background shows through).
    static bool shade(const Instance &in, double u, double v, int C, float out[3], float band) {
        const double half = in.canvas_mm / 2;
        if (std::abs(u) > in.sheet_half || std::abs(v) > in.sheet_half) return false;
        const float il = in.light.illum(u, v), sp = in.light.specular(u, v);
        const bool on_canvas = std::abs(u) <= half && std::abs(v) <= half;
        if (!on_canvas) { for (int ch = 0; ch < C; ++ch) out[ch] = (C == 3 ? in.sheet[ch] : lin_luma(in.sheet)) * il + sp; return true; }
        double cu = (in.mirror ? -u : u) + half, cv = v + half;
        for (int ch = 0; ch < C; ++ch) {
            float val; bool hole = false;
            if (in.display) val = in.screen.sample(cu, cv, ch) * band;
            else val = in.tex.c[ch].bilinear(cu / in.canvas_mm * in.tex.w(), cv / in.canvas_mm * in.tex.h());
            if (!in.damage.empty()) {
                float rv = val;
                if (damage_at(in.damage, u + half, v + half, ch, rv, hole)) {
                    if (hole) return false;  // torn away: whatever is behind shows
                    val = rv;
                }
            }
            out[ch] = in.display ? static_cast<float>(val + in.screen.m.glass_reflect * il + sp) : val * il + sp;
        }
        return true;
    }

    // Draws an instance into the radiance image with exposure-time integration.
    // Returns the ROI it touched.
    Rect draw_instance(Image &rad, const Instance &in, const CaseSpec &c, Rng &rng) {
        const int W = rad.w(), H = rad.h(), C = rad.channels();
        // ROI from the projected sheet boundary (dense samples handle bend/distortion).
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18; bool any = false;
        const double s = in.sheet_half;
        for (int i = 0; i <= 64; ++i) {
            const double t = -s + 2 * s * i / 64.0;
            for (auto [u, v] : {std::pair{t, -s}, std::pair{t, s}, std::pair{-s, t}, std::pair{s, t}}) {
                double px, py; if (!in.cam.project(in.pose, u, v, px, py)) continue;
                any = true; x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py);
            }
        }
        if (!any) return Rect{0, 0, 0, 0};
        const double extra = c.motion_px * (1 + std::abs(c.rs_ratio)) + c.rot_blur_deg / 57.3 * std::max(x1 - x0, y1 - y0) + c.zoom_blur * std::max(x1 - x0, y1 - y0) + 4;
        Rect roi = clip_rect(Rect{static_cast<int>(x0 - extra), static_cast<int>(y0 - extra), static_cast<int>(x1 + extra) + 1, static_cast<int>(y1 + extra) + 1}, W, H);
        // Spatial samples per pixel. Motion, rotation and zoom blur during the
        // exposure are applied afterwards as image-space kernels (exposure_blur);
        // the rolling shutter is exact: each row is sampled at its own readout time.
        const int K = 1;
        const int N = std::max(1, c.samples);
        // R2 low-discrepancy subpixel offsets, stratified times.
        std::vector<double> ox(N), oy(N), tt(N);
        const double g = 1.32471795724474602596, a1 = 1 / g, a2 = 1 / (g * g);
        for (int k = 0; k < N; ++k) { ox[k] = std::fmod(0.5 + a1 * (k + 1), 1.0); oy[k] = std::fmod(0.5 + a2 * (k + 1), 1.0); tt[k] = K > 1 ? (k + 0.5) / N - 0.5 : 0; }
        const double vx = c.motion_px * std::cos(deg2rad(c.motion_angle)) * c.distance_mm / c.focal_px;
        const double vy = c.motion_px * std::sin(deg2rad(c.motion_angle)) * c.distance_mm / c.focal_px;
        const bool moving = c.rs_ratio != 0 && (c.motion_px > 0 || c.rot_blur_deg > 0 || c.zoom_blur > 0);
        const DisplayModel *dm = in.display ? &in.screen.m : nullptr;
        float acc[3], val[3];
        (void)rng;
        // Exact unprojection at pixel corners; subsamples interpolate bilinearly
        // inside the pixel (the projective map is smooth at sub-pixel scale).
        // A corner that misses the surface makes the pixel fall back to exact samples.
        const int rw = roi.x1 - roi.x0 + 1;
        std::vector<double> cu0(rw), cv0(rw), cu1(rw), cv1(rw);
        std::vector<uint8_t> ok0(rw), ok1(rw);
        Mat3 R = rotation(in.pose.roll, in.pose.yaw, in.pose.pitch); Vec3 T{in.pose.tx, in.pose.ty, in.pose.tz};
        const auto row_pose = [&](int y) {
            if (!moving) return;
            const double t = c.rs_ratio * ((y + 0.5) / H - 0.5);
            Pose p = in.pose; p.tx += vx * t; p.ty += vy * t; p.roll += c.rot_blur_deg * t; p.tz *= 1 + c.zoom_blur * t;
            R = rotation(p.roll, p.yaw, p.pitch); T = Vec3{p.tx, p.ty, p.tz};
        };
        const auto corners = [&](int y, std::vector<double> &U, std::vector<double> &V, std::vector<uint8_t> &O) {
            const bool bent = in.cam.S.bend_radius != 0;
            for (int i = 0; i < rw; ++i) {
                // Warm start from the left neighbour (or the corner above) on bent surfaces.
                double g[2]; const double *gp = nullptr;
                if (bent && i > 0 && O[i - 1]) { g[0] = U[i - 1]; g[1] = V[i - 1]; gp = g; }
                O[i] = in.cam.unproject(R, T, roi.x0 + i, y, U[i], V[i], gp);
            }
        };
        row_pose(roi.y0); corners(roi.y0, cu0, cv0, ok0);
        for (int y = roi.y0; y < roi.y1; ++y) {
            row_pose(y); corners(y + 1, cu1, cv1, ok1);
            float band = 1;
            if (dm && dm->refresh_amp > 0) band = static_cast<float>(1 - dm->refresh_amp * (0.5 + 0.5 * std::sin(2 * kPiD * (y / dm->refresh_period_rows + dm->refresh_phase))));
            const double half_canvas = in.canvas_mm / 2;
            const auto off_canvas = [&](double u, double v) { return std::abs(u) > half_canvas || std::abs(v) > half_canvas; };
            for (int x = roi.x0; x < roi.x1; ++x) {
                const int i = x - roi.x0;
                const bool interp = ok0[i] && ok0[i + 1] && ok1[i] && ok1[i + 1];
                acc[0] = acc[1] = acc[2] = 0; int hits = 0;
                // Pixels wholly off the printed canvas (sheet or background) are smooth: one sample suffices.
                const bool smooth = interp && in.damage.empty() && off_canvas(cu0[i], cv0[i]) && off_canvas(cu0[i + 1], cv0[i + 1]) && off_canvas(cu1[i], cv1[i]) && off_canvas(cu1[i + 1], cv1[i + 1]) &&
                                    ((cu0[i] > half_canvas && cu0[i + 1] > half_canvas && cu1[i] > half_canvas && cu1[i + 1] > half_canvas) ||
                                     (cu0[i] < -half_canvas && cu0[i + 1] < -half_canvas && cu1[i] < -half_canvas && cu1[i + 1] < -half_canvas) ||
                                     (cv0[i] > half_canvas && cv0[i + 1] > half_canvas && cv1[i] > half_canvas && cv1[i + 1] > half_canvas) ||
                                     (cv0[i] < -half_canvas && cv0[i + 1] < -half_canvas && cv1[i] < -half_canvas && cv1[i + 1] < -half_canvas));
                const int NN = smooth ? 1 : N;
                for (int k = 0; k < NN; ++k) {
                    double u, v;
                    bool okk;
                    if (interp) {
                        const double fx = NN == 1 ? 0.5 : ox[k], fy = NN == 1 ? 0.5 : oy[k];
                        u = (cu0[i] * (1 - fx) + cu0[i + 1] * fx) * (1 - fy) + (cu1[i] * (1 - fx) + cu1[i + 1] * fx) * fy;
                        v = (cv0[i] * (1 - fx) + cv0[i + 1] * fx) * (1 - fy) + (cv1[i] * (1 - fx) + cv1[i + 1] * fx) * fy;
                        okk = true;
                    } else okk = in.cam.unproject(R, T, x + ox[k], y + oy[k], u, v);
                    if (!okk || !shade(in, u, v, C, val, band)) {
                        for (int ch = 0; ch < C; ++ch) acc[ch] += rad.c[ch].at(x, y);
                        continue;
                    }
                    ++hits; for (int ch = 0; ch < C; ++ch) acc[ch] += val[ch];
                }
                if (hits) for (int ch = 0; ch < C; ++ch) rad.c[ch].at(x, y) = acc[ch] / NN;
            }
            cu0.swap(cu1); cv0.swap(cv1); ok0.swap(ok1);
        }
        return roi;
    }

    // Image motion during the exposure, as image-space kernels over the symbol's
    // region (camera shake moves symbol and nearby background alike):
    // translation = line kernel of motion_px; rotation = arc about the symbol
    // centre; zoom = radial segment. Valid for the moderate perspective of
    // hand-held capture; documented in simulation/README.md.
    void exposure_blur(Image &rad, Rect roi, const Instance &in, const CaseSpec &c) {
        // Linear motion below 1 px is folded into the lens Gaussian (variance L^2/12; see the lens stage).
        const bool lin = c.motion_px >= 1.0, rot = c.rot_blur_deg > 1e-3, zoom = c.zoom_blur > 1e-5;
        if (!lin && !rot && !zoom) return;
        const int W = rad.w(), H = rad.h();
        const int pad = static_cast<int>(c.motion_px) + 4;
        roi = clip_rect(Rect{roi.x0 - pad, roi.y0 - pad, roi.x1 + pad, roi.y1 + pad}, W, H);
        double cx, cy;
        if (!in.cam.project(in.pose, 0, 0, cx, cy)) return;
        const double ca = std::cos(deg2rad(c.motion_angle)), sa = std::sin(deg2rad(c.motion_angle));
        const double th = deg2rad(c.rot_blur_deg);
        const int C = rad.channels();
        std::vector<Plane> src(rad.c.begin(), rad.c.end());
        if (lin && !rot && !zoom) {
            // Pure translation: every pixel shares one kernel; precompute its bilinear taps.
            const int taps = std::clamp(static_cast<int>(std::ceil(c.motion_px)) + 1, 2, 64);
            std::map<std::pair<int, int>, double> k;
            for (int t = 0; t < taps; ++t) {
                const double u = (t + 0.5) / taps - 0.5, ox = c.motion_px * u * ca, oy = c.motion_px * u * sa;
                const int ix = static_cast<int>(std::floor(ox)), iy = static_cast<int>(std::floor(oy)); const double fx = ox - ix, fy = oy - iy;
                k[{ix, iy}] += (1 - fx) * (1 - fy) / taps; k[{ix + 1, iy}] += fx * (1 - fy) / taps;
                k[{ix, iy + 1}] += (1 - fx) * fy / taps; k[{ix + 1, iy + 1}] += fx * fy / taps;
            }
            std::vector<std::pair<std::pair<int, int>, float>> kv;
            for (const auto &[o, w] : k) if (w > 1e-6) kv.push_back({o, static_cast<float>(w)});
            for (int ch = 0; ch < C; ++ch)
                for (int y = roi.y0; y < roi.y1; ++y) for (int x = roi.x0; x < roi.x1; ++x) {
                    float a = 0; for (const auto &[o, w] : kv) a += w * src[ch].clamped(x + o.first, y + o.second);
                    rad.c[ch].at(x, y) = a;
                }
            return;
        }
        for (int y = roi.y0; y < roi.y1; ++y) for (int x = roi.x0; x < roi.x1; ++x) {
            const double px = x + 0.5, py = y + 0.5, dx = px - cx, dy = py - cy, rr = std::hypot(dx, dy);
            // Per-pixel blur path length decides the tap count (about one tap per pixel of path).
            const double len = (lin ? c.motion_px : 0) + (rot ? rr * th : 0) + (zoom ? rr * c.zoom_blur : 0);
            const int taps = std::clamp(static_cast<int>(std::ceil(len)) + 1, 1, 64);
            if (taps <= 1) continue;
            double acc[3] = {0, 0, 0};
            for (int t = 0; t < taps; ++t) {
                const double u = (t + 0.5) / taps - 0.5;
                double qx = dx, qy = dy;
                if (rot) { const double a = th * u, cs = std::cos(a), sn = std::sin(a); const double nx = qx * cs - qy * sn, ny = qx * sn + qy * cs; qx = nx; qy = ny; }
                if (zoom) { qx *= 1 + c.zoom_blur * u; qy *= 1 + c.zoom_blur * u; }
                qx += cx + (lin ? c.motion_px * u * ca : 0); qy += cy + (lin ? c.motion_px * u * sa : 0);
                for (int ch = 0; ch < C; ++ch) acc[ch] += src[ch].bilinear(qx, qy);
            }
            for (int ch = 0; ch < C; ++ch) rad.c[ch].at(x, y) = static_cast<float>(acc[ch] / taps);
        }
    }

    bool project_quad(const Instance &in, const double mm[8], double out[8]) const {
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            double u = mm[2 * i] - in.canvas_mm / 2, v = mm[2 * i + 1] - in.canvas_mm / 2;
            if (in.mirror) u = -u;
            ok &= in.cam.project(in.pose, u, v, out[2 * i], out[2 * i + 1]);
        }
        return ok;
    }

    static void place_finger(Finger &fg, const CaseSpec &c, const double gt[8], Rng &r, int W, int H) {
        // A defocused fingertip entering from the nearest frame edge, sized to cover ~occl_frac of the field.
        double cx = 0, cy = 0; for (int i = 0; i < 4; ++i) { cx += gt[2 * i] / 4; cy += gt[2 * i + 1] / 4; }
        const double side = std::hypot(gt[2] - gt[0], gt[3] - gt[1]);
        const double area = std::clamp(c.occl_frac, 0.0, 0.9) * side * side;
        fg.rx = std::sqrt(area / kPiD) * 1.8; fg.ry = fg.rx / 1.8 * 0.9 + 1;
        const double a = r.uniform(0, 2 * kPiD);
        const double reach = side * 0.5 + fg.ry * 0.2;
        fg.cx = cx + std::cos(a) * reach; fg.cy = cy + std::sin(a) * reach; fg.angle = a * 180 / kPiD;
        fg.edge_px = std::max(3.0, side * 0.05);
        const float skin[3] = {0.45f, 0.30f, 0.24f};
        for (int i = 0; i < 3; ++i) fg.col[i] = skin[i] * static_cast<float>(r.uniform(0.6, 1.1));
        (void)W; (void)H;
    }

    static void chromatic(Image &rad, double ca) {
        const int W = rad.w(), H = rad.h(); const double cx = W / 2.0, cy = H / 2.0, rmax = std::hypot(cx, cy);
        for (int ch : {0, 2}) {
            const Plane src = rad.c[ch]; const double s = 1 + (ch == 0 ? 1 : -1) * ca / rmax;
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) rad.c[ch].at(x, y) = src.bilinear(cx + (x + .5 - cx) / s, cy + (y + .5 - cy) / s);
        }
    }

    // Physical observability of the module pattern; see analysis/README.md.
    void physics(Frame &f, const CaseSpec &c, const SymbolArt &art, const Instance &in, const Finger &finger, const SensorStats &st, double rs) {
        Phys &p = f.phys;
        const double module_mm = in.canvas_mm * (art.field_units / art.canvas_units) / art.modules;
        double fq[8]; field_corners_mm(art, in.canvas_mm, fq);
        double mn = 1e18, sum = 0; int n = 0;
        for (int j = 0; j < 5; ++j) for (int i = 0; i < 5; ++i) {
            const double u = fq[0] + (fq[2] - fq[0]) * (i + 0.5) / 5 - in.canvas_mm / 2, v = fq[1] + (fq[5] - fq[1]) * (j + 0.5) / 5 - in.canvas_mm / 2;
            double x0, y0, xa, ya, xb, yb; const double h = 0.01 * in.canvas_mm;
            if (!in.cam.project(in.pose, u, v, x0, y0) || !in.cam.project(in.pose, u + h, v, xa, ya) || !in.cam.project(in.pose, u, v + h, xb, yb)) continue;
            const double a = (xa - x0) / h, b = (xb - x0) / h, cc = (ya - y0) / h, d = (yb - y0) / h;  // Jacobian px/mm
            const double s1 = a * a + b * b + cc * cc + d * d, det = a * d - b * cc;
            const double smin = std::sqrt(std::max(0.0, (s1 - std::sqrt(std::max(0.0, s1 * s1 - 4 * det * det))) / 2));
            mn = std::min(mn, smin * module_mm * rs); sum += std::sqrt(std::abs(det)) * module_mm * rs; ++n;
        }
        p.ppm_min = n ? mn : 0; p.ppm_mean = n ? sum / n : 0;
        // Combined blur in output px: lens (at the symbol's field position), defocus, motion, pixel aperture.
        double cx = 0, cy = 0; for (int i = 0; i < 4; ++i) { cx += (f.gt[2 * i] + 0.5) / rs / 4; cy += (f.gt[2 * i + 1] + 0.5) / rs / 4; }
        const double rr = std::min(1.0, (std::pow(cx - c.frame_w / 2.0, 2) + std::pow(cy - c.frame_h / 2.0, 2)) / (std::pow(c.frame_w / 2.0, 2) + std::pow(c.frame_h / 2.0, 2)));
        const double lens = c.lens_sigma_px + (c.lens_sigma_edge_px - c.lens_sigma_px) * rr;
        const double g2 = lens * lens + c.defocus_gauss_px * c.defocus_gauss_px;
        p.blur_sigma_px = std::sqrt(g2 + std::pow(c.defocus_disc_px / 2, 2) + c.motion_px * c.motion_px / 12 + 1.0 / 12) * rs;
        const double fpx = 0.5 / std::max(1e-6, p.ppm_min / rs);  // cycles per sensor px at the module fundamental
        auto sinc = [](double x) { return std::abs(x) < 1e-9 ? 1.0 : std::abs(std::sin(kPiD * x) / (kPiD * x)); };
        double mtf = std::exp(-2 * kPiD * kPiD * g2 * fpx * fpx) * sinc(fpx) * sinc(c.motion_px * fpx);
        if (c.defocus_disc_px > 0.3) { const double z = 2 * kPiD * c.defocus_disc_px * fpx; mtf *= std::abs(2 * std::cyl_bessel_j(1.0, z) / z); }
        if (fpx >= 1.0) mtf = 0;  // beyond the sampling limit (< 0.5 px per module)
        p.mtf = mtf;
        float fg[3], bg[3]; rgb_lin(c.fg_rgb, fg); rgb_lin(c.bg_rgb, bg);
        double Lf = lin_luma(fg), Lb = lin_luma(bg);
        if (in.display) { Lf = c.disp_brightness * Lf + c.disp_glass; Lb = c.disp_brightness * Lb + c.disp_glass; }
        p.contrast = std::abs(Lf - Lb) / std::max(1e-6, Lf + Lb);
        const double illum_c = in.light.illum(0, 0) * (1 - c.vignette * rr);
        const double k = st.electrons_white, dsig = std::abs(Lf - Lb) * illum_c * k * mtf, mean_e = (Lf + Lb) / 2 * illum_c * k;
        p.dprime = dsig / std::sqrt(std::max(1e-9, mean_e + c.read_noise_e * c.read_noise_e));
        // Occlusion over a 64x64 field grid (surface damage + finger).
        int occ = 0, tot = 0;
        for (int j = 0; j < 64; ++j) for (int i = 0; i < 64; ++i) {
            const double cu = fq[0] + (fq[2] - fq[0]) * (i + 0.5) / 64, cv = fq[1] + (fq[5] - fq[1]) * (j + 0.5) / 64;
            float dummy = 0; bool hole = false; bool hit = damage_at(in.damage, cu, cv, 0, dummy, hole);
            if (!hit && finger.on) { double px, py; if (in.cam.project(in.pose, (in.mirror ? -1 : 1) * (cu - in.canvas_mm / 2), cv - in.canvas_mm / 2, px, py)) hit = finger_alpha(finger, px, py) > 0.5f; }
            occ += hit; ++tot;
        }
        p.occluded = static_cast<double>(occ) / tot;
        // Saturation inside the field quad.
        int sat = 0, cnt = 0;
        const int hi = c.video_range ? 234 : 254, lo = c.video_range ? 17 : 1;
        double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
        for (int i = 0; i < 4; ++i) { bx0 = std::min(bx0, f.gt[2 * i]); bx1 = std::max(bx1, f.gt[2 * i]); by0 = std::min(by0, f.gt[2 * i + 1]); by1 = std::max(by1, f.gt[2 * i + 1]); }
        const int step = std::max(1, static_cast<int>((bx1 - bx0) / 160));
        for (int y = std::max(0, static_cast<int>(by0)); y < std::min(f.h, static_cast<int>(by1)); y += step)
            for (int x = std::max(0, static_cast<int>(bx0)); x < std::min(f.w, static_cast<int>(bx1)); x += step) {
                if (!inside_quad(f.gt, x + .5, y + .5)) continue;
                const uint8_t v = f.gray[static_cast<size_t>(y) * f.w + x]; sat += v >= hi || v <= lo; ++cnt;
            }
        p.saturated = cnt ? static_cast<double>(sat) / cnt : 0;
        const double margin = std::max(1.0, p.ppm_mean);
        p.in_frame = f.gt_valid;
        for (int i = 0; i < 4; ++i) p.in_frame &= f.gt[2 * i] >= margin && f.gt[2 * i + 1] >= margin && f.gt[2 * i] <= f.w - margin && f.gt[2 * i + 1] <= f.h - margin;
        classify(p, art);
    }

    static bool inside_quad(const double q[8], double x, double y) {
        bool pos = false, neg = false;
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) % 4;
            const double cr = (q[2 * j] - q[2 * i]) * (y - q[2 * i + 1]) - (q[2 * j + 1] - q[2 * i + 1]) * (x - q[2 * i]);
            pos |= cr > 0; neg |= cr < 0;
        }
        return !(pos && neg);
    }

public:
    // recov-v1 thresholds (mirrored in configs/strata.yaml; checked by preflight).
    static void classify(Phys &p, const SymbolArt &art) {
        // Pseudo-random bit placement: a site-loss fraction a damages a byte with probability 1-(1-a)^8.
        const double a = std::min(1.0, p.occluded + p.saturated);
        p.byte_err_est = 1 - std::pow(1 - a, 8);
        const double ecc = art.ecc_fraction, era = 2 * ecc;  // errors-only vs. erasure-assisted capacity
        std::string why;
        if (art.blank || art.allowed.empty()) { p.cls = "NOT_APPLICABLE"; p.reason = "negative"; return; }
        if (!p.in_frame) why = "CROPPED";
        else if (p.ppm_min < 1.0) why = "SUBSAMPLED";
        else if (p.mtf * p.contrast < 0.01 || p.dprime < 1.0) why = "NO_MODULATION";
        else if (p.byte_err_est > era) why = "ECC_EXCEEDED";
        if (!why.empty()) { p.cls = "UNRECOVERABLE"; p.reason = why; return; }
        if (p.ppm_min < 2.0) why = "LOW_RESOLUTION";
        else if (p.dprime < 3.0) why = "LOW_SNR";
        else if (p.byte_err_est > ecc) why = "NEEDS_ERASURES";
        p.cls = why.empty() ? "RECOVERABLE" : "MARGINAL"; p.reason = why;
    }
};

}  // namespace ckt
#endif
