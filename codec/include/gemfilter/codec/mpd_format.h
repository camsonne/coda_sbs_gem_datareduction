// gemfilter codec: bit-level constants and helpers for the SSP/VTP MPD data format.
// Reference: docs/gem_format.md (SSP_MPD_EventFormat 2020 + SBS-offline MPDModule.cxx).
#pragma once
#include <cstdint>

namespace gemfilter {
namespace codec {
namespace mpd {

// ---- word categories -------------------------------------------------------
constexpr uint32_t kDefiningBit = 0x80000000u;   // bit 31
constexpr uint32_t kTypeMask    = 0x78000000u;   // bits 30..27
constexpr int      kTypeShift   = 27;

inline bool     is_defining(uint32_t w) { return (w & kDefiningBit) != 0; }
inline uint32_t type_of(uint32_t w)     { return (w & kTypeMask) >> kTypeShift; }
inline uint32_t make_defining(uint32_t type, uint32_t payload27) {
  return kDefiningBit | ((type & 0xFu) << kTypeShift) | (payload27 & 0x07FFFFFFu);
}

// ---- data types ------------------------------------------------------------
enum Type : uint32_t {
  kBlockHeader  = 0,
  kBlockTrailer = 1,
  kEventHeader  = 2,
  kTriggerTime  = 3,
  kMpdFrame     = 5,
  kMpdEventInfo = 12,   // SBS-offline only (reserved in the 2020 SSP doc)
  kMpdDebug     = 13,   // SBS-offline only: online common-mode values
  kDataNotValid = 14,
  kFiller       = 15,
};

// Number of continuation words that must follow a defining word of fixed size.
// -1 = variable (MPD frame: 3N) or not fixed.
inline int expected_continuations(uint32_t type) {
  switch (type) {
    case kBlockHeader: case kBlockTrailer: case kEventHeader:
    case kDataNotValid: case kFiller: return 0;
    case kTriggerTime: return 1;
    case kMpdEventInfo: case kMpdDebug: return 2;
    default: return -1;
  }
}

// ---- block header / trailer (type 0 / 1) -----------------------------------
inline uint32_t bh_slot(uint32_t w)        { return (w >> 22) & 0x1Fu; }
inline uint32_t bh_block_number(uint32_t w){ return (w >> 8) & 0x3FFu; }
inline uint32_t bh_block_size(uint32_t w)  { return w & 0xFFu; }
inline uint32_t make_block_header(uint32_t slot, uint32_t blknum, uint32_t blksize) {
  return make_defining(kBlockHeader, ((slot & 0x1Fu) << 22) | ((blknum & 0x3FFu) << 8) | (blksize & 0xFFu));
}
constexpr uint32_t kNumWordsMask = 0x003FFFFFu;   // block trailer bits 21..0
inline uint32_t bt_slot(uint32_t w)      { return (w >> 22) & 0x1Fu; }
inline uint32_t bt_num_words(uint32_t w) { return w & kNumWordsMask; }
inline uint32_t make_block_trailer(uint32_t slot, uint32_t nwords) {
  return make_defining(kBlockTrailer, ((slot & 0x1Fu) << 22) | (nwords & kNumWordsMask));
}

// ---- event header (type 2) -------------------------------------------------
// UNVERIFIED: TRIGGER_NUMBER width (27 bits per SSP doc text, 22 bits in the v5 decoder).
inline uint32_t eh_trigger_number(uint32_t w) { return w & 0x07FFFFFFu; }
inline uint32_t make_event_header(uint32_t trig) { return make_defining(kEventHeader, trig & 0x07FFFFFFu); }

// ---- trigger time (type 3) -------------------------------------------------
inline uint32_t make_trigger_time1(uint64_t t) { return make_defining(kTriggerTime, uint32_t(t & 0xFFFFFFu)); }
inline uint32_t make_trigger_time2(uint64_t t) { return uint32_t((t >> 24) & 0xFFFFFFu); }

// ---- MPD frame header (type 5) ---------------------------------------------
constexpr uint32_t kEnableCmBit        = 1u << 26;
constexpr uint32_t kBuildAllSamplesBit = 1u << 25;
constexpr uint32_t kCmOrBit            = 1u << 24;
inline uint32_t fh_fiber(uint32_t w, int fiber_bits) {
  return (w >> 16) & (fiber_bits >= 6 ? 0x3Fu : 0x1Fu);
}
inline uint32_t fh_mpd_id(uint32_t w) { return w & 0x1Fu; }
inline uint32_t make_frame_header(uint32_t fiber, uint32_t mpd_id, bool enable_cm,
                                  bool build_all, bool cm_or) {
  return make_defining(kMpdFrame, (enable_cm ? kEnableCmBit : 0u) | (build_all ? kBuildAllSamplesBit : 0u) |
                                  (cm_or ? kCmOrBit : 0u) | ((fiber & 0x3Fu) << 16) | (mpd_id & 0x1Fu));
}

// ---- 3-word strip hit (continuation words of a type-5 frame) ----------------
constexpr uint32_t kSampleMask = 0x1FFFu;    // 13 bits
inline int16_t sample13(uint32_t field) {
  field &= kSampleMask;
  return static_cast<int16_t>((field & 0x1000u) ? int32_t(field) - 0x2000 : int32_t(field));
}
inline uint32_t hit_lo_sample(uint32_t w) { return w & kSampleMask; }
inline uint32_t hit_hi_sample(uint32_t w) { return (w >> 13) & kSampleMask; }
inline uint32_t hit_apv_chan(uint32_t h0, uint32_t h1) {
  return (((h1 >> 26) & 0x3u) << 5) | ((h0 >> 26) & 0x1Fu);
}
inline uint32_t hit_apv_id(uint32_t h2) { return (h2 >> 26) & 0x1Fu; }
constexpr uint32_t kHitFieldMask = 0x7C000000u;   // bits 30..26 of each hit word

// Pack word i (0..2) of a hit: preserves bits 31..26 of `orig` (channel / APV id / spare bits).
inline uint32_t repack_hit_word(uint32_t orig, int16_t s_lo, int16_t s_hi) {
  return (orig & 0xFC000000u) | ((uint32_t(uint16_t(s_hi)) & kSampleMask) << 13) |
         (uint32_t(uint16_t(s_lo)) & kSampleMask);
}
inline void make_hit(uint32_t apv_chan, uint32_t apv_id, const int16_t s[6], uint32_t out[3]) {
  out[0] = ((apv_chan & 0x1Fu) << 26);
  out[1] = (((apv_chan >> 5) & 0x3u) << 26);
  out[2] = ((apv_id & 0x1Fu) << 26);
  for (int i = 0; i < 3; ++i) out[i] = repack_hit_word(out[i], s[2 * i], s[2 * i + 1]);
}

// ---- MPD event info (type 12) and debug (type 13) --------------------------
inline uint32_t make_event_info0(uint32_t ts_lo24) { return make_defining(kMpdEventInfo, ts_lo24 & 0xFFFFFFu); }
inline uint32_t make_debug0(int16_t cm0, int16_t cm1) {
  return make_defining(kMpdDebug, ((uint32_t(uint16_t(cm1)) & kSampleMask) << 13) | (uint32_t(uint16_t(cm0)) & kSampleMask));
}

constexpr uint32_t kFillerWord       = 0xF8000000u;   // type 15, payload 0
constexpr uint32_t kDataNotValidWord = 0xF0000000u;   // type 14, payload 0

}  // namespace mpd
}  // namespace codec
}  // namespace gemfilter
