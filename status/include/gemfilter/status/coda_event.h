// gemfilter — CODA3 built-event classification and minimal EVIO bank walking.
//
// Scope: only what the CODA-integration layer needs to decide "may this event be touched?",
// to read the event number, to locate ROC banks, and to append/locate the GEMFILTER status
// bank.  Full bank parsing/rewriting of ROC payloads belongs to evioio/ and codec/.
//
// All buffers are host-endian 32-bit words (as delivered by evRead() or by an ET event whose
// et_event_needtoswap() is false).  Swapped data is rejected (classified Unknown).
//
// Event-type tags are taken from Podd hana_decode/CodaDecoder.cxx::InterpretBankTag() and
// Decoder.h (analyzer master): see docs/coda.md section "Always-pass event list".
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gemfilter {
namespace coda {

// --- CODA3 reserved top-level bank tags (Podd CodaDecoder::InterpretBankTag) -------------
constexpr uint16_t kTagSync      = 0xFFD0;  // sync event (CODA3 control range 0xFFD0..)
constexpr uint16_t kTagPrestart  = 0xFFD1;
constexpr uint16_t kTagGo        = 0xFFD2;
constexpr uint16_t kTagPause     = 0xFFD3;
constexpr uint16_t kTagEnd       = 0xFFD4;
// Built physics events: Podd accepts exactly these four as physics (evtype 1).
constexpr uint16_t kTagPhysics       = 0xFF50;
constexpr uint16_t kTagPhysicsSync   = 0xFF58;  // physics event with sync bit
constexpr uint16_t kTagPhysicsStream = 0xFF70;  // (Podd treats as physics)
constexpr uint16_t kTagPhysicsSync2  = 0xFF78;
// Trigger bank tags in built events (Podd: 0xFF20..0xFF27 valid).
constexpr uint16_t kTrigBankMin = 0xFF20;
constexpr uint16_t kTrigBankMax = 0xFF27;

// --- Podd legacy user event types (Decoder.h) — inserted by CODA as user events ----------
constexpr uint16_t kEvTsPrescale = 120, kEvEpics = 131, kEvPrescale = 133, kEvDetMap = 135,
                   kEvTriggerFile = 136, kEvDaqConfig1 = 137, kEvDaqConfig2 = 138,
                   kEvScaler = 140, kEvSbsScaler = 141, kEvHvData = 150;

// --- SBS bank tags (SBS-replay DB/db_cratemap.dat, master, fetched 2026-10) ---------------
constexpr uint16_t kSbsMpdBankTag = 3561;  // MPD (SSP- and VTP-era) GEM data bank, slot 11

enum class EventClass : uint8_t {
  Physics,      // built physics event (0xFF50/58/70/78) -> candidate for filtering
  Control,      // sync / prestart / go / pause / end (0xFFD0..0xFFDF) -> never touched
  User,         // tag < 0xFF00: EPICS, scalers, config files, ET insertions -> never touched
  OtherReserved,// any other 0xFF00..0xFFFF tag (e.g. ROC raw, unknown) -> never touched
  Malformed     // length/type inconsistent -> never touched
};
const char* to_string(EventClass c);

struct BankHeader {          // EVIO bank (2-word) header
  uint32_t length = 0;       // words following the length word
  uint16_t tag = 0;
  uint8_t  pad = 0;          // padding bytes (2 bits)
  uint8_t  type = 0;         // 6-bit content type
  uint8_t  num = 0;
};
BankHeader parse_bank_header(const uint32_t* p);
uint32_t   make_bank_word1(uint16_t tag, uint8_t type, uint8_t num, uint8_t pad = 0);

constexpr uint8_t kTypeUint32 = 0x01, kTypeSegment = 0x20, kTypeBank = 0x10, kTypeBankAlt = 0x0e,
                  kTypeTagSegment = 0x0c, kTypeSegmentAlt = 0x0d;
inline bool is_bank_of_banks(uint8_t type) { return type == kTypeBank || type == kTypeBankAlt; }

// Classify a built event.  nwords = words available in buf (>= bank length+1 required).
EventClass classify(const uint32_t* buf, size_t nwords);

// A child bank located inside a bank-of-banks: offset (words from the event start) of its
// length word, and its header.
struct BankRef { size_t offset = 0; BankHeader hdr; };

// List direct children of the bank whose length word is at buf[parent].  Returns false if the
// children do not tile the parent exactly (malformed).
bool list_children(const uint32_t* buf, size_t nwords, size_t parent, std::vector<BankRef>& out);

struct PhysicsLayout {
  BankHeader top;
  std::optional<BankRef> trigger;       // trigger bank (first child, tag 0xFF20..0xFF27)
  std::vector<BankRef> rocs;            // ROC banks (remaining children)
  uint64_t first_event_number = 0;      // from trigger bank segment 1 (0 if absent)
  uint8_t  block_level = 0;             // top.num (events in this block)
};
// Parse the top level of a built physics event.  Returns false if malformed.
bool parse_physics(const uint32_t* buf, size_t nwords, PhysicsLayout& out);

// ROC id as Podd reads it: (word1 & 0x0fff0000) >> 16 (12 bits).
inline uint16_t roc_id(const BankHeader& h) { return static_cast<uint16_t>(h.tag & 0x0fff); }

// True if the ROC bank at `roc` contains a direct child bank with tag `bank_tag`.
bool roc_has_bank(const uint32_t* buf, size_t nwords, const BankRef& roc, uint16_t bank_tag);

}  // namespace coda
}  // namespace gemfilter
