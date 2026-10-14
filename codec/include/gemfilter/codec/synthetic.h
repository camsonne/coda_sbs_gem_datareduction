// gemfilter codec: SYNTHETIC SSP/VTP MPD payload generator for tests and benchmarks.
// The output is synthetic (random strips/samples). It follows docs/gem_format.md; where the
// real word ordering is UNVERIFIED (type-12/13 placement) it uses the choice documented in
// docs/codec.md §"Synthetic data". It is NOT real detector data.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "gemfilter/types.h"

namespace gemfilter {
namespace codec {
namespace synth {

enum class Layout : uint8_t {
  Ssp = 0,             // block-wrapped, one block per slot in `slots` (GMn-style SSP)
  VtpBlock,            // block-wrapped with a single block, slot 11
  VtpBare,             // no block header/trailer, event headers + trigger time per event
  VtpBareNoEventHeader // no block, no event headers: forces events_per_block = 1
};

struct Config {
  Layout layout = Layout::Ssp;
  uint32_t events_per_block = 5;          // block level
  std::vector<uint16_t> slots = {11};     // Ssp: one block per slot
  uint32_t nfibers = 4;                   // MPDs (fibers) per block
  uint32_t fiber_offset = 0;              // first fiber number (>= 32 exercises the 6-bit field)
  uint32_t apvs_per_mpd = 4;              // APVs per MPD, ids 0..apvs_per_mpd-1 (<= 16)
  double   occupancy = 0.1;               // probability that a strip is read out
  bool     enable_cm = true;              // ENABLE_CM flag in frame headers
  double   full_readout_fraction = 0.0;   // fraction of MPD frames with BUILD_ALL_SAMPLES=1 (128 strips)
  bool     event_info = true;             // emit type-12 group after each MPD's hits
  bool     debug_words = false;           // emit one type-13 triple per APV (one frame header per APV)
  uint32_t filler_align = 4;              // pad block (or bare payload) to a multiple of this (0 = none)
  bool     reserved_word = false;         // insert a reserved type-7 word + 1 continuation per event
  bool     data_not_valid = false;        // append a type-14 word after the last block
  uint32_t first_trigger = 1000;
  uint32_t block_number = 1;
  uint32_t seed = 12345;
};

struct TruthHit {
  uint32_t event = 0;
  ApvAddress addr;
  uint8_t chan = 0;
  std::array<int16_t, 6> adc{};
  bool full_readout = false;
};

struct Generated {
  std::vector<uint32_t> payload;
  std::vector<TruthHit> hits;   // in decode order (event-major, then payload order)
  uint32_t nevents = 0;
};

Generated generate(const Config& cfg, uint16_t crate);

// Serialise per-event APV frames (all of ONE crate) into a block-wrapped MPD payload in the
// VTP-era layout: block header (slot, block number, N) | per event: event header, 2 trigger-time
// words, then per fiber a frame header followed by 3-word strip hits | block trailer (inclusive
// word count) | filler to an even length.  Used for SYNTHETIC end-to-end tests.
std::vector<uint32_t> write_block(const std::vector<std::vector<ApvFrame>>& events, uint16_t slot = 11,
                                  uint32_t first_trigger = 1, uint32_t block_number = 1, bool enable_cm = true);

}  // namespace synth
}  // namespace codec
}  // namespace gemfilter
