// gemfilter / seeds : JLab FADC250 data decoding (one ROC bank payload holding one or more
// FADC250 blocks) and SBS-offline-compatible pulse processing.
//
// Word formats follow Podd hana_decode/Fadc250Module.cxx (analyzer master):
//   bit31 = 1 : data-type defining word, type = bits 30..27; bit31 = 0 : continuation
//   type 0  block header   : slot 26..22, module id 21..18, block number 17..8, nevents 7..0
//   type 1  block trailer  : slot 26..22, nwords 21..0
//   type 2  event header   : slot 26..22, trigger number 11..0 (Podd mask) [time 21..12]
//   type 3  trigger time   : 23..0 word 1 (low), continuation 23..0 word 2 (high)
//   type 4  window raw data: chan 26..23, width 11..0; continuation: 2 samples per word
//                            (sample x: 28..16 incl. overflow bit 28, invalid bit 29;
//                             sample x+1: 12..0 incl. overflow bit 12, invalid bit 13)
//   type 6  pulse raw data : chan 26..23, pulse 22..21, tc sample 9..0; continuation as type 4
//   type 7  pulse integral : chan 26..23, pulse 22..21, quality 20..19, integral 18..0
//   type 8  pulse time     : chan 26..23, pulse 22..21, quality 20..19, time 15..0 (1/64 sample)
//   type 9  pulse params   : event-in-block 26..19, chan 18..15, ped quality 14, ped sum 13..0;
//                            continuation with bit30=1: integral 29..12, flags 11..9, nsamp>thr 8..0
//                            continuation with bit30=0: time 29..15 (coarse 29..21, fine 20..15),
//                            peak 14..3, flags 2..0
//   type 10 pulse pedestal : chan 26..23, pulse 22..21, pedestal 20..12, peak 11..0
//   types 5,11,13 undefined, 12 scaler header, 14 data not valid, 15 filler
// SBS BBCal (SBSBBShower) and HCal (SBSHCal) are configured in SBS-offline with
// SBSModeADC::kWaveform, i.e. the FADC250 window raw samples (type 4) are what the
// analysis uses; pulse-mode words (7/8/9/10) are decoded for completeness.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gemfilter {
namespace seeds {

struct FadcChannelData {
  uint8_t slot = 0, chan = 0;
  std::vector<uint16_t> samples;       // type 4/6 samples (13 bits incl. overflow bit 12)
  std::vector<uint32_t> integral;      // type 7, or type 9 sample sum
  std::vector<uint32_t> time;          // type 8 / type 9 (1/64 of a 4 ns sample)
  std::vector<uint16_t> peak;          // type 9 / 10
  std::vector<uint32_t> pedestal;      // type 9 pedestal SUM, or type 10 pedestal
  bool invalid_samples = false, overflow = false;
};

struct FadcEvent {
  uint32_t trigger_number = 0;         // event header (Podd mask 12 bits)
  uint64_t trigger_time = 0;           // 48-bit trigger time
  std::vector<FadcChannelData> channels;
  const FadcChannelData* find(int slot, int chan) const;
  FadcChannelData& get(int slot, int chan);   // create if missing
};

struct FadcDecodeResult {
  std::vector<FadcEvent> events;       // one per event header (block level N -> N events)
  uint32_t nwords = 0, nblocks = 0, nerrors = 0;
  std::vector<std::string> messages;
};

// Decode all FADC250 words of one bank payload (several slots, several events per block).
FadcDecodeResult decodeFadc250(const uint32_t* words, size_t n);
inline FadcDecodeResult decodeFadc250(const std::vector<uint32_t>& w) { return decodeFadc250(w.data(), w.size()); }

// Pulse processing parameters (per block, from the calorimeter DB: adc.conv, adc.thres,
// adc.NSB/NSA/NPedBin/FixThresBin, adc.gain, adc.timeoffset).
struct PulseParams {
  double chan_to_mV = 0.48828;   // adc.conv
  double gain = 1.0;             // adc.gain (pC -> GeV in SBS calorimeter DBs)
  double thres_mV = 1.0;         // adc.thres
  int nsb = 3, nsa = 10, npedbin = 4, fixthresbin = 10;
  double tcal = 4.0;             // ns per sample
  double timeoffset = 0.0;       // adc.timeoffset
  double amp_cal = 1.0;          // AmpToIntRatio * gain
};

struct PulseResult {
  bool has_data = false;
  bool threshold_crossed = false;
  double ped_mV = 0;
  double integral_raw = 0;       // pC
  double energy = 0;             // (sum - ped*nbins) * gain
  double time_ns = 0;            // SBS-offline "fine time" (half-max interpolation) * tcal + offset
  double amplitude_mV = 0;
};

// Port of SBSData::Waveform::Process (single-pulse mode used for BBCal/HCal in SBS-offline).
PulseResult processWaveform(const std::vector<uint16_t>& samples, const PulseParams& p);
// Port of SBSData::ADC::Process (FADC pulse mode, SBSModeADC::kADC).
PulseResult processPulse(double integral, double time, double peak, double pedestal, const PulseParams& p);

// ---- SYNTHETIC test-data encoder (writes the same formats; used by unit tests only) ----
struct FadcSynthChannel {
  int chan = 0;
  std::vector<uint16_t> samples;     // window raw data (type 4)
};
struct FadcSynthSlot {
  int slot = 0;
  std::vector<std::vector<FadcSynthChannel>> events;   // [event][channel]
};
std::vector<uint32_t> encodeFadc250Block(const std::vector<FadcSynthSlot>& slots, uint32_t first_trigger,
                                         uint32_t block_number = 0);

}  // namespace seeds
}  // namespace gemfilter
