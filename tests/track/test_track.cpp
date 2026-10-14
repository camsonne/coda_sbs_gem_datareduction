// Unit tests for gemfilter_track on SYNTHETIC toy events in the BigBite GEM geometry (SBS-replay DB).
#include <cstdio>
#include "gemfilter/track/GemEventFilter.h"
#include "gemfilter/track/Toy.h"
#include "testing.h"

using namespace gemfilter;
using namespace gemfilter::geom;
using namespace gemfilter::track;

static GemTracker& bb() { static GemTracker t = loadGemTracker(DbDir(GF_TEST_DB), "bb.gem", civilSeconds(2022, 1, 10)); return t; }
static ApvMap& bbmap() { static ApvMap m(bb()); return m; }

static seeds::ConstraintPoints truthCP(const ToyTrack& t, double w) {
  seeds::ConstraintPoints cp;
  cp.front = t.p0;
  cp.back = t.p0 + t.dir * (3.0 / t.dir.z);
  cp.fwx = cp.fwy = cp.bwx = cp.bwy = w;
  return cp;
}
static std::vector<const ApvFrame*> ptrs(const ToyEvent& e) {
  std::vector<const ApvFrame*> p; for (auto& f : e.frames) p.push_back(&f); return p;
}
struct Score { uint32_t sig_in = 0, sig_kept = 0, kept = 0, total = 0; };
static Score score(const ToyEvent& e, const FilterResult& r) {
  Score s;
  for (size_t f = 0; f < e.frames.size(); ++f)
    for (size_t h = 0; h < e.frames[f].hits.size(); ++h) {
      s.total++; s.kept += r.keep[f][h];
      if (e.truth[f][h]) { s.sig_in++; s.sig_kept += r.keep[f][h]; }
    }
  return s;
}

TEST_CASE(clean_track_found_all_layers) {
  ToyGenerator g(bb(), bbmap(), 7);
  ToyConfig c; c.occupancy = 0;
  ToyTrack t{{0.02, 0.01, 0}, Vec3{0.05, -0.02, 1}.unit()};
  ToyEvent e = g.generate({t}, c);
  CHECK(e.total_strips > 20);
  GemEventFilter f(bb(), bbmap());
  auto r = f.run(ptrs(e), nullptr);
  CHECK_EQ(f.unmapped(), 0u);
  CHECK_EQ(r.tracks.size(), size_t(1));
  CHECK(int(r.tracks[0].hits.size()) >= bb().nlayers - 1);
  CHECK(std::abs(r.tracks[0].xp - t.dir.x / t.dir.z) < 2e-3);
  Score s = score(e, r);
  CHECK_EQ(s.sig_kept, s.sig_in);
}

TEST_CASE(road_only_keeps_all_signal_and_reduces) {
  ToyGenerator g(bb(), bbmap(), 11);
  ToyConfig c; c.occupancy = 0.10;
  GemEventFilter f(bb(), bbmap(), [] { TrackerConfig k; k.mode = TrackerConfig::Mode::RoadOnly; return k; }());
  uint64_t sig = 0, sigk = 0, tot = 0, kept = 0;
  for (int i = 0; i < 50; ++i) {
    ToyTrack t = g.randomTrack(0.15, 0.05, 0.15, 0.05);
    ToyEvent e = g.generate({t}, c);
    auto w = seeds::moduleWindows(bb(), {truthCP(t, 0.03)});
    auto r = f.run(ptrs(e), &w);
    Score s = score(e, r);
    sig += s.sig_in; sigk += s.sig_kept; tot += s.total; kept += s.kept;
  }
  std::printf("road-only: signal kept %llu/%llu, strips kept %llu/%llu\n", (unsigned long long)sigk,
              (unsigned long long)sig, (unsigned long long)kept, (unsigned long long)tot);
  CHECK_EQ(sigk, sig);
  CHECK(kept * 5 < tot);   // > 5x reduction at 10% occupancy with 3 cm roads
}

TEST_CASE(track_mode_high_occupancy) {
  ToyGenerator g(bb(), bbmap(), 13);
  ToyConfig c; c.occupancy = 0.30;
  GemEventFilter f(bb(), bbmap());
  uint64_t sig = 0, sigk = 0, tot = 0, kept = 0; int found = 0, fb = 0;
  for (int i = 0; i < 50; ++i) {
    ToyTrack t = g.randomTrack(0.15, 0.05, 0.15, 0.05);
    ToyEvent e = g.generate({t}, c);
    auto w = seeds::moduleWindows(bb(), {truthCP(t, 0.03)});
    auto r = f.run(ptrs(e), &w);
    Score s = score(e, r);
    sig += s.sig_in; sigk += s.sig_kept; tot += s.total; kept += s.kept;
    found += !r.tracks.empty(); fb += r.fallback;
  }
  std::printf("track 30%%: found %d/50 (fallback %d), signal kept %llu/%llu, strips kept %llu/%llu\n", found, fb,
              (unsigned long long)sigk, (unsigned long long)sig, (unsigned long long)kept, (unsigned long long)tot);
  CHECK(found >= 45);
  CHECK(double(sigk) / double(sig) > 0.97);
  CHECK(kept * 10 < tot);
}

TEST_CASE(no_window_global_keeps_everything_when_no_track) {
  ToyGenerator g(bb(), bbmap(), 17);
  ToyConfig c; c.occupancy = 0.02;
  ToyEvent e = g.generate({}, c);
  GemEventFilter f(bb(), bbmap());
  auto r = f.run(ptrs(e), nullptr);
  if (r.tracks.empty()) { CHECK(r.fallback); CHECK_EQ(r.strips_out, r.strips_in); }
}

TEST_MAIN()
