// gemfilter evioio: CODA3 built-event structure access and rewrite on in-memory uint32 buffers.
// No evio library dependency: used by both the file path (events from evio C evRead) and the
// ET path (events arrive as raw buffers). All buffers are in HOST byte order; use
// swap_event() (evio_swap.h) on foreign-endian ET buffers first.
//
// Layout (Podd CodaDecoder / CODA3 event-building note):
//   physics event bank   tag 0xFF50/58/70/78, type 0x10, num = block level
//    ├ trigger bank      tag 0xFF20..0xFF27 (bank of segments), num = #ROCs
//    ├ ROC bank          tag = ROC id (low 12 bits), type 0x10, num = block level
//    │  └ data bank      tag = bank id (e.g. 3561 = MPD/GEM), type 0x01, payload = module data
//    └ ...
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gemfilter {
namespace evioio {

enum class EvioErrorCode : uint8_t {
  BadLength = 1,      // bank length exceeds buffer / zero-length header
  BadStructure,       // child structures do not tile the parent exactly
  BadTriggerBank,     // malformed built trigger bank
  BadArgument,        // invalid index / aliasing / capacity
  Unsupported,        // e.g. composite data when swapping
  Io,                 // file I/O error (evio C status in message)
};
const char* to_string(EvioErrorCode c);

class EvioError : public std::runtime_error {
 public:
  EvioError(EvioErrorCode c, const std::string& msg);
  EvioErrorCode code() const { return code_; }
 private:
  EvioErrorCode code_;
};

// EVIO content types (bits 13..8 of bank header word 1).
namespace type {
constexpr uint8_t kUnknown32 = 0x00, kUInt32 = 0x01, kFloat32 = 0x02, kCharStar8 = 0x03, kShort16 = 0x04,
                  kUShort16 = 0x05, kChar8 = 0x06, kUChar8 = 0x07, kDouble64 = 0x08, kLong64 = 0x09,
                  kULong64 = 0x0a, kInt32 = 0x0b, kTagSegment = 0x0c, kSegment = 0x0d, kBank = 0x0e,
                  kComposite = 0x0f, kBank2 = 0x10, kSegment2 = 0x20;
inline bool is_bank_container(uint8_t t) { return t == kBank || t == kBank2; }
inline bool is_segment_container(uint8_t t) { return t == kSegment || t == kSegment2; }
inline bool is_container(uint8_t t) { return is_bank_container(t) || is_segment_container(t) || t == kTagSegment; }
}  // namespace type

// Two-word bank header.
struct BankHeader {
  uint32_t length = 0;   // words following the length word
  uint16_t tag = 0;
  uint8_t pad = 0;       // padding bytes (8/16-bit data)
  uint8_t dtype = 0;     // content type
  uint8_t num = 0;
  static BankHeader decode(const uint32_t* p) {
    BankHeader h;
    h.length = p[0];
    h.tag = uint16_t(p[1] >> 16);
    h.pad = uint8_t((p[1] >> 14) & 0x3u);
    h.dtype = uint8_t((p[1] >> 8) & 0x3Fu);
    h.num = uint8_t(p[1] & 0xFFu);
    return h;
  }
  uint32_t word1() const {
    return (uint32_t(tag) << 16) | (uint32_t(pad & 0x3u) << 14) | (uint32_t(dtype & 0x3Fu) << 8) | num;
  }
};

// CODA3 event classification by top-level tag.
bool is_physics_tag(uint16_t tag);   // 0xFF50..0xFF5F, 0xFF70..0xFF7F (Podd accepts 50/58/70/78)
bool is_control_tag(uint16_t tag);   // 0xFFD0..0xFFDF (sync/prestart/go/pause/end)
bool is_trigger_bank_tag(uint16_t tag);   // 0xFF10..0xFF4F (built: 0xFF20..0xFF27)

struct TriggerBankInfo {
  bool present = false;
  uint16_t tag = 0;
  uint8_t nrocs = 0;              // header num
  uint32_t offset = 0;            // word offset of the trigger bank length word in the event
  uint32_t total_words = 0;       // length + 1
  uint64_t first_event_number = 0;
  bool has_timestamps = false;    // tag & 1
  bool has_run_info = false;      // tag & 2
  std::vector<uint64_t> timestamps;    // per event of the block (if has_timestamps)
  std::vector<uint16_t> event_types;   // per event of the block
  uint64_t run_info = 0;
  std::vector<uint16_t> roc_segment_ids;   // tags of the per-ROC segments
};

struct RocBankRef {
  uint16_t roc_id = 0;      // tag & 0x0FFF (Podd)
  uint16_t tag = 0;         // full ROC bank tag (upper 4 bits: CODA status flags)
  uint8_t num = 0, dtype = 0;
  uint32_t offset = 0;      // word offset of the ROC bank length word in the event
  uint32_t total_words = 0; // length + 1
  uint32_t bank_begin = 0, bank_end = 0;   // range in EventView::banks() (empty if not a bank container)
};

struct DataBankRef {
  uint32_t roc_index = 0;   // index into EventView::rocs()
  uint16_t roc_id = 0;
  uint16_t tag = 0;
  uint8_t num = 0, dtype = 0, pad = 0;
  uint32_t offset = 0;          // word offset of this bank's length word in the event
  uint32_t payload_offset = 0;  // = offset + 2
  uint32_t payload_len = 0;     // words
  const uint32_t* payload = nullptr;   // into the parsed buffer (valid while it lives)
};

// Read-only view of one event (does not copy the buffer).
class EventView {
 public:
  EventView() = default;
  // Parse an event bank. `nwords` is the buffer capacity; the event occupies buf[0]+1 words.
  static EventView parse(const uint32_t* buf, size_t nwords);

  const uint32_t* data() const { return buf_; }
  size_t nwords() const { return nwords_; }   // buf[0]+1
  const BankHeader& header() const { return hdr_; }
  uint16_t tag() const { return hdr_.tag; }
  uint8_t block_level() const { return hdr_.num; }
  bool is_physics() const { return is_physics_tag(hdr_.tag); }
  bool is_control() const { return is_control_tag(hdr_.tag); }
  const TriggerBankInfo& trigger() const { return trig_; }
  const std::vector<RocBankRef>& rocs() const { return rocs_; }
  const std::vector<DataBankRef>& banks() const { return banks_; }
  // indices into banks() matching ROC id and bank tag (roc_id < 0: any ROC)
  std::vector<size_t> find_banks(int roc_id, uint16_t tag) const;
  // index into rocs() or -1
  int find_roc(uint16_t roc_id) const;

 private:
  const uint32_t* buf_ = nullptr;
  size_t nwords_ = 0;
  BankHeader hdr_;
  TriggerBankInfo trig_;
  std::vector<RocBankRef> rocs_;
  std::vector<DataBankRef> banks_;
  struct Child { uint32_t offset, total_words; int roc; };   // top-level children in order
  std::vector<Child> children_;
  friend class EventEditor;
};

// Builds a modified copy of an event: replace data-bank payloads, append banks.
// All enclosing lengths (data bank, ROC bank, event bank) are recomputed. The source buffer
// referenced by the EventView must stay alive and unmodified until build.
class EventEditor {
 public:
  explicit EventEditor(const EventView& view);

  // Replace the payload of view.banks()[bank_index] (any length, including 0 or longer).
  // The bank header word 1 is kept except that the padding bits are cleared (uint32 payload).
  void replace_payload(size_t bank_index, const uint32_t* p, size_t n);
  void replace_payload(size_t bank_index, std::vector<uint32_t>&& v);
  // Append a new data bank as last child of view.rocs()[roc_index] (Podd-safe placement).
  void append_bank_in_roc(size_t roc_index, uint16_t tag, uint8_t num, const uint32_t* p, size_t n,
                          uint8_t dtype = type::kUInt32);
  // Append a new bank as last child of the event bank. WARNING: for CODA3 physics events Podd
  // requires (#top-level banks after the trigger bank) == trigger-bank num, so a top-level
  // addition makes Podd reject the event; prefer append_bank_in_roc().
  void append_top_level_bank(uint16_t tag, uint8_t num, const uint32_t* p, size_t n,
                             uint8_t dtype = type::kUInt32);

  bool modified() const { return !repl_.empty() || !roc_app_.empty() || !top_app_.empty(); }
  size_t built_words() const;                       // size of the result in words
  void build_into(std::vector<uint32_t>& out) const;
  std::vector<uint32_t> build() const { std::vector<uint32_t> v; build_into(v); return v; }
  // Write into caller memory (e.g. an ET event). dst must not overlap the source buffer.
  // Returns words written; throws EvioError(BadArgument) if capacity is too small.
  size_t build_into(uint32_t* dst, size_t capacity) const;

 private:
  struct Appended { uint32_t parent; uint16_t tag; uint8_t num; uint8_t dtype; std::vector<uint32_t> words; };
  struct Repl { uint32_t bank; std::vector<uint32_t> words; };
  EventView v_;                       // copy of the view (small); the buffer itself is not copied
  std::vector<Repl> repl_;            // sorted by bank index on build
  std::vector<Appended> roc_app_;     // parent = roc index
  std::vector<Appended> top_app_;
  template <class Sink> void emit(Sink& s) const;
};

// Build a complete standalone bank (2-word header + payload).
std::vector<uint32_t> make_bank(uint16_t tag, uint8_t dtype, uint8_t num, const std::vector<uint32_t>& payload);

}  // namespace evioio
}  // namespace gemfilter
