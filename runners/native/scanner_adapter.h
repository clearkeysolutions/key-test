// Scanner API adapter (ckt-adapter-v1).
//
// A faithful C++ port of the production reader loop in
// clearkeysolutions/ckey sdk/swift/Sources/CKeyScanKit/CKeyDecoder.swift
// (which documents itself as ios/CKeyReader/CameraScanner.swift's pipeline):
//   ckey_find_borders_gray8 (max 6 candidates)
//   for each candidate, in order:
//     refine CKEY_FIELD_SCALABLE (score >= 0) -> ckey_decode_scalable_gray8
//     refine CKEY_FIELD_LOCATOR  (score >= 0) -> ckey_decode_gray8 (16 bytes)
//     refine CKEY_FIELD_TEXT     (score >= 0) -> ckey_decode_text_gray8
//     refine CKEY_FIELD_EXTENDED (score >= 0) -> ckey_decode_extended_gray8
//   the first CKEY_OK is the reader's answer.
// The adapter adds instrumentation only; it never changes which calls are
// made, their order, or the acceptance rule. Any divergence from the Swift
// loop is a harness bug (see certification/conformance/adapter_parity.md).
#ifndef CKT_SCANNER_ADAPTER_H
#define CKT_SCANNER_ADAPTER_H

#include "ckey/ckey.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ckt {

inline constexpr const char *ADAPTER_VERSION = "ckt-adapter-v1(CKeyDecoder.swift@ede38f5)";

struct Attempt {
    int field = 0;           // ckey_field
    double score = -1;       // refine score
    double quad[8]{};
    int status = -1;         // decode status, -1 = not attempted (score < 0)
    uint32_t corrected = 0, erasures = 0;
    std::string message;
};

struct CandidateTrace {
    double border[8]{};
    std::vector<Attempt> attempts;
};

struct ScanResult {
    bool accepted = false;
    int profile = -1;                  // 0..3
    uint32_t version = 0, layout = 0;  // Profile 0 identification
    std::vector<uint8_t> payload;
    size_t borders = 0;
    int accepted_candidate = -1;
    double accepted_quad[8]{};
    uint32_t corrected = 0, erasures = 0;
    std::vector<CandidateTrace> candidates;
    double ms = 0;                     // wall time of the whole reader call
};

// Oracle-assisted diagnostic decode: the production decoder for one profile
// given a supplied quadrilateral (e.g. the ground-truth field corners). It
// is used only for failure triage (localization vs. sampling/decoding) and
// never changes a case's outcome.
inline bool decode_with_quad(int profile, const uint8_t *g, size_t w, size_t h, size_t s, const double quad[8], std::vector<uint8_t> &payload) {
    std::vector<uint8_t> out(65535); size_t len = 0; ckey_diagnostics d{}; ckey_status st;
    if (profile == 0) { uint32_t v = 0, l = 0; st = ckey_decode_scalable_gray8(g, w, h, s, quad, out.data(), out.size(), &len, &v, &l, &d); }
    else if (profile == 1) { st = ckey_decode_gray8(g, w, h, s, quad, out.data(), CKEY_LOCATOR_PAYLOAD_BYTES, &d); len = CKEY_LOCATOR_PAYLOAD_BYTES; }
    else if (profile == 2) st = ckey_decode_text_gray8(g, w, h, s, quad, out.data(), out.size(), &len, &d);
    else st = ckey_decode_extended_gray8(g, w, h, s, quad, out.data(), out.size(), &len, &d);
    if (st != CKEY_OK) return false;
    payload.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(len));
    return true;
}

class Scanner {
public:
    static constexpr size_t kMaxCandidates = 6;

    ScanResult decode(const uint8_t *gray, size_t width, size_t height, size_t stride) {
        ScanResult r;
        const auto t0 = std::chrono::steady_clock::now();
        if (width > 0 && height > 0 && stride >= width) run(gray, width, height, stride, r);
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    }

private:
    std::vector<uint8_t> out_ = std::vector<uint8_t>(65535);

    static bool refine(const uint8_t *g, size_t w, size_t h, size_t s, const double border[8], ckey_field field, Attempt &a) {
        a.field = field;
        a.score = ckey_refine_quad_gray8(g, w, h, s, border, field, a.quad);
        return a.score >= 0;
    }

    void run(const uint8_t *g, size_t w, size_t h, size_t s, ScanResult &r) {
        double borders[kMaxCandidates * 8];
        const size_t count = ckey_find_borders_gray8(g, w, h, s, borders, kMaxCandidates);
        r.borders = count;
        for (size_t i = 0; i < count && i < kMaxCandidates; ++i) {
            CandidateTrace ct; for (int k = 0; k < 8; ++k) ct.border[k] = borders[8 * i + k];
            const bool done = candidate(g, w, h, s, ct, r);
            r.candidates.push_back(std::move(ct));
            if (done) { r.accepted_candidate = static_cast<int>(i); return; }
        }
    }

    bool accept(ScanResult &r, int profile, size_t len, const Attempt &a) {
        r.accepted = true; r.profile = profile;
        r.payload.assign(out_.begin(), out_.begin() + static_cast<std::ptrdiff_t>(len));
        for (int k = 0; k < 8; ++k) r.accepted_quad[k] = a.quad[k];
        r.corrected = a.corrected; r.erasures = a.erasures;
        return true;
    }

    bool candidate(const uint8_t *g, size_t w, size_t h, size_t s, CandidateTrace &ct, ScanResult &r) {
        ckey_diagnostics d{};
        // Profile 0 first (decodeScalable).
        {
            Attempt a;
            if (refine(g, w, h, s, ct.border, CKEY_FIELD_SCALABLE, a)) {
                size_t len = 0; uint32_t version = 0, layout = 0; d = ckey_diagnostics{};
                const ckey_status st = ckey_decode_scalable_gray8(g, w, h, s, a.quad, out_.data(), out_.size(), &len, &version, &layout, &d);
                a.status = st; a.corrected = d.corrected_symbols; a.erasures = d.erasures_used; a.message = d.message;
                ct.attempts.push_back(a);
                if (st == CKEY_OK && len <= out_.size()) { r.version = version; r.layout = layout; return accept(r, 0, len, a); }
            } else ct.attempts.push_back(a);
        }
        // decodeLegacy: Profile 1 (fixed 16 bytes).
        {
            Attempt a;
            if (refine(g, w, h, s, ct.border, CKEY_FIELD_LOCATOR, a)) {
                d = ckey_diagnostics{};
                const ckey_status st = ckey_decode_gray8(g, w, h, s, a.quad, out_.data(), CKEY_LOCATOR_PAYLOAD_BYTES, &d);
                a.status = st; a.corrected = d.corrected_symbols; a.erasures = d.erasures_used; a.message = d.message;
                ct.attempts.push_back(a);
                if (st == CKEY_OK) return accept(r, 1, CKEY_LOCATOR_PAYLOAD_BYTES, a);
            } else ct.attempts.push_back(a);
        }
        // Profiles 2 and 3, each refined against its own keys.
        {
            Attempt a; size_t len = 0;
            if (refine(g, w, h, s, ct.border, CKEY_FIELD_TEXT, a)) {
                d = ckey_diagnostics{};
                const ckey_status st = ckey_decode_text_gray8(g, w, h, s, a.quad, out_.data(), out_.size(), &len, &d);
                a.status = st; a.corrected = d.corrected_symbols; a.erasures = d.erasures_used; a.message = d.message;
                ct.attempts.push_back(a);
                if (st == CKEY_OK && len <= out_.size()) return accept(r, 2, len, a);
            } else ct.attempts.push_back(a);
        }
        {
            Attempt a; size_t len = 0;
            if (refine(g, w, h, s, ct.border, CKEY_FIELD_EXTENDED, a)) {
                d = ckey_diagnostics{};
                const ckey_status st = ckey_decode_extended_gray8(g, w, h, s, a.quad, out_.data(), out_.size(), &len, &d);
                a.status = st; a.corrected = d.corrected_symbols; a.erasures = d.erasures_used; a.message = d.message;
                ct.attempts.push_back(a);
                if (st == CKEY_OK && len <= out_.size()) return accept(r, 3, len, a);
            } else ct.attempts.push_back(a);
        }
        return false;
    }
};

}  // namespace ckt
#endif
