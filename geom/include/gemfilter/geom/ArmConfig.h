// gemfilter / geom : spectrometer-level constraint/optics parameters for the two SBS arms.
//
//  BigBite (SBSBigBite::ReadDatabase, prefix "bb.", global search 1; db_bb.dat) +
//          (SBSBigBite::ReadRunDatabase, "bb.magdist", db_run.dat) +
//          (SBSBBTotalShower, "bb.ts.MaxDx/MaxDy", db_bb.ts.dat)
//  SBS     (SBSEArm::ReadDatabase, prefix "sbs.", global search 1; db_sbs.dat) +
//          (SBSEArm::ReadRunDatabase, "sbs.magdist", "sbs.hcaldist", db_run.dat)
// Defaults are the SBS-offline constructor defaults where a key is absent.
#pragma once

#include <string>
#include <vector>

#include "gemfilter/geom/PoddDb.h"

namespace gemfilter {
namespace geom {

struct ConstraintWindow {        // half-widths [m] and centring offsets [m]
  double front_wx = 1.5, front_wy = 1.5, back_wx = 1.5, back_wy = 1.5;
  double front_x0 = 0, front_y0 = 0, back_x0 = 0, back_y0 = 0;
};

struct BigBiteConfig {
  ConstraintWindow cw;
  double magdist = 1.80;                 // [m], bb.magdist (db_run.dat)
  double optics_angle = 10.0 * 3.14159265358979323846 / 180.0;  // bb.opticstheta [rad]
  double ecalo_fudge = 1.0;
  int precon_flag = 0;
  double A_pth1 = 0.28615 * 0.97, B_pth1 = 0.1976, C_pth1 = 0.4764;
  bool downbending = false;
  // first-order optics coefficients extracted from bb.optics_parameters
  // (exponent columns 5..9 = (om, ol, ok, oj, oi) as in SBSBigBite::ReadDatabase)
  int optics_order = -1;
  double Ptheta_00000 = 0, Ptheta_10000 = 0, Ptheta_00100 = 0;
  double Xptar_10000 = 0, Xptar_00100 = 0, Ytar_01000 = 0, Ytar_00010 = 0;
  // shower / preshower position resolutions used to weight the back constraint
  double sigx_sh = 0.085 / 3.4641016151377544, sigy_sh = 0.085 / 3.4641016151377544;
  double sigx_ps = 0.09 / 3.4641016151377544, sigy_ps = 0.37 / 3.4641016151377544;
  // total-shower PS/SH matching (SBSBBTotalShower defaults 0.4 m)
  double ts_maxdx = 0.4, ts_maxdy = 0.4;
  std::vector<DbValue> provenance;
};

struct SbsArmConfig {
  // index 0 = front tracker (used for the SBS GEM tracker), 1 = back tracker (polarimeter)
  std::vector<double> front_wx{0.75, 1.0}, front_wy{0.2, 0.3}, back_wx{2.0, 2.0}, back_wy{1.0, 1.0};
  std::vector<double> front_x0{0, 0}, front_y0{0, 0}, back_x0{0, 0}, back_y0{0, 0};
  bool use_dynamic = false;
  double dyn_thslope = 1.0 / 17.0, dyn_th0 = 0, dyn_phslope = 1.0 / 17.0, dyn_ph0 = 0;
  double magdist = 2.25, hcaldist = 17.0;
  ConstraintWindow window(int tracker = 0) const;
  std::vector<DbValue> provenance;
};

BigBiteConfig loadBigBiteConfig(const DbDir& dir, DbTime date);
SbsArmConfig loadSbsArmConfig(const DbDir& dir, DbTime date);

}  // namespace geom
}  // namespace gemfilter
