// ckt_worker: executes case plans against the production CKey reader.
//
//   ckt_worker --plan P.jsonl --results R.jsonl --traces T.jsonl --progress G.log
//              [--images DIR] [--fail-image-cap N] [--success-sample-ppm N]
//              [--crosscheck-strata S01] [--trace-uri-prefix PFX]
//   ckt_worker --repro LINE.json --out DIR       (regenerate one case's frames as PNG)
//   ckt_worker --ml-sites-compare PLAN SITES.ckml OUT.jsonl (site model vs classical reading)
//   ckt_worker --ml-sites PLAN OUT.bin              (site-model training records)
//   ckt_worker --ml-compare PLAN MODEL.ckml|- OUT.jsonl (classical vs ML scan, identical frames)
//   ckt_worker --ml-frames PLAN OUT.bin             (locator/quality training frames)
//   ckt_worker --ml-export PLAN OUT.bin             (ML training records; see runners/native/ml.h)
//   ckt_worker --ml-eval PLAN MODEL.bin OUT.jsonl     (classical vs ML vs hybrid on identical frames)
//   ckt_worker --selftest | --print-schema | --print-config | --capacity-table
//
// Output files are append-only. Each result line is one completed case or
// an explicit HARNESS_ERROR (not a completed case: the supervisor reruns it).
// The progress log brackets every case and every reader call so a crash or
// hang can be attributed to the reader (completed failure) or the harness.
#include "ckey/ckey.h"
#include "runners/native/ml.h"
#include "runners/native/scanner_adapter.h"
#include "runners/native/symbol_factory.h"
#include "simulation/common/sha256.h"
#include "simulation/pipeline.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifndef CKT_BUILD_INFO
#define CKT_BUILD_INFO "{}"
#endif
#ifndef CKT_CKEY_SHA
#define CKT_CKEY_SHA "unknown"
#endif
#ifndef CKT_HARNESS_SHA
#define CKT_HARNESS_SHA "unknown"
#endif

namespace ckt_alloc { extern size_t current, peak; }

namespace ckt {

inline constexpr const char *WORKER_VERSION = "ckt-worker-v1";

struct Options {
    std::string plan, results, traces, progress, images, trace_prefix = "traces";
    int fail_cap = 10, wrong_cap = 2000;
    int success_ppm = 1000;  // success images sampled per million
    std::set<std::string> crosscheck_strata{"S01"};
};

class Progress {
public:
    explicit Progress(const std::string &path) {
        if (!path.empty()) fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    ~Progress() { if (fd_ >= 0) ::close(fd_); }
    void mark(const char *tag, const std::string &id) {
        if (fd_ < 0) return;
        const std::string line = std::string(tag) + " " + id + "\n";
        (void)!::write(fd_, line.data(), line.size());
    }
private:
    int fd_ = -1;
};

static std::string hexs(const std::vector<uint8_t> &v) { return Sha256::of(v.data(), v.size()); }
// Hash of exactly what was rendered (splices hash both halves); the SVG itself is regenerable from the case line.
static std::string svg_hash(const SymbolArt &a) {
    Sha256 h; h.update(a.svg.data(), a.svg.size());
    if (!a.svg_b.empty()) { const std::string sep = "\n#splice " + std::to_string(a.splice) + "\n"; h.update(sep.data(), sep.size()); h.update(a.svg_b.data(), a.svg_b.size()); }
    return h.hex();
}

static std::string quad_json(const double *q) {
    std::string s = "[";
    for (int i = 0; i < 8; ++i) s += (i ? "," : "") + jnum(q[i]);
    return s + "]";
}

static bool mkdirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i + 1 == path.size()) { if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false; }
    }
    return true;
}

static bool write_png(const std::string &path, const std::vector<uint8_t> &gray, int w, int h, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w, x1); y1 = std::min(h, y1);
    if (x1 <= x0 || y1 <= y0) return false;
    const int cw = x1 - x0, ch = y1 - y0;
    std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch);
    for (int y = 0; y < ch; ++y) std::memcpy(&crop[static_cast<size_t>(y) * cw], &gray[static_cast<size_t>(y + y0) * w + x0], cw);
    size_t need = 0;
    ckey_encode_png(crop.data(), cw, ch, cw, CKEY_PNG_GRAY8, 0, nullptr, 0, &need);
    std::vector<uint8_t> png(need);
    if (ckey_encode_png(crop.data(), cw, ch, cw, CKEY_PNG_GRAY8, 0, png.data(), png.size(), &need) != CKEY_OK) return false;
    FILE *f = std::fopen(path.c_str(), "wb"); if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, need, f) == need; std::fclose(f);
    return ok;
}

// Claim one of `cap` image slots for a bin, atomically across workers.
class SlotClaimer {
public:
    explicit SlotClaimer(std::string root) : root_(std::move(root)) {}
    bool claim(const std::string &bin, int cap, std::string &dir) {
        if (root_.empty() || full_.count(bin)) return false;
        dir = root_ + "/" + bin;
        mkdirs(dir + "/.slots");
        for (int k = 0; k < cap; ++k) {
            const std::string slot = dir + "/.slots/" + std::to_string(k);
            const int fd = ::open(slot.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd >= 0) { ::close(fd); return true; }
        }
        full_.insert(bin); return false;
    }
private:
    std::string root_;
    std::set<std::string> full_;
};

struct FrameRecord { ScanResult scan; Frame frame; };

static bool payload_in(const std::vector<uint8_t> &p, const std::vector<std::vector<uint8_t>> &set) {
    for (const auto &q : set) if (q == p) return true;
    return false;
}

class Runner {
public:
    explicit Runner(const Options &o) : opt_(o), slots_(o.images) {}

    // Executes one case; returns the result JSON line (and fills trace).
    std::string run_case(const std::string &line, Progress &prog, std::string &trace, std::vector<FrameRecord> *keep = nullptr) {
        CaseSpec c;
        try { c = parse_case(line); }
        catch (const std::exception &e) { return error_line("", "INVALID_GENERATION", e.what()); }
        prog.mark("BEGIN", c.case_id);
        const size_t mem0 = ckt_alloc::current; ckt_alloc::peak = mem0;
        SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) { prog.mark("END", c.case_id); return error_line(c.case_id, "INVALID_GENERATION", art.error, &c); }
        SymbolArt second; const SymbolArt *sp = nullptr;
        if (c.second_code) {
            second = build_symbol(c, 0xB2ull);
            if (!second.error.empty()) { prog.mark("END", c.case_id); return error_line(c.case_id, "INVALID_GENERATION", "second code: " + second.error, &c); }
            if (second.payload == art.payload) { prog.mark("END", c.case_id); return error_line(c.case_id, "INVALID_GENERATION", "second code payload equals primary", &c); }
            art.allowed.push_back(second.payload);
            sp = &second;
        }
        if (c.expected == "DECODE" && art.allowed.size() != 1) { prog.mark("END", c.case_id); return error_line(c.case_id, "INVALID_GENERATION", "DECODE with multiple allowed payloads; use DECODE_ANY_OF", &c); }

        const int frames = std::clamp(c.frames, 1, 64);
        std::vector<FrameRecord> recs;
        Sha256 img_hash;
        double sim_ms = 0, scan_ms = 0, tts_ms = 0;
        int success_frame = -1;
        const bool cross = opt_.crosscheck_strata.count(c.stratum) > 0;
        for (int fi = 0; fi < frames; ++fi) {
            FrameRecord fr;
            fr.frame = sim_.run(c, art, sp, fi, cross && fi == 0);
            sim_ms += fr.frame.sim_ms;
            const uint32_t dims[2] = {static_cast<uint32_t>(fr.frame.w), static_cast<uint32_t>(fr.frame.h)};
            img_hash.update(dims, sizeof dims); img_hash.update(fr.frame.gray.data(), fr.frame.gray.size());
            prog.mark("SCAN", c.case_id + "#" + std::to_string(fi));
            fr.scan = scanner_.decode(fr.frame.gray.data(), fr.frame.w, fr.frame.h, fr.frame.w);
            prog.mark("SCANNED", c.case_id + "#" + std::to_string(fi));
            scan_ms += fr.scan.ms;
            tts_ms += std::max(fr.scan.ms, frames > 1 ? 50.0 : 0.0);  // the iOS reader samples at most 20 frames/s
            recs.push_back(std::move(fr));
            if (recs.back().scan.accepted) { success_frame = fi; break; }  // the reader publishes the first decode
        }
        const size_t peak = ckt_alloc::peak - mem0;
        const FrameRecord &first = recs.front();
        const FrameRecord &last = recs.back();
        const ScanResult &fin = last.scan;
        const Frame &fr0 = first.frame;

        // Outcome against expectation.
        const bool accepted = fin.accepted;
        std::string match = "none";
        if (accepted) match = fin.payload == art.payload ? "A" : payload_in(fin.payload, art.allowed) ? "ALLOWED" : "OTHER";
        const bool wrong = accepted && !payload_in(fin.payload, art.allowed);
        bool success;
        if (c.expected == "DECODE" || c.expected == "DECODE_ANY_OF") success = accepted && !wrong;
        else if (c.expected == "REJECT") success = !accepted;
        else success = !wrong;  // NO_WRONG
        const std::string observed = !accepted ? "NO_READ" : wrong ? "WRONG_PAYLOAD" : "DECODED";

        // Geometry diagnostics on the frame the outcome came from.
        const Frame &fx = last.frame;
        int gt_rank = -1;
        double loc_err = -1; uint32_t fec = fin.corrected, era = fin.erasures;
        std::string reason;
        const int want_field = art.profile;  // ckey_field enum equals the profile index
        if (fx.gt_valid || fx.gt2_valid) {
            for (size_t i = 0; i < fin.candidates.size() && gt_rank < 0; ++i) {
                const double side = std::max(8.0, std::hypot(fx.gt[2] - fx.gt[0], fx.gt[3] - fx.gt[1]));
                if ((fx.gt_valid && quad_distance(fin.candidates[i].border, fx.gt) < 0.08 * side + 3) ||
                    (fx.gt2_valid && quad_distance(fin.candidates[i].border, fx.gt2) < 0.08 * side + 3)) gt_rank = static_cast<int>(i);
            }
        }
        const Attempt *diag = nullptr;
        if (gt_rank >= 0) for (const auto &a : fin.candidates[gt_rank].attempts) if (a.field == want_field) diag = &a;
        if (accepted && fx.gt_valid) loc_err = std::min(quad_distance(fin.accepted_quad, fx.gt), fx.gt2_valid ? quad_distance(fin.accepted_quad, fx.gt2) : 1e18);
        else if (diag && diag->score >= 0 && fx.gt_valid) loc_err = quad_distance(diag->quad, fx.gt);
        if (!accepted && diag) { fec = diag->corrected; era = diag->erasures; }
        // Triage only: would the production decoder read this frame given the true field corners?
        int gt_quad_decodes = -1;
        if (!success && art.must_accept && fx.gt_valid) {
            std::vector<uint8_t> pl;
            gt_quad_decodes = decode_with_quad(art.profile, fx.gray.data(), fx.w, fx.h, fx.w, fx.gt, pl) && payload_in(pl, art.allowed) ? 1 : 0;
        }
        if (!success) {
            if (wrong) reason = art.must_accept || !art.allowed.empty() ? "WRONG_PAYLOAD" : "FALSE_ACCEPT";
            else if (c.expected == "REJECT" || c.expected == "NO_WRONG") reason = "FALSE_ACCEPT";
            else if (fin.borders == 0) reason = "NO_BORDER";
            else if (gt_rank < 0) reason = "BORDER_MISS";
            else if (!diag || diag->score < 0) reason = "REFINE_REJECTED";
            else if (diag->status == CKEY_UNCORRECTABLE) reason = "UNCORRECTABLE";
            else if (diag->status == CKEY_INTEGRITY_FAILURE) reason = "INTEGRITY_FAILURE";
            else reason = "DECODE_STATUS_" + std::to_string(diag->status);
        }

        // Images: wrong payloads (full frame), capped failures per bin, sampled successes (crops).
        std::string image_path;
        if (!opt_.images.empty()) {
            std::string dir; bool save = false, full = false;
            const std::string bin = c.stratum + "_" + c.profile + "_" + (success ? std::string("ok") : reason);
            if (wrong) { save = slots_.claim("wrong_payload", opt_.wrong_cap, dir); full = true; }
            else if (!success) save = slots_.claim(bin, opt_.fail_cap, dir);
            else if (std::strtoull(Sha256::of(c.case_id.data(), c.case_id.size()).substr(0, 8).c_str(), nullptr, 16) % 1000000 < static_cast<unsigned long long>(opt_.success_ppm)) save = slots_.claim(bin, opt_.fail_cap, dir);
            if (save) {
                double bx0 = 0, by0 = 0, bx1 = fx.w, by1 = fx.h;
                if (fx.gt_valid && !full) {
                    bx0 = 1e18; by0 = 1e18; bx1 = -1e18; by1 = -1e18;
                    for (int i = 0; i < 4; ++i) { bx0 = std::min(bx0, fx.gt[2 * i]); bx1 = std::max(bx1, fx.gt[2 * i]); by0 = std::min(by0, fx.gt[2 * i + 1]); by1 = std::max(by1, fx.gt[2 * i + 1]); }
                    const double m = 0.3 * std::max(bx1 - bx0, by1 - by0); bx0 -= m; by0 -= m; bx1 += m; by1 += m;
                }
                image_path = dir + "/" + c.case_id + ".png";
                if (!write_png(image_path, fx.gray, fx.w, fx.h, static_cast<int>(bx0), static_cast<int>(by0), static_cast<int>(bx1), static_cast<int>(by1))) image_path.clear();
            }
        }

        // Trace line.
        std::ostringstream t;
        t << "{\"case_id\":" << jstr(c.case_id) << ",\"gt\":" << quad_json(fx.gt) << ",\"frames\":[";
        for (size_t fi = 0; fi < recs.size(); ++fi) {
            const auto &s = recs[fi].scan;
            t << (fi ? "," : "") << "{\"borders\":" << s.borders << ",\"accepted\":" << (s.accepted ? "true" : "false") << ",\"profile\":" << s.profile
              << ",\"ms\":" << jnum(s.ms) << ",\"sim_ms\":" << jnum(recs[fi].frame.sim_ms) << ",\"cands\":[";
            for (size_t k = 0; k < s.candidates.size(); ++k) {
                t << (k ? "," : "") << "{\"border\":" << quad_json(s.candidates[k].border) << ",\"att\":[";
                for (size_t j = 0; j < s.candidates[k].attempts.size(); ++j) {
                    const auto &a = s.candidates[k].attempts[j];
                    t << (j ? "," : "") << "[" << a.field << "," << jnum(a.score) << "," << a.status << "," << a.corrected << "," << a.erasures << "," << jstr(a.message) << "]";
                }
                t << "]}";
            }
            t << "]}";
        }
        t << "]}";
        trace = t.str();

        // Result line (schemas/result.json).
        const Phys &p = fx.phys;
        std::ostringstream o;
        o << "{\"status\":\"COMPLETED\",\"case_id\":" << jstr(c.case_id) << ",\"cohort\":" << jstr(c.cohort) << ",\"stratum\":" << jstr(c.stratum)
          << ",\"profile\":" << jstr(c.profile) << ",\"mode\":" << jstr(c.mode) << ",\"sev_bin\":" << c.sev_bin << ",\"factor\":" << jstr(c.factor)
          << ",\"severity\":" << jnum(c.severity) << ",\"payload_family\":" << jstr(c.payload_family)
          << ",\"payload_hash\":" << jstr(hexs(art.payload)) << ",\"svg_sha256\":" << jstr(svg_hash(art)) << ",\"payload_len\":" << art.payload.size() << ",\"format_version\":" << jstr(art.format)
          << ",\"seed\":" << jstr(c.seed) << ",\"renderer_sha\":" << jstr(CKT_CKEY_SHA) << ",\"scanner_sha\":" << jstr(CKT_CKEY_SHA)
          << ",\"model_sha\":\"none\",\"harness_sha\":" << jstr(CKT_HARNESS_SHA) << ",\"camera_model\":" << jstr(c.device)
          << ",\"transformations\":" << jstr(summary(c)) << ",\"image_hash\":" << jstr(img_hash.hex())
          << ",\"expected_outcome\":" << jstr(c.expected) << ",\"observed_outcome\":" << jstr(observed) << ",\"success\":" << (success ? "true" : "false")
          << ",\"wrong_payload\":" << (wrong ? "true" : "false") << ",\"accepted_match\":" << jstr(match) << ",\"accepted_profile\":" << fin.profile
          << ",\"neg_kind\":" << jstr(c.neg_kind)
          << ",\"finder_detected\":" << (fx.gt_valid ? (gt_rank >= 0 ? "true" : "false") : "null")
          << ",\"orientation_correct\":" << (accepted ? (wrong ? "false" : "true") : "null")
          << ",\"localization_error\":" << (loc_err >= 0 ? jnum(loc_err) : "null")
          << ",\"localization_error_modules\":" << (loc_err >= 0 && p.ppm_mean > 0 ? jnum(loc_err / p.ppm_mean) : "null")
          << ",\"FEC_corrections\":" << fec << ",\"erasures\":" << era << ",\"frame_count\":" << recs.size() << ",\"frames_planned\":" << frames
          << ",\"success_frame\":" << success_frame << ",\"elapsed_ms\":" << jnum(scan_ms) << ",\"decode_ms_first\":" << jnum(first.scan.ms)
          << ",\"tts_ms\":" << jnum(tts_ms) << ",\"sim_ms\":" << jnum(sim_ms) << ",\"peak_memory\":" << peak
          << ",\"failure_reason\":" << jstr(reason) << ",\"trace_uri\":" << jstr(opt_.trace_prefix + "#" + c.case_id)
          << ",\"image_uri\":" << jstr(image_path) << ",\"borders\":" << fin.borders << ",\"gt_rank\":" << gt_rank << ",\"gt_quad_decodes\":" << gt_quad_decodes
          << ",\"frame_w\":" << fr0.w << ",\"frame_h\":" << fr0.h << ",\"p0_version\":" << art.p0_version << ",\"read_version\":" << fin.version
          << ",\"phys_class\":" << jstr(p.cls) << ",\"phys_reason\":" << jstr(p.reason) << ",\"ppm_min\":" << jnum(p.ppm_min) << ",\"ppm_mean\":" << jnum(p.ppm_mean)
          << ",\"blur_px\":" << jnum(p.blur_sigma_px) << ",\"blur_modules\":" << jnum(p.ppm_mean > 0 ? p.blur_sigma_px / p.ppm_mean : -1) << ",\"mtf\":" << jnum(p.mtf)
          << ",\"dprime\":" << jnum(std::min(p.dprime, 1e6)) << ",\"contrast\":" << jnum(p.contrast) << ",\"occluded\":" << jnum(p.occluded)
          << ",\"saturated\":" << jnum(p.saturated) << ",\"glare_loss\":" << jnum(p.glare_loss) << ",\"byte_err_est\":" << jnum(p.byte_err_est) << ",\"in_frame\":" << (p.in_frame ? "true" : "false")
          << ",\"ecc_fraction\":" << jnum(art.ecc_fraction) << ",\"rs_verified\":" << art.rs_verified << ",\"crosscheck\":" << jnum(fr0.crosscheck)
          << ",\"texture_px\":" << jnum(fr0.texture_px) << ",\"electrons_white\":" << jnum(fr0.electrons_white) << ",\"digital_gain\":" << jnum(fr0.digital_gain)
          << ",\"versions\":" << jstr(std::string(WORKER_VERSION) + "|" + SIM_VERSION + "|" + ADAPTER_VERSION + "|" + RECOVERABILITY_VERSION + "|" + CASE_SCHEMA_VERSION) << "}";
        prog.mark("END", c.case_id);
        if (keep) *keep = std::move(recs);
        return o.str();
    }

private:
    static std::string summary(const CaseSpec &c) {
        char b[512];
        std::snprintf(b, sizeof b, "sub=%s d=%.0fmm sym=%.1fmm roll=%.1f yaw=%.1f pitch=%.1f bend=%.0f defoc=%.2f/%.2f mot=%.1f@%.0f rs=%.2f illum=%.3f glare=%d occl=%s:%.3f jpeg=%d rescale=%.2f frames=%d neg=%s",
                      c.substrate.c_str(), c.distance_mm, c.symbol_mm, c.roll, c.yaw, c.pitch, c.bend_radius_mm, c.defocus_gauss_px, c.defocus_disc_px, c.motion_px, c.motion_angle,
                      c.rs_ratio, c.illum, c.glare_spots, c.occl_kind.c_str(), c.occl_frac, c.jpeg_q, c.rescale, c.frames, c.neg_kind.c_str());
        return b;
    }
    std::string error_line(const std::string &id, const char *status, const std::string &why, const CaseSpec *c = nullptr) {
        std::ostringstream o;
        o << "{\"status\":\"" << status << "\",\"case_id\":" << jstr(id);
        if (c) o << ",\"cohort\":" << jstr(c->cohort) << ",\"stratum\":" << jstr(c->stratum) << ",\"profile\":" << jstr(c->profile);
        o << ",\"error\":" << jstr(why) << "}";
        return o.str();
    }

    Options opt_;
    Simulator sim_;
    Scanner scanner_;
    SlotClaimer slots_;
};

std::string capacity_table() {
    std::ostringstream o;
    o << "{\"p0\":[";
    for (uint32_t v = 1; v <= CKEY_SCALABLE_VERSION_COUNT; ++v) {
        o << (v > 1 ? "," : "") << "{\"version\":" << v << ",\"modules\":" << ckey_scalable_modules_across(v);
        for (int layout = 0; layout < 2; ++layout) {
            o << ",\"" << (layout ? "print" : "standard") << "\":[";
            for (int l = 0; l < 4; ++l) o << (l ? "," : "") << ckey_scalable_capacity(v, static_cast<ckey_ecc_level>(l), static_cast<ckey_scalable_layout>(layout));
            o << "]";
        }
        o << "}";
    }
    o << "],\"p1\":" << CKEY_LOCATOR_PAYLOAD_BYTES << ",\"p2\":" << CKEY_TEXT_PAYLOAD_MAX_BYTES << ",\"p3_tiers\":[256,384,512,768,1024,1536,2048],\"p3_max\":" << CKEY_EXTENDED_PAYLOAD_MAX_BYTES
      << ",\"software_version\":" << jstr(ckey_get_software_version()) << ",\"abi\":" << ckey_get_abi_version() << "}";
    return o.str();
}

std::string config_json() {
    std::ostringstream o;
    o << "{\"worker\":" << jstr(WORKER_VERSION) << ",\"sim\":" << jstr(SIM_VERSION) << ",\"adapter\":" << jstr(ADAPTER_VERSION)
      << ",\"recoverability\":" << jstr(RECOVERABILITY_VERSION) << ",\"prng\":" << jstr(PRNG_VERSION) << ",\"schema\":" << jstr(CASE_SCHEMA_VERSION)
      << ",\"ckey_sha\":" << jstr(CKT_CKEY_SHA) << ",\"harness_sha\":" << jstr(CKT_HARNESS_SHA) << ",\"ckey_software_version\":" << jstr(ckey_get_software_version())
      << ",\"recov_thresholds\":{\"unrecoverable\":{\"ppm_min_lt\":1.0,\"dprime_lt\":1.0,\"mtf_contrast_lt\":0.01,\"byte_err_gt\":\"2*ecc\",\"cropped\":true},"
      << "\"marginal\":{\"ppm_min_lt\":2.0,\"dprime_lt\":3.0,\"byte_err_gt\":\"ecc\"}},\"build\":" << CKT_BUILD_INFO << "}";
    return o.str();
}

int selftest();

// ---- ML localization option -----------------------------------------------------
static bool valid_p0(const CaseSpec &c) { return c.profile == "P0" && c.neg_kind == "none" && c.second_code == 0; }

static int stratum_num(const std::string &s) { return s.size() == 3 && s[0] == 'S' ? std::atoi(s.c_str() + 1) : 0; }

// One record per production border candidate of every valid P0 case.
int ml_export(const std::string &plan_path, const std::string &out_path) {
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "ab");
    if (!plan || !out) { std::fprintf(stderr, "ml-export: cannot open files\n"); return 3; }
    Simulator sim; Scanner scanner; std::string line; size_t cases = 0, records = 0, matched = 0;
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (!valid_p0(c)) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        if (!f.gt_valid) continue;
        const ScanResult sr = scanner.decode(f.gray.data(), f.w, f.h, f.w);
        std::vector<uint8_t> pl;
        const bool gtq = decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, f.gt, pl) && pl == art.payload;
        const std::string ch = Sha256::of(c.case_id.data(), c.case_id.size());
        ++cases;
        for (size_t i = 0; i < sr.candidates.size(); ++i) {
            const auto &cand = sr.candidates[i];
            MlRecord r;
            for (int k = 0; k < 16; ++k) r.case_hash[k] = static_cast<uint8_t>(std::stoi(ch.substr(2 * k, 2), nullptr, 16));
            const double side = std::max(8.0, std::hypot(f.gt[2] - f.gt[0], f.gt[3] - f.gt[1]));
            r.cand_idx = static_cast<uint8_t>(i); r.matched = quad_distance(cand.border, f.gt) < 0.08 * side + 3;
            r.reader_ok = sr.accepted && sr.payload == art.payload; r.gtq_ok = gtq; r.layout = c.p0_layout == "print";
            r.p0_version = static_cast<uint8_t>(art.p0_version); r.stratum = static_cast<uint8_t>(stratum_num(c.stratum));
            r.ppm = static_cast<float>(f.phys.ppm_mean);
            int perm[4]; quad_perm(cand.border, f.gt, perm);
            const Attempt *ref = nullptr;
            for (const auto &a : cand.attempts) if (a.field == CKEY_FIELD_SCALABLE && a.score >= 0) ref = &a;
            int rperm[4]; if (ref) quad_perm(cand.border, ref->quad, rperm);
            for (int k = 0; k < 4; ++k) {
                const LocalFrame lf = corner_frame(cand.border, k);
                r.side_px = static_cast<float>(lf.side);
                sample_patch(f.gray.data(), f.w, f.h, lf, r.patch[k]);
                const float nan = std::numeric_limits<float>::quiet_NaN();
                if (!r.matched || !to_local(lf, f.gt[2 * perm[k]], f.gt[2 * perm[k] + 1], r.label[k])) r.label[k][0] = r.label[k][1] = nan;
                if (!ref || !to_local(lf, ref->quad[2 * rperm[k]], ref->quad[2 * rperm[k] + 1], r.refined[k])) r.refined[k][0] = r.refined[k][1] = nan;
            }
            matched += r.matched;
            std::fwrite(&r, sizeof r, 1, out); ++records;
        }
    }
    std::fclose(out);
    std::fprintf(stderr, "ml-export: %zu cases, %zu records (%zu on the symbol)\n", cases, records, matched);
    return 0;
}


#if defined(CKEY_SCAN_PROFILE_SCALABLE)  // ckey_scan_frame_gray8 (core build 86 and later)
// ---- ML frame export (locator / quality network) -------------------------------------
// One fixed-size record per case: the frame area-downscaled so its longer side is
// kMlIn pixels, letterboxed (pad 128) into kMlIn x kMlIn, plus labels. Includes
// negatives (no symbol). Labels: ground-truth field corners in input pixels (canonical
// symbol order), version, layout, whether the production one-call scan decodes the full
// frame, and whether the decoder reads it from the true corners.
constexpr int kMlIn = 256;
#pragma pack(push, 1)
struct MlFrameRec {
    uint32_t magic = 0x464D4B43;   // "CKMF"
    uint32_t version = 1;
    uint8_t case_hash[16]{};
    uint8_t has_symbol = 0, scan_ok = 0, gtq_ok = 0, layout = 0;
    uint8_t p0_version = 0, stratum = 0, neg = 0, phys = 0;   // phys: 0 recoverable 1 marginal 2 unrecoverable 3 n/a
    float ppm = 0, scale = 0, off_x = 0, off_y = 0;           // input = (frame - 0) * scale + off
    float scan_ms = 0;
    uint16_t frame_w = 0, frame_h = 0;
    float quad[8]{};
    uint8_t img[kMlIn * kMlIn]{};
};
#pragma pack(pop)

static void ml_downscale(const Frame &f, MlFrameRec &r) {
    const double sc = static_cast<double>(kMlIn) / std::max(f.w, f.h);
    const int ow = std::max(1, static_cast<int>(std::lround(f.w * sc))), oh = std::max(1, static_cast<int>(std::lround(f.h * sc)));
    const int ox = (kMlIn - ow) / 2, oy = (kMlIn - oh) / 2;
    std::memset(r.img, 128, sizeof r.img);
    for (int y = 0; y < oh; ++y) {
        const int y0 = static_cast<int>(y / sc), y1 = std::min(f.h, std::max(y0 + 1, static_cast<int>((y + 1) / sc)));
        for (int x = 0; x < ow; ++x) {
            const int x0 = static_cast<int>(x / sc), x1 = std::min(f.w, std::max(x0 + 1, static_cast<int>((x + 1) / sc)));
            uint32_t acc = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) for (int xx = x0; xx < x1; ++xx) { acc += f.gray[static_cast<size_t>(yy) * f.w + xx]; ++n; }
            r.img[(oy + y) * kMlIn + ox + x] = static_cast<uint8_t>(acc / std::max<uint32_t>(1, n));
        }
    }
    r.scale = static_cast<float>(sc); r.off_x = static_cast<float>(ox); r.off_y = static_cast<float>(oy);
    r.frame_w = static_cast<uint16_t>(f.w); r.frame_h = static_cast<uint16_t>(f.h);
}

int ml_frames(const std::string &plan_path, const std::string &out_path) {
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "ab");
    if (!plan || !out) { std::fprintf(stderr, "ml-frames: cannot open files\n"); return 3; }
    Simulator sim; std::string line; size_t n = 0, sym = 0, ok = 0;
    std::vector<uint8_t> payload(65536);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        MlFrameRec r;
        const std::string ch = Sha256::of(c.case_id.data(), c.case_id.size());
        for (int k = 0; k < 16; ++k) r.case_hash[k] = static_cast<uint8_t>(std::stoi(ch.substr(2 * k, 2), nullptr, 16));
        const bool valid = c.neg_kind == "none";
        r.neg = !valid; r.layout = c.p0_layout == "print"; r.p0_version = static_cast<uint8_t>(art.p0_version);
        r.stratum = static_cast<uint8_t>(stratum_num(c.stratum)); r.ppm = static_cast<float>(f.phys.ppm_mean);
        r.phys = 3;
        ml_downscale(f, r);
        // Negatives that still draw a symbol-shaped thing (mirror, corruption) keep their
        // geometry as "a symbol is here": the locator finds it, RS + CRC reject it.
        r.has_symbol = f.gt_valid && c.neg_kind != "blank" && c.neg_kind != "foreign_grid";
        if (f.gt_valid) for (int k = 0; k < 4; ++k) { r.quad[2 * k] = static_cast<float>(f.gt[2 * k] * r.scale + r.off_x); r.quad[2 * k + 1] = static_cast<float>(f.gt[2 * k + 1] * r.scale + r.off_y); }
        ckey_scan_options o; ckey_scan_options_init(&o); o.profiles = CKEY_SCAN_PROFILE_SCALABLE;
        ckey_scan_result res{}; res.abi_version = CKEY_ABI_VERSION;
        const auto t0 = std::chrono::steady_clock::now();
        const ckey_status st = ckey_scan_frame_gray8(f.gray.data(), f.w, f.h, f.w, &o, payload.data(), payload.size(), &res);
        r.scan_ms = static_cast<float>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        r.scan_ok = valid && st == CKEY_OK && res.payload_length == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin());
        if (valid && f.gt_valid) { std::vector<uint8_t> pl; r.gtq_ok = decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, f.gt, pl) && pl == art.payload; }
        std::fwrite(&r, sizeof r, 1, out); ++n; sym += r.has_symbol; ok += r.scan_ok;
    }
    std::fclose(out);
    std::fprintf(stderr, "ml-frames: %zu records, %zu with a symbol, %zu decoded by the scan\n", n, sym, ok);
    return 0;
}


#endif

#if defined(CKEY_SCAN_OPTION_ML)  // ML locator API (core build 87 and later)

// No ML models at all (built-ins included) where the core supports it.
static void ml_none() {
#if defined(CKEY_ML_HAS_CLEAR)
    ckey_ml_clear_models();
#else
    ckey_ml_load_model(nullptr, 0);
#endif
}

// Site model on identical frames: decode from the true corners and the full one-call scan,
// each without and with the site model. One JSON line per case; wrong payloads reported.
int ml_sites_compare(const std::string &plan_path, const std::string &model_path, const std::string &out_path) {
    std::ifstream mf(model_path, std::ios::binary); const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(mf)), {});
    if (ckey_ml_load_model(blob.data(), blob.size()) != CKEY_OK || ckey_ml_site_model_id() == 0) { std::fprintf(stderr, "ml-sites-compare: bad site model\n"); return 3; }
    ml_none();
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "a");
    if (!plan || !out) { std::fprintf(stderr, "ml-sites-compare: cannot open files\n"); return 3; }
    Simulator sim; std::string line; std::vector<uint8_t> payload(65536);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        const bool valid = c.neg_kind == "none";
        std::fprintf(out, "{\"case_id\":%s,\"stratum\":%s,\"valid\":%d,\"v\":%u,\"print\":%d,\"ppm\":%.3f", jstr(c.case_id).c_str(), jstr(c.stratum).c_str(), valid ? 1 : 0,
                     art.p0_version, c.p0_layout == "print" ? 1 : 0, f.phys.ppm_mean);
        const auto right = [&](size_t len) { return valid && len == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin()); };
        const auto mirror_ok = [&](size_t len) { return c.neg_kind == "mirror" && len == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin()); };
        for (int arm = 0; arm < 2; ++arm) {
            if (arm == 1) { ml_none(); ckey_ml_load_model(blob.data(), blob.size()); } else ml_none();
            const char *tag = arm ? "ml" : "classical";
            if (f.gt_valid) {
                size_t len = 0; uint32_t v = 0, l = 0; ckey_diagnostics d{};
                const auto t0 = std::chrono::steady_clock::now();
                const ckey_status st = ckey_decode_scalable_gray8(f.gray.data(), f.w, f.h, f.w, f.gt, payload.data(), payload.size(), &len, &v, &l, &d);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const bool ok = st == CKEY_OK && right(len), wrong = st == CKEY_OK && !ok && !mirror_ok(len);
                std::fprintf(out, ",\"gtq_%s\":{\"ok\":%d,\"wrong\":%d,\"ms\":%.2f,\"ml\":%d,\"erasures\":%u}", tag, ok ? 1 : 0, wrong ? 1 : 0, ms,
                             st == CKEY_OK && std::strstr(d.message, "ML site") ? 1 : 0, d.erasures_used);
            }
            ckey_scan_options o; ckey_scan_options_init(&o); o.profiles = CKEY_SCAN_PROFILE_SCALABLE;
            ckey_scan_result res{}; res.abi_version = CKEY_ABI_VERSION;
            const auto t0 = std::chrono::steady_clock::now();
            const ckey_status st = ckey_scan_frame_gray8(f.gray.data(), f.w, f.h, f.w, &o, payload.data(), payload.size(), &res);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool ok = st == CKEY_OK && right(res.payload_length), wrong = st == CKEY_OK && !ok && !mirror_ok(res.payload_length);
            std::fprintf(out, ",\"scan_%s\":{\"ok\":%d,\"wrong\":%d,\"ms\":%.2f,\"ml\":%d}", tag, ok ? 1 : 0, wrong ? 1 : 0, ms, st == CKEY_OK && std::strstr(res.message, "ML site") ? 1 : 0);
        }
        ml_none();
        std::fprintf(out, "}\n"); std::fflush(out);
    }
    std::fclose(out);
    return 0;
}

// ML site-model training records (variable length, little-endian):
//   "CKMS", u32 version=1, u8 case_hash[16], u8 stratum, p0_version, layout, dark, gtq_ok, pad[3],
//   f32 ppm, f32 jitter_modules, u32 side, u32 sites, u8 px[side*side],
//   per site: u16 x, u16 y (canonical units * 64), u8 kind, u8 printed.
// The field is rectified through the true corners plus Gaussian corner noise
// (sigma = jitter, in module pitches), because the decoder's corners are never exact.
int ml_sites_export(const std::string &plan_path, const std::string &out_path) {
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "ab");
    if (!plan || !out) { std::fprintf(stderr, "ml-sites: cannot open files\n"); return 3; }
    Simulator sim; std::string line; size_t n = 0;
    std::vector<uint8_t> px(2048 * 2048), kind, printed; std::vector<float> xy;
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code || c.neg_kind != "none") continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty() || !art.has_p0) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        if (!f.gt_valid) continue;
        const std::string ch = Sha256::of(c.case_id.data(), c.case_id.size());
        uint8_t hash[16]; for (int k = 0; k < 16; ++k) hash[k] = static_cast<uint8_t>(std::stoi(ch.substr(2 * k, 2), nullptr, 16));
        // Corner noise from the case hash (deterministic).
        uint64_t st = 0; for (int k = 0; k < 8; ++k) st = st << 8 | hash[k];
        auto unit = [&]() { st = st * 6364136223846793005ull + 1442695040888963407ull; return ((st >> 11) & ((1ull << 53) - 1)) / 9007199254740992.0; };
        auto gauss = [&]() { const double u = std::max(1e-12, unit()), v = unit(); return std::sqrt(-2 * std::log(u)) * std::cos(6.283185307179586 * v); };
        const double jitter = unit() < 0.2 ? 0.0 : 0.3 * unit();
        const double side_px = std::hypot(f.gt[2] - f.gt[0], f.gt[3] - f.gt[1]);
        const double module_px = side_px / std::max(1u, ckey_scalable_modules_across(art.p0_version));
        double q[8]; for (int k = 0; k < 8; ++k) q[k] = f.gt[k] + gauss() * jitter * module_px;
        uint32_t side = 0; int dark = 1;
        if (ckey_ml_rectify_gray8(f.gray.data(), f.w, f.h, f.w, q, art.p0_version, 2, 8, px.data(), px.size(), &side, &dark) != CKEY_OK) continue;
        size_t sites = 0;
        ckey_ml_site_map(&art.p0, nullptr, nullptr, nullptr, 0, &sites);
        xy.resize(2 * sites); kind.resize(sites); printed.resize(sites);
        if (ckey_ml_site_map(&art.p0, xy.data(), kind.data(), printed.data(), sites, &sites) != CKEY_OK) continue;
        std::vector<uint8_t> pl;
        const uint8_t gtq = decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, f.gt, pl) && pl == art.payload;
        const uint32_t magic = 0x534D4B43, version = 1;
        const uint8_t meta[8] = {static_cast<uint8_t>(stratum_num(c.stratum)), static_cast<uint8_t>(art.p0_version), static_cast<uint8_t>(art.p0.layout), static_cast<uint8_t>(dark), gtq, 0, 0, 0};
        const float ppm = static_cast<float>(f.phys.ppm_mean), jit = static_cast<float>(jitter);
        const uint32_t ns = static_cast<uint32_t>(sites);
        std::fwrite(&magic, 4, 1, out); std::fwrite(&version, 4, 1, out); std::fwrite(hash, 16, 1, out); std::fwrite(meta, 8, 1, out);
        std::fwrite(&ppm, 4, 1, out); std::fwrite(&jit, 4, 1, out); std::fwrite(&side, 4, 1, out); std::fwrite(&ns, 4, 1, out);
        std::fwrite(px.data(), 1, static_cast<size_t>(side) * side, out);
        for (size_t i = 0; i < sites; ++i) {
            const uint16_t x = static_cast<uint16_t>(std::lround(std::clamp(xy[2 * i], 0.0f, 29.0f) * 64)), y = static_cast<uint16_t>(std::lround(std::clamp(xy[2 * i + 1], 0.0f, 29.0f) * 64));
            std::fwrite(&x, 2, 1, out); std::fwrite(&y, 2, 1, out); std::fwrite(&kind[i], 1, 1, out); std::fwrite(&printed[i], 1, 1, out);
        }
        ++n;
    }
    std::fclose(out);
    std::fprintf(stderr, "ml-sites: %zu records\n", n);
    return 0;
}
// Classical one-call scan vs ML proposals vs ML + gate on identical frames.
// One JSON line per case. A wrong payload in any arm is reported (and must stay 0).

#if defined(CKEY_SCAN_VERSION_HINT)
// Ceiling of the version hint: the full scan without and with the true version as the
// hint (the best a density estimate could do). No ML models in either arm.
int ml_hint_compare(const std::string &plan_path, const std::string &out_path) {
    ml_none();
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "a");
    if (!plan || !out) { std::fprintf(stderr, "ml-hint-compare: cannot open files\n"); return 3; }
    Simulator sim; std::string line; std::vector<uint8_t> payload(65536);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        const bool valid = c.neg_kind == "none";
        std::fprintf(out, "{\"case_id\":%s,\"stratum\":%s,\"valid\":%d,\"v\":%u,\"print\":%d", jstr(c.case_id).c_str(), jstr(c.stratum).c_str(), valid ? 1 : 0, art.p0_version, c.p0_layout == "print" ? 1 : 0);
        for (int arm = 0; arm < 2; ++arm) {
            ckey_scan_options o; ckey_scan_options_init(&o); o.profiles = CKEY_SCAN_PROFILE_SCALABLE;
            if (arm) o.flags |= CKEY_SCAN_VERSION_HINT(art.p0_version);
            ckey_scan_result res{}; res.abi_version = CKEY_ABI_VERSION;
            const auto t0 = std::chrono::steady_clock::now();
            const ckey_status st = ckey_scan_frame_gray8(f.gray.data(), f.w, f.h, f.w, &o, payload.data(), payload.size(), &res);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool ok = st == CKEY_OK && valid && res.payload_length == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin());
            const bool mirror_ok = c.neg_kind == "mirror" && st == CKEY_OK && res.payload_length == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin());
            std::fprintf(out, ",\"%s\":{\"ok\":%d,\"wrong\":%d,\"ms\":%.2f}", arm ? "hint" : "classical", ok ? 1 : 0, (st == CKEY_OK && !ok && !mirror_ok) ? 1 : 0, ms);
        }
        std::fprintf(out, "}\n"); std::fflush(out);
    }
    std::fclose(out);
    return 0;
}
#endif


#if defined(CKEY_ML_HAS_CLEAR)
// ---- corner refiner records -----------------------------------------------------------
// Homography canonical (0,0),(29,0),(29,29),(0,29) -> quad, and its inverse mapping.
static bool homography_from(const double q[8], double H[9]) {
    const double src[8] = {0, 0, 29, 0, 29, 29, 0, 29};
    double A[8][9];
    for (int k = 0; k < 4; ++k) {
        const double x = src[2 * k], y = src[2 * k + 1], u = q[2 * k], v = q[2 * k + 1];
        double r1[9] = {x, y, 1, 0, 0, 0, -u * x, -u * y, u}, r2[9] = {0, 0, 0, x, y, 1, -v * x, -v * y, v};
        std::copy(r1, r1 + 9, A[2 * k]); std::copy(r2, r2 + 9, A[2 * k + 1]);
    }
    for (int c = 0; c < 8; ++c) {
        int p = c; for (int r = c + 1; r < 8; ++r) if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
        if (std::abs(A[p][c]) < 1e-12) return false;
        std::swap(A[c], A[p]);
        for (int r = 0; r < 8; ++r) if (r != c) { const double f = A[r][c] / A[c][c]; for (int j = c; j < 9; ++j) A[r][j] -= f * A[c][j]; }
    }
    for (int i = 0; i < 8; ++i) H[i] = A[i][8] / A[i][i];
    H[8] = 1; return true;
}
static bool unmap(const double H[9], double u, double v, double &x, double &y) {
    // Solve H [x y 1] ~ [u v 1].
    const double a = H[0] - u * H[6], b = H[1] - u * H[7], c = u * H[8] - H[2];
    const double d = H[3] - v * H[6], e = H[4] - v * H[7], f = v * H[8] - H[5];
    const double det = a * e - b * d; if (std::abs(det) < 1e-12) return false;
    x = (c * e - b * f) / det; y = (a * f - c * d) / det; return true;
}

//   "CKMR", u32 version=1, u8 case_hash[16], u8 stratum, p0_version, layout, gtq_ok, f32 sigma_modules,
//   f32 target[8] (canonical units: true corner i in the given corners' frame minus canonical corner i),
//   u8 px[128*128] (canonical [-3, 32]^2 through the given corners)
constexpr uint32_t kRefineSide = 128;
int ml_refine_export(const std::string &plan_path, const std::string &out_path) {
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "ab");
    if (!plan || !out) { std::fprintf(stderr, "ml-refine: cannot open files\n"); return 3; }
    Simulator sim; std::string line; size_t n = 0; std::vector<uint8_t> px(kRefineSide * kRefineSide);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code || c.neg_kind != "none") continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty() || !art.has_p0) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        if (!f.gt_valid) continue;
        const std::string ch = Sha256::of(c.case_id.data(), c.case_id.size());
        uint8_t hash[16]; for (int k = 0; k < 16; ++k) hash[k] = static_cast<uint8_t>(std::stoi(ch.substr(2 * k, 2), nullptr, 16));
        uint64_t st = 0; for (int k = 0; k < 8; ++k) st = st << 8 | hash[k];
        auto unit = [&]() { st = st * 6364136223846793005ull + 1442695040888963407ull; return ((st >> 11) & ((1ull << 53) - 1)) / 9007199254740992.0; };
        auto gauss = [&]() { const double u = std::max(1e-12, unit()), v = unit(); return std::sqrt(-2 * std::log(u)) * std::cos(6.283185307179586 * v); };
        const double side_px = std::hypot(f.gt[2] - f.gt[0], f.gt[3] - f.gt[1]);
        const double module_px = side_px / std::max(1u, ckey_scalable_modules_across(art.p0_version));
        const double sigma = 0.2 + 1.3 * unit();
        double p[8];
        for (int k = 0; k < 8; ++k) p[k] = f.gt[k] + gauss() * sigma * module_px;
        if (unit() < 0.3) {   // a global error too: scale and rotation about the centre
            double cx = 0, cy = 0; for (int k = 0; k < 4; ++k) { cx += p[2 * k] / 4; cy += p[2 * k + 1] / 4; }
            const double s = 1 + 0.06 * gauss(), a = 0.03 * gauss();
            for (int k = 0; k < 4; ++k) { const double x = p[2 * k] - cx, y = p[2 * k + 1] - cy; p[2 * k] = cx + s * (x * std::cos(a) - y * std::sin(a)); p[2 * k + 1] = cy + s * (x * std::sin(a) + y * std::cos(a)); }
        }
        // Any corner order and winding, as the decoder may be handed.
        const int shift = static_cast<int>(unit() * 4) & 3; const bool reverse = unit() < 0.5;
        double q[8], g[8];
        for (int k = 0; k < 4; ++k) {
            const int src = reverse ? (shift - k + 8) % 4 : (shift + k) % 4;
            q[2 * k] = p[2 * src]; q[2 * k + 1] = p[2 * src + 1]; g[2 * k] = f.gt[2 * src]; g[2 * k + 1] = f.gt[2 * src + 1];
        }
        double H[9]; if (!homography_from(q, H)) continue;
        float target[8]; bool ok = true;
        const double canon[8] = {0, 0, 29, 0, 29, 29, 0, 29};
        for (int k = 0; k < 4 && ok; ++k) { double x, y; ok = unmap(H, g[2 * k], g[2 * k + 1], x, y); target[2 * k] = static_cast<float>(x - canon[2 * k]); target[2 * k + 1] = static_cast<float>(y - canon[2 * k + 1]); }
        if (!ok) continue;
        if (ckey_ml_rectify_region_gray8(f.gray.data(), f.w, f.h, f.w, q, -3.0, -3.0, 35.0, kRefineSide, px.data(), px.size()) != CKEY_OK) continue;
        std::vector<uint8_t> pl;
        ml_none();
        const uint8_t gtq = decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, f.gt, pl) && pl == art.payload;
        const uint32_t magic = 0x524D4B43, version = 1;
        const uint8_t meta[4] = {static_cast<uint8_t>(stratum_num(c.stratum)), static_cast<uint8_t>(art.p0_version), static_cast<uint8_t>(art.p0.layout), gtq};
        const float sg = static_cast<float>(sigma);
        std::fwrite(&magic, 4, 1, out); std::fwrite(&version, 4, 1, out); std::fwrite(hash, 16, 1, out); std::fwrite(meta, 4, 1, out);
        std::fwrite(&sg, 4, 1, out); std::fwrite(target, 4, 8, out); std::fwrite(px.data(), 1, px.size(), out);
        ++n;
    }
    std::fclose(out);
    std::fprintf(stderr, "ml-refine: %zu records\n", n);
    return 0;
}

// The refiner on identical frames: decode from perturbed true corners (the refiner's
// direct job) and the full scan, without and with the refiner. No other ML model.
int ml_refine_compare(const std::string &plan_path, const std::string &model_path, const std::string &out_path) {
    std::ifstream mf(model_path, std::ios::binary); const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(mf)), {});
    ml_none();
    if (ckey_ml_load_model(blob.data(), blob.size()) != CKEY_OK || ckey_ml_refiner_model_id() == 0) { std::fprintf(stderr, "ml-refine-compare: bad refiner\n"); return 3; }
    ml_none();
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "a");
    if (!plan || !out) { std::fprintf(stderr, "ml-refine-compare: cannot open files\n"); return 3; }
    Simulator sim; std::string line; std::vector<uint8_t> payload(65536);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        const bool valid = c.neg_kind == "none";
        std::fprintf(out, "{\"case_id\":%s,\"stratum\":%s,\"valid\":%d,\"v\":%u,\"print\":%d", jstr(c.case_id).c_str(), jstr(c.stratum).c_str(), valid ? 1 : 0, art.p0_version, c.p0_layout == "print" ? 1 : 0);
        const auto right = [&](size_t len) { return valid && len == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin()); };
        const auto mirror_ok = [&](size_t len) { return c.neg_kind == "mirror" && len == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin()); };
        // Perturbed true corners: 0.6 module per corner (deterministic per case).
        double pq[8]; bool have_pq = f.gt_valid && art.has_p0;
        if (have_pq) {
            uint64_t s = 0x9e3779b97f4a7c15ull; for (char ch : c.case_id) s = (s ^ static_cast<uint8_t>(ch)) * 1099511628211ull;
            auto unit = [&]() { s = s * 6364136223846793005ull + 1442695040888963407ull; return ((s >> 11) & ((1ull << 53) - 1)) / 9007199254740992.0; };
            const double mp = std::hypot(f.gt[2] - f.gt[0], f.gt[3] - f.gt[1]) / std::max(1u, ckey_scalable_modules_across(art.p0_version));
            for (int k = 0; k < 8; ++k) { const double u = std::max(1e-12, unit()), v = unit(); pq[k] = f.gt[k] + std::sqrt(-2 * std::log(u)) * std::cos(6.283185307179586 * v) * 0.6 * mp; }
        }
        for (int arm = 0; arm < 2; ++arm) {
            if (arm) { ml_none(); ckey_ml_load_model(blob.data(), blob.size()); } else ml_none();
            const char *tag = arm ? "ml" : "classical";
            if (have_pq) {
                size_t len = 0; uint32_t v = 0, l = 0; ckey_diagnostics d{};
                const auto t0 = std::chrono::steady_clock::now();
                const ckey_status st = ckey_decode_scalable_gray8(f.gray.data(), f.w, f.h, f.w, pq, payload.data(), payload.size(), &len, &v, &l, &d);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                const bool ok = st == CKEY_OK && right(len);
                std::fprintf(out, ",\"pq_%s\":{\"ok\":%d,\"wrong\":%d,\"ms\":%.2f,\"ml\":%d}", tag, ok ? 1 : 0, (st == CKEY_OK && !ok && !mirror_ok(len)) ? 1 : 0, ms, st == CKEY_OK && std::strstr(d.message, "ML corners") ? 1 : 0);
            }
            ckey_scan_options o; ckey_scan_options_init(&o); o.profiles = CKEY_SCAN_PROFILE_SCALABLE;
            ckey_scan_result res{}; res.abi_version = CKEY_ABI_VERSION;
            const auto t0 = std::chrono::steady_clock::now();
            const ckey_status st = ckey_scan_frame_gray8(f.gray.data(), f.w, f.h, f.w, &o, payload.data(), payload.size(), &res);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool ok = st == CKEY_OK && right(res.payload_length);
            std::fprintf(out, ",\"scan_%s\":{\"ok\":%d,\"wrong\":%d,\"ms\":%.2f,\"ml\":%d}", tag, ok ? 1 : 0, (st == CKEY_OK && !ok && !mirror_ok(res.payload_length)) ? 1 : 0, ms, st == CKEY_OK && std::strstr(res.message, "ML corners") ? 1 : 0);
        }
        ml_none();
        std::fprintf(out, "}\n"); std::fflush(out);
    }
    std::fclose(out);
    return 0;
}
#endif


#if defined(CKEY_ML_HAS_CLEAR)
// Multi-frame on identical sequences: each case as 6 frames (mild hand shake, frame motion,
// focus and exposure drift if the case has none). "any": some single frame decodes (a live
// scanner retries every frame); "acc": the frame accumulator over the same frames. Each
// without ML and with the built-in models.
int ml_multiframe_compare(const std::string &plan_path, const std::string &out_path) {
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "a");
    if (!plan || !out) { std::fprintf(stderr, "ml-multiframe-compare: cannot open files\n"); return 3; }
    Simulator sim; std::string line; std::vector<uint8_t> payload(65536);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code || c.neg_kind != "none") continue;
        c.frames = 6;
        if (c.shake_px == 0) c.shake_px = 3.0;
        if (c.frame_motion_px == 0) c.frame_motion_px = 1.0;
        if (c.focus_drift == 0) c.focus_drift = 0.1;
        if (c.exposure_drift == 0) c.exposure_drift = 0.05;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        std::vector<Frame> frames;
        for (int i = 0; i < c.frames; ++i) frames.push_back(sim.run(c, art, nullptr, i, false));
        std::fprintf(out, "{\"case_id\":%s,\"stratum\":%s,\"v\":%u", jstr(c.case_id).c_str(), jstr(c.stratum).c_str(), art.p0_version);
        const auto right = [&](size_t len) { return len == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin()); };
        for (int ml = 0; ml < 2; ++ml) {
            if (ml) ckey_ml_load_model(nullptr, 0); else ml_none();
            int any = -1, acc = -1, wrong = 0; double ms_any = 0, ms_acc = 0;
            ckey_frame_accumulator *a = ckey_accumulator_create();
            for (int i = 0; i < c.frames; ++i) {
                const Frame &f = frames[static_cast<size_t>(i)];
                ckey_scan_options o; ckey_scan_options_init(&o); o.profiles = CKEY_SCAN_PROFILE_SCALABLE;
                ckey_scan_result r{}; r.abi_version = CKEY_ABI_VERSION;
                auto t0 = std::chrono::steady_clock::now();
                ckey_status st = ckey_scan_frame_gray8(f.gray.data(), f.w, f.h, f.w, &o, payload.data(), payload.size(), &r);
                if (any < 0) ms_any += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                if (st == CKEY_OK) { if (right(r.payload_length)) { if (any < 0) any = i; } else ++wrong; }
                if (acc < 0) {
                    ckey_scan_result ra{}; ra.abi_version = CKEY_ABI_VERSION;
                    t0 = std::chrono::steady_clock::now();
                    st = ckey_scan_frame_accumulate_gray8(f.gray.data(), f.w, f.h, f.w, &o, a, payload.data(), payload.size(), &ra);
                    ms_acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    if (st == CKEY_OK) { if (right(ra.payload_length)) acc = i; else ++wrong; }
                }
            }
            ckey_accumulator_destroy(a);
            std::fprintf(out, ",\"%s\":{\"any\":%d,\"acc\":%d,\"wrong\":%d,\"ms_any\":%.1f,\"ms_acc\":%.1f}", ml ? "ml" : "classical", any, acc, wrong, ms_any, ms_acc);
        }
        ml_none();
        std::fprintf(out, "}\n"); std::fflush(out);
    }
    std::fclose(out);
    return 0;
}
#endif

int ml_compare(const std::string &plan_path, const std::string &model_path, const std::string &out_path) {
    ml_none();   // the arms differ only by the locator: no site model in any of them
    if (model_path != "-") {
        std::ifstream mf(model_path, std::ios::binary); std::vector<uint8_t> blob((std::istreambuf_iterator<char>(mf)), {});
        if (ckey_ml_load_model(blob.data(), blob.size()) != CKEY_OK) { std::fprintf(stderr, "ml-compare: bad model\n"); return 3; }
    }
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "a");
    if (!plan || !out) { std::fprintf(stderr, "ml-compare: cannot open files\n"); return 3; }
    Simulator sim; std::string line; std::vector<uint8_t> payload(65536);
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (c.profile != "P0" || c.second_code) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        const bool valid = c.neg_kind == "none";
        std::fprintf(out, "{\"case_id\":%s,\"stratum\":%s,\"valid\":%d,\"v\":%u,\"print\":%d,\"ppm\":%.3f", jstr(c.case_id).c_str(), jstr(c.stratum).c_str(), valid ? 1 : 0,
                     art.p0_version, c.p0_layout == "print" ? 1 : 0, f.phys.ppm_mean);
        if (valid && f.gt_valid) { std::vector<uint8_t> pl; const bool g = decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, f.gt, pl) && pl == art.payload; std::fprintf(out, ",\"gtq\":%d", g ? 1 : 0); }
        const uint32_t arms[3] = {0u, CKEY_SCAN_OPTION_ML, CKEY_SCAN_OPTION_ML | CKEY_SCAN_OPTION_ML_GATE};
        const char *names[3] = {"classical", "ml", "ml_gate"};
        for (int a = 0; a < 3; ++a) {
            ckey_scan_options o; ckey_scan_options_init(&o); o.profiles = CKEY_SCAN_PROFILE_SCALABLE; o.flags = arms[a];
            ckey_scan_result res{}; res.abi_version = CKEY_ABI_VERSION;
            const auto t0 = std::chrono::steady_clock::now();
            const ckey_status st = ckey_scan_frame_gray8(f.gray.data(), f.w, f.h, f.w, &o, payload.data(), payload.size(), &res);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool got = st == CKEY_OK;
            const bool right = got && valid && res.payload_length == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin());
            const bool wrong = got && !right && !(c.neg_kind == "mirror" && res.payload_length == art.payload.size() && std::equal(art.payload.begin(), art.payload.end(), payload.begin()));
            std::fprintf(out, ",\"%s\":{\"ok\":%d,\"wrong\":%d,\"ms\":%.2f,\"ml\":%d}", names[a], right ? 1 : 0, wrong ? 1 : 0, ms, (res.flags & CKEY_SCAN_FLAG_ML) ? 1 : 0);
        }
        ckey_ml_detection det[3]; const size_t nd = ckey_ml_locate_gray8(f.gray.data(), f.w, f.h, f.w, 0.15, det, 3);
        std::fprintf(out, ",\"ml_det\":%zu,\"ml_score\":%.3f,\"ml_quality\":%.3f,\"ml_version\":%.2f}\n", nd, nd ? det[0].score : 0.0, nd ? det[0].quality : 0.0, nd ? det[0].version : 0.0);
        std::fflush(out);
    }
    std::fclose(out);
    return 0;
}

#endif

// Classical vs ML vs hybrid on identical frames. ML: predicted corners -> production decoder (RS + CRC gate).
int ml_eval(const std::string &plan_path, const std::string &model_path, const std::string &out_path) {
    Mlp mlp; std::string err;
    if (!mlp.load(model_path, err)) { std::fprintf(stderr, "ml-eval: %s\n", err.c_str()); return 3; }
    std::ifstream plan(plan_path); FILE *out = std::fopen(out_path.c_str(), "a");
    if (!plan || !out) { std::fprintf(stderr, "ml-eval: cannot open files\n"); return 3; }
    Simulator sim; Scanner scanner; std::string line;
    float x[kPatch * kPatch];
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        CaseSpec c; try { c = parse_case(line); } catch (...) { continue; }
        if (!valid_p0(c)) continue;
        const SymbolArt art = build_symbol(c, 0);
        if (!art.error.empty()) continue;
        const Frame f = sim.run(c, art, nullptr, 0, false);
        const ScanResult sr = scanner.decode(f.gray.data(), f.w, f.h, f.w);
        const bool ok_c = sr.accepted && sr.payload == art.payload, wrong_c = sr.accepted && !ok_c;
        bool ok_m = false, wrong_m = false, ok_mr = false, wrong_mr = false; double ml_ms = 0, ml_err = -1, ref_err = -1;
        const double side = std::max(8.0, std::hypot(f.gt[2] - f.gt[0], f.gt[3] - f.gt[1]));
        for (const auto &cand : sr.candidates) {
            const auto t0 = std::chrono::steady_clock::now();
            double q[8];
            for (int k = 0; k < 4; ++k) {
                const LocalFrame lf = corner_frame(cand.border, k);
                uint8_t patch[kPatch][kPatch]; sample_patch(f.gray.data(), f.w, f.h, lf, patch);
                normalise(patch, x); float o[2]; mlp.predict(x, o);
                from_local(lf, o[0], o[1], q[2 * k], q[2 * k + 1]);
            }
            ml_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const bool on_symbol = f.gt_valid && quad_distance(cand.border, f.gt) < 0.08 * side + 3;
            if (on_symbol) {
                ml_err = quad_distance(q, f.gt);
                for (const auto &a : cand.attempts) if (a.field == CKEY_FIELD_SCALABLE && a.score >= 0) ref_err = quad_distance(a.quad, f.gt);
            }
            std::vector<uint8_t> pl;
            if (!ok_m && !wrong_m && decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, q, pl)) {
                if (pl == art.payload) ok_m = true; else wrong_m = true;
            }
            // ML proposal as the starting quad of the production refiner, then the production decoder.
            if (!ok_mr && !wrong_mr) {
                double rq[8];
                if (ckey_refine_quad_gray8(f.gray.data(), f.w, f.h, f.w, q, CKEY_FIELD_SCALABLE, rq) >= 0 && decode_with_quad(0, f.gray.data(), f.w, f.h, f.w, rq, pl)) {
                    if (pl == art.payload) ok_mr = true; else wrong_mr = true;
                }
            }
            if ((ok_m || wrong_m) && (ok_mr || wrong_mr)) break;
        }
        std::fprintf(out, "{\"case_id\":%s,\"stratum\":%s,\"sev_bin\":%d,\"layout\":%s,\"p0_version\":%u,\"phys_class\":%s,\"ppm\":%s,"
                          "\"ok_classical\":%s,\"ok_ml\":%s,\"ok_ml_refine\":%s,\"ok_hybrid\":%s,\"wrong_classical\":%s,\"wrong_ml\":%s,\"wrong_ml_refine\":%s,\"ml_ms\":%s,\"ml_loc_err\":%s,\"refine_loc_err\":%s,\"model_params\":%zu}\n",
                     jstr(c.case_id).c_str(), jstr(c.stratum).c_str(), c.sev_bin, jstr(c.p0_layout).c_str(), art.p0_version, jstr(f.phys.cls).c_str(), jnum(f.phys.ppm_mean).c_str(),
                     ok_c ? "true" : "false", ok_m ? "true" : "false", ok_mr ? "true" : "false", (ok_c || (!wrong_c && (ok_m || ok_mr))) ? "true" : "false",
                     wrong_c ? "true" : "false", wrong_m ? "true" : "false", wrong_mr ? "true" : "false",
                     jnum(ml_ms).c_str(), ml_err >= 0 ? jnum(ml_err).c_str() : "null", ref_err >= 0 ? jnum(ref_err).c_str() : "null", mlp.parameters());
        std::fflush(out);
    }
    std::fclose(out);
    return 0;
}

}  // namespace ckt

int main(int argc, char **argv) {
    using namespace ckt;
    Options opt; std::string repro, out_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); } return argv[++i]; };
        if (a == "--selftest") return selftest();
        if (a == "--print-schema") { std::puts(case_schema_json().c_str()); return 0; }
        if (a == "--print-config") { std::puts(config_json().c_str()); return 0; }
        if (a == "--capacity-table") { std::puts(capacity_table().c_str()); return 0; }
        if (a == "--ml-export" && i + 2 < argc) return ml_export(argv[i + 1], argv[i + 2]);
#if defined(CKEY_SCAN_PROFILE_SCALABLE)
        if (a == "--ml-frames" && i + 2 < argc) return ml_frames(argv[i + 1], argv[i + 2]);
#endif
#if defined(CKEY_SCAN_OPTION_ML)
        if (a == "--ml-compare" && i + 3 < argc) return ml_compare(argv[i + 1], argv[i + 2], argv[i + 3]);
        if (a == "--ml-sites" && i + 2 < argc) return ml_sites_export(argv[i + 1], argv[i + 2]);
        if (a == "--ml-sites-compare" && i + 3 < argc) return ml_sites_compare(argv[i + 1], argv[i + 2], argv[i + 3]);
#if defined(CKEY_ML_HAS_CLEAR)
        if (a == "--ml-refine" && i + 2 < argc) return ml_refine_export(argv[i + 1], argv[i + 2]);
        if (a == "--ml-refine-compare" && i + 3 < argc) return ml_refine_compare(argv[i + 1], argv[i + 2], argv[i + 3]);
        if (a == "--ml-multiframe-compare" && i + 2 < argc) return ml_multiframe_compare(argv[i + 1], argv[i + 2]);
#endif
#if defined(CKEY_SCAN_VERSION_HINT)
        if (a == "--ml-hint-compare" && i + 2 < argc) return ml_hint_compare(argv[i + 1], argv[i + 2]);
#endif
#endif
        if (a == "--ml-eval" && i + 3 < argc) return ml_eval(argv[i + 1], argv[i + 2], argv[i + 3]);
        if (a == "--plan") opt.plan = next();
        else if (a == "--results") opt.results = next();
        else if (a == "--traces") opt.traces = next();
        else if (a == "--progress") opt.progress = next();
        else if (a == "--images") opt.images = next();
        else if (a == "--trace-uri-prefix") opt.trace_prefix = next();
        else if (a == "--fail-image-cap") opt.fail_cap = std::stoi(next());
        else if (a == "--wrong-image-cap") opt.wrong_cap = std::stoi(next());
        else if (a == "--success-sample-ppm") opt.success_ppm = std::stoi(next());
        else if (a == "--crosscheck-strata") { opt.crosscheck_strata.clear(); std::stringstream s(next()); std::string t; while (std::getline(s, t, ',')) opt.crosscheck_strata.insert(t); }
        else if (a == "--repro") repro = next();
        else if (a == "--out") out_dir = next();
        else { std::fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }
    if (!repro.empty()) {
        std::ifstream in(repro); std::string line; std::getline(in, line);
        Options o = opt; o.images.clear();
        Runner r(o); Progress p(""); std::string trace; std::vector<FrameRecord> keep;
        const std::string res = r.run_case(line, p, trace, &keep);
        if (!out_dir.empty()) {
            mkdirs(out_dir);
            for (size_t i = 0; i < keep.size(); ++i) write_png(out_dir + "/frame" + std::to_string(i) + ".png", keep[i].frame.gray, keep[i].frame.w, keep[i].frame.h, 0, 0, keep[i].frame.w, keep[i].frame.h);
            std::ofstream(out_dir + "/trace.json") << trace << "\n";
            try {
                const CaseSpec c = parse_case(line);
                const SymbolArt art = build_symbol(c, 0);
                if (!art.svg.empty()) std::ofstream(out_dir + "/symbol.svg") << art.svg;
                if (!art.svg_b.empty()) std::ofstream(out_dir + "/symbol_splice_b.svg") << art.svg_b;
            } catch (...) {}
        }
        std::puts(res.c_str());
        return 0;
    }
    if (opt.plan.empty() || opt.results.empty()) { std::fprintf(stderr, "usage: ckt_worker --plan P --results R [--traces T --progress G --images DIR]\n"); return 2; }
    std::ifstream plan(opt.plan);
    if (!plan) { std::fprintf(stderr, "cannot open plan %s\n", opt.plan.c_str()); return 3; }
    FILE *res = std::fopen(opt.results.c_str(), "a");
    FILE *tr = opt.traces.empty() ? nullptr : std::fopen(opt.traces.c_str(), "a");
    if (!res || (!opt.traces.empty() && !tr)) { std::fprintf(stderr, "cannot open outputs\n"); return 3; }
    Runner runner(opt); Progress prog(opt.progress);
    std::string line; size_t n = 0;
    while (std::getline(plan, line)) {
        if (line.empty()) continue;
        std::string trace;
        std::string out;
        try { out = runner.run_case(line, prog, trace); }
        catch (const std::exception &e) {
            // A harness exception is not a completed case; the supervisor reruns it.
            out = std::string("{\"status\":\"HARNESS_ERROR\",\"case_id\":") + jstr(parse_flat_json(line).count("case_id") ? parse_flat_json(line).at("case_id").str : "") + ",\"error\":" + jstr(e.what()) + "}";
        }
        if (tr && !trace.empty()) { std::fputs(trace.c_str(), tr); std::fputc('\n', tr); }
        std::fputs(out.c_str(), res); std::fputc('\n', res);
        std::fflush(res); if (tr) std::fflush(tr);  // every case: a crash loses at most the case in flight
        ++n;
    }
    std::fflush(res); if (tr) std::fflush(tr);
    fsync(fileno(res)); if (tr) fsync(fileno(tr));
    std::fclose(res); if (tr) std::fclose(tr);
    return 0;
}
