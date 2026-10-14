// SYNTHETIC CODA3-like built events for tests.  Structure follows Podd CodaDecoder
// expectations (top bank 0xFF50 bank-of-banks, trigger bank 0xFF20 of segments with a 64-bit
// event number segment, a uint16 event-type segment and one segment per ROC, then one ROC
// bank per crate containing data banks).  Payload words are a deterministic pattern so tests
// can verify which words survived.  This is NOT real SBS data.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "gemfilter/status/coda_event.h"

namespace synth {

struct BankSpec { uint16_t tag; uint32_t nwords; };
struct RocSpec { uint16_t roc; std::vector<BankSpec> banks; };

inline uint32_t pattern(uint64_t ev, uint16_t roc, uint16_t tag, uint32_t i) {
  return static_cast<uint32_t>(ev * 2654435761u) ^ (uint32_t(roc) << 24) ^ (uint32_t(tag) << 8) ^ (i * 40503u);
}

inline std::vector<uint32_t> physics(uint64_t evnum, uint8_t blk, const std::vector<RocSpec>& rocs,
                                     uint16_t top_tag = gemfilter::coda::kTagPhysics) {
  using gemfilter::coda::make_bank_word1;
  std::vector<uint32_t> e{0, make_bank_word1(top_tag, 0x10, blk)};
  // trigger bank
  const size_t tb = e.size();
  e.push_back(0);
  e.push_back(make_bank_word1(0xFF20, 0x20, static_cast<uint8_t>(rocs.size())));
  e.push_back((1u << 24) | (0x0au << 16) | 2u);  // seg1: EB id 1, uint64, 2 words
  uint32_t w[2];
  std::memcpy(w, &evnum, sizeof(evnum));
  e.push_back(w[0]);
  e.push_back(w[1]);
  const uint32_t n16 = (blk + 1u) / 2u;          // seg2: uint16 event types
  e.push_back((0u << 24) | (0x05u << 16) | n16);
  for (uint32_t i = 0; i < n16; ++i) e.push_back(0x00010001u);
  for (const auto& r : rocs) {                   // ROC segments (uint32, 1 word each here)
    e.push_back((uint32_t(r.roc & 0xff) << 24) | (0x01u << 16) | 1u);
    e.push_back(0xA0000000u | r.roc);
  }
  e[tb] = static_cast<uint32_t>(e.size() - tb - 1);
  for (const auto& r : rocs) {
    const size_t rb = e.size();
    e.push_back(0);
    e.push_back(make_bank_word1(r.roc, 0x10, blk));
    for (const auto& b : r.banks) {
      e.push_back(b.nwords + 1);
      e.push_back(make_bank_word1(b.tag, 0x01, blk));
      for (uint32_t i = 0; i < b.nwords; ++i) e.push_back(pattern(evnum, r.roc, b.tag, i));
    }
    e[rb] = static_cast<uint32_t>(e.size() - rb - 1);
  }
  e[0] = static_cast<uint32_t>(e.size() - 1);
  return e;
}

inline std::vector<uint32_t> control(uint16_t tag, uint32_t run = 1234) {
  using gemfilter::coda::make_bank_word1;
  return {4, make_bank_word1(tag, 0x01, 0xCC), 0x5f000000u, run, 1u};
}

// User event (e.g. EPICS 131 as a char-string bank, scaler 140 as uint32).
inline std::vector<uint32_t> user(uint16_t tag, uint32_t nwords) {
  using gemfilter::coda::make_bank_word1;
  std::vector<uint32_t> e{nwords + 1, make_bank_word1(tag, tag == 131 ? 0x03 : 0x01, 0)};
  for (uint32_t i = 0; i < nwords; ++i) e.push_back(0x41424344u + i);
  return e;
}

// Default SBS-like layout: a GEM ROC (bank 3561) between two non-GEM ROCs.
inline std::vector<RocSpec> default_rocs(uint32_t gem_words, uint32_t other_words = 20) {
  return {{5, {{250, other_words}}}, {19, {{3561, gem_words}, {4, 3}}}, {21, {{4, 6}}}};
}

}  // namespace synth
