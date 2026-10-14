// gemfilter / geom : BigBite / SBS arm constraint + optics parameters (see ArmConfig.h).
#include "gemfilter/geom/ArmConfig.h"
#include "gemfilter/geom/Vec3.h"

#include <cmath>

namespace gemfilter {
namespace geom {

namespace {
void append(std::vector<DbValue>& dst, const std::vector<DbValue>& src) { dst.insert(dst.end(), src.begin(), src.end()); }
}  // namespace

BigBiteConfig loadBigBiteConfig(const DbDir& dir, DbTime date) {
  BigBiteConfig c;
  {
    DbFile f = dir.open("bb", date);
    DbReader r(f, date, "bb.");
    const int S = 1;  // LoadDB(file, date, request, fPrefix, 1)
    c.cw.front_wx = r.getDouble("frontconstraintwidth_x", c.cw.front_wx, S);
    c.cw.front_wy = r.getDouble("frontconstraintwidth_y", c.cw.front_wy, S);
    c.cw.back_wx = r.getDouble("backconstraintwidth_x", c.cw.back_wx, S);
    c.cw.back_wy = r.getDouble("backconstraintwidth_y", c.cw.back_wy, S);
    c.cw.front_x0 = r.getDouble("frontconstraint_x0", c.cw.front_x0, S);
    c.cw.front_y0 = r.getDouble("frontconstraint_y0", c.cw.front_y0, S);
    c.cw.back_x0 = r.getDouble("backconstraint_x0", c.cw.back_x0, S);
    c.cw.back_y0 = r.getDouble("backconstraint_y0", c.cw.back_y0, S);
    c.optics_angle = r.getDouble("opticstheta", c.optics_angle / kDegToRad, S) * kDegToRad;
    c.ecalo_fudge = r.getDouble("ecalo_fudgefactor", c.ecalo_fudge, S);
    c.precon_flag = static_cast<int>(r.getInt("preconflag", c.precon_flag, S));
    c.A_pth1 = r.getDouble("A_pth1", c.A_pth1, S);
    c.B_pth1 = r.getDouble("B_pth1", c.B_pth1, S);
    c.C_pth1 = r.getDouble("C_pth1", c.C_pth1, S);
    c.downbending = r.getInt("downbending_mode", 0LL, S) != 0;
    c.sigx_sh = r.getDouble("xsigma_shower", c.sigx_sh, S);
    c.sigy_sh = r.getDouble("ysigma_shower", c.sigy_sh, S);
    c.sigx_ps = r.getDouble("xsigma_preshower", c.sigx_ps, S);
    c.sigy_ps = r.getDouble("ysigma_preshower", c.sigy_ps, S);
    c.optics_order = static_cast<int>(r.getInt("optics_order", -1LL, S));
    auto par = r.getDoubles("optics_parameters", S, false);
    if (c.optics_order >= 0) {
      int nterms = 0;
      for (int i = 0; i <= c.optics_order; i++)
        for (int j = 0; j <= c.optics_order - i; j++)
          for (int k = 0; k <= c.optics_order - i - j; k++)
            for (int l = 0; l <= c.optics_order - i - j - k; l++)
              for (int m = 0; m <= c.optics_order - i - j - k - l; m++) nterms++;
      if (static_cast<int>(par.size()) != 9 * nterms)
        throw DbError("bb.optics_parameters: " + std::to_string(par.size()) + " values, expected " +
                      std::to_string(9 * nterms) + " for order " + std::to_string(c.optics_order));
      for (int i = 0; i < nterms; ++i) {
        double xptar = par[9 * i], ytar = par[9 * i + 2], pinv = par[9 * i + 3];
        int om = static_cast<int>(par[9 * i + 4]), ol = static_cast<int>(par[9 * i + 5]),
            ok = static_cast<int>(par[9 * i + 6]), oj = static_cast<int>(par[9 * i + 7]),
            oi = static_cast<int>(par[9 * i + 8]);
        if (om == 0 && ol == 0 && ok == 0 && oj == 0 && oi == 0) c.Ptheta_00000 = pinv;
        if (om == 1 && ol == 0 && ok == 0 && oj == 0 && oi == 0) { c.Ptheta_10000 = pinv; c.Xptar_10000 = xptar; }
        if (om == 0 && ol == 0 && ok == 1 && oj == 0 && oi == 0) { c.Ptheta_00100 = pinv; c.Xptar_00100 = xptar; }
        if (om == 0 && ol == 1 && ok == 0 && oj == 0 && oi == 0) c.Ytar_01000 = ytar;
        if (om == 0 && ol == 0 && ok == 0 && oj == 1 && oi == 0) c.Ytar_00010 = ytar;
      }
    }
    append(c.provenance, r.used());
  }
  {
    DbFile f = dir.open("run", date);
    DbReader r(f, date, "bb.");
    c.magdist = r.getDouble("magdist", 1);  // required in SBS-offline
    append(c.provenance, r.used());
  }
  try {
    DbFile f = dir.open("bb.ts", date);
    DbReader r(f, date, "bb.ts.");
    c.ts_maxdx = r.getDouble("MaxDx", c.ts_maxdx, 0);
    c.ts_maxdy = r.getDouble("MaxDy", c.ts_maxdy, 0);
    append(c.provenance, r.used());
  } catch (const DbError&) {
    // optional file
  }
  return c;
}

ConstraintWindow SbsArmConfig::window(int t) const {
  auto at = [t](const std::vector<double>& v, double def) {
    return t < static_cast<int>(v.size()) ? v[t] : (v.empty() ? def : v.back());
  };
  ConstraintWindow w;
  w.front_wx = at(front_wx, 0.75);
  w.front_wy = at(front_wy, 0.2);
  w.back_wx = at(back_wx, 2.0);
  w.back_wy = at(back_wy, 1.0);
  w.front_x0 = at(front_x0, 0);
  w.front_y0 = at(front_y0, 0);
  w.back_x0 = at(back_x0, 0);
  w.back_y0 = at(back_y0, 0);
  return w;
}

SbsArmConfig loadSbsArmConfig(const DbDir& dir, DbTime date) {
  SbsArmConfig c;
  {
    DbFile f = dir.open("run", date);
    DbReader r(f, date, "sbs.");
    c.magdist = r.getDouble("magdist", 1);
    c.hcaldist = r.getDouble("hcaldist", 1);
    append(c.provenance, r.used());
  }
  c.dyn_thslope = 1.0 / c.hcaldist;
  c.dyn_phslope = 1.0 / c.hcaldist;
  {
    DbFile f = dir.open("sbs", date);
    DbReader r(f, date, "sbs.");
    const int S = 1;
    auto vec = [&](const char* k, std::vector<double>& dst) {
      auto v = r.getDoubles(k, S, false);
      if (!v.empty()) dst = v;  // SBS-offline kDoubleV replaces the defaults
    };
    vec("frontconstraintwidth_x", c.front_wx);
    vec("frontconstraintwidth_y", c.front_wy);
    vec("backconstraintwidth_x", c.back_wx);
    vec("backconstraintwidth_y", c.back_wy);
    vec("frontconstraint_x0", c.front_x0);
    vec("frontconstraint_y0", c.front_y0);
    vec("backconstraint_x0", c.back_x0);
    vec("backconstraint_y0", c.back_y0);
    c.use_dynamic = r.getInt("usedynamicconstraint", 0LL, S) != 0;
    c.dyn_thslope = r.getDouble("dynamicthslope", c.dyn_thslope, S);
    c.dyn_th0 = r.getDouble("dynamicth0", c.dyn_th0, S);
    c.dyn_phslope = r.getDouble("dynamicphslope", c.dyn_phslope, S);
    c.dyn_ph0 = r.getDouble("dynamicph0", c.dyn_ph0, S);
    append(c.provenance, r.used());
  }
  return c;
}

}  // namespace geom
}  // namespace gemfilter
