// gemfilter / track : SYNTHETIC toy generator (see Toy.h)
#include "gemfilter/track/Toy.h"

#include <algorithm>
#include <cmath>

#include "gemfilter/track/GemEventFilter.h"

namespace gemfilter {
namespace track {

using geom::Axis;
using geom::Vec3;

ToyGenerator::ToyGenerator(const geom::GemTracker& trk, const geom::ApvMap& map, uint32_t seed)
    : trk_(trk), rng_(seed) {
  inv_.assign(trk.modules.size() * 2, {});
  for (size_t m = 0; m < trk.modules.size(); ++m)
    for (int a = 0; a < 2; ++a) {
      ChanRef none; none.addr.crate = 0xFFFF; none.chan = 0;
      inv_[2 * m + size_t(a)].assign(size_t(std::max(0, trk.modules[m].nstrips[a])), none);
    }
  for (size_t m = 0; m < trk.modules.size(); ++m) {
    for (const auto& e : trk.modules[m].apvs) {
      ApvAddress ad; ad.crate = e.crate; ad.slot = e.slot; ad.fiber = e.mpd_id; ad.mpd = e.mpd_id; ad.adc = e.adc_id;
      for (int ch = 0; ch < geom::kApvChannels; ++ch) {
        geom::StripRef s = map.strip(ad, ch);
        if (!s.valid()) continue;
        auto& v = inv_[size_t(2 * s.module + int(s.axis))];
        if (s.strip < int(v.size())) v[size_t(s.strip)] = {ad, uint8_t(ch)};
      }
    }
  }
}

ToyTrack ToyGenerator::randomTrack(double mx0, double my0, double mxp, double myp) {
  std::uniform_real_distribution<double> u(-1, 1);
  ToyTrack t;
  t.p0 = {mx0 * u(rng_), my0 * u(rng_), 0};
  t.dir = Vec3{mxp * u(rng_), myp * u(rng_), 1.0}.unit();
  return t;
}

namespace {
double crrc(double t, double tau) { return t <= 0 ? 0 : (t / tau) * std::exp(1 - t / tau); }
struct Acc { std::array<double, kMaxSamples> a{}; bool sig = false; };
}  // namespace

ToyEvent ToyGenerator::generate(const std::vector<ToyTrack>& tracks, const ToyConfig& c) {
  ToyEvent ev;
  ev.tracks = tracks;
  // per module-axis accumulated strip samples
  std::vector<std::map<int, Acc>> acc(inv_.size());
  auto deposit = [&](size_t ma, double centre, double amp, double t0, bool sig, double sigma) {
    int n = int(inv_[ma].size());
    int lo = std::max(0, int(std::floor(centre - 3 * sigma))), hi = std::min(n - 1, int(std::ceil(centre + 3 * sigma)));
    for (int s = lo; s <= hi; ++s) {
      double w = std::exp(-0.5 * std::pow((s - centre) / sigma, 2));
      if (w < 0.02) continue;
      Acc& A = acc[ma][s];
      for (int i = 0; i < kMaxSamples; ++i) A.a[size_t(i)] += amp * w * crrc(i * kApvSampleNs - t0, c.tau_ns);
      A.sig |= sig;
    }
  };
  // signal
  for (const auto& t : tracks) {
    for (size_t m = 0; m < trk_.modules.size(); ++m) {
      const auto& mod = trk_.modules[m];
      Vec3 p = mod.trackToDet(mod.intersect(t.p0, t.dir));
      if (!mod.inActiveArea(p.x, p.y)) continue;
      double u, v;
      mod.xyToUV(p.x, p.y, u, v);
      std::gamma_distribution<double> landau_like(4.0, 0.25);   // mean 1, long tail
      double amp = c.signal_amp * landau_like(rng_);
      double su = mod.stripCoordinate(Axis::U, u), sv = mod.stripCoordinate(Axis::V, v);
      std::normal_distribution<double> share(1.0, 0.15);        // U/V charge sharing
      double r = std::max(0.3, share(rng_));
      deposit(2 * m, su, amp * r, c.signal_t0, true, c.cluster_sigma);
      deposit(2 * m + 1, sv, amp / r, c.signal_t0, true, c.cluster_sigma);
    }
  }
  // background clusters
  std::uniform_real_distribution<double> U01(0, 1);
  std::exponential_distribution<double> ampd(1.0 / c.bkg_amp);
  for (size_t ma = 0; ma < inv_.size(); ++ma) {
    int n = int(inv_[ma].size());
    if (n == 0) continue;
    double ncl_mean = c.occupancy * n / std::max(1.0, c.bkg_cluster_mean);
    std::poisson_distribution<int> pn(ncl_mean);
    int ncl = pn(rng_);
    for (int k = 0; k < ncl; ++k) {
      double centre = U01(rng_) * n;
      double sigma = std::max(0.3, c.bkg_cluster_mean / 3.5);
      double t0 = c.bkg_t0_min + U01(rng_) * (c.bkg_t0_max - c.bkg_t0_min);
      deposit(ma, centre, std::max(100.0, ampd(rng_)), t0, false, sigma);
    }
  }
  // build frames
  std::map<uint64_t, size_t> frame_of;
  auto key = [](const ApvAddress& a) {
    return (uint64_t(a.crate) << 48) | (uint64_t(a.slot) << 32) | (uint64_t(a.fiber) << 16) | a.adc;
  };
  for (size_t ma = 0; ma < inv_.size(); ++ma) {
    for (auto& [s, A] : acc[ma]) {
      double mx = *std::max_element(A.a.begin(), A.a.end());
      if (mx < c.strip_threshold) continue;
      const ChanRef& cr = inv_[ma][size_t(s)];
      if (cr.addr.crate == 0xFFFF) continue;
      uint64_t k = key(cr.addr);
      auto it = frame_of.find(k);
      if (it == frame_of.end()) {
        it = frame_of.emplace(k, ev.frames.size()).first;
        ApvFrame f; f.addr = cr.addr;
        ev.frames.push_back(f);
        ev.truth.emplace_back();
      }
      ApvFrame& f = ev.frames[it->second];
      StripHit h;
      h.apv_chan = cr.chan;
      for (int i = 0; i < kMaxSamples; ++i) h.adc[size_t(i)] = int16_t(std::clamp(A.a[size_t(i)], -4096.0, 4095.0));
      h.src_index = uint32_t(f.hits.size());
      f.hits.push_back(h);
      ev.truth[it->second].push_back(A.sig ? 1 : 0);
      ev.signal_strips += A.sig;
      ++ev.total_strips;
    }
  }
  return ev;
}

}  // namespace track
}  // namespace gemfilter
