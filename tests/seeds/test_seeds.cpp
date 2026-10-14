// Unit tests for gemfilter_geom + gemfilter_seeds.  DB values come from the SBS-replay snapshot in
// tests/seeds/data/DB (see PROVENANCE.txt); calorimeter / track inputs are SYNTHETIC.
#include <cmath>
#include <set>
#include <string>

#include "gemfilter/geom/ArmConfig.h"
#include "gemfilter/geom/CaloGeometry.h"
#include "gemfilter/geom/GemGeometry.h"
#include "gemfilter/seeds/CaloClustering.h"
#include "gemfilter/seeds/Roads.h"
#include "gemfilter/seeds/VtpCluster.h"
#include "testing.h"

using namespace gemfilter;
using namespace gemfilter::geom;
using namespace gemfilter::seeds;

static DbDir db() { return DbDir(GF_TEST_DB); }
static DbTime gmn() { return civilSeconds(2022, 1, 10); }
static DbTime gep() { return civilSeconds(2025, 4, 2); }

TEST_CASE(bb_gem_loads_and_apvmap_bijective) {
  GemTracker t = loadGemTracker(db(), "bb.gem", gmn());
  CHECK(t.nlayers >= 4);
  CHECK(t.modules.size() >= 4);
  size_t napv = 0;
  for (auto& m : t.modules) napv += m.apvs.size();
  CHECK(napv > 100);
  ApvMap map(t);
  CHECK_EQ(map.size(), napv);
  CHECK(map.duplicates().empty());
  // every APV maps every channel to a distinct strip within range
  for (auto& m : t.modules) {
    for (auto& a : m.apvs) {
      ApvAddress addr; addr.crate = a.crate; addr.slot = a.slot; addr.fiber = a.mpd_id; addr.adc = a.adc_id;
      std::set<int> strips;
      for (int ch = 0; ch < kApvChannels; ++ch) {
        StripRef s = map.strip(addr, ch);
        CHECK(s.valid());
        CHECK(s.strip < m.nstrips[int(s.axis)]);
        strips.insert(s.strip);
      }
      CHECK_EQ(strips.size(), size_t(kApvChannels));
    }
  }
}

TEST_CASE(ft_asbuilt_layer_z) {
  GemTracker t = loadGemTracker(db(), "sbs.gemFT", gep());
  CHECK_EQ(t.nlayers, 8);
  // As-built z (DB/20250401): 0, .075, .202, .341, .484, .624, .737/.769, .865/.897 m
  const double zmin[8] = {0, 0.075, 0.202, 0.341, 0.484, 0.624, 0.737, 0.865};
  const double zmax[8] = {0, 0.075, 0.202, 0.341, 0.484, 0.624, 0.769, 0.897};
  for (int l = 0; l < 8; ++l) {
    for (int im : t.modules_by_layer[size_t(l)]) {
      double z = t.modules[size_t(im)].origin.z;
      // later DB block holds alignment-corrected positions (e.g. 0.0697 vs nominal 0.075 m)
      CHECK(z > zmin[l] - 0.03 && z < zmax[l] + 0.03);
    }
  }
}

TEST_CASE(calo_cluster_synthetic) {
  CrateMap cm = CrateMap::load(db(), gmn());
  CaloGeometry sh = loadCalorimeter(db(), "bb.sh", gmn(), &cm);
  CHECK(sh.nelem() == 189);
  int b0 = sh.blockAt(10, 3);
  CHECK(b0 >= 0);
  std::vector<BlockHit> hits;
  hits.push_back({b0, 1.0, 0, 0});
  hits.push_back({sh.blockAt(11, 3), 0.5, 1, 0});
  hits.push_back({sh.blockAt(10, 4), 0.25, -1, 0});
  hits.push_back({sh.blockAt(2, 1), 0.3, 0, 0});   // separate cluster
  ClusterOptions o; o.emin_seed = 0.1;
  auto seeds = clusterBlocks(sh, hits, o, CaloSeed::Arm::BigBite);
  CHECK_EQ(seeds.size(), size_t(2));
  const auto& B0 = sh.blocks[size_t(b0)];
  const auto& B1 = sh.blocks[size_t(sh.blockAt(11, 3))];
  const auto& B2 = sh.blocks[size_t(sh.blockAt(10, 4))];
  double xe = (1.0 * B0.x + 0.5 * B1.x + 0.25 * B2.x) / 1.75;
  CHECK(std::abs(seeds[0].x - xe) < 1e-9);
  CHECK(std::abs(seeds[0].E - 1.75) < 1e-12);
  CHECK_EQ(seeds[0].nblocks, 3);
}

TEST_CASE(road_contains_straight_track) {
  GemTracker t = loadGemTracker(db(), "bb.gem", gmn());
  // synthetic straight track in tracker frame
  Vec3 p0{0.05, -0.02, 0.0}, dir = Vec3{0.12, 0.03, 1.0}.unit();
  ConstraintPoints cp;
  cp.front = p0;
  double zb = 3.0;
  cp.back = p0 + dir * (zb / dir.z);
  cp.fwx = cp.fwy = cp.bwx = cp.bwy = 0.02;
  auto wins = moduleWindows(t, {cp});
  int nin = 0;
  for (size_t i = 0; i < t.modules.size(); ++i) {
    const auto& m = t.modules[i];
    Vec3 hit = m.trackToDet(m.intersect(p0, dir));
    if (!m.inActiveArea(hit.x, hit.y)) continue;
    const auto& w = wins[i];
    CHECK(w.active);
    double u, v; m.xyToUV(hit.x, hit.y, u, v);
    CHECK(u >= w.umin && u <= w.umax && v >= w.vmin && v <= w.vmax);
    CHECK(hit.x >= w.xmin && hit.x <= w.xmax);
    // a point 5 cm away is outside in x
    CHECK(!(hit.x + 0.05 >= w.xmin && hit.x + 0.05 <= w.xmax));
    ++nin;
  }
  CHECK(nin >= 4);
  Road r = makeRoad(cp, t);
  CHECK_EQ(int(r.layers.size()), t.nlayers);
}

TEST_CASE(bigbite_constraint_from_db) {
  BigBiteConfig c = loadBigBiteConfig(db(), gmn());
  CrateMap cm = CrateMap::load(db(), gmn());
  CaloGeometry sh = loadCalorimeter(db(), "bb.sh", gmn(), &cm);
  CaloGeometry ps = loadCalorimeter(db(), "bb.ps", gmn(), &cm);
  CaloSeed s; s.x = 0.1; s.y = 0.02; s.E = 2.0;
  CaloSeed p; p.x = 0.12; p.y = 0.0; p.E = 0.5;
  ConstraintPoints cp = bigbiteConstraint(c, sh, s, &ps, &p);
  CHECK(std::isfinite(cp.front.x) && std::isfinite(cp.front.y));
  CHECK(cp.back.z > 0.5);              // calorimeter is well behind the first GEM
  CHECK(std::abs(cp.front.z) < 1e-12);
  CHECK(std::abs(cp.Etot - 2.5) < 1e-12);
  CHECK(cp.fwx > 0 && cp.bwx > 0);
}

TEST_CASE(vtp_roundtrip_to_seed) {
  VtpEvent ev; ev.trigger_number = 7; ev.slot = 11;
  VtpCluster cl; cl.det = VtpDet::HCal; cl.subtype = 13; cl.energy = 1234; cl.row = 12; cl.col = 6; cl.nhits = 4; cl.time = 100;
  ev.clusters.push_back(cl);
  auto words = encodeVtpBlock(11, {ev});
  auto dec = decodeVtp(words);
  CHECK_EQ(dec.nerrors, 0u);
  CHECK_EQ(dec.events.size(), size_t(1));
  CHECK_EQ(dec.events[0].clusters.size(), size_t(1));
  CHECK_EQ(dec.events[0].clusters[0].row, 12u);
  CHECK_EQ(dec.events[0].clusters[0].col, 6u);
  CrateMap cm = CrateMap::load(db(), gep());
  CaloGeometry hcal = loadCalorimeter(db(), "sbs.hcal", gep(), &cm);
  auto seeds = vtpSeeds(dec.events[0], hcal, CaloSeed::Arm::SBS);
  CHECK_EQ(seeds.size(), size_t(1));
  int b = hcal.blockAt(12, 6);
  CHECK(b >= 0);
  CHECK(std::abs(seeds[0].x - hcal.blocks[size_t(b)].x) < 1e-9);
}

TEST_MAIN()
