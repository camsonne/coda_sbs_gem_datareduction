// gemfilter / geom : crate map (db_cratemap.dat) and calorimeter block geometry / channel
// maps / gains (db_bb.sh.dat, db_bb.ps.dat, db_sbs.hcal.dat) in the SBS-offline conventions
// of SBSGenericDetector::ReadDatabase and SBSCalorimeter::ReadDatabase.
//
//  * Element (block) index k runs over rows, then columns, then layers:
//      k = sum_{r'<r} ncols[r']*nlayers + c*nlayers + l
//    block position x = xyz[0] - r*dxdydz[0], y = xyz[1] - c*dxdydz[1], z = xyz[2] - l*dxdydz[2],
//    overridden element-by-element by "xpos"/"ypos" arrays if they have Nelem entries.
//    Rows vary with x (vertical, transport frame), columns with y.
//  * detmap tuples: crate slot ch_lo ch_hi refindex (5 values, THaDetMap::kFillRefIndex),
//    or crate slot ch_lo ch_hi model refindex (6 values) if model_in_detmap = 1.
//  * chanmap: one logical element number per detmap channel in detmap order;
//    -1 = unused channel, -1000 = reference channel; element = value - start_chanmap.
//    For detectors with ADC and TDC (HCal), ADC and TDC channels each map onto the Nelem
//    blocks; ADC/TDC is decided by the module model from the crate map (Podd module table).
//  * Cluster positions in SBS-offline are block (x,y) in the calorimeter-local frame; the
//    calorimeter "position" is the origin in the arm's GEM tracker frame.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gemfilter/geom/PoddDb.h"
#include "gemfilter/geom/Vec3.h"

namespace gemfilter {
namespace geom {

// ---------------------------------------------------------------- crate map
struct CrateSlot {
  int crate = -1, slot = -1, model = 0;
  int bank = -1;            // CODA bank tag if "slot model bank" form
  std::string cfg;          // text after "cfg:"
};

class CrateMap {
 public:
  // Port of THaCrateMap::init(string) time-stamp logic: blocks with time stamp later than
  // 'date' are ignored; a "==== Crate N type T" line resets crate N.
  static CrateMap fromString(const std::string& text, DbTime date);
  static CrateMap load(const std::string& path, DbTime date);
  static CrateMap load(const DbDir& dir, DbTime date) { return load(dir.resolve("cratemap", date), date); }

  int model(int crate, int slot) const;            // 0 if unknown
  int bank(int crate, int slot) const;             // -1 if none
  const CrateSlot* slot(int crate, int slot) const;
  std::vector<int> crates() const;
  std::vector<int> slotsWithModel(int crate, int model) const;
  int tsroc() const { return tsroc_; }
  std::string type(int crate) const;

  static bool isAdcModel(int model);   // Podd THaDetMap module_list (+ FADC250)
  static bool isTdcModel(int model);   // incl. VETROC 526/527 forced TDC in SBSGenericDetector

 private:
  std::map<int, std::map<int, CrateSlot>> crates_;
  std::map<int, std::string> types_;
  int tsroc_ = 21;
};

// ---------------------------------------------------------------- calorimeter
struct DetMapModule {
  int crate = 0, slot = 0, lo = 0, hi = 0, model = 0, refindex = -1;
  bool is_tdc = false;
  int nchan() const { return hi - lo + 1; }
};

struct CaloBlock {
  int index = -1;           // element index k (0-based)
  int row = 0, col = 0, layer = 0;
  double x = 0, y = 0, z = 0;          // calorimeter-local [m]
  int adc_crate = -1, adc_slot = -1, adc_chan = -1;
  int tdc_crate = -1, tdc_slot = -1, tdc_chan = -1;
  // ADC calibration (SBSGenericDetector defaults if absent)
  double ped = 0, gain = 1, conv = 1, thres = 1, timeoffset = 0, amp_to_int = 1;
  int nsb = 3, nsa = 10, npedbin = 4, fixthresbin = 10;
};

struct CaloClusterDbParams {          // SBSCalorimeter::ReadDatabase keys (defaults if absent)
  double emin = 0.001, tmax = 1000, emin_clSeed = 0.001, emin_clTotal = 0.001;
  double rmax_dis = 0.30;
  int nmax_cluster = 10;
  std::vector<int> cluster_dim{3, 3};
};

struct CaloGeometry {
  std::string name;          // "bb.sh"
  std::string dbfile;
  DbTime date = 0;
  int nrows = 0, nlayers = 1;
  std::vector<int> ncols;    // per row
  Vec3 origin;               // "position" in the arm tracker frame [m]
  double size[3] = {0, 0, 0};
  double size_row = 0, size_col = 0;     // dxdydz[0], dxdydz[1] (fSizeRow, fSizeCol)
  int chanmap_start = 0;
  std::vector<DetMapModule> detmap;
  std::vector<CaloBlock> blocks;
  std::vector<std::vector<int>> grid;    // grid[row][col] -> block index (layer 0)
  CaloClusterDbParams cluster;
  std::vector<DbValue> provenance;

  int nelem() const { return static_cast<int>(blocks.size()); }
  int blockAt(int row, int col) const;   // -1 if outside
  int blockForAdc(int crate, int slot, int chan) const;
  int blockForTdc(int crate, int slot, int chan) const;

  std::map<uint64_t, int> adc_lookup, tdc_lookup;
  static uint64_t chanKey(int crate, int slot, int chan) {
    return (static_cast<uint64_t>(crate & 0xFFFF) << 32) | (static_cast<uint64_t>(slot & 0xFFFF) << 16) |
           static_cast<uint64_t>(chan & 0xFFFF);
  }
};

struct CaloLoadOptions {
  bool with_adc = true;
  bool with_tdc = false;     // HCal: true (SBSHCal: kWaveform + kTDCSimple)
};

CaloGeometry loadCalorimeter(const DbDir& dir, const std::string& name, DbTime date, const CrateMap* cratemap,
                             const CaloLoadOptions& opt = {});
CaloGeometry loadCalorimeter(const DbFile& file, const std::string& name, DbTime date, const CrateMap* cratemap,
                             const CaloLoadOptions& opt = {});

}  // namespace geom
}  // namespace gemfilter
