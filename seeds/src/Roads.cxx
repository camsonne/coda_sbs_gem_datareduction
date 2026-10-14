// gemfilter / seeds : constraint points and road corridors (see Roads.h for the SBS-offline mapping).
#include "gemfilter/seeds/Roads.h"

#include <algorithm>
#include <cmath>

namespace gemfilter {
namespace seeds {

using geom::Vec3;

ConstraintPoints bigbiteConstraint(const geom::BigBiteConfig& c, const geom::CaloGeometry& sh,
                                   const CaloSeed& s, const geom::CaloGeometry* ps, const CaloSeed* p) {
  double wxs = 1.0 / (c.sigx_sh * c.sigx_sh), wys = 1.0 / (c.sigy_sh * c.sigy_sh);
  double x = s.x * wxs, y = s.y * wys, z = sh.origin.z * (wxs + wys);
  double sx = wxs, sy = wys, sz = wxs + wys;
  double Etot = s.E;
  if (ps && p) {
    double wxp = 1.0 / (c.sigx_ps * c.sigx_ps), wyp = 1.0 / (c.sigy_ps * c.sigy_ps);
    x += p->x * wxp; y += p->y * wyp; z += ps->origin.z * (wxp + wyp);
    sx += wxp; sy += wyp; sz += wxp + wyp;
    Etot += p->E;
  }
  // (timing hodoscope term of SBS-offline omitted: not used for the online road)
  double xb = x / sx, yb = y / sy, zb = z / sz;
  double yp = 0;
  double dy = c.Ytar_01000 * zb - c.Ytar_00010;
  if (std::abs(dy) > 1e-12) yp = yb * c.Ytar_01000 / dy;
  double E = Etot * c.ecalo_fudge;
  if (c.downbending) E = -E;
  double xp = 0;
  if (c.precon_flag != 1) {
    double den = E * (c.Xptar_00100 - 1.0 - c.Xptar_10000 * zb) + c.Ptheta_10000 * zb - c.Ptheta_00100;
    if (std::abs(den) > 1e-12)
      xp = (c.Ptheta_00000 + c.Ptheta_10000 * xb - E * (c.optics_angle + c.Xptar_10000 * xb)) / den;
  } else {
    double Mx = c.Xptar_10000, Mxp = c.Xptar_00100;
    double k = c.B_pth1 + c.C_pth1 * c.magdist;
    double den = c.A_pth1 * k * (Mxp - zb * Mx) + E * (1.0 + Mx * zb - Mxp);
    if (std::abs(den) > 1e-12)
      xp = (E * (c.optics_angle + Mx * xb) - c.A_pth1 * (1.0 + k * Mx * xb)) / den;
  }
  ConstraintPoints cp;
  double zf = 0.0;
  cp.front = {xb + xp * (zf - zb) + c.cw.front_x0, yb + yp * (zf - zb) + c.cw.front_y0, zf};
  cp.back = {xb + c.cw.back_x0, yb + c.cw.back_y0, zb};
  cp.fwx = c.cw.front_wx; cp.fwy = c.cw.front_wy; cp.bwx = c.cw.back_wx; cp.bwy = c.cw.back_wy;
  cp.arm = CaloSeed::Arm::BigBite;
  cp.Etot = Etot;
  return cp;
}

ConstraintPoints sbsConstraint(const geom::SbsArmConfig& c, const geom::CaloGeometry& hcal, const CaloSeed& s,
                               int tracker) {
  geom::ConstraintWindow w = c.window(tracker);
  double xb = s.x + hcal.origin.x, yb = s.y + hcal.origin.y, zb = hcal.origin.z;
  double xf = 0, yf = 0, zf = 0;
  if (c.use_dynamic) {
    double th = xb * c.dyn_thslope + c.dyn_th0, ph = yb * c.dyn_phslope + c.dyn_ph0;
    xf = xb + th * (zf - zb);
    yf = yb + ph * (zf - zb);
  }
  ConstraintPoints cp;
  cp.back = {xb + w.back_x0, yb + w.back_y0, zb};
  cp.front = {xf + w.front_x0, yf + w.front_y0, zf};
  cp.fwx = w.front_wx; cp.fwy = w.front_wy; cp.bwx = w.back_wx; cp.bwy = w.back_wy;
  cp.arm = CaloSeed::Arm::SBS;
  cp.Etot = s.E;
  return cp;
}

namespace {
struct Box { double xc, yc, hx, hy; bool ok; };
Box boxAt(const geom::GemModule& m, const ConstraintPoints& cp, const RoadOptions& o) {
  Vec3 d = cp.back - cp.front;
  double L = d.mag();
  if (L <= 0) return {0, 0, 0, 0, false};
  Vec3 u = d / L;
  if (std::abs(m.zax.dot(u)) < 1e-9) return {0, 0, 0, 0, false};
  double s = 0;
  Vec3 p = m.intersect(cp.front, u, &s);
  double f = std::max(0.0, std::min(1.0, s / L));
  double hx = (cp.fwx * (1 - f) + cp.bwx * f) * o.width_scale + o.margin;
  double hy = (cp.fwy * (1 - f) + cp.bwy * f) * o.width_scale + o.margin;
  Vec3 loc = m.trackToDet(p);
  return {loc.x, loc.y, hx, hy, true};
}
}  // namespace

ModuleWindow moduleWindow(const geom::GemModule& m, int imod, const std::vector<ConstraintPoints>& cps,
                          const RoadOptions& o) {
  ModuleWindow w;
  w.module = imod; w.layer = m.layer;
  double big = 1e12;
  w.xmin = w.ymin = w.umin = w.vmin = big;
  w.xmax = w.ymax = w.umax = w.vmax = -big;
  for (const auto& cp : cps) {
    Box b = boxAt(m, cp, o);
    if (!b.ok) continue;
    double x0 = b.xc - b.hx, x1 = b.xc + b.hx, y0 = b.yc - b.hy, y1 = b.yc + b.hy;
    if (o.require_in_active_area && (x1 < -m.half_x || x0 > m.half_x || y1 < -m.half_y || y0 > m.half_y)) continue;
    w.active = true;
    w.xmin = std::min(w.xmin, x0); w.xmax = std::max(w.xmax, x1);
    w.ymin = std::min(w.ymin, y0); w.ymax = std::max(w.ymax, y1);
    const double xs[2] = {x0, x1}, ys[2] = {y0, y1};
    for (double xx : xs)
      for (double yy : ys) {
        double uu, vv;
        m.xyToUV(xx, yy, uu, vv);
        w.umin = std::min(w.umin, uu); w.umax = std::max(w.umax, uu);
        w.vmin = std::min(w.vmin, vv); w.vmax = std::max(w.vmax, vv);
      }
  }
  if (!w.active) w.xmin = w.xmax = w.ymin = w.ymax = w.umin = w.umax = w.vmin = w.vmax = 0;
  return w;
}

std::vector<ModuleWindow> moduleWindows(const geom::GemTracker& trk, const std::vector<ConstraintPoints>& cps,
                                        const RoadOptions& o) {
  std::vector<ModuleWindow> out;
  out.reserve(trk.modules.size());
  for (size_t i = 0; i < trk.modules.size(); ++i) out.push_back(moduleWindow(trk.modules[i], int(i), cps, o));
  return out;
}

Road makeRoad(const ConstraintPoints& cp, const geom::GemTracker& trk, const RoadOptions& o) {
  Road r;
  r.seed_index = cp.seed_index;
  r.arm = cp.arm;
  double dz = cp.back.z - cp.front.z;
  for (int l = 0; l < trk.nlayers; ++l) {
    double z = trk.layerZ(l);
    double f = std::abs(dz) > 0 ? std::max(0.0, std::min(1.0, (z - cp.front.z) / dz)) : 0.0;
    double t = std::abs(dz) > 0 ? (z - cp.front.z) / dz : 0.0;
    RoadLayer rl;
    rl.layer = l;
    rl.xc = cp.front.x + t * (cp.back.x - cp.front.x);
    rl.yc = cp.front.y + t * (cp.back.y - cp.front.y);
    rl.hx = (cp.fwx * (1 - f) + cp.bwx * f) * o.width_scale + o.margin;
    rl.hy = (cp.fwy * (1 - f) + cp.bwy * f) * o.width_scale + o.margin;
    r.layers.push_back(rl);
  }
  return r;
}

}  // namespace seeds
}  // namespace gemfilter
