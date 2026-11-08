// Harness self-test: checks the oracle and simulator before any campaign
// case is counted. Run by scripts/preflight.sh; any FAIL blocks BASELINE_LOCKED.
#include "runners/native/scanner_adapter.h"
#include "runners/native/symbol_factory.h"
#include "simulation/common/sha256.h"
#include "simulation/pipeline.h"

#include <cstdio>
#include <string>

namespace ckt {

static int g_fail = 0;
static void check(bool ok, const std::string &what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str()); if (!ok) ++g_fail; }
// Reader behaviour is what the campaign measures; the selftest only reports it.
static void info(bool ok, const std::string &what) { std::printf("INFO reader %s: %s\n", ok ? "decoded" : "did NOT decode", what.c_str()); }

static CaseSpec base(const char *profile) {
    CaseSpec c; c.case_id = std::string("selftest-") + profile; c.profile = profile; c.seed = "12345"; c.payload_seed = "777";
    c.payload_len = std::string(profile) == "P1" ? 16 : std::string(profile) == "P2" ? 120 : std::string(profile) == "P3" ? 1000 : 60;
    c.p0_version = std::string(profile) == "P0" ? 6 : 0;
    c.fg_rgb = {20, 20, 20}; c.bg_rgb = {235, 235, 235};
    return c;
}

static double target_distance(const CaseSpec &c, const SymbolArt &a, double ppm) {
    const double module_mm = c.symbol_mm * (a.field_units / a.canvas_units) / a.modules;
    return c.focal_px * module_mm / ppm;
}

int selftest() {
    // PRNG determinism.
    { Rng a(42, "x"), b(42, "x"), d(42, "y"); bool same = true; for (int i = 0; i < 1000; ++i) same &= a.next() == b.next(); check(same && Rng(42, "x").next() != d.next(), "prng: streams deterministic and independent"); }
    // SHA-256 known answer ("abc").
    check(Sha256::of("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256: FIPS 180-4 'abc' vector");
    // The rasterizer cross-check must be able to fail: two different symbols must disagree.
    {
        CaseSpec x = base("P1"), y = base("P1"); y.payload_seed = "778";
        const SymbolArt ax = build_symbol(x), ay = build_symbol(y);
        const double d = raster_disagreement(raster_reference(ax.svg, 256, 3), raster_independent(ay.svg, 256, 3));
        check(d > 0.01, "rasterizer cross-check detects a different symbol (disagreement " + std::to_string(d) + ")");
    }
    // Camera round trip.
    {
        double worst = 0;
        for (double bend : {0.0, 60.0, -80.0}) {
            Camera cam; cam.K.k1 = -0.08; cam.K.k2 = 0.01; cam.S.bend_radius = bend; cam.S.bend_axis = 30; cam.prepare(); cam.build_undistort_grid();
            Pose p; p.roll = 23; p.yaw = 31; p.pitch = -17; p.tz = 180; p.tx = 12; p.ty = -9;
            for (double u = -12; u <= 12; u += 4) for (double v = -12; v <= 12; v += 4) {
                double x, y, uu = 1e9, vv = 1e9; cam.project(p, u, v, x, y); cam.unproject(p, x, y, uu, vv); worst = std::max(worst, std::hypot(uu - u, vv - v));
            }
        }
        check(worst < 2e-3, "camera: project/unproject round trip, planar/convex/concave, with distortion LUT (max err " + std::to_string(worst) + " mm)");
    }
    Scanner scanner; Simulator sim;
    for (const char *prof : {"P0", "P1", "P2", "P3"}) {
        CaseSpec c = base(prof);
        SymbolArt a = build_symbol(c);
        check(a.error.empty(), std::string(prof) + ": production encoder + renderer (" + a.format + ")");
        if (!a.error.empty()) continue;
        if (a.profile == 1 || a.profile == 2) check(a.rs_verified == 1, std::string(prof) + ": independent RS reproduces production parity");
        const double xc = Simulator::renderer_crosscheck(a);
        check(xc < 0.01, std::string(prof) + ": reference vs independent rasterizer disagreement " + std::to_string(xc));
        // Ideal path round trip.
        CaseSpec ci = c; ci.ideal = 1; ci.ideal_ppm = a.profile == 3 ? 6 : 8; ci.frame_w = 2160; ci.frame_h = 3840;
        Frame fi = sim.run(ci, a, nullptr, 0, false);
        std::vector<uint8_t> pl;
        check(decode_with_quad(a.profile, fi.gray.data(), fi.w, fi.h, fi.w, fi.gt, pl) && pl == a.payload, std::string(prof) + ": ideal raster decodes with the ground-truth quad (simulator + GT convention)");
        ScanResult ri = scanner.decode(fi.gray.data(), fi.w, fi.h, fi.w);
        info(ri.accepted && ri.payload == a.payload, std::string(prof) + " ideal raster at " + std::to_string(ci.ideal_ppm) + " px/module");
        if (ri.accepted) { const double e = quad_distance(ri.accepted_quad, fi.gt); check(e < 2.0, std::string(prof) + ": ideal ground-truth corner convention (err " + std::to_string(e) + " px)"); }
        // Physical clean capture.
        CaseSpec cp = c; cp.substrate = "ideal";  // painted surface through the full camera model (no ink spread)
        if (a.profile == 3) cp.symbol_mm = 90;  // ~9 printer dots per module at 600 dpi (30 mm would be ~3)
        cp.distance_mm = target_distance(cp, a, a.profile == 3 ? 7 : 9);
        Frame fp = sim.run(cp, a, nullptr, 0, false);
        check(decode_with_quad(a.profile, fp.gray.data(), fp.w, fp.h, fp.w, fp.gt, pl) && pl == a.payload, std::string(prof) + ": physical clean capture decodes with the ground-truth quad (ppm " + std::to_string(fp.phys.ppm_min) + ", class " + fp.phys.cls + ")");
        ScanResult rp = scanner.decode(fp.gray.data(), fp.w, fp.h, fp.w);
        info(rp.accepted && rp.payload == a.payload, std::string(prof) + " physical clean capture");
        if (rp.accepted) { const double e = quad_distance(rp.accepted_quad, fp.gt); check(e < 3.0, std::string(prof) + ": physical ground-truth localization (err " + std::to_string(e) + " px)"); }
        // Print path through the reader (informational: ink spread is a real effect).
        { CaseSpec pr = cp; pr.substrate = "print"; Frame fq = sim.run(pr, a, nullptr, 0, false); ScanResult rq = scanner.decode(fq.gray.data(), fq.w, fq.h, fq.w); info(rq.accepted && rq.payload == a.payload, std::string(prof) + " physical clean 600 dpi print"); }
        // Determinism: identical frame bytes on a re-run.
        Frame fp2 = sim.run(cp, a, nullptr, 0, false);
        check(Sha256::of(fp.gray.data(), fp.gray.size()) == Sha256::of(fp2.gray.data(), fp2.gray.size()), std::string(prof) + ": simulation is deterministic");
        // Negatives.
        CaseSpec cn = cp; cn.neg_kind = "random_codeword"; cn.expected = "REJECT";
        SymbolArt an = build_symbol(cn); Frame fn = sim.run(cn, an, nullptr, 0, false);
        check(!scanner.decode(fn.gray.data(), fn.w, fn.h, fn.w).accepted, std::string(prof) + ": random codeword rejected");
    }
    {
        CaseSpec c = base("P1"); c.neg_kind = "blank"; c.expected = "REJECT"; c.substrate = "print";
        SymbolArt a = build_symbol(c); Frame f = sim.run(c, a, nullptr, 0, false);
        check(!scanner.decode(f.gray.data(), f.w, f.h, f.w).accepted, "blank scene rejected");
        CaseSpec g = base("P2"); g.neg_kind = "crc_forge"; g.expected = "NO_WRONG"; g.substrate = "print";
        SymbolArt ag = build_symbol(g);
        check(ag.error.empty(), "crc_forge negative constructible (RS oracle agrees)");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "SELFTEST FAILED" : "SELFTEST PASSED", g_fail);
    return g_fail ? 1 : 0;
}

}  // namespace ckt
