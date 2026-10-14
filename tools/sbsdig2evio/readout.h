// gemfilter / tools / sbsdig2evio : real-readout maps used by the sbsdig -> EVIO converter and
// by its verifier.  Everything comes from a Podd DB tree (SBS-replay DB, or the hybrid
// SIDIS-sim DB written by make_sim_db.py) through the gemfilter geom readers, i.e. the same
// code the online filter uses to decode.
#pragma once
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "gemfilter/geom/CaloGeometry.h"
#include "gemfilter/geom/GemGeometry.h"
#include "gemfilter/geom/PoddDb.h"
#include "sdg_format.h"

namespace gemfilter {
namespace sbsdig {

struct ReadoutConfig {
  std::string db_dir;
  std::string date = "2023-06-01 00:00:00";   // GEn-II BigBite GEM layout (DB/20220701)
  std::string gem = "bb.gem";
  std::vector<std::string> calo = {"bb.sh", "bb.ps", "sbs.hcal"};   // "" entries skipped
  std::vector<int> module_map;   // sim module (plane/2) -> DB module index; empty = identity
  int gem_baseline = 1500;
};

struct Readout {
  ReadoutConfig cfg;
  geom::DbTime date = 0;
  std::unique_ptr<geom::GemTracker> gem;
  std::unique_ptr<geom::ApvMap> apvmap;
  geom::CrateMap cmap;
  geom::CaloGeometry calo[kNPmtDet];
  bool have_calo[kNPmtDet] = {false, false, false};

  int dbModule(int sim_module) const {
    if (cfg.module_map.empty()) return sim_module;
    return sim_module < int(cfg.module_map.size()) ? cfg.module_map[size_t(sim_module)] : -1;
  }
  int simModule(int db_module) const {
    if (cfg.module_map.empty()) return db_module;
    for (size_t i = 0; i < cfg.module_map.size(); ++i)
      if (cfg.module_map[i] == db_module) return int(i);
    return -1;
  }

  static std::unique_ptr<Readout> load(const ReadoutConfig& c) {
    auto r = std::make_unique<Readout>();
    r->cfg = c;
    auto d = geom::parseDateTime(c.date);
    if (!d) throw std::runtime_error("bad --date " + c.date);
    r->date = *d;
    geom::DbDir dir(c.db_dir);
    r->gem = std::make_unique<geom::GemTracker>(geom::loadGemTracker(dir, c.gem, r->date));
    r->apvmap = std::make_unique<geom::ApvMap>(*r->gem, geom::ApvKeyField::Fiber, true);
    r->cmap = geom::CrateMap::load(dir, r->date);
    for (int k = 0; k < kNPmtDet; ++k) {
      bool want = false;
      for (auto& n : c.calo) want |= (n == pmtDetName(k));
      if (!want) continue;
      geom::CaloLoadOptions o;
      o.with_adc = true;
      o.with_tdc = false;
      r->calo[k] = geom::loadCalorimeter(dir, pmtDetName(k), r->date, &r->cmap, o);
      r->have_calo[k] = true;
    }
    return r;
  }
};

inline std::vector<int> parseIntList(const std::string& s) {
  std::vector<int> v;
  std::stringstream ss(s);
  std::string t;
  while (std::getline(ss, t, ',')) if (!t.empty()) v.push_back(std::stoi(t));
  return v;
}
inline std::vector<std::string> parseStrList(const std::string& s) {
  std::vector<std::string> v;
  std::stringstream ss(s);
  std::string t;
  while (std::getline(ss, t, ',')) if (!t.empty()) v.push_back(t);
  return v;
}

}  // namespace sbsdig
}  // namespace gemfilter
