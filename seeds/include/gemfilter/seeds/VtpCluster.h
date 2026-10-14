// gemfilter / seeds : SBS HCal / ECal VTP cluster words (data type 12) as documented in
// SBS-offline master VTPModule.cxx (based on B. Raydo's manual, Jan 28 2025).
//
//   type 0 block header : slot 26..22, (code) nevents 9..0, block number 15..8
//   type 2 event header : trigger number 21..0
//   type 3 trigger time : 23..0 low, continuation 23..0 high
//   type 12 cluster     : word 1 (bit31=1): det_id/subtype 26..23, energy 13..0 (code mask)
//                         word 2 (bit31=0):
//                           HCal: col 23..20 (4 b), row 19..15 (5 b), nhits 14..11, time 10..0 (4 ns)
//                           ECal: col 26..22 (5 b), row 21..15 (7 b), nhits 14..11, time 10..0 (4 ns)
//   type 13 trigger decision: pattern 15..0, time 22..16
//
// Known inconsistencies in the source (flagged UNVERIFIED, configurable below):
//  * header comment: HCal subtype = 11, ECal = 12, HCal energy 16 bits;
//    code: enum { HCAL = 13, ECAL = 12 }, energy always masked with 0x3FFF (14 bits).
//    Default here: subtype 11 OR 13 -> HCal layout, 12 -> ECal; energy mask follows the code.
//  * "row"/"col" origin (0- or 1-based) and orientation w.r.t. the SBS-offline block grid
//    are not documented; default assumes 0-based, row = SBS-offline row, col = column.
//  * energy unit (FADC integral sum, ADC counts?) is not documented; seeds carry
//    E = energy * energy_scale (default 1 = raw units).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gemfilter/geom/CaloGeometry.h"
#include "gemfilter/types.h"

namespace gemfilter {
namespace seeds {

enum class VtpDet : uint8_t { Unknown = 0, HCal = 1, ECal = 2 };

struct VtpCluster {
  VtpDet det = VtpDet::Unknown;
  uint32_t subtype = 0;   // bits 26..23 of word 1
  uint32_t energy = 0;
  uint32_t row = 0, col = 0, nhits = 0, time = 0;   // time in 4 ns units
};

struct VtpTriggerDecision {
  uint32_t pattern = 0, time = 0;
};

struct VtpEvent {
  uint32_t trigger_number = 0;
  uint64_t trigger_time = 0;
  int slot = -1;
  std::vector<VtpCluster> clusters;
  std::vector<VtpTriggerDecision> decisions;
};

struct VtpDecodeOptions {
  uint32_t hcal_subtype_a = 13;     // SBS-offline code
  uint32_t hcal_subtype_b = 11;     // VTPModule.cxx header comment  // UNVERIFIED
  uint32_t ecal_subtype = 12;
  uint32_t energy_mask = 0x3FFF;    // code; header comment says 16 bits for HCal  // UNVERIFIED
};

struct VtpDecodeResult {
  std::vector<VtpEvent> events;
  uint32_t nerrors = 0;
};

VtpDecodeResult decodeVtp(const uint32_t* words, size_t n, const VtpDecodeOptions& opt = {});
inline VtpDecodeResult decodeVtp(const std::vector<uint32_t>& w, const VtpDecodeOptions& opt = {}) {
  return decodeVtp(w.data(), w.size(), opt);
}

struct VtpSeedOptions {
  double energy_scale = 1.0;        // UNVERIFIED unit conversion VTP energy -> GeV
  int row_offset = 0, col_offset = 0;   // add to VTP row/col to get SBS-offline row/col
  double min_energy = 0;            // in scaled units
  double time_ns_per_count = 4.0;
};

// Convert VTP clusters to CaloSeeds: position = block (x,y) of (row,col) in the calorimeter
// local frame, z = calorimeter origin z (tracker frame); Source::VTP.
std::vector<CaloSeed> vtpSeeds(const VtpEvent& ev, const geom::CaloGeometry& calo, CaloSeed::Arm arm,
                               const VtpSeedOptions& opt = {});

// SYNTHETIC encoder for tests.
std::vector<uint32_t> encodeVtpBlock(int slot, const std::vector<VtpEvent>& events, const VtpDecodeOptions& opt = {});

}  // namespace seeds
}  // namespace gemfilter
