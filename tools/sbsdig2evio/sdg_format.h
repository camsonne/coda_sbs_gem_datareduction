// gemfilter / tools / sbsdig2evio : ".sdg" intermediate file (digitized libsbsdig content,
// written by sbsdig_extract.py from the ROOT tree with uproot, read here by the C++ writer and
// by the round-trip verifier).  Little-endian, byte-packed, no alignment:
//
//   file header : u32 magic 'SDG1' (0x31474453) | u32 version (=1) | u32 flags | u32 reserved
//                 flags bit0: GEM truth from a per-sample signal branch (Earm.BBGEM.dighit.sig)
//                       bit1: GEM truth from g4sbs Earm.BBGEM.hit geometry (approximate)
//   event       : u32 'EVNT' (0x544E5645) | u32 tree entry | u32 n_gem | u32 n_pmt
//     n_gem x   : u16 plane (= Earm.BBGEM.dighit.module) | u16 strip | u8 nsamp | i8 truth
//                 (1 signal, 0 background, -1 unknown) | u16 reserved | nsamp x i16 raw ADC
//                 (libsbsdig value, common-mode baseline INCLUDED)
//     n_pmt x   : u8 det (0 bb.sh, 1 bb.ps, 2 sbs.hcal) | u8 reserved | u16 sim chan
//                 | u16 nsamp | u16 ntdc | nsamp x u16 FADC samples | ntdc x i32 TDC words
//   trailer     : u32 'END!' (0x21444E45) | u32 number of events
#pragma once
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace gemfilter {
namespace sbsdig {

constexpr uint32_t kSdgMagic = 0x31474453u, kSdgEvent = 0x544E5645u, kSdgEnd = 0x21444E45u;

enum PmtDet : uint8_t { kBBSH = 0, kBBPS = 1, kHCAL = 2, kNPmtDet = 3 };
inline const char* pmtDetName(int d) { return d == kBBSH ? "bb.sh" : d == kBBPS ? "bb.ps" : "sbs.hcal"; }

struct SdgGem {
  uint16_t plane = 0, strip = 0;
  int8_t truth = -1;
  std::vector<int16_t> s;
};
struct SdgPmt {
  uint8_t det = 0;
  uint16_t chan = 0;
  std::vector<uint16_t> s;
  std::vector<int32_t> tdc;
};
struct SdgEvent {
  uint32_t entry = 0;
  std::vector<SdgGem> gem;
  std::vector<SdgPmt> pmt;
};

class SdgReader {
 public:
  explicit SdgReader(const std::string& path) : f_(std::fopen(path.c_str(), "rb")) {
    if (!f_) throw std::runtime_error("cannot open " + path);
    uint32_t h[4];
    if (std::fread(h, 4, 4, f_) != 4 || h[0] != kSdgMagic) throw std::runtime_error(path + ": not an .sdg file");
    if (h[1] != 1) throw std::runtime_error(path + ": unsupported .sdg version " + std::to_string(h[1]));
    flags_ = h[2];
  }
  ~SdgReader() { if (f_) std::fclose(f_); }
  SdgReader(const SdgReader&) = delete;
  SdgReader& operator=(const SdgReader&) = delete;
  uint32_t flags() const { return flags_; }
  uint32_t trailer_count() const { return trailer_n_; }
  bool next(SdgEvent& ev) {
    uint32_t m = 0;
    if (!rd(&m, 4)) return false;
    if (m == kSdgEnd) { rd(&trailer_n_, 4); return false; }
    if (m != kSdgEvent) throw std::runtime_error("sdg: bad event marker");
    uint32_t h[3];
    need(h, 12);
    ev.entry = h[0];
    ev.gem.resize(h[1]);
    ev.pmt.resize(h[2]);
    for (auto& g : ev.gem) {
      uint8_t b[8];
      need(b, 8);
      g.plane = uint16_t(b[0] | (b[1] << 8));
      g.strip = uint16_t(b[2] | (b[3] << 8));
      g.s.resize(b[4]);
      g.truth = int8_t(b[5]);
      if (!g.s.empty()) need(g.s.data(), 2 * g.s.size());
    }
    for (auto& p : ev.pmt) {
      uint8_t b[8];
      need(b, 8);
      p.det = b[0];
      p.chan = uint16_t(b[2] | (b[3] << 8));
      p.s.resize(uint16_t(b[4] | (b[5] << 8)));
      p.tdc.resize(uint16_t(b[6] | (b[7] << 8)));
      if (!p.s.empty()) need(p.s.data(), 2 * p.s.size());
      if (!p.tdc.empty()) need(p.tdc.data(), 4 * p.tdc.size());
    }
    return true;
  }

 private:
  bool rd(void* p, size_t n) { return std::fread(p, 1, n, f_) == n; }
  void need(void* p, size_t n) { if (!rd(p, n)) throw std::runtime_error("sdg: truncated file"); }
  std::FILE* f_ = nullptr;
  uint32_t flags_ = 0, trailer_n_ = 0;
};

// libsbsdig PMT channel -> SBS-offline logical channel (element index), exactly as
// SBSSimDecoder::LoadDetector (SBS-offline mcpass4) does before ChanToROC().
inline int simChanToLogical(int det, int c) {
  switch (det) {
    case kBBSH: { int row = c % 27, col = c / 27; return row * 7 + col; }
    case kBBPS: { int row = c % 26, col = c / 26; return row * 2 + col; }
    case kHCAL: { int col = c % 12, row = (c - col) / 12; col = 12 - 1 - col; return row * 12 + col; }
  }
  return -1;
}

// Real-format MPD sample from a libsbsdig GEM sample: subtract the common-mode baseline the
// digitizer added (commonmode_array_bbgem, 1500 in the mcpass4 configs; SBS-offline's MC DB
// subtracts the same value as pedu/pedv), clamp to the signed 13-bit MPD range.
inline int16_t gemSampleToMpd(int raw, int baseline) {
  int v = raw - baseline;
  if (v < -4096) v = -4096;
  if (v > 4095) v = 4095;
  return int16_t(v);
}

}  // namespace sbsdig
}  // namespace gemfilter
