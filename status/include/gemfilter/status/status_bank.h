// gemfilter — GEMFILTER status bank: encoder, decoder, and in-place insertion.
//
// Placement (see docs/coda.md "GEMFILTER status bank"): the bank is appended as the LAST
// child of the GEM ROC bank that the filter modified (the "host ROC").  It is never placed
// at the top level of the built event: Podd CodaDecoder::FindRocsCoda3() treats every
// top-level bank after the trigger bank as a ROC (tag & 0x0fff < MAXROC, count must equal the
// trigger-bank ROC count), so an extra top-level bank makes Podd reject the event.  Inside a
// bank-structured ROC, Podd bank_decode() only records child banks by tag and looks up the
// banks named in the crate map, so an unknown child tag is ignored.
//
// Tag choice: 0x4746 (= 18246, ASCII "GF").
//   * outside the CODA3 reserved range 0xFF00-0xFFFF;
//   * not one of the bank tags in SBS-replay DB/db_cratemap.dat (3, 6, 7, 9, 86, 250, 526,
//     1190, 3561) nor the JLab ROC conventions for TI/trigger banks;
//   * > 0x0fff, so even if mis-placed at the top level it could never alias a ROC id.
// UNVERIFIED: absence of 0x4746 in every SBS ROC readout list (rol) of the next run — check
// with `gemfilter_file --list-banks` on a raw file before data taking.
//
// Bank layout (EVIO bank, content type 0x01 = uint32, num = format version):
//   w0 length (= payload words + 1)   w1 tag=0x4746 | type=0x01 | num=version
//   payload (version 1, 13 words):
//     p0  format version (1)          p1  config hash low 32    p2  config hash high 32
//     p3  strips_in                   p4  strips_out
//     p5  bytes_in                    p6  bytes_out             (as reported by processor)
//     p7  nseeds | nroads << 16       p8  ntracks | flags << 16
//     p9  prescale counter (physics events seen by this filter instance, 1-based)
//     p10 prescale factor (0 = no raw pass-through)
//     p11 cpu time of the processor for this event, microseconds (thread CPU clock)
//     p12 filter instance id (host/pid/thread hash; distinguishes parallel stations)
// Decoders must accept longer payloads (later minor versions append words).
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gemfilter/types.h"

namespace gemfilter {

constexpr uint16_t kStatusBankTag = 0x4746;   // "GF"
constexpr uint8_t  kStatusBankVersion = 1;
constexpr size_t   kStatusPayloadWordsV1 = 13;
constexpr size_t   kStatusBankWordsV1 = kStatusPayloadWordsV1 + 2;

// FilterStats::flags bits.  Bits 0-2 are defined by the contract (types.h); the rest are
// added by the CODA-integration track.
namespace flags {
constexpr uint16_t kPassthroughPrescale = 1u << 0;  // contract: raw pass-through (prescale)
constexpr uint16_t kDecodeError         = 1u << 1;  // contract: decode/encode error -> unmodified
constexpr uint16_t kNoSeed              = 1u << 2;  // contract: no calorimeter seed
constexpr uint16_t kNoRoom              = 1u << 3;  // result did not fit capacity -> unmodified
constexpr uint16_t kNotPhysics          = 1u << 4;  // control/user event -> untouched (metrics only)
constexpr uint16_t kNoGemRoc            = 1u << 5;  // no ROC with GEM bank -> no status bank
constexpr uint16_t kMalformed           = 1u << 6;  // event structure invalid -> untouched
constexpr uint16_t kUnsupportedFraming  = 1u << 7;  // ET buffer swapped/compressed -> untouched
constexpr uint16_t kStatusNotAppended   = 1u << 8;  // status bank could not be appended
}  // namespace flags

struct StatusRecord {
  uint32_t version = kStatusBankVersion;
  uint64_t config_hash = 0;
  FilterStats stats;
  uint32_t prescale_counter = 0;
  uint32_t prescale_factor = 0;
  uint32_t cpu_us = 0;
  uint32_t instance_id = 0;
  bool operator==(const StatusRecord& o) const;
};

// Encode the full bank (2 header words + payload) into a vector.
std::vector<uint32_t> encode_status_bank(const StatusRecord& r);
// Encode into caller memory; returns words written (kStatusBankWordsV1) or 0 if cap too small.
size_t encode_status_bank(const StatusRecord& r, uint32_t* out, size_t cap);

// Decode a bank starting at its length word.  Returns nullopt if tag/type/length invalid.
std::optional<StatusRecord> decode_status_bank(const uint32_t* bank, size_t nwords_avail);

// Find a GEMFILTER status bank inside a built physics event (searched as a direct child of
// each ROC bank).  Returns the decoded record and, optionally, the ROC id that hosts it.
std::optional<StatusRecord> find_status_bank(const uint32_t* ev, size_t nwords,
                                             uint16_t* host_roc = nullptr);

// Insert the status bank as the last child of the ROC bank whose length word is at
// ev[roc_offset].  Shifts following data, updates ROC and top-level bank lengths and nwords.
// Returns false (buffer untouched) if the event would exceed `cap` words or is malformed.
bool append_status_to_roc(uint32_t* ev, size_t& nwords, size_t cap, size_t roc_offset,
                          const StatusRecord& r);

// Choose the host ROC: the first ROC bank containing a child with one of `gem_bank_tags`.
// Returns the offset of its length word, or nullopt.
std::optional<size_t> find_gem_host_roc(const uint32_t* ev, size_t nwords,
                                        const std::vector<uint16_t>& gem_bank_tags);

// 64-bit FNV-1a, used for the configuration hash stored in p1/p2.
uint64_t fnv1a64(const std::string& s);

}  // namespace gemfilter
