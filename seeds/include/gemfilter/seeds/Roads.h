// gemfilter / seeds : constraint points and road corridors from calorimeter seeds.
//
// Mirrors SBS-offline:
//  * SBSBigBite::CoarseReconstruct (single-track branch): back constraint point = weighted mean of
//    shower and preshower cluster positions (weights 1/sigma^2), z = weighted origin z; front
//    constraint at z = 0 from first-order optics with the calorimeter energy (Precon_flag 0 or 1).
//  * SBSEArm::CoarseReconstruct (non-polarimeter): back = HCal cluster + HCal origin, front at z=0
//    either (0,0) or, with the dynamic constraint, x_bcp + (x_bcp*thslope+th0)*(0 - z_bcp).
//  * SBSGEMTrackerBase (lines ~1281-1312): for each module, intersect the line front->back with the
//    module plane, interpolate the widths linearly in path length, convert the centre to module-local
//    coordinates.  SBSGEMModule (lines ~3080-3110) then restricts 1D clustering to the U/V interval
//    spanned by the corners of that box.  ModuleWindow reproduces exactly those U/V intervals, so an
//    online "road" cut with the same widths never removes a strip SBS-offline would have clustered.
#pragma once
#include <vector>

#include "gemfilter/geom/ArmConfig.h"
#include "gemfilter/geom/CaloGeometry.h"
#include "gemfilter/geom/GemGeometry.h"
#include "gemfilter/geom/Vec3.h"
#include "gemfilter/types.h"

namespace gemfilter {
namespace seeds {

struct ConstraintPoints {
  geom::Vec3 front, back;           // tracker frame [m]
  double fwx = 1.5, fwy = 1.5, bwx = 1.5, bwy = 1.5;   // half-widths [m]
  int seed_index = -1;
  CaloSeed::Arm arm = CaloSeed::Arm::BigBite;
  double Etot = 0;
};

// BigBite. `sh_seed`/`ps_seed` positions are calorimeter-local (as SBSCalorimeterCluster::GetX/GetY);
// as in SBS-offline the local x,y are used directly and only the origin z enters.
ConstraintPoints bigbiteConstraint(const geom::BigBiteConfig& cfg, const geom::CaloGeometry& sh,
                                   const CaloSeed& sh_seed, const geom::CaloGeometry* ps = nullptr,
                                   const CaloSeed* ps_seed = nullptr);

// SBS hadron arm (HCal seed), tracker index 0 = front tracker.
ConstraintPoints sbsConstraint(const geom::SbsArmConfig& cfg, const geom::CaloGeometry& hcal,
                               const CaloSeed& seed, int tracker = 0);

// Per-module acceptance window derived from one or more constraint pairs.
struct ModuleWindow {
  int module = -1;
  int layer = -1;
  bool active = false;                 // false: no constraint intersects / road misses module
  double xmin = 0, xmax = 0, ymin = 0, ymax = 0;    // module-local box (union over pairs) [m]
  double umin = 0, umax = 0, vmin = 0, vmax = 0;    // strip-coordinate intervals (SBSGEMModule)
};

struct RoadOptions {
  double width_scale = 1.0;            // multiply all constraint widths
  double margin = 0.0;                 // added to every half-width [m]
  bool require_in_active_area = false; // deactivate windows whose box misses the active area
};

// Window for one module from a set of constraint pairs (union, as SBSGEMModule does).
ModuleWindow moduleWindow(const geom::GemModule& mod, int imod, const std::vector<ConstraintPoints>& cps,
                          const RoadOptions& opt = {});
std::vector<ModuleWindow> moduleWindows(const geom::GemTracker& trk, const std::vector<ConstraintPoints>& cps,
                                        const RoadOptions& opt = {});

// Contract Road (per layer corridor in tracker frame at the mean layer z) for one constraint pair.
Road makeRoad(const ConstraintPoints& cp, const geom::GemTracker& trk, const RoadOptions& opt = {});

}  // namespace seeds
}  // namespace gemfilter
