// gemfilter / track : SYNTHETIC toy event generator in real SBS GEM geometry (DB-driven).
// Produces ApvFrames (online-ZS style: only fired strips) with truth flags, for unit tests,
// synthetic EVIO files and performance studies.  Not a detector simulation: background is a
// uniform per-strip cluster occupancy with random timing; signal is straight tracks.
#pragma once
#include <cstdint>
#include <map>
#include <random>
#include <vector>

#include "gemfilter/geom/GemGeometry.h"
#include "gemfilter/types.h"

namespace gemfilter {
namespace track {

struct ToyTrack { geom::Vec3 p0; geom::Vec3 dir; };   // tracker frame, p0 at z = 0

struct ToyConfig {
  double occupancy = 0.05;        // fraction of strips fired by background (per module & axis)
  double bkg_cluster_mean = 3.0;  // mean strips per background cluster
  double signal_amp = 1500;       // ADC peak of a track cluster (central strip, before sharing)
  double bkg_amp = 600;           // mean background cluster amplitude
  double cluster_sigma = 0.55;    // charge-sharing sigma [strips]
  double tau_ns = 56;             // CR-RC shaping time
  double signal_t0 = 30;          // ns, track pulse start time w.r.t. first sample
  double bkg_t0_min = -150, bkg_t0_max = 150;   // ns, background start-time range
  double strip_threshold = 50;    // ADC: online-ZS emulation on max sample
};

struct ToyEvent {
  std::vector<ApvFrame> frames;
  std::vector<std::vector<char>> truth;   // [frame][hit] 1 = contains signal charge
  std::vector<ToyTrack> tracks;
  uint32_t signal_strips = 0, total_strips = 0;
};

class ToyGenerator {
 public:
  ToyGenerator(const geom::GemTracker& trk, const geom::ApvMap& map, uint32_t seed = 1);
  ToyEvent generate(const std::vector<ToyTrack>& tracks, const ToyConfig& cfg);
  // random track pointing into the tracker acceptance (slopes uniform in +-max_slope)
  ToyTrack randomTrack(double max_x0, double max_y0, double max_xp, double max_yp);
  std::mt19937& rng() { return rng_; }

 private:
  struct ChanRef { ApvAddress addr; uint8_t chan; };
  const geom::GemTracker& trk_;
  std::vector<std::vector<ChanRef>> inv_;   // [2*module+axis][strip] -> channel (addr.crate == 0xFFFF: none)
  std::mt19937 rng_;
};

}  // namespace track
}  // namespace gemfilter
