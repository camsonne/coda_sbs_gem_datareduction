// gemfilter / geom : crate map + calorimeter geometry (see CaloGeometry.h).
#include "gemfilter/geom/CaloGeometry.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

namespace gemfilter {
namespace geom {

// ---------------------------------------------------------------- CrateMap
bool CrateMap::isAdcModel(int m) {
  static const std::set<int> adc{1881, 3123, 1182, 792, 250};
  return adc.count(m) > 0;
}
bool CrateMap::isTdcModel(int m) {
  static const std::set<int> tdc{1875, 1877, 1872, 775, 767, 3201, 6401, 1190, 526, 527};
  return tdc.count(m) > 0;
}

CrateMap CrateMap::fromString(const std::string& text, DbTime date) {
  CrateMap cm;
  std::istringstream s(text);
  std::string line;
  DbTime keydate = 0, prevdate = 0;
  bool do_ignore = false, in_crate = false;
  int crate = -1;
  int lineno = 0;
  while (std::getline(s, line)) {
    ++lineno;
    auto pos = line.find_first_of("!#");
    if (pos != std::string::npos) line.erase(pos);
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    if (auto d = parseDbDateTag(line)) {
      keydate = *d;
      do_ignore = (keydate > date || keydate < prevdate);
      in_crate = false;
      continue;
    } else if (do_ignore) {
      continue;
    }
    prevdate = keydate;
    if (!in_crate && line.find("TSROC") != std::string::npos) {
      unsigned ts = 0;
      if (std::sscanf(line.c_str(), " TSROC %u", &ts) == 1) cm.tsroc_ = static_cast<int>(ts);
      continue;
    }
    pos = line.find("Crate");
    if (pos != std::string::npos) {
      line.erase(0, pos);
      unsigned c = 0;
      char ctype[21] = {0};
      if (std::sscanf(line.c_str(), "Crate %u type %20s", &c, ctype) != 2)
        throw DbError("cratemap: bad crate line " + std::to_string(lineno));
      crate = static_cast<int>(c);
      cm.crates_[crate].clear();  // resetCrate
      cm.types_[crate] = ctype;
      in_crate = std::string(ctype) != "unused";
      if (!in_crate) cm.crates_.erase(crate);
      continue;
    }
    if (crate < 0 || !in_crate) throw DbError("cratemap: slot line outside crate at line " + std::to_string(lineno));
    // loadConfig: strip "cfg:" part
    std::string cfg;
    auto cpos = line.find("cfg:");
    if (cpos != std::string::npos) {
      cfg = line.substr(cpos + 4);
      line.erase(cpos);
    }
    unsigned slot = 0, ichan = 0, idata = 0, iheader = 0, mask = 0;
    int imodel = 0, cword = 1;
    int nread = std::sscanf(line.c_str(), "%u %d %d %x %x %u %u", &slot, &imodel, &cword, &iheader, &mask, &ichan,
                            &idata);
    if (nread < 2) throw DbError("cratemap: bad slot line " + std::to_string(lineno));
    CrateSlot cs;
    cs.crate = crate;
    cs.slot = static_cast<int>(slot);
    cs.model = imodel;
    if (nread == 3) cs.bank = cword;
    cs.cfg = cfg;
    cm.crates_[crate][cs.slot] = cs;
  }
  return cm;
}

CrateMap CrateMap::load(const std::string& path, DbTime date) {
  std::ifstream in(path);
  if (!in) throw DbError("cannot open crate map " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return fromString(ss.str(), date);
}

const CrateSlot* CrateMap::slot(int crate, int slot) const {
  auto c = crates_.find(crate);
  if (c == crates_.end()) return nullptr;
  auto s = c->second.find(slot);
  return s == c->second.end() ? nullptr : &s->second;
}
int CrateMap::model(int crate, int s) const {
  auto* p = slot(crate, s);
  return p ? p->model : 0;
}
int CrateMap::bank(int crate, int s) const {
  auto* p = slot(crate, s);
  return p ? p->bank : -1;
}
std::vector<int> CrateMap::crates() const {
  std::vector<int> v;
  for (auto& c : crates_) v.push_back(c.first);
  return v;
}
std::vector<int> CrateMap::slotsWithModel(int crate, int m) const {
  std::vector<int> v;
  auto c = crates_.find(crate);
  if (c == crates_.end()) return v;
  for (auto& s : c->second)
    if (s.second.model == m) v.push_back(s.first);
  return v;
}
std::string CrateMap::type(int crate) const {
  auto t = types_.find(crate);
  return t == types_.end() ? std::string{} : t->second;
}

// ---------------------------------------------------------------- calorimeter
int CaloGeometry::blockAt(int row, int col) const {
  if (row < 0 || row >= static_cast<int>(grid.size())) return -1;
  if (col < 0 || col >= static_cast<int>(grid[row].size())) return -1;
  return grid[row][col];
}
int CaloGeometry::blockForAdc(int crate, int slot, int chan) const {
  auto it = adc_lookup.find(chanKey(crate, slot, chan));
  return it == adc_lookup.end() ? -1 : it->second;
}
int CaloGeometry::blockForTdc(int crate, int slot, int chan) const {
  auto it = tdc_lookup.find(chanKey(crate, slot, chan));
  return it == tdc_lookup.end() ? -1 : it->second;
}

CaloGeometry loadCalorimeter(const DbDir& dir, const std::string& name, DbTime date, const CrateMap* cratemap,
                             const CaloLoadOptions& opt) {
  DbFile f = dir.open(name, date);
  return loadCalorimeter(f, name, date, cratemap, opt);
}

namespace {
template <typename T>
std::vector<T> expandParam(const std::vector<T>& in, T def, int nelem, const std::string& key) {
  if (in.empty()) return std::vector<T>(nelem, def);
  if (in.size() == 1) return std::vector<T>(nelem, in[0]);
  if (static_cast<int>(in.size()) != nelem)
    throw DbError("inconsistent number of " + key + " values: " + std::to_string(in.size()) + " for " +
                  std::to_string(nelem) + " elements");
  return in;
}
std::vector<int> toIntVec(const std::vector<long long>& v) { return {v.begin(), v.end()}; }
}  // namespace

CaloGeometry loadCalorimeter(const DbFile& file, const std::string& name, DbTime date, const CrateMap* cratemap,
                             const CaloLoadOptions& opt) {
  CaloGeometry g;
  g.name = name;
  g.dbfile = file.path();
  g.date = date;
  DbReader r(file, date, name + ".");

  // THaDetectorBase::ReadGeometry(required=true): position (search 0), size (search 1)
  auto pos = r.getDoubles("position", 0);
  if (pos.size() != 3) throw DbError(name + ".position must have 3 values");
  g.origin = {pos[0], pos[1], pos[2]};
  auto size = r.getDoubles("size", 1);
  if (size.size() == 3)
    for (int i = 0; i < 3; ++i) g.size[i] = size[i];

  // SBSGenericDetector::ReadDatabase config_request (prefix search 0)
  auto detmap = toIntVec(r.getInts("detmap", 0));
  int model_in_detmap = static_cast<int>(r.getInt("model_in_detmap", 0LL, 0));
  auto chanmap = toIntVec(r.getInts("chanmap", 0, false));
  g.chanmap_start = static_cast<int>(r.getInt("start_chanmap", 0LL, 0));
  g.nrows = static_cast<int>(r.getInt("nrows", 1LL, 0));
  auto ncols = toIntVec(r.getInts("ncols", 0));
  g.nlayers = static_cast<int>(r.getInt("nlayers", 1LL, 0));
  auto xyz = r.getDoubles("xyz", 0);
  auto dxyz = r.getDoubles("dxdydz", 0);
  if (xyz.size() < 3 || dxyz.size() < 3) throw DbError(name + ": xyz/dxdydz need 3 values");
  g.size_row = dxyz[0];
  g.size_col = dxyz[1];
  if (g.nrows <= 0 || ncols.empty() || static_cast<int>(ncols.size()) > g.nrows || g.nlayers <= 0)
    throw DbError(name + ": illegal rows/cols/layers");
  {
    int ntemp = static_cast<int>(ncols.size());
    for (int rr = ntemp, i = 0; rr < g.nrows; ++rr, ++i) ncols.push_back(ncols[i % ntemp]);
  }
  g.ncols = ncols;
  int nelem = 0;
  for (int rr = 0; rr < g.nrows; ++rr) nelem += ncols[rr] * g.nlayers;

  // detmap (THaDetMap::Fill with kFillRefIndex [+kFillModel])
  size_t tuple = model_in_detmap ? 6 : 5;
  for (size_t i = 0; i < detmap.size(); i += tuple) {
    if (detmap[i] < 0) break;
    if (i + tuple > detmap.size()) throw DbError(name + ": detmap has incomplete tuple");
    DetMapModule m;
    m.crate = detmap[i];
    m.slot = detmap[i + 1];
    m.lo = detmap[i + 2];
    m.hi = detmap[i + 3];
    size_t k = 4;
    if (model_in_detmap) m.model = detmap[i + k++];
    m.refindex = detmap[i + k++];
    if (m.model == 0 && cratemap) m.model = cratemap->model(m.crate, m.slot);
    if (m.model != 0) {
      m.is_tdc = CrateMap::isTdcModel(m.model);
      if (!m.is_tdc && !CrateMap::isAdcModel(m.model))
        throw DbError(name + ": unknown module model " + std::to_string(m.model) + " at crate " +
                      std::to_string(m.crate) + " slot " + std::to_string(m.slot));
    } else {
      // UNVERIFIED: no crate map / model available. Heuristic: if the detector has TDCs,
      // modules with more than 16 channels (F1/VETROC) are TDCs; FADC250 has 16 channels.
      m.is_tdc = opt.with_tdc && (m.hi >= 16);
    }
    g.detmap.push_back(m);
  }
  if (g.detmap.empty()) throw DbError(name + ": empty detmap");

  // blocks
  g.blocks.resize(nelem);
  g.grid.assign(g.nrows, {});
  {
    int k = 0;
    for (int rr = 0; rr < g.nrows; ++rr) {
      g.grid[rr].assign(ncols[rr], -1);
      for (int c = 0; c < ncols[rr]; ++c)
        for (int l = 0; l < g.nlayers; ++l, ++k) {
          CaloBlock& b = g.blocks[k];
          b.index = k;
          b.row = rr;
          b.col = c;
          b.layer = l;
          b.x = xyz[0] - rr * dxyz[0];
          b.y = xyz[1] - c * dxyz[1];
          b.z = xyz[2] - l * dxyz[2];
          if (l == 0) g.grid[rr][c] = k;
        }
    }
  }
  // xpos/ypos overrides (SBSCalorimeter::ReadDatabase)
  auto xpos = r.getDoubles("xpos", 0, false);
  auto ypos = r.getDoubles("ypos", 0, false);
  if (static_cast<int>(xpos.size()) == nelem)
    for (int k = 0; k < nelem; ++k) g.blocks[k].x = xpos[k];
  if (static_cast<int>(ypos.size()) == nelem)
    for (int k = 0; k < nelem; ++k) g.blocks[k].y = ypos[k];

  // channel -> element map
  int nskipped = 0, nref = 0;
  for (int v : chanmap) {
    if (v == -1) ++nskipped;
    if (v == -1000) ++nref;
  }
  int k = 0, ka = 0, kt = 0;
  for (const auto& m : g.detmap) {
    if (m.nchan() <= 0) throw DbError(name + ": detmap module with no channels");
    bool adc = !m.is_tdc;
    for (int chan = 0; chan < m.nchan(); ++chan, ++k) {
      int km;
      if (!chanmap.empty()) {
        if (k >= static_cast<int>(chanmap.size())) throw DbError(name + ": chanmap shorter than detmap");
        int v = chanmap[k];
        if (v == -1) continue;
        if (v == -1000) continue;  // reference channel
        km = v - g.chanmap_start;
      } else {
        km = adc ? ka : kt;
      }
      if (adc) ++ka; else ++kt;
      if (km < 0 || km >= nelem) throw DbError(name + ": chanmap element out of range: " + std::to_string(km));
      CaloBlock& b = g.blocks[km];
      if (adc) {
        if (b.adc_crate >= 0) throw DbError(name + ": block " + std::to_string(km) + " has two ADC channels");
        b.adc_crate = m.crate; b.adc_slot = m.slot; b.adc_chan = m.lo + chan;
        g.adc_lookup[CaloGeometry::chanKey(m.crate, m.slot, m.lo + chan)] = km;
      } else {
        if (b.tdc_crate >= 0) throw DbError(name + ": block " + std::to_string(km) + " has two TDC channels");
        b.tdc_crate = m.crate; b.tdc_slot = m.slot; b.tdc_chan = m.lo + chan;
        g.tdc_lookup[CaloGeometry::chanKey(m.crate, m.slot, m.lo + chan)] = km;
      }
    }
  }
  (void)nskipped;
  (void)nref;
  if (opt.with_adc && ka != nelem)
    throw DbError(name + ": inconsistent ADC channels, found " + std::to_string(ka) + " expected " +
                  std::to_string(nelem));
  if (opt.with_tdc && kt != nelem)
    throw DbError(name + ": inconsistent TDC channels, found " + std::to_string(kt) + " expected " +
                  std::to_string(nelem));

  // ADC calibration arrays (SBSGenericDetector::ReadDatabase second request)
  if (opt.with_adc) {
    auto ped = expandParam(r.getDoubles("adc.pedestal", 0, false), 0.0, nelem, "adc.pedestal");
    auto gain = expandParam(r.getDoubles("adc.gain", 0, false), 1.0, nelem, "adc.gain");
    auto conv = expandParam(r.getDoubles("adc.conv", 0, false), 1.0, nelem, "adc.conv");
    auto thr = expandParam(r.getDoubles("adc.thres", 0, false), 1.0, nelem, "adc.thres");
    auto toff = expandParam(r.getDoubles("adc.timeoffset", 0, false), 0.0, nelem, "adc.timeoffset");
    auto a2i = expandParam(r.getDoubles("adc.AmpToIntRatio", 0, false), 1.0, nelem, "adc.AmpToIntRatio");
    auto ftb = expandParam(toIntVec(r.getInts("adc.FixThresBin", 0, false)), 10, nelem, "adc.FixThresBin");
    auto nsb = expandParam(toIntVec(r.getInts("adc.NSB", 0, false)), 3, nelem, "adc.NSB");
    auto nsa = expandParam(toIntVec(r.getInts("adc.NSA", 0, false)), 10, nelem, "adc.NSA");
    auto npb = expandParam(toIntVec(r.getInts("adc.NPedBin", 0, false)), 4, nelem, "adc.NPedBin");
    for (int i = 0; i < nelem; ++i) {
      CaloBlock& b = g.blocks[i];
      b.ped = ped[i]; b.gain = gain[i]; b.conv = conv[i]; b.thres = thr[i]; b.timeoffset = toff[i];
      b.amp_to_int = a2i[i]; b.fixthresbin = ftb[i]; b.nsb = nsb[i]; b.nsa = nsa[i]; b.npedbin = npb[i];
    }
  }

  // SBSCalorimeter cluster parameters (all optional)
  g.cluster.emin = r.getDouble("emin", g.cluster.emin, 0);
  g.cluster.tmax = r.getDouble("tmax", g.cluster.tmax, 0);
  g.cluster.emin_clSeed = r.getDouble("emin_clSeed", g.cluster.emin_clSeed, 0);
  g.cluster.emin_clTotal = r.getDouble("emin_clTotal", g.cluster.emin_clTotal, 0);
  g.cluster.rmax_dis = r.getDouble("Rmax_dis", g.cluster.rmax_dis, 0);
  g.cluster.nmax_cluster = static_cast<int>(r.getInt("nmax_cluster", g.cluster.nmax_cluster, 0));
  auto cdim = toIntVec(r.getInts("cluster_dim", 0, false));
  if (cdim.size() == 2) g.cluster.cluster_dim = cdim;

  g.provenance = r.used();
  return g;
}

}  // namespace geom
}  // namespace gemfilter
