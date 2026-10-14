// gemfilter / geom : SBS GEM tracker geometry and APV25 -> strip mapping, read from the
// SBS-replay Podd database (db_bb.gem.dat, db_sbs.gem.dat, db_sbs.gemFT.dat, ...).
//
// Conventions mirror SBS-offline master (SBSGEMModule.cxx, SBSGEMTrackerBase.cxx):
//  * Module "position" / "angle" are given in the TRACKER frame (first layer at z ~ 0).
//    With three angles (deg) the module axes are the columns of R = Rz(az) Ry(ay) Rx(ax)
//    (TRotation::RotateX, then RotateY, then RotateZ); with one angle a rotation about y
//    (THaDetectorBase::DefineAxes). detToTrack(p) = p.x*Xax + p.y*Yax + p.z*Zax + origin,
//    trackToDet(p) = ((p-o).Xax, (p-o).Yax, (p-o).Zax).
//  * Strip position along axis a: (istrip + 0.5 - 0.5*nstrips_a) * pitch_a + offset_a.
//  * U = x*cos(uangle) + y*sin(uangle), V likewise (SBSGEMModule::XYtoUV).
//  * Decode map ("chanmap") rows: crate slot mpd_id gem_id adc_ch i2c pos invert axis,
//    where in the production (VTP/SSP) decoder MPDModule::LoadSlot the "mpd_id" column is
//    the optical FIBER number carried in the MPD frame header (effChan = fiber<<4 | apv_id).
//  * strip = APVMAP[apvmap][apv_chan]; strip += (127 - 2*strip)*invert; strip += 128*pos
//    (SBSGEMModule::GetStripNumber), with APVMAP tables built as in InitAPVMAP().
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gemfilter/geom/PoddDb.h"
#include "gemfilter/geom/Vec3.h"
#include "gemfilter/types.h"

namespace gemfilter {
namespace geom {

enum class Axis : uint8_t { U = 0, V = 1 };  // SBSGEM::kUaxis = 0, kVaxis = 1
enum class ApvMapping : int { INFN = 0, UVA_XY = 1, UVA_UV = 2, MC = 3 };

constexpr int kApvChannels = 128;

// APVMAP[mapping][apv_chan] -> strip within the APV (0..127), as SBSGEMModule::InitAPVMAP.
const std::array<int, kApvChannels>& apvChannelMap(ApvMapping m);
// SBSGEMModule::GetStripNumber (non-MC branch).
int stripNumber(ApvMapping m, int apv_chan, int pos, int invert);

struct ApvEntry {           // one row of the "chanmap" decode map
  uint16_t crate = 0, slot = 0;
  uint16_t mpd_id = 0;      // = fiber number in VTP/SSP-era data (see header)
  uint16_t gem_id = 0, adc_id = 0, i2c = 0, pos = 0, invert = 0;
  Axis axis = Axis::U;
  int module = -1;          // index in GemTracker::modules
  int index = -1;           // row index within the module decode map
};

struct GemModule {
  std::string name;         // "m0"
  std::string prefix;       // "bb.gem.m0."
  int layer = 0;
  Vec3 origin;              // tracker frame [m]
  Vec3 xax{1, 0, 0}, yax{0, 1, 0}, zax{0, 0, 1};
  double half_x = 1e9, half_y = 1e9, thickness = 0;   // active area half sizes [m]
  int nstrips[2] = {0, 0};
  double angle_deg[2] = {0, 90};
  double pitch[2] = {0.0004, 0.0004};
  double offset[2] = {0, 0};
  ApvMapping apvmap = ApvMapping::UVA_XY;
  double pxu = 1, pyu = 0, pxv = 0, pyv = 1;          // cos/sin of strip angles
  std::vector<ApvEntry> apvs;

  Vec3 detToTrack(const Vec3& local) const { return xax * local.x + yax * local.y + zax * local.z + origin; }
  Vec3 trackToDet(const Vec3& p) const {
    Vec3 v = p - origin;
    return {v.dot(xax), v.dot(yax), v.dot(zax)};
  }
  void xyToUV(double x, double y, double& u, double& v) const { u = x * pxu + y * pyu; v = x * pxv + y * pyv; }
  void uvToXY(double u, double v, double& x, double& y) const;
  double stripPosition(Axis a, double istrip) const {
    int i = static_cast<int>(a);
    return (istrip + 0.5 - 0.5 * nstrips[i]) * pitch[i] + offset[i];
  }
  double stripCoordinate(Axis a, double pos) const {  // inverse of stripPosition
    int i = static_cast<int>(a);
    return (pos - offset[i]) / pitch[i] - 0.5 + 0.5 * nstrips[i];
  }
  bool inActiveArea(double x, double y) const { return std::abs(x) <= half_x && std::abs(y) <= half_y; }
  // Intersection of a straight line with the module plane (SBSGEMTrackerBase::TrackIntersect).
  Vec3 intersect(const Vec3& point, const Vec3& dir, double* s = nullptr) const;
};

struct GemTracker {
  std::string prefix;       // "bb.gem."
  std::string dbfile;       // resolved file
  DbTime date = 0;
  std::vector<GemModule> modules;
  int nlayers = 0;
  std::vector<std::vector<int>> modules_by_layer;
  std::vector<DbValue> provenance;   // every DB value used (key, block time, line)

  double layerZ(int layer) const;    // mean module origin z of the layer
  int findModule(const std::string& name) const;
};

// Load a tracker. 'name' is the detector name as in SBS-offline ("bb.gem", "sbs.gem",
// "sbs.gemFT", "sbs.gemFPP"); the file db_<name>.dat is resolved through DbDir at 'date'.
GemTracker loadGemTracker(const DbDir& dir, const std::string& name, DbTime date);
GemTracker loadGemTracker(const DbFile& file, const std::string& name, DbTime date);

// ApvAddress -> (module, axis, strip) lookup built from all decode maps of a tracker.
struct StripRef {
  int module = -1;
  Axis axis = Axis::U;
  int strip = -1;           // strip index on the module along 'axis'
  int layer = -1;
  const ApvEntry* apv = nullptr;
  bool valid() const { return module >= 0 && strip >= 0; }
};

enum class ApvKeyField : uint8_t {
  Fiber,  // match DB "mpd_id" column against ApvAddress::fiber (production decoder convention)
  Mpd     // match DB "mpd_id" column against ApvAddress::mpd
};

class ApvMap {
 public:
  ApvMap() = default;
  // match_slot=false ignores ApvAddress::slot (useful if a codec reports the physical SSP slot
  // while the DB uses the SBS-offline VTP convention slot 11).
  explicit ApvMap(const GemTracker& tracker, ApvKeyField field = ApvKeyField::Fiber, bool match_slot = true);

  const ApvEntry* find(const ApvAddress& a) const;
  StripRef strip(const ApvAddress& a, int apv_chan) const;
  size_t size() const { return entries_.size(); }
  const std::vector<std::string>& duplicates() const { return duplicates_; }  // DB conflicts
  static uint64_t key(uint16_t crate, uint16_t slot, uint16_t mpd, uint16_t adc, bool match_slot);

 private:
  const GemTracker* tracker_ = nullptr;
  ApvKeyField field_ = ApvKeyField::Fiber;
  bool match_slot_ = true;
  std::map<uint64_t, const ApvEntry*> entries_;
  std::vector<std::string> duplicates_;
};

}  // namespace geom
}  // namespace gemfilter
