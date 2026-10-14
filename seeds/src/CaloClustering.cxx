// gemfilter / seeds : FADC block hits and simple calorimeter clustering (see CaloClustering.h).
//
// Clustering follows the spirit of SBSCalorimeter::MakeGoodBlocks/FindClusters in SBS-offline:
// blocks sorted by energy, highest unused block above emin_clSeed starts a cluster, neighbours
// within +-half_rows/+-half_cols and |t - t_seed| < tmax are added, position is energy-weighted.
// This is a crude online version: no cluster splitting, no rmax_dis iteration.
#include "gemfilter/seeds/CaloClustering.h"

#include <algorithm>
#include <cmath>

namespace gemfilter {
namespace seeds {

const FadcChannelData* FadcEventStore::find(int crate, int slot, int chan) const {
  auto it = events_.find(crate);
  if (it == events_.end()) return nullptr;
  for (const FadcEvent* ev : it->second) {
    if (const FadcChannelData* d = ev->find(slot, chan)) return d;
  }
  return nullptr;
}

std::vector<BlockHit> blockHitsFromFadc(const geom::CaloGeometry& calo, const FadcEventStore& store,
                                        const BlockHitOptions& opt) {
  std::vector<BlockHit> out;
  out.reserve(64);
  for (const auto& b : calo.blocks) {
    if (b.adc_crate < 0) continue;
    const FadcChannelData* d = store.find(b.adc_crate, b.adc_slot, b.adc_chan);
    if (!d) continue;
    PulseParams p;
    p.chan_to_mV = b.conv;
    p.gain = b.gain;
    p.thres_mV = b.thres;
    p.nsb = b.nsb; p.nsa = b.nsa; p.npedbin = b.npedbin; p.fixthresbin = b.fixthresbin;
    p.timeoffset = b.timeoffset;
    p.amp_cal = b.amp_to_int * b.gain;
    PulseResult r;
    if (!d->samples.empty()) {
      r = processWaveform(d->samples, p);
    } else if (opt.use_pulse_mode && !d->integral.empty()) {
      double t = d->time.empty() ? 0.0 : double(d->time[0]);
      double pk = d->peak.empty() ? 0.0 : double(d->peak[0]);
      double ped = d->pedestal.empty() ? 0.0 : double(d->pedestal[0]);
      r = processPulse(double(d->integral[0]), t, pk, ped, p);
    } else {
      continue;
    }
    if (!r.has_data) continue;
    if (opt.require_threshold && !r.threshold_crossed) continue;
    if (r.energy <= opt.min_energy) continue;
    out.push_back({b.index, r.energy, r.time_ns, r.amplitude_mV});
  }
  return out;
}

ClusterOptions ClusterOptions::fromDb(const geom::CaloGeometry& calo) {
  ClusterOptions o;
  o.emin_seed = calo.cluster.emin_clSeed;
  o.emin_block = calo.cluster.emin;
  o.emin_total = calo.cluster.emin_clTotal;
  o.tmax = calo.cluster.tmax;
  o.nmax = calo.cluster.nmax_cluster;
  if (calo.cluster.cluster_dim.size() >= 2) {
    o.half_rows = std::max(0, (calo.cluster.cluster_dim[0] - 1) / 2);
    o.half_cols = std::max(0, (calo.cluster.cluster_dim[1] - 1) / 2);
  }
  return o;
}

std::vector<CaloSeed> clusterBlocks(const geom::CaloGeometry& calo, const std::vector<BlockHit>& hits,
                                    const ClusterOptions& opt, CaloSeed::Arm arm, CaloSeed::Source src) {
  std::vector<CaloSeed> seeds;
  if (hits.empty()) return seeds;
  std::vector<size_t> order(hits.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return hits[a].E > hits[b].E; });
  std::vector<char> used(hits.size(), 0);
  // block index -> hit index for neighbour lookup
  std::vector<int> hit_of_block(size_t(std::max(calo.nelem(), 1)), -1);
  for (size_t i = 0; i < hits.size(); ++i)
    if (hits[i].block >= 0 && hits[i].block < calo.nelem()) hit_of_block[size_t(hits[i].block)] = int(i);

  for (size_t oi : order) {
    if (used[oi]) continue;
    const BlockHit& s = hits[oi];
    if (s.E < opt.emin_seed) break;   // sorted: no further seeds
    if (s.block < 0 || s.block >= calo.nelem()) continue;
    const auto& sb = calo.blocks[size_t(s.block)];
    double esum = 0, xs = 0, ys = 0;
    int nb = 0;
    for (int dr = -opt.half_rows; dr <= opt.half_rows; ++dr) {
      for (int dc = -opt.half_cols; dc <= opt.half_cols; ++dc) {
        int bi = calo.blockAt(sb.row + dr, sb.col + dc);
        if (bi < 0) continue;
        int hi = hit_of_block[size_t(bi)];
        if (hi < 0 || used[size_t(hi)]) continue;
        const BlockHit& h = hits[size_t(hi)];
        if (h.E < opt.emin_block) continue;
        if (std::abs(h.t - s.t) > opt.tmax) continue;
        const auto& bb = calo.blocks[size_t(bi)];
        esum += h.E; xs += h.E * bb.x; ys += h.E * bb.y; ++nb;
        used[size_t(hi)] = 1;
      }
    }
    if (nb == 0 || esum < opt.emin_total) continue;
    CaloSeed c;
    c.arm = arm; c.source = src;
    c.x = xs / esum; c.y = ys / esum; c.z = 0.0;
    c.E = esum; c.t = s.t; c.nblocks = nb;
    seeds.push_back(c);
    if (int(seeds.size()) >= opt.nmax) break;
  }
  return seeds;
}

std::vector<PreshowerMatch> matchPreshower(const std::vector<CaloSeed>& sh_seeds, const geom::CaloGeometry& ps,
                                           const std::vector<BlockHit>& ps_hits, double maxdx, double maxdy,
                                           double ps_tmax) {
  // Cluster the preshower with a wide row window (PS blocks are long in y), then attach the
  // nearest PS cluster to each shower seed within (maxdx, maxdy) and |dt| < ps_tmax.
  ClusterOptions o = ClusterOptions::fromDb(ps);
  o.tmax = ps_tmax;
  auto ps_seeds = clusterBlocks(ps, ps_hits, o, CaloSeed::Arm::BigBite, CaloSeed::Source::FADC);
  std::vector<PreshowerMatch> out(sh_seeds.size());
  std::vector<char> taken(ps_seeds.size(), 0);
  for (size_t i = 0; i < sh_seeds.size(); ++i) {
    double best = 1e30; int ib = -1;
    for (size_t j = 0; j < ps_seeds.size(); ++j) {
      if (taken[j]) continue;
      double dx = ps_seeds[j].x - sh_seeds[i].x, dy = ps_seeds[j].y - sh_seeds[i].y;
      if (std::abs(dx) > maxdx || std::abs(dy) > maxdy) continue;
      if (std::abs(ps_seeds[j].t - sh_seeds[i].t) > ps_tmax) continue;
      double d2 = dx * dx / (maxdx * maxdx) + dy * dy / (maxdy * maxdy);
      if (d2 < best) { best = d2; ib = int(j); }
    }
    if (ib >= 0) { out[i].ps = ps_seeds[size_t(ib)]; out[i].matched = true; taken[size_t(ib)] = 1; }
  }
  return out;
}

}  // namespace seeds
}  // namespace gemfilter
