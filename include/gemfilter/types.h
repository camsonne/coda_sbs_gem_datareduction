// gemfilter shared types (contract v0). Extend by adding fields/types only.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace gemfilter {

constexpr int kMaxSamples = 6;   // APV25 time samples read out in SBS (configurable up to 6)

// Identifies one APV25 in the readout tree.
struct ApvAddress {
  uint16_t crate = 0;   // ROC id (CODA bank tag of the ROC)
  uint16_t slot = 0;    // VME slot of SSP/VTP (VTP-era: SBS-offline uses slot 11)
  uint16_t fiber = 0;   // optical fibre index on SSP/VTP
  uint16_t mpd = 0;     // MPD id
  uint16_t adc = 0;     // APV index on MPD (ADC channel, 0..15)
  bool operator==(const ApvAddress& o) const {
    return crate==o.crate && slot==o.slot && fiber==o.fiber && mpd==o.mpd && adc==o.adc;
  }
};

// One strip hit as delivered by online-zero-suppressed MPD/SSP/VTP readout.
struct StripHit {
  uint8_t  apv_chan = 0;                      // 0..127 APV channel (before strip mapping)
  std::array<int16_t, kMaxSamples> adc{};     // signed, CM/pedestal-subtracted samples
  uint8_t  nsamples = kMaxSamples;
  uint32_t src_index = 0;                     // index of this hit within its ApvFrame (for re-encode)
};

// All strips of one APV in one event, plus any per-APV words (CM debug, etc.) kept opaque.
struct ApvFrame {
  ApvAddress addr;
  std::vector<StripHit> hits;
  std::vector<uint32_t> extra_words;          // e.g. common-mode debug words, carried through verbatim
};

// Strip keep decision: parallel to ApvFrame::hits (true = keep).
using KeepMask = std::vector<std::vector<bool>>;   // [frame][hit]

// Calorimeter seed (global Hall coordinates, metres; energy GeV or ADC-calibrated; time ns).
struct CaloSeed {
  enum class Arm : uint8_t { BigBite = 0, SBS = 1 };
  enum class Source : uint8_t { VTP = 0, FADC = 1 };
  Arm arm = Arm::BigBite;
  Source source = Source::VTP;
  double x = 0, y = 0, z = 0;                 // cluster position in detector-local frame of the calorimeter
  double E = 0;                               // calibrated where gains known, else raw sum
  double t = 0;
  int    nblocks = 0;
};

// Road corridor for one GEM layer: centre and half-widths in the tracker frame (metres).
struct RoadLayer { int layer = 0; double xc = 0, yc = 0, hx = 0, hy = 0; };
struct Road {
  int seed_index = -1;
  CaloSeed::Arm arm = CaloSeed::Arm::BigBite;
  std::vector<RoadLayer> layers;
};

struct FilterStats {
  uint32_t strips_in = 0, strips_out = 0;
  uint32_t bytes_in = 0, bytes_out = 0;
  uint16_t nseeds = 0, nroads = 0, ntracks = 0;
  uint16_t flags = 0;                         // bit0 passthrough(prescale), bit1 decode error, bit2 no seed
};

} // namespace gemfilter
