// gemfilter / seeds : FADC250 decoding and pulse processing (see Fadc250.h).
#include "gemfilter/seeds/Fadc250.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace gemfilter {
namespace seeds {

const FadcChannelData* FadcEvent::find(int slot, int chan) const {
  for (const auto& c : channels)
    if (c.slot == slot && c.chan == chan) return &c;
  return nullptr;
}

FadcChannelData& FadcEvent::get(int slot, int chan) {
  for (auto& c : channels)
    if (c.slot == slot && c.chan == chan) return c;
  channels.emplace_back();
  channels.back().slot = static_cast<uint8_t>(slot);
  channels.back().chan = static_cast<uint8_t>(chan);
  return channels.back();
}

FadcDecodeResult decodeFadc250(const uint32_t* words, size_t n) {
  FadcDecodeResult res;
  res.nwords = static_cast<uint32_t>(n);
  uint32_t type = 15;          // FILLER until a type-defining word is seen
  int slot = -1;
  int ievt = -1;               // event index within the current block
  int chan = -1;
  uint32_t win_width = 0;
  uint32_t tt_w1 = 0;
  auto event = [&]() -> FadcEvent& {
    int i = std::max(ievt, 0);
    if (static_cast<int>(res.events.size()) <= i) res.events.resize(i + 1);
    return res.events[i];
  };
  for (size_t iw = 0; iw < n; ++iw) {
    const uint32_t w = words[iw];
    const bool defining = (w >> 31) & 1u;
    if (defining) type = (w >> 27) & 0xFu;
    switch (type) {
      case 0:  // block header (second header word: PL/NSB/NSA, ignored)
        if (defining) {
          slot = static_cast<int>((w >> 22) & 0x1F);
          ievt = -1;
          ++res.nblocks;
        }
        break;
      case 1:  // block trailer
        ievt = -1;
        break;
      case 2: {  // event header
        ++ievt;
        FadcEvent& e = event();
        if (e.channels.empty() && e.trigger_number == 0) e.trigger_number = w & 0xFFF;
        break;
      }
      case 3: {  // trigger time
        if (defining)
          tt_w1 = w & 0xFFFFFF;
        else {
          FadcEvent& e = event();
          e.trigger_time = (static_cast<uint64_t>(w & 0xFFFFFF) << 24) | tt_w1;
        }
        if (defining) event().trigger_time = tt_w1;
        break;
      }
      case 4:    // window raw data
      case 6: {  // pulse raw data
        if (defining) {
          chan = static_cast<int>((w >> 23) & 0xF);
          win_width = (type == 4) ? (w & 0xFFF) : 0xFFFFFFFFu;
          if (slot < 0) { ++res.nerrors; res.messages.push_back("raw data before block header"); }
          event().get(slot < 0 ? 0 : slot, chan);
        } else {
          if (chan < 0) { ++res.nerrors; break; }
          FadcChannelData& c = event().get(slot < 0 ? 0 : slot, chan);
          bool inv1 = (w >> 29) & 1u, inv2 = (w >> 13) & 1u;
          uint16_t s1 = inv1 ? 0 : static_cast<uint16_t>((w >> 16) & 0x1FFF);
          uint16_t s2 = inv2 ? 0 : static_cast<uint16_t>(w & 0x1FFF);
          c.samples.push_back(s1);
          c.invalid_samples |= inv1;
          c.overflow |= ((s1 >> 12) & 1u) != 0;
          if (inv2 && c.samples.size() == win_width) break;  // odd window: last half-word unused
          c.samples.push_back(s2);
          c.invalid_samples |= inv2;
          c.overflow |= ((s2 >> 12) & 1u) != 0;
        }
        break;
      }
      case 7: {  // pulse integral
        int ch = static_cast<int>((w >> 23) & 0xF);
        event().get(slot < 0 ? 0 : slot, ch).integral.push_back(w & 0x7FFFF);
        break;
      }
      case 8: {  // pulse time
        int ch = static_cast<int>((w >> 23) & 0xF);
        event().get(slot < 0 ? 0 : slot, ch).time.push_back(w & 0x7FFF);
        break;
      }
      case 9: {  // pulse parameters (firmware v3 modes 9/10)
        if (defining) {
          chan = static_cast<int>((w >> 15) & 0xF);
          event().get(slot < 0 ? 0 : slot, chan).pedestal.push_back(w & 0x3FFF);
        } else {
          if (chan < 0) { ++res.nerrors; break; }
          FadcChannelData& c = event().get(slot < 0 ? 0 : slot, chan);
          if ((w >> 30) & 1u) {
            c.integral.push_back((w >> 12) & 0x3FFFF);
            c.overflow |= ((w >> 10) & 1u) != 0;
          } else {
            c.time.push_back((w >> 15) & 0x7FFF);
            c.peak.push_back(static_cast<uint16_t>((w >> 3) & 0xFFF));
          }
        }
        break;
      }
      case 10: {  // pulse pedestal
        int ch = static_cast<int>((w >> 23) & 0xF);
        auto& c = event().get(slot < 0 ? 0 : slot, ch);
        c.pedestal.push_back((w >> 12) & 0x1FF);
        c.peak.push_back(static_cast<uint16_t>(w & 0xFFF));
        break;
      }
      case 12:  // scaler header + scaler words: skip the counted words
        if (defining) iw += (w & 0x3F);
        break;
      default:  // 5, 11, 13 undefined; 14 not valid; 15 filler
        break;
    }
  }
  return res;
}

PulseResult processWaveform(const std::vector<uint16_t>& vals, const PulseParams& p) {
  // Port of SBSData::Waveform::Process (SBS-offline master). Unsigned arithmetic kept.
  PulseResult r;
  const uint32_t n = static_cast<uint32_t>(vals.size());
  if (n == 0) return r;
  std::vector<double> raw(n);
  for (uint32_t i = 0; i < n; ++i) raw[i] = vals[i] * p.chan_to_mV;
  int NPedsum = std::min(p.npedbin, static_cast<int>(n));
  double pf = 0, pl = 0;
  for (int i = 0; i < NPedsum; ++i) {
    pf += raw[i];
    pl += raw[(n - 1) - i];
  }
  double ped = NPedsum > 0 ? std::min(pf, pl) / NPedsum : 0.0;
  uint32_t tc = static_cast<uint32_t>(std::max(NPedsum - 1, 0));
  while (tc < n && raw[tc] < ped + p.thres_mV) ++tc;
  const uint32_t NSB = static_cast<uint32_t>(p.nsb), NSA = static_cast<uint32_t>(p.nsa);
  const uint32_t fix = static_cast<uint32_t>(p.fixthresbin);
  uint32_t lo = 0, hi = n;
  if (tc < n) {
    lo = std::max(tc - NSB, 0u);   // NB: unsigned, as in SBS-offline
    hi = std::min(tc + NSA - 1, n);
    r.threshold_crossed = true;
  } else {
    lo = std::max(fix - NSB, 0u);
    hi = std::min(fix + NSA - 1, n);
  }
  const double pC = p.tcal / 50.;
  double mx = 0, sum = 0, sped = 0, fine = 0;
  bool peak_found = false;
  uint32_t peak_bin = 0;
  for (uint32_t i = lo; i < hi; ++i) {
    sped += ped * pC;
    sum += raw[i] * pC;
    if (i >= tc && !peak_found) {
      if (raw[i] > mx)
        mx = raw[i];
      else {
        peak_found = true;
        peak_bin = i - 1;
      }
    }
  }
  double vmid = (mx + ped) / 2.;
  if (peak_found) {
    for (uint32_t i = lo; i < peak_bin + 1 && i + 1 < n; ++i)
      if (vmid >= raw[i] && vmid < raw[i + 1]) fine = i + (vmid - raw[i]) / (raw[i + 1] - raw[i]);
  }
  r.has_data = true;
  r.ped_mV = ped;
  r.integral_raw = sum;
  r.energy = (sum - sped) * p.gain;
  r.time_ns = fine * p.tcal + p.timeoffset;
  r.amplitude_mV = (mx == 0) ? 0.0 : (mx - ped) * p.amp_cal;
  return r;
}

PulseResult processPulse(double integral, double time, double peak, double pedestal, const PulseParams& p) {
  // Port of SBSData::ADC::Process
  PulseResult r;
  const double pC = p.tcal / 50.;
  double ped = pedestal * p.chan_to_mV;
  r.has_data = true;
  r.threshold_crossed = true;
  r.ped_mV = ped;
  r.time_ns = time * p.tcal / 64. + p.timeoffset;
  r.integral_raw = integral * p.chan_to_mV * pC;
  r.energy = (r.integral_raw - ped * (p.nsa + p.nsb + 1) * pC) * p.gain;
  r.amplitude_mV = (peak * p.chan_to_mV - ped) * p.amp_cal;
  return r;
}

std::vector<uint32_t> encodeFadc250Block(const std::vector<FadcSynthSlot>& slots, uint32_t first_trigger,
                                         uint32_t block_number) {
  std::vector<uint32_t> out;
  for (const auto& s : slots) {
    size_t start = out.size();
    uint32_t nev = static_cast<uint32_t>(s.events.size());
    out.push_back((1u << 31) | (0u << 27) | ((s.slot & 0x1Fu) << 22) | (1u << 18) | ((block_number & 0x3FFu) << 8) |
                  (nev & 0xFFu));
    for (uint32_t ie = 0; ie < nev; ++ie) {
      uint32_t trig = first_trigger + ie;
      out.push_back((1u << 31) | (2u << 27) | ((s.slot & 0x1Fu) << 22) | (trig & 0xFFFu));
      uint64_t tt = 1000ull * trig;
      out.push_back((1u << 31) | (3u << 27) | static_cast<uint32_t>(tt & 0xFFFFFF));
      out.push_back(static_cast<uint32_t>((tt >> 24) & 0xFFFFFF));
      for (const auto& ch : s.events[ie]) {
        uint32_t width = static_cast<uint32_t>(ch.samples.size());
        out.push_back((1u << 31) | (4u << 27) | ((ch.chan & 0xFu) << 23) | (width & 0xFFFu));
        for (uint32_t i = 0; i < width; i += 2) {
          uint32_t s1 = ch.samples[i] & 0x1FFFu;
          uint32_t w = s1 << 16;
          if (i + 1 < width)
            w |= ch.samples[i + 1] & 0x1FFFu;
          else
            w |= (1u << 13);  // invalid second sample
          out.push_back(w);
        }
      }
    }
    uint32_t nwords = static_cast<uint32_t>(out.size() - start + 1);
    out.push_back((1u << 31) | (1u << 27) | ((s.slot & 0x1Fu) << 22) | (nwords & 0x3FFFFFu));
  }
  // pad with filler words to an even count (as the FADC250 does in 64-bit DMA)
  if (out.size() % 2) out.push_back(0xF8000000u);
  return out;
}

}  // namespace seeds
}  // namespace gemfilter
