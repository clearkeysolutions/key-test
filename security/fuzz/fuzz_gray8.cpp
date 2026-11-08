// libFuzzer: the full-frame reader path (find borders, refine, decode) on
// arbitrary small images and quadrilaterals: no crash, no unbounded work.
#include "ckey/ckey.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    const size_t w = 16 + data[0] % 240, h = 16 + data[1] % 240;
    std::vector<uint8_t> img(w * h);
    for (size_t i = 0; i < img.size(); ++i) img[i] = data[2 + i % (size - 2)];
    double borders[6 * 8];
    const size_t n = ckey_find_borders_gray8(img.data(), w, h, w, borders, 6);
    std::vector<uint8_t> out(65535); size_t len = 0; ckey_diagnostics d{};
    for (size_t i = 0; i < n && i < 6; ++i) {
        double q[8]; uint32_t v = 0, l = 0;
        if (ckey_refine_quad_gray8(img.data(), w, h, w, borders + 8 * i, CKEY_FIELD_SCALABLE, q) >= 0)
            ckey_decode_scalable_gray8(img.data(), w, h, w, q, out.data(), out.size(), &len, &v, &l, &d);
        if (ckey_refine_quad_gray8(img.data(), w, h, w, borders + 8 * i, CKEY_FIELD_LOCATOR, q) >= 0)
            ckey_decode_gray8(img.data(), w, h, w, q, out.data(), 16, &d);
    }
    // Arbitrary (even degenerate or non-finite-free) quads straight into the decoders.
    double q[8]; for (int k = 0; k < 8; ++k) q[k] = (data[(3 + k) % size] / 255.0) * (k % 2 ? h : w) * 1.5 - 0.25 * (k % 2 ? h : w);
    ckey_decode_text_gray8(img.data(), w, h, w, q, out.data(), out.size(), &len, &d);
    ckey_decode_extended_gray8(img.data(), w, h, w, q, out.data(), out.size(), &len, &d);
    return 0;
}
