// The case specification: every knob the simulator and symbol factory read.
// One X-macro defines each field's type, name and default, so the C++
// parser, the list of known keys, and the schema emitted for the Python
// generator (ckt_worker --print-schema) cannot drift apart. A plan line with
// an unknown key or a wrongly typed value is an invalid generation, never
// silently ignored.
#ifndef CKT_CASE_SPEC_H
#define CKT_CASE_SPEC_H

#include "simulation/common/flatjson.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace ckt {

inline constexpr const char *CASE_SCHEMA_VERSION = "ckt-case-v1";

// T: S string, D double, I integer, A number array (default given as CSV).
#define CKT_CASE_FIELDS(X)                                                                         \
    /* identity and design */                                                                     \
    X(S, case_id, "")                                                                             \
    X(S, cohort, "")                                                                              \
    X(S, stratum, "")                                                                             \
    X(S, profile, "P1")             /* P0 (scalable, incl. P0.1 print layout), P1, P2, P3 */      \
    X(S, seed, "0")                 /* uint64 as decimal string */                                \
    X(S, mode, "")                  /* sweep | joint | negative */                                \
    X(S, pool, "")                  /* adaptive sub-pool: failure_targeted | boundary_search | cross_condition */ \
    X(I, sev_bin, 0)                                                                              \
    X(S, factor, "")                /* primary factor varied by the stratum */                   \
    X(D, severity, 0)               /* primary factor value (stratum-specific units) */          \
    X(S, payload_family, "")                                                                      \
    X(S, expected, "DECODE")        /* DECODE | DECODE_ANY_OF | REJECT | NO_WRONG */              \
    /* symbol */                                                                                  \
    X(I, p0_version, 0)             /* 0 = smallest that fits */                                 \
    X(S, p0_level, "M")                                                                           \
    X(S, p0_layout, "standard")     /* standard (Format 0.27) | print (Format 0.28, P0.1) */     \
    X(I, p3_tier, 0)                /* 0 = auto, else 256..2048 */                               \
    X(I, payload_len, 16)                                                                         \
    X(S, payload_pattern, "random")                                                               \
    X(S, payload_seed, "0")                                                                       \
    X(S, neg_kind, "none")          /* none random_codeword heavy_corruption t_plus_one crc_forge splice mirror foreign_grid blank */ \
    X(D, neg_param, 0)                                                                            \
    X(I, second_code, 0)            /* 1: a second, distinct valid symbol in frame */            \
    X(S, rasterizer, "harness")     /* harness (fast, independent) | ckey (ckey's svg_raster.h) */ \
    X(S, second_profile, "")                                                                      \
    /* appearance */                                                                              \
    X(S, color_mode, "mono")        /* mono | rgb */                                             \
    X(A, fg_rgb, "20,20,20")        /* mark colour, sRGB 8-bit */                                 \
    X(A, bg_rgb, "235,235,235")     /* symbol background colour, sRGB 8-bit */                   \
    X(D, bg_alpha, 1)               /* <1: transparent background over a textured surface */      \
    X(I, mix_srgb, 1)               /* antialiased edges mixed in sRGB (as renderers do) */      \
    /* substrate */                                                                               \
    X(S, substrate, "ideal")        /* ideal | print | display */                                \
    X(D, symbol_mm, 30)             /* physical width of the full SVG canvas (incl. quiet margin) */ \
    X(D, sheet_margin, 0.25)        /* sheet beyond the canvas, fraction of symbol_mm */         \
    X(A, sheet_rgb, "235,235,235")                                                                \
    X(D, print_dpi, 600)                                                                          \
    X(D, dot_gain_mm, 0.02)                                                                       \
    X(D, paper_blur_mm, 0.01)                                                                     \
    X(D, halftone_lpi, 0)                                                                         \
    X(D, ink_density, 1)                                                                          \
    X(D, paper_texture, 0.02)                                                                     \
    X(D, disp_ppi, 460)                                                                           \
    X(D, disp_fill, 0.8)                                                                          \
    X(I, disp_layout, 0)                                                                          \
    X(D, disp_brightness, 1.6)                                                                    \
    X(D, disp_refresh_amp, 0)                                                                     \
    X(D, disp_refresh_period, 240)                                                                \
    X(D, disp_glass, 0.04)                                                                        \
    /* camera and sensor */                                                                       \
    X(S, device, "ios_4k_wide")                                                                   \
    X(I, frame_w, 2160)                                                                           \
    X(I, frame_h, 3840)                                                                           \
    X(D, focal_px, 2360)                                                                          \
    X(D, k1, 0)                                                                                   \
    X(D, k2, 0)                                                                                   \
    X(D, full_well_e, 6000)                                                                       \
    X(D, read_noise_e, 3)                                                                         \
    X(D, prnu, 0.006)                                                                             \
    X(I, adc_bits, 10)                                                                            \
    X(D, max_signal_e, 4000)                                                                      \
    X(S, device_seed, "1")                                                                        \
    /* pose */                                                                                    \
    X(D, distance_mm, 200)                                                                        \
    X(D, roll, 0)                                                                                 \
    X(D, yaw, 0)                                                                                  \
    X(D, pitch, 0)                                                                                \
    X(D, center_x, 0.5)             /* symbol centre, normalized frame coordinates */            \
    X(D, center_y, 0.5)                                                                           \
    X(D, bend_radius_mm, 0)                                                                       \
    X(D, bend_axis, 0)                                                                            \
    /* optics */                                                                                  \
    X(D, lens_sigma_px, 0.6)        /* residual lens blur at the axis */                          \
    X(D, lens_sigma_edge_px, 0.9)   /* at the frame corner (field curvature) */                  \
    X(D, defocus_gauss_px, 0)                                                                     \
    X(D, defocus_disc_px, 0)        /* geometric defocus disc radius */                           \
    X(D, ca_px, 0)                  /* lateral chromatic aberration at the corner (rgb) */       \
    X(D, vignette, 0.25)                                                                          \
    X(D, motion_px, 0)              /* image motion during exposure */                           \
    X(D, motion_angle, 0)                                                                         \
    X(D, rs_ratio, 0)               /* rolling-shutter readout time / exposure time */            \
    X(D, rot_blur_deg, 0)           /* rotation during exposure */                               \
    X(D, zoom_blur, 0)              /* fractional distance change during exposure */             \
    X(I, samples, 4)                /* camera samples per pixel (>= time samples) */             \
    /* light */                                                                                   \
    X(D, illum, 1)                                                                                \
    X(D, ae_bias, 0)                                                                              \
    X(I, bayer, 0)                                                                                \
    X(D, grad, 0)                                                                                 \
    X(D, grad_angle, 0)                                                                           \
    X(I, shadows, 0)                                                                              \
    X(D, shadow_depth, 0.5)                                                                       \
    X(D, shadow_soft_mm, 1)                                                                       \
    X(I, glare_spots, 0)                                                                          \
    X(D, glare_peak, 2)                                                                           \
    X(D, glare_radius_frac, 0.1)                                                                  \
    X(D, veil, 0)                                                                                 \
    /* occlusion */                                                                               \
    X(S, occl_kind, "none")         /* none scratch tear missing_corner sticker finder_cover smudge finger */ \
    X(D, occl_frac, 0)                                                                            \
    X(I, occl_count, 1)                                                                           \
    /* ISP */                                                                                     \
    X(I, tone, 0)                                                                                 \
    X(D, denoise, 0)                                                                              \
    X(D, sharpen, 0.3)                                                                            \
    X(D, sharpen_sigma, 1)                                                                        \
    X(I, jpeg_q, 0)                                                                               \
    X(D, rescale, 1)                                                                              \
    X(I, video_range, 0)                                                                          \
    /* scene */                                                                                   \
    X(D, clutter, 0.3)                                                                            \
    X(I, distractors, 0)                                                                          \
    X(D, bg_level, 0.35)                                                                          \
    X(I, ideal, 0)                  /* 1: mathematically perfect raster, no camera model */      \
    X(D, ideal_ppm, 0)                                                                            \
    X(D, target_ppm, 0)             /* informational: the ppm the generator aimed for */         \
    /* video */                                                                                   \
    X(I, frames, 1)                                                                               \
    X(D, shake_px, 0)                                                                             \
    X(D, shake_roll, 0)                                                                           \
    X(D, focus_drift, 0)                                                                          \
    X(D, exposure_drift, 0)                                                                       \
    X(D, frame_motion_px, 0)

inline std::vector<double> csv_doubles(const char *s) {
    std::vector<double> out; std::stringstream in(s); std::string tok;
    while (std::getline(in, tok, ',')) out.push_back(std::stod(tok));
    return out;
}

#define CKT_T_S std::string
#define CKT_T_D double
#define CKT_T_I int
#define CKT_T_A std::vector<double>
#define CKT_INIT_S(d) d
#define CKT_INIT_D(d) d
#define CKT_INIT_I(d) d
#define CKT_INIT_A(d) csv_doubles(d)

struct CaseSpec {
#define CKT_DECL(T, n, d) CKT_T_##T n = CKT_INIT_##T(d);
    CKT_CASE_FIELDS(CKT_DECL)
#undef CKT_DECL
};

inline void assign_field(std::string &dst, const JVal &v, const std::string &k) { if (v.kind != JVal::Str) throw std::runtime_error("field " + k + " must be a string"); dst = v.str; }
inline void assign_field(double &dst, const JVal &v, const std::string &k) { if (v.kind != JVal::Num && v.kind != JVal::Bool) throw std::runtime_error("field " + k + " must be a number"); dst = v.num; }
inline void assign_field(int &dst, const JVal &v, const std::string &k) {
    if (v.kind != JVal::Num && v.kind != JVal::Bool) throw std::runtime_error("field " + k + " must be an integer");
    if (v.num != static_cast<double>(static_cast<long long>(v.num))) throw std::runtime_error("field " + k + " must be integral");
    dst = static_cast<int>(v.num);
}
inline void assign_field(std::vector<double> &dst, const JVal &v, const std::string &k) { if (v.kind != JVal::Arr) throw std::runtime_error("field " + k + " must be an array"); dst = v.arr; }

inline CaseSpec parse_case(const std::string &line) {
    const JObj obj = parse_flat_json(line);
    CaseSpec c;
    for (const auto &[k, v] : obj) {
        bool known = false;
#define CKT_SET(T, n, d) if (k == #n) { assign_field(c.n, v, k); known = true; }
        CKT_CASE_FIELDS(CKT_SET)
#undef CKT_SET
        if (!known) throw std::runtime_error("unknown case field: " + k);
    }
    return c;
}

// The schema the Python generator checks its plans against.
inline std::string case_schema_json() {
    std::string s = std::string("{\"version\":\"") + CASE_SCHEMA_VERSION + "\",\"fields\":{";
    bool first = true;
#define CKT_SCHEMA(T, n, d) s += std::string(first ? "" : ",") + "\"" #n "\":\"" #T "\""; first = false;
    CKT_CASE_FIELDS(CKT_SCHEMA)
#undef CKT_SCHEMA
    return s + "}}";
}

}  // namespace ckt
#endif
