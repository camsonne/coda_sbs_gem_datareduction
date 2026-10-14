// gemfilter / geom : GEM tracker geometry + APV map (see GemGeometry.h).
#include "gemfilter/geom/GemGeometry.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

namespace gemfilter {
namespace geom {

namespace {
std::array<std::array<int, kApvChannels>, 4> buildApvMaps() {
  std::array<std::array<int, kApvChannels>, 4> t{};
  for (int i = 0; i < kApvChannels; ++i) {
    // Verbatim from SBSGEMModule::InitAPVMAP (integer arithmetic)
    int strip1 = 32 * (i % 4) + 8 * (i / 4) - 31 * (i / 16);
    int strip2 = strip1 + 1 + strip1 % 4 - 5 * ((strip1 / 4) % 2);
    int strip3 = (strip2 % 2 == 0) ? strip2 / 2 + 32 : ((strip2 < 64) ? (63 - strip2) / 2 : 127 + (65 - strip2) / 2);
    t[0][i] = strip1;
    t[1][i] = strip2;
    t[2][i] = strip3;
    t[3][i] = i;
  }
  return t;
}
}  // namespace

const std::array<int, kApvChannels>& apvChannelMap(ApvMapping m) {
  static const auto tables = buildApvMaps();
  int idx = static_cast<int>(m);
  if (idx < 0 || idx > 3) idx = 1;
  return tables[idx];
}

int stripNumber(ApvMapping m, int apv_chan, int pos, int invert) {
  if (apv_chan < 0 || apv_chan >= kApvChannels) return -1;
  int s = apvChannelMap(m)[apv_chan];
  s = s + (127 - 2 * s) * invert;
  return s + 128 * pos;
}

void GemModule::uvToXY(double u, double v, double& x, double& y) const {
  double det = pxu * pyv - pyu * pxv;
  x = (pyv * u - pyu * v) / det;
  y = (pxu * v - pxv * u) / det;
}

Vec3 GemModule::intersect(const Vec3& point, const Vec3& dir, double* s) const {
  double si = zax.dot(origin - point) / zax.dot(dir);
  if (s) *s = si;
  return point + dir * si;
}

double GemTracker::layerZ(int layer) const {
  if (layer < 0 || layer >= static_cast<int>(modules_by_layer.size()) || modules_by_layer[layer].empty()) return 0;
  double z = 0;
  for (int m : modules_by_layer[layer]) z += modules[m].origin.z;
  return z / modules_by_layer[layer].size();
}

int GemTracker::findModule(const std::string& name) const {
  for (size_t i = 0; i < modules.size(); ++i)
    if (modules[i].name == name) return static_cast<int>(i);
  return -1;
}

GemTracker loadGemTracker(const DbDir& dir, const std::string& name, DbTime date) {
  DbFile f = dir.open(name, date);
  return loadGemTracker(f, name, date);
}

GemTracker loadGemTracker(const DbFile& file, const std::string& name, DbTime date) {
  GemTracker t;
  t.prefix = name + ".";
  t.dbfile = file.path();
  t.date = date;

  DbReader top(file, date, t.prefix);
  // SBSGEMSpectrometerTracker: { "modules", kString, mandatory, search 1 }
  std::vector<std::string> names = top.getTokens("modules", 1);
  for (auto& v : top.used()) t.provenance.push_back(v);

  for (const auto& mname : names) {
    GemModule m;
    m.name = mname;
    m.prefix = t.prefix + mname + ".";
    DbReader r(file, date, m.prefix);
    // SBSGEMModule::ReadDatabase uses LoadDB(..., fPrefix, 1): items with search 0 inherit 1.
    const int S = 1;
    std::vector<long long> chanmap = r.getInts("chanmap", S);
    m.apvmap = static_cast<ApvMapping>(r.getInt("apvmap", 1, S));
    if (static_cast<int>(m.apvmap) < 0 || static_cast<int>(m.apvmap) > 3) m.apvmap = ApvMapping::UVA_XY;
    m.layer = static_cast<int>(r.getInt("layer", S));
    m.nstrips[0] = static_cast<int>(r.getInt("nstripsu", S));
    m.nstrips[1] = static_cast<int>(r.getInt("nstripsv", S));
    m.angle_deg[0] = r.getDouble("uangle", S);
    m.angle_deg[1] = r.getDouble("vangle", S);
    m.offset[0] = r.getDouble("uoffset", 0.0, S);
    m.offset[1] = r.getDouble("voffset", 0.0, S);
    m.pitch[0] = r.getDouble("upitch", S);
    m.pitch[1] = r.getDouble("vpitch", S);
    m.pxu = std::cos(m.angle_deg[0] * kDegToRad);
    m.pyu = std::sin(m.angle_deg[0] * kDegToRad);
    m.pxv = std::cos(m.angle_deg[1] * kDegToRad);
    m.pyv = std::sin(m.angle_deg[1] * kDegToRad);

    // SBSGEMModule::ReadGeometry: position (required, search 0), size (required, search 1),
    // angle (optional, search 0)
    std::vector<double> pos = r.getDoubles("position", 0);
    std::vector<double> size = r.getDoubles("size", 1);
    std::vector<double> ang = r.getDoubles("angle", 0, false);
    if (pos.size() != 3) throw DbError(m.prefix + "position must have 3 values");
    if (size.size() != 3) throw DbError(m.prefix + "size must have 3 values");
    m.origin = {pos[0], pos[1], pos[2]};
    m.half_x = 0.5 * std::abs(size[0]);
    m.half_y = 0.5 * std::abs(size[1]);
    m.thickness = std::abs(size[2]);
    if (ang.size() == 3) {
      Rot3 R = Rot3::aboutZ(ang[2] * kDegToRad) * Rot3::aboutY(ang[1] * kDegToRad) * Rot3::aboutX(ang[0] * kDegToRad);
      m.xax = R.column(0);
      m.yax = R.column(1);
      m.zax = R.column(2);
    } else if (ang.size() == 1) {
      double a = ang[0] * kDegToRad;  // DefineAxes: X rotated about y, Z = X x Y
      m.xax = {std::cos(a), 0, -std::sin(a)};
      m.yax = {0, 1, 0};
      m.zax = m.xax.cross(m.yax);
    } else if (!ang.empty()) {
      throw DbError(m.prefix + "angle must have 1 or 3 values");
    }

    constexpr size_t kRow = 9;  // fMPDMAP_ROW_SIZE
    if (chanmap.size() % kRow != 0)
      throw DbError(m.prefix + "chanmap size " + std::to_string(chanmap.size()) + " not a multiple of 9");
    for (size_t i = 0; i < chanmap.size() / kRow; ++i) {
      ApvEntry e;
      e.crate = static_cast<uint16_t>(chanmap[kRow * i + 0]);
      e.slot = static_cast<uint16_t>(chanmap[kRow * i + 1]);
      e.mpd_id = static_cast<uint16_t>(chanmap[kRow * i + 2]);
      e.gem_id = static_cast<uint16_t>(chanmap[kRow * i + 3]);
      e.adc_id = static_cast<uint16_t>(chanmap[kRow * i + 4]);
      e.i2c = static_cast<uint16_t>(chanmap[kRow * i + 5]);
      e.pos = static_cast<uint16_t>(chanmap[kRow * i + 6]);
      e.invert = static_cast<uint16_t>(chanmap[kRow * i + 7]);
      e.axis = chanmap[kRow * i + 8] == 0 ? Axis::U : Axis::V;
      e.module = static_cast<int>(t.modules.size());
      e.index = static_cast<int>(i);
      m.apvs.push_back(e);
    }
    for (auto& v : r.used()) t.provenance.push_back(v);
    t.modules.push_back(std::move(m));
  }
  std::set<int> layers;
  for (auto& m : t.modules) layers.insert(m.layer);
  int maxlayer = layers.empty() ? -1 : *layers.rbegin();
  t.nlayers = static_cast<int>(layers.size());
  t.modules_by_layer.assign(maxlayer + 1, {});
  for (size_t i = 0; i < t.modules.size(); ++i) t.modules_by_layer[t.modules[i].layer].push_back(static_cast<int>(i));
  return t;
}

// ---------------------------------------------------------------- ApvMap
uint64_t ApvMap::key(uint16_t crate, uint16_t slot, uint16_t mpd, uint16_t adc, bool match_slot) {
  return (static_cast<uint64_t>(crate) << 48) | (static_cast<uint64_t>(match_slot ? slot : 0xFFFF) << 32) |
         (static_cast<uint64_t>(mpd) << 16) | adc;
}

ApvMap::ApvMap(const GemTracker& tracker, ApvKeyField field, bool match_slot)
    : tracker_(&tracker), field_(field), match_slot_(match_slot) {
  for (const auto& m : tracker.modules) {
    for (const auto& e : m.apvs) {
      uint64_t k = key(e.crate, e.slot, e.mpd_id, e.adc_id, match_slot_);
      auto it = entries_.find(k);
      if (it != entries_.end()) {
        std::ostringstream os;
        os << "duplicate APV (crate " << e.crate << " slot " << e.slot << " mpd/fiber " << e.mpd_id << " adc "
           << e.adc_id << ") in " << m.name << " and " << tracker.modules[it->second->module].name;
        duplicates_.push_back(os.str());
        continue;
      }
      entries_[k] = &e;
    }
  }
}

const ApvEntry* ApvMap::find(const ApvAddress& a) const {
  uint16_t id = (field_ == ApvKeyField::Fiber) ? a.fiber : a.mpd;
  auto it = entries_.find(key(a.crate, a.slot, id, a.adc, match_slot_));
  return it == entries_.end() ? nullptr : it->second;
}

StripRef ApvMap::strip(const ApvAddress& a, int apv_chan) const {
  StripRef r;
  const ApvEntry* e = find(a);
  if (!e || !tracker_) return r;
  const GemModule& m = tracker_->modules[e->module];
  r.module = e->module;
  r.axis = e->axis;
  r.layer = m.layer;
  r.apv = e;
  r.strip = stripNumber(m.apvmap, apv_chan, e->pos, e->invert);
  return r;
}

}  // namespace geom
}  // namespace gemfilter
