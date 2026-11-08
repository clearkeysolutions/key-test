// Display substrate: the symbol is shown on an emissive screen. The screen
// is a grid of pixels at `ppi`; each pixel is three subpixel stripes with a
// black-matrix fill factor. The camera samples this procedurally (no huge
// texture), so screen-to-camera moire and colour aliasing emerge from the
// sampling itself. PWM / refresh banding is a row-wise modulation applied
// with the rolling-shutter row time (see pipeline.h).
#ifndef CKT_DISPLAY_H
#define CKT_DISPLAY_H

#include "simulation/common/image.h"

#include <cmath>

namespace ckt {

struct DisplayModel {
    double ppi = 460;
    double fill = 0.8;            // linear fill factor of each subpixel (black matrix gaps)
    int layout = 0;               // 0 RGB stripe, 1 BGR stripe, 2 no subpixel structure (e-paper like)
    double brightness = 1.6;      // emitted white radiance relative to white paper under nominal light
    double black = 0.003;         // emitted black level (relative)
    double glass_reflect = 0.04;  // ambient reflection off the cover glass
    double refresh_amp = 0.0;     // PWM / refresh banding depth (0..1)
    double refresh_period_rows = 240;
    double refresh_phase = 0;
};

// The screen image: one value per screen pixel and channel (linear, 0..1 of
// full white), produced by the pipeline from the symbol coverage.
struct Screen {
    Image px;          // screen pixels
    double pitch_mm;   // screen pixel pitch
    double canvas_mm;  // canvas size on the screen
    DisplayModel m;

    // Emitted radiance at canvas position (u,v) mm from the canvas top-left.
    float sample(double u, double v, int ch) const {
        const double sx = u / pitch_mm, sy = v / pitch_mm;
        const int i = static_cast<int>(std::floor(sx)), j = static_cast<int>(std::floor(sy));
        if (i < 0 || j < 0 || i >= px.w() || j >= px.h()) return static_cast<float>(m.black);
        const double fx = sx - i, fy = sy - j;
        const int pc = px.channels() == 3 ? ch : 0;
        const float value = px.c[pc].at(i, j);
        double mask;
        const double gap = (1 - m.fill) / 2;
        if (m.layout == 2) mask = (fx >= gap && fx <= 1 - gap && fy >= gap && fy <= 1 - gap) ? 1.0 : 0.0;
        else {
            const int sub = std::min(2, static_cast<int>(fx * 3));
            const int want = m.layout == 1 ? 2 - ch : ch;
            const double f3 = fx * 3 - sub;
            mask = (sub == want && f3 >= gap && f3 <= 1 - gap && fy >= gap && fy <= 1 - gap) ? 3.0 : 0.0;  // x3: one of three stripes carries the channel
            if (px.channels() == 1) mask = (f3 >= gap && f3 <= 1 - gap && fy >= gap && fy <= 1 - gap) ? 1.0 : 0.0;
        }
        const double norm = 1.0 / (m.fill * m.fill);  // keep mean emitted radiance independent of fill factor
        return static_cast<float>(m.black + (m.brightness - m.black) * value * mask * norm);
    }
};

}  // namespace ckt
#endif
