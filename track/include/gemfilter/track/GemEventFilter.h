// gemfilter / track : online strip clustering, 2D hit formation and crude road tracking, producing
// a per-strip keep mask for the codec re-encoder.
//
// Modes
//  * RoadOnly : keep every 1D cluster that overlaps the per-module U/V window of the calorimeter
//               roads (= SBS-offline SBSGEMModule constraint acceptance, see seeds/Roads.h), plus a
//               margin.  Lossless w.r.t. offline tracking run with the same constraint widths.
//  * Track    : inside the roads, form 2D hits and search straight-line candidates with >= min_layers
//               layers; keep only strips of clusters on accepted candidates (+ margin).  If no
//               candidate is found, fall back to RoadOnly (default) or drop.
//  Without windows (no seed) the caller decides: keep all, drop all, or global tracking.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "gemfilter/geom/GemGeometry.h"
#include "gemfilter/seeds/Roads.h"
#include "gemfilter/types.h"

namespace gemfilter {
namespace track {

constexpr double kApvSampleNs = 24.0;   // 250 MHz / 6 = 41.67 MHz APV clock

struct StripRef {                       // one decoded strip mapped to detector coordinates
  int module = -1;
  geom::Axis axis = geom::Axis::U;
  int strip = -1;
  uint32_t frame = 0, hit = 0;          // index into the caller's frame list / ApvFrame::hits
  float qmax = 0;                       // max sample ADC
  float tmean = 0;                      // ADC-weighted sample time [ns]
  uint8_t imax = 0;                     // index of max sample
};

struct Cluster1D {
  int module = -1;
  geom::Axis axis = geom::Axis::U;
  int lo = 0, hi = 0;                   // strip range
  double pos = 0;                       // centroid along the strip-coordinate axis [m]
  double pos_lo = 0, pos_hi = 0;        // positions of strips lo / hi [m]
  double q = 0;                         // sum of qmax
  double t = 0;                         // charge-weighted time [ns]
  int imax_peak = 0;                    // max-sample index of the peak strip
  std::vector<uint32_t> strips;         // indices into the StripRef vector
};

struct Hit2D {
  int module = -1, layer = -1;
  int cu = -1, cv = -1;                 // cluster indices
  double xl = 0, yl = 0;                // module-local [m]
  geom::Vec3 g;                         // tracker frame [m]
  double q = 0, asym = 0, dt = 0;
};

struct TrackCand {
  std::vector<int> hits;                // Hit2D indices, one per layer
  double x0 = 0, xp = 0, y0 = 0, yp = 0;   // x = x0 + xp*z
  double chi2ndf = 0;
};

struct TrackerConfig {
  enum class Mode : uint8_t { RoadOnly = 0, Track = 1 };
  Mode mode = Mode::Track;
  // 1D clustering
  int max_strip_gap = 0;                // allowed missing strips inside a cluster
  int max_cluster_size = 12;
  double min_strip_adc = 0;             // on qmax (online ZS already applied upstream)
  double min_cluster_q = 0;
  int peak_sample_min = 0, peak_sample_max = 5;   // timing cut on the peak strip
  // 2D hits
  double max_asym = 0.6;                // |qU-qV|/(qU+qV)
  double max_dt_ns = 40;                // |tU - tV|
  int max_hits_per_module = 400;
  // tracking
  int min_layers = 3;
  double search_window = 0.006;         // [m] around the seed-pair line in intermediate layers
  double sigma_hit = 1.5e-4;            // [m]
  double max_chi2ndf = 30;
  int max_tracks = 8;
  long max_pairs = 200000;              // guard against combinatorics
  // keep
  int keep_margin_strips = 3;
  double keep_track_tol = 0.004;        // [m] along-track keep tolerance (U/V coordinate)
  bool fallback_road_if_no_track = true;
  int max_hits2d_track = 0;             // more 2D hits in the roads -> skip tracking, keep road (0 = off)
  int min_layers_confident = 0;         // best candidate with fewer layers -> keep whole road (0 = off)
};

struct FilterResult {
  KeepMask keep;                        // parallel to the input frames
  uint32_t strips_in = 0, strips_out = 0;
  uint32_t nclusters = 0, nhits2d = 0;
  std::vector<TrackCand> tracks;
  bool fallback = false;
  bool pair_limit_hit = false;
};

class GemEventFilter {
 public:
  GemEventFilter(const geom::GemTracker& trk, const geom::ApvMap& map, TrackerConfig cfg = {});

  // frames: all APV frames of ONE physics event (any number of banks).
  // windows: per-module windows (size == modules) or nullptr for "no road" (global search).
  FilterResult run(const std::vector<const ApvFrame*>& frames, const std::vector<seeds::ModuleWindow>* windows);

  // exposed for tests / monitoring (valid after run)
  const std::vector<StripRef>& strips() const { return strips_; }
  const std::vector<Cluster1D>& clusters() const { return clusters_; }
  const std::vector<Hit2D>& hits() const { return hits_; }
  uint32_t unmapped() const { return unmapped_; }
  const TrackerConfig& config() const { return cfg_; }

 private:
  void mapStrips(const std::vector<const ApvFrame*>& frames);
  void cluster1D();
  void make2D(const std::vector<seeds::ModuleWindow>* windows);
  void findTracks(FilterResult& r);
  bool clusterInWindow(const Cluster1D& c, const seeds::ModuleWindow& w) const;
  void keepCluster(const Cluster1D& c, FilterResult& r) const;

  const geom::GemTracker& trk_;
  const geom::ApvMap& map_;
  TrackerConfig cfg_;
  std::vector<StripRef> strips_;
  std::vector<Cluster1D> clusters_;
  std::vector<Hit2D> hits_;
  std::vector<std::vector<int>> mod_axis_clusters_;   // [2*module + axis] -> cluster indices
  std::vector<std::vector<int>> strips_by_modaxis_;   // [2*module + axis] -> strip indices (sorted)
  uint32_t unmapped_ = 0;
};

}  // namespace track
}  // namespace gemfilter
