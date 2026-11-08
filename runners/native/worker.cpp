// ckt_worker: executes case plans against the production CKey reader.
//
//   ckt_worker --plan P.jsonl --results R.jsonl --traces T.jsonl --progress G.log
//              [--images DIR] [--fail-image-cap N] [--success-sample-ppm N]
//              [--crosscheck-strata S01] [--trace-uri-prefix PFX]
//   ckt_worker --repro LINE.json --out DIR       (regenerate one case's frames as PNG)
//   ckt_worker --selftest | --print-schema | --print-config | --capacity-table
//
// Output files are append-only. Each result line is one completed case or
// an explicit HARNESS_ERROR (not a completed case: the supervisor reruns it).
// The progress log brackets every case and every reader call so a crash or
// hang can be attributed to the reader (completed failure) or the harness.
#include "ckey/ckey.h"
#include "runners/native/scanner_adapter.h"
#include "runners/native/symbol_factory.h"
#include "simulation/common/sha256.h"
#include "simulation/pipeline.h"

#include <cerrno>
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
          << ",\"payload_hash\":" << jstr(hexs(art.payload)) << ",\"payload_len\":" << art.payload.size() << ",\"format_version\":" << jstr(art.format)
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
          << ",\"saturated\":" << jnum(p.saturated) << ",\"byte_err_est\":" << jnum(p.byte_err_est) << ",\"in_frame\":" << (p.in_frame ? "true" : "false")
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
