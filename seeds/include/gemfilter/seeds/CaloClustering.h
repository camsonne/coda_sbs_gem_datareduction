// gemfilter / seeds : calorimeter block hits from FADC250 data and simple seed clustering.
//
// Block energies/times are computed per block with the SBS-offline waveform algorithm
// (processWaveform) using that block's DB parameters (adc.conv, adc.thres, NSB, NSA,
// NPedBin, FixThresBin, adc.gain, adc.timeoffset). Reference-time subtraction
// (refadc channels) is NOT applied (block times are raw FADC times + timeoffset).
//
// Clustering (filter-oriented, NOT a bit-exact port of SBSCalorimeter::MakeCluster):
//   seeds    = blocks with E >= emin_seed that are local maxima in a
//              (2*nrow+1) x (2*ncol+1) neighbourhood (ties broken by block index)
//   members  = unassigned blocks inside the neighbourhood of the seed with E >= emin_block
//              and |t - t_seed| <= tmax
//   position = energy-weighted block (x,y), calorimeter-local frame; z = calo origin z
//   time     = seed-block time; E = sum of member energies
// Defaults come from the calorimeter DB (emin_clSeed, emin, tmax, cluster_dim).
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "gemfilter/geom/CaloGeometry.h"
#include "gemfilter/seeds/Fadc250.h"
#include "gemfilter/types.h"

namespace gemfilter {
namespace seeds {

struct BlockHit {
  int block = -1;
  double E = 0;       // calibrated (DB gain), GeV for the SBS calorimeter DBs
  double t = 0;       // ns
  double amp_mV = 0;
};

// Per-crate FADC event data for one physics event (crate = ROC id).
class FadcEventStore {
 public:
  void add(int crate, const FadcEvent& ev) { events_[crate].push_back(&ev); }
  const FadcChannelData* find(int crate, int slot, int chan) const;
  void clear() { events_.clear(); }

 private:
  std::map<int, std::vector<const FadcEvent*>> events_;
};

struct BlockHitOptions {
  double min_energy = -1e30;      // keep blocks with E above this (calibrated units)
  bool require_threshold = true;  // drop blocks whose waveform never crossed threshold
  bool use_pulse_mode = true;     // fall back to pulse-mode words if no samples
};

std::vector<BlockHit> blockHitsFromFadc(const geom::CaloGeometry& calo, const FadcEventStore& store,
                                        const BlockHitOptions& opt = {});

struct ClusterOptions {
  double emin_seed = 0.02, emin_block = 0.001, tmax = 50;  // GeV, GeV, ns
  int half_rows = 1, half_cols = 1;                        // 1,1 = 3x3
  int nmax = 10;
  double emin_total = 0;
  static ClusterOptions fromDb(const geom::CaloGeometry& calo);
};

std::vector<CaloSeed> clusterBlocks(const geom::CaloGeometry& calo, const std::vector<BlockHit>& hits,
                                    const ClusterOptions& opt, CaloSeed::Arm arm,
                                    CaloSeed::Source src = CaloSeed::Source::FADC);

// BigBite "total shower": attach preshower blocks within |dx|<=MaxDx, |dy|<=MaxDy,
// |dt|<=ps_tmax of each shower seed (mirrors the SBSBBTotalShower match criterion), producing
// an energy-weighted preshower cluster per shower seed (nblocks == 0 if none matched).
struct PreshowerMatch {
  CaloSeed ps;          // preshower cluster (local frame of the preshower)
  bool matched = false;
};
std::vector<PreshowerMatch> matchPreshower(const std::vector<CaloSeed>& sh_seeds, const geom::CaloGeometry& ps,
                                           const std::vector<BlockHit>& ps_hits, double maxdx, double maxdy,
                                           double ps_tmax);

}  // namespace seeds
}  // namespace gemfilter
