// gemfilter / track : see GemEventFilter.h
#include "gemfilter/track/GemEventFilter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gemfilter {
namespace track {

using geom::Axis;
using geom::Vec3;

GemEventFilter::GemEventFilter(const geom::GemTracker& trk, const geom::ApvMap& map, TrackerConfig cfg)
    : trk_(trk), map_(map), cfg_(cfg) {}

void GemEventFilter::mapStrips(const std::vector<const ApvFrame*>& frames) {
  strips_.clear();
  unmapped_ = 0;
  const size_t nma = trk_.modules.size() * 2;
  if (strips_by_modaxis_.size() != nma) strips_by_modaxis_.assign(nma, {});
  for (auto& v : strips_by_modaxis_) v.clear();
  for (uint32_t f = 0; f < frames.size(); ++f) {
    const ApvFrame& fr = *frames[f];
    for (uint32_t h = 0; h < fr.hits.size(); ++h) {
      const StripHit& sh = fr.hits[h];
      geom::StripRef sr = map_.strip(fr.addr, sh.apv_chan);
      if (!sr.valid()) { ++unmapped_; continue; }
      StripRef s;
      s.module = sr.module; s.axis = sr.axis; s.strip = sr.strip; s.frame = f; s.hit = h;
      double qmax = -1e9, sw = 0, st = 0;
      int imax = 0;
      int ns = std::min<int>(sh.nsamples, kMaxSamples);
      for (int i = 0; i < ns; ++i) {
        double a = sh.adc[size_t(i)];
        if (a > qmax) { qmax = a; imax = i; }
        if (a > 0) { sw += a; st += a * i * kApvSampleNs; }
      }
      s.qmax = float(qmax);
      s.imax = uint8_t(imax);
      s.tmean = float(sw > 0 ? st / sw : imax * kApvSampleNs);
      strips_by_modaxis_[size_t(2 * s.module + int(s.axis))].push_back(int(strips_.size()));
      strips_.push_back(s);
    }
  }
  for (auto& v : strips_by_modaxis_)
    std::sort(v.begin(), v.end(), [&](int a, int b) { return strips_[size_t(a)].strip < strips_[size_t(b)].strip; });
}

void GemEventFilter::cluster1D() {
  clusters_.clear();
  const size_t nma = trk_.modules.size() * 2;
  if (mod_axis_clusters_.size() != nma) mod_axis_clusters_.assign(nma, {});
  for (auto& v : mod_axis_clusters_) v.clear();
  for (size_t ma = 0; ma < nma; ++ma) {
    const auto& idx = strips_by_modaxis_[ma];
    if (idx.empty()) continue;
    const int imod = int(ma / 2);
    const Axis ax = Axis(ma % 2);
    const auto& mod = trk_.modules[size_t(imod)];
    size_t i = 0;
    while (i < idx.size()) {
      size_t j = i + 1;
      while (j < idx.size() &&
             strips_[size_t(idx[j])].strip - strips_[size_t(idx[j - 1])].strip <= 1 + cfg_.max_strip_gap)
        ++j;
      // [i, j) is a contiguous group; split if larger than max_cluster_size
      for (size_t a = i; a < j; a += size_t(std::max(1, cfg_.max_cluster_size))) {
        size_t b = std::min(j, a + size_t(std::max(1, cfg_.max_cluster_size)));
        Cluster1D c;
        c.module = imod; c.axis = ax;
        double q = 0, qx = 0, qt = 0, qpk = -1e9;
        for (size_t k = a; k < b; ++k) {
          const StripRef& s = strips_[size_t(idx[k])];
          c.strips.push_back(uint32_t(idx[k]));
          double w = std::max(0.0, double(s.qmax));
          q += w; qx += w * s.strip; qt += w * s.tmean;
          if (s.qmax > qpk) { qpk = s.qmax; c.imax_peak = s.imax; }
        }
        c.lo = strips_[size_t(idx[a])].strip;
        c.hi = strips_[size_t(idx[b - 1])].strip;
        double cs = q > 0 ? qx / q : 0.5 * (c.lo + c.hi);
        c.pos = mod.stripPosition(ax, cs);
        c.pos_lo = mod.stripPosition(ax, c.lo);
        c.pos_hi = mod.stripPosition(ax, c.hi);
        c.q = q;
        c.t = q > 0 ? qt / q : 0;
        mod_axis_clusters_[ma].push_back(int(clusters_.size()));
        clusters_.push_back(std::move(c));
      }
      i = j;
    }
  }
}

bool GemEventFilter::clusterInWindow(const Cluster1D& c, const seeds::ModuleWindow& w) const {
  if (!w.active) return false;
  const auto& mod = trk_.modules[size_t(c.module)];
  double m = cfg_.keep_margin_strips * mod.pitch[int(c.axis)];
  double lo = std::min(c.pos_lo, c.pos_hi) - m, hi = std::max(c.pos_lo, c.pos_hi) + m;
  double wmin = c.axis == Axis::U ? w.umin : w.vmin;
  double wmax = c.axis == Axis::U ? w.umax : w.vmax;
  return hi >= wmin && lo <= wmax;
}

void GemEventFilter::make2D(const std::vector<seeds::ModuleWindow>* windows) {
  hits_.clear();
  for (size_t imod = 0; imod < trk_.modules.size(); ++imod) {
    const auto& mod = trk_.modules[imod];
    const auto& cu = mod_axis_clusters_[2 * imod];
    const auto& cv = mod_axis_clusters_[2 * imod + 1];
    if (cu.empty() || cv.empty()) continue;
    const seeds::ModuleWindow* w = windows ? &(*windows)[imod] : nullptr;
    if (w && !w->active) continue;
    int nmod = 0;
    for (int iu : cu) {
      const Cluster1D& U = clusters_[size_t(iu)];
      if (U.q < cfg_.min_cluster_q || U.imax_peak < cfg_.peak_sample_min || U.imax_peak > cfg_.peak_sample_max) continue;
      if (w && !clusterInWindow(U, *w)) continue;
      for (int iv : cv) {
        const Cluster1D& V = clusters_[size_t(iv)];
        if (V.q < cfg_.min_cluster_q || V.imax_peak < cfg_.peak_sample_min || V.imax_peak > cfg_.peak_sample_max) continue;
        if (w && !clusterInWindow(V, *w)) continue;
        double qs = U.q + V.q;
        double asym = qs > 0 ? std::abs(U.q - V.q) / qs : 1.0;
        if (asym > cfg_.max_asym) continue;
        double dt = U.t - V.t;
        if (std::abs(dt) > cfg_.max_dt_ns) continue;
        double x, y;
        mod.uvToXY(U.pos, V.pos, x, y);
        if (!mod.inActiveArea(x, y)) continue;
        if (w && (x < w->xmin - 1e-9 || x > w->xmax + 1e-9 || y < w->ymin - 1e-9 || y > w->ymax + 1e-9)) continue;
        Hit2D h;
        h.module = int(imod); h.layer = mod.layer; h.cu = iu; h.cv = iv;
        h.xl = x; h.yl = y; h.g = mod.detToTrack({x, y, 0}); h.q = 0.5 * qs; h.asym = asym; h.dt = dt;
        hits_.push_back(h);
        if (++nmod >= cfg_.max_hits_per_module) break;
      }
      if (nmod >= cfg_.max_hits_per_module) break;
    }
  }
}

namespace {
struct Fit { double x0, xp, y0, yp, chi2ndf; };
Fit fitLine(const std::vector<const Hit2D*>& hs, double sigma) {
  double S = 0, Sz = 0, Szz = 0, Sx = 0, Szx = 0, Sy = 0, Szy = 0;
  for (auto* h : hs) {
    double z = h->g.z;
    S += 1; Sz += z; Szz += z * z; Sx += h->g.x; Szx += z * h->g.x; Sy += h->g.y; Szy += z * h->g.y;
  }
  double D = S * Szz - Sz * Sz;
  Fit f{0, 0, 0, 0, 1e30};
  if (std::abs(D) < 1e-15) return f;
  f.xp = (S * Szx - Sz * Sx) / D; f.x0 = (Sx - f.xp * Sz) / S;
  f.yp = (S * Szy - Sz * Sy) / D; f.y0 = (Sy - f.yp * Sz) / S;
  double c2 = 0;
  for (auto* h : hs) {
    double dx = h->g.x - (f.x0 + f.xp * h->g.z), dy = h->g.y - (f.y0 + f.yp * h->g.z);
    c2 += (dx * dx + dy * dy) / (sigma * sigma);
  }
  int ndf = 2 * int(hs.size()) - 4;
  f.chi2ndf = ndf > 0 ? c2 / ndf : 0;
  return f;
}
}  // namespace

void GemEventFilter::findTracks(FilterResult& r) {
  const int L = trk_.nlayers;
  std::vector<std::vector<int>> by_layer(size_t(std::max(L, 1)));
  for (size_t i = 0; i < hits_.size(); ++i)
    if (hits_[i].layer >= 0 && hits_[i].layer < L) by_layer[size_t(hits_[i].layer)].push_back(int(i));
  int nl_with = 0;
  for (auto& v : by_layer) nl_with += !v.empty();
  if (nl_with < cfg_.min_layers) return;

  struct Cand { TrackCand t; int nl; };
  std::vector<Cand> cands;
  long npairs = 0;
  // seed-layer pairs ordered by decreasing span
  std::vector<std::pair<int, int>> lp;
  for (int a = 0; a < L; ++a)
    for (int b = a + cfg_.min_layers - 1; b < L; ++b) lp.push_back({a, b});
  std::stable_sort(lp.begin(), lp.end(), [](auto p, auto q) { return (p.second - p.first) > (q.second - q.first); });
  std::vector<char> seeded(hits_.size(), 0);
  for (auto [la, lb] : lp) {
    for (int ia : by_layer[size_t(la)]) {
      if (seeded[size_t(ia)]) continue;
      for (int ib : by_layer[size_t(lb)]) {
        if (++npairs > cfg_.max_pairs) { r.pair_limit_hit = true; goto done; }
        const Hit2D& A = hits_[size_t(ia)];
        const Hit2D& B = hits_[size_t(ib)];
        double dz = B.g.z - A.g.z;
        if (std::abs(dz) < 1e-6) continue;
        double xp = (B.g.x - A.g.x) / dz, yp = (B.g.y - A.g.y) / dz;
        std::vector<const Hit2D*> hs{&A, &B};
        std::vector<int> hi{ia, ib};
        for (int l = 0; l < L; ++l) {
          if (l == la || l == lb) continue;
          int best = -1; double bd = cfg_.search_window * cfg_.search_window;
          for (int ic : by_layer[size_t(l)]) {
            const Hit2D& C = hits_[size_t(ic)];
            double ex = A.g.x + xp * (C.g.z - A.g.z) - C.g.x;
            double ey = A.g.y + yp * (C.g.z - A.g.z) - C.g.y;
            double d2 = ex * ex + ey * ey;
            if (d2 < bd) { bd = d2; best = ic; }
          }
          if (best >= 0) { hs.push_back(&hits_[size_t(best)]); hi.push_back(best); }
        }
        if (int(hs.size()) < cfg_.min_layers) continue;
        Fit f = fitLine(hs, cfg_.sigma_hit);
        if (f.chi2ndf > cfg_.max_chi2ndf) continue;
        Cand c;
        c.t.hits = hi; c.t.x0 = f.x0; c.t.xp = f.xp; c.t.y0 = f.y0; c.t.yp = f.yp; c.t.chi2ndf = f.chi2ndf;
        c.nl = int(hs.size());
        for (int h : hi) seeded[size_t(h)] = 1;   // do not reseed from hits already on a candidate
        cands.push_back(std::move(c));
        break;
      }
    }
  }
done:
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    return a.nl != b.nl ? a.nl > b.nl : a.t.chi2ndf < b.t.chi2ndf;
  });
  std::vector<char> used(hits_.size(), 0);
  for (auto& c : cands) {
    if (int(r.tracks.size()) >= cfg_.max_tracks) break;
    bool clash = false;
    for (int h : c.t.hits) clash |= bool(used[size_t(h)]);
    if (clash) continue;
    for (int h : c.t.hits) used[size_t(h)] = 1;
    r.tracks.push_back(c.t);
  }
}

void GemEventFilter::keepCluster(const Cluster1D& c, FilterResult& r) const {
  const auto& idx = strips_by_modaxis_[size_t(2 * c.module + int(c.axis))];
  int lo = c.lo - cfg_.keep_margin_strips, hi = c.hi + cfg_.keep_margin_strips;
  auto it = std::lower_bound(idx.begin(), idx.end(), lo,
                             [&](int si, int v) { return strips_[size_t(si)].strip < v; });
  for (; it != idx.end() && strips_[size_t(*it)].strip <= hi; ++it) {
    const StripRef& s = strips_[size_t(*it)];
    r.keep[s.frame][s.hit] = true;
  }
}

FilterResult GemEventFilter::run(const std::vector<const ApvFrame*>& frames,
                                 const std::vector<seeds::ModuleWindow>* windows) {
  FilterResult r;
  r.keep.resize(frames.size());
  for (size_t f = 0; f < frames.size(); ++f) {
    r.keep[f].assign(frames[f]->hits.size(), false);
    r.strips_in += uint32_t(frames[f]->hits.size());
  }
  mapStrips(frames);
  // strips the map does not know are never dropped (cannot judge them)
  if (unmapped_) {
    std::vector<std::vector<char>> mapped(frames.size());
    for (size_t f = 0; f < frames.size(); ++f) mapped[f].assign(frames[f]->hits.size(), 0);
    for (auto& s : strips_) mapped[s.frame][s.hit] = 1;
    for (size_t f = 0; f < frames.size(); ++f)
      for (size_t h = 0; h < mapped[f].size(); ++h)
        if (!mapped[f][h]) r.keep[f][h] = true;
  }
  cluster1D();
  r.nclusters = uint32_t(clusters_.size());

  auto keepRoad = [&]() {
    if (!windows) {   // no road at all: keep everything
      for (auto& v : r.keep) std::fill(v.begin(), v.end(), true);
      return;
    }
    for (const auto& c : clusters_)
      if (clusterInWindow(c, (*windows)[size_t(c.module)])) keepCluster(c, r);
  };

  if (cfg_.mode == TrackerConfig::Mode::RoadOnly) {
    keepRoad();
  } else {
    make2D(windows);
    r.nhits2d = uint32_t(hits_.size());
    bool too_busy = windows && cfg_.max_hits2d_track > 0 && int(hits_.size()) > cfg_.max_hits2d_track;
    if (!too_busy) findTracks(r);
    size_t best_nl = r.tracks.empty() ? 0 : r.tracks.front().hits.size();
    if (r.tracks.empty()) {
      if (cfg_.fallback_road_if_no_track) { r.fallback = true; keepRoad(); }
    } else if (windows && int(best_nl) < cfg_.min_layers_confident) {
      r.fallback = true;
      keepRoad();
    } else {
      // keep, on every module the fitted track crosses, all U/V clusters whose extent comes within
      // keep_track_tol of the projected track coordinate (robust against wrong U/V pairing and
      // signal clusters merged with background at high occupancy)
      for (const auto& t : r.tracks) {
        Vec3 p0{t.x0, t.y0, 0}, dir = Vec3{t.xp, t.yp, 1}.unit();
        for (size_t imod = 0; imod < trk_.modules.size(); ++imod) {
          const auto& mod = trk_.modules[imod];
          Vec3 pl = mod.trackToDet(mod.intersect(p0, dir));
          if (std::abs(pl.x) > mod.half_x + cfg_.keep_track_tol || std::abs(pl.y) > mod.half_y + cfg_.keep_track_tol) continue;
          double u, v;
          mod.xyToUV(pl.x, pl.y, u, v);
          for (int a = 0; a < 2; ++a) {
            double c0 = a == 0 ? u : v;
            for (int ic : mod_axis_clusters_[2 * imod + size_t(a)]) {
              const Cluster1D& c = clusters_[size_t(ic)];
              double lo = std::min(c.pos_lo, c.pos_hi), hi = std::max(c.pos_lo, c.pos_hi);
              if (c0 >= lo - cfg_.keep_track_tol && c0 <= hi + cfg_.keep_track_tol) keepCluster(c, r);
            }
          }
        }
        for (int h : t.hits) {
          keepCluster(clusters_[size_t(hits_[size_t(h)].cu)], r);
          keepCluster(clusters_[size_t(hits_[size_t(h)].cv)], r);
        }
      }
    }
  }
  for (auto& v : r.keep)
    for (bool b : v) r.strips_out += b;
  return r;
}

}  // namespace track
}  // namespace gemfilter
