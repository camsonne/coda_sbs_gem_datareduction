// gemfilter codec: decode an SSP/VTP MPD GEM data-bank payload into ApvFrames and
// re-encode an arbitrary subset of strips bit-exactly in the original format.
//
//   DecodedBank db = codec::decode(payload, nwords, crate);
//   KeepMask keep = ...;                       // keep[f][j] parallel to db.frames[f].hits
//   std::vector<uint32_t> out = codec::encode(db.layout, db.frames, keep);
//
// With an all-true mask and default EncodeOptions the output equals the input word for word.
// See docs/codec.md and docs/gem_format.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "gemfilter/types.h"

namespace gemfilter {
namespace codec {

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
enum class ErrorCode : uint8_t {
  OrphanContinuation = 1,   // continuation word with no preceding defining word
  IncompleteHit,            // MPD frame continuation words not a multiple of 3
  BadContinuationCount,     // type 3/12/13 group with wrong number of continuation words (strict)
  HitOutsideEvent,          // MPD frame before any event header inside a block
  BlockNotClosed,           // block header without trailer
  TrailerWithoutHeader,     // block trailer outside a block
  NestedBlock,              // block header inside an open block
  BlockSizeMismatch,        // #event headers != BLOCK_SIZE, or blocks disagree on BLOCK_SIZE
  BlockCountMismatch,       // NUM_WORDS != inclusive count (only with strict_block_count)
  SlotMismatch,             // trailer SLOTID != header SLOTID (strict)
  UnknownType,              // reserved data type (only with strict_unknown_types)
  TooManyEvents,            // more events than DecodeOptions::max_events
  LayoutMismatch,           // encode: frames / mask inconsistent with layout
  SampleOutOfRange,         // encode (repack): sample outside 13-bit signed range
  CountUnderflow,           // encode: NUM_WORDS would become negative
};
const char* to_string(ErrorCode c);

class CodecError : public std::runtime_error {
 public:
  CodecError(ErrorCode code, size_t word_index, const std::string& what);
  ErrorCode code() const { return code_; }
  size_t word_index() const { return word_index_; }   // position in the payload (decode) or layout
 private:
  ErrorCode code_;
  size_t word_index_;
};
class DecodeError : public CodecError { using CodecError::CodecError; };
class EncodeError : public CodecError { using CodecError::CodecError; };

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct DecodeOptions {
  uint16_t default_slot = 11;        // slot for bare (VTP) payloads, SBS-offline fSLOTID_VTP
  int      fiber_bits = 6;           // 6 = 40-MPD firmware (bits 21..16); 5 = SSP doc 2020 (20..16)
  bool     strict_counts = true;     // enforce 1/2/2 continuation words for types 3/12/13
  bool     strict_block_count = false; // require NUM_WORDS == header..trailer inclusive
  bool     strict_slot = true;       // trailer SLOTID must equal header SLOTID
  bool     strict_unknown_types = false; // throw on reserved types (else carried verbatim)
  bool     allow_orphan_words = false;   // tolerate continuation words before any defining word
  uint32_t max_events = 256;         // sanity limit on events per bank
};

enum class FillerPolicy : uint8_t {
  Preserve = 0,   // keep filler words exactly as in the input (required for bit-exact identity)
  Realign  = 1,   // recompute fillers inside each block (or bare payload) to a multiple of `align`
};

struct EncodeOptions {
  FillerPolicy filler_policy = FillerPolicy::Preserve;
  uint32_t align = 2;                // modulus for FillerPolicy::Realign (2 or 4)
  // Frames whose MPD frame header has BUILD_ALL_SAMPLES=1 or CM_OR=1 are read out in full and
  // SBS-offline computes their common mode from all 128 strips: keep them whole regardless of mask.
  bool protect_full_readout = true;
  // Re-pack the 13-bit samples from StripHit::adc (bits 31..26 of each word stay as in the input).
  // Default false: kept hits are copied verbatim from the input words.
  bool repack_samples = false;
};

struct EncodeStats {
  uint32_t strips_in = 0, strips_out = 0;
  uint32_t words_in = 0, words_out = 0;
  uint32_t protected_frames = 0;     // frames kept whole by protect_full_readout
};

// ---------------------------------------------------------------------------
// Decoded metadata
// ---------------------------------------------------------------------------
enum class Variant : uint8_t { Empty = 0, BlockWrapped = 1, Bare = 2 };

// Per-frame information beyond the shared ApvFrame (parallel to DecodedBank::frames).
struct FrameInfo {
  uint32_t event = 0;            // event index within the bank (0..nevents-1)
  uint32_t block = 0;            // block index (block-wrapped) or 0
  uint32_t frame_header = 0;     // raw MPD frame header word of the first hit of this APV
  bool enable_cm = false, build_all_samples = false, cm_or = false;
  bool full_readout() const { return build_all_samples || cm_or; }
};

struct BlockInfo {
  uint32_t header_word = 0, trailer_word = 0;
  uint32_t header_pos = 0, trailer_pos = 0;   // word positions in the payload
  uint16_t slot = 0;
  uint16_t block_number = 0;
  uint16_t block_size = 0;
  uint32_t num_words = 0;                     // NUM_WORDS field as read
  bool count_is_inclusive = false;            // NUM_WORDS == trailer_pos - header_pos + 1
};

struct EventInfo {
  uint32_t event_header = 0;      // raw type-2 word (0 if none)
  bool     has_header = false;
  uint32_t trigger_number = 0;
  uint64_t trigger_time = 0;      // 48-bit (first trigger-time pair seen for this event)
  bool     has_trigger_time = false;
};

// Opaque re-encoding layout: retains every input word and the order of all items.
class GemBankLayout {
 public:
  enum class Kind : uint8_t { Verbatim = 0, Hit = 1, BlockHeader = 2, BlockTrailer = 3, Filler = 4 };
  struct Item {
    uint32_t begin = 0;   // first word in words()
    uint32_t len = 0;     // number of words
    uint32_t frame = 0;   // Hit: frame index
    uint32_t hit = 0;     // Hit: src_index within frame
    uint32_t block = 0;   // block index for BlockHeader/BlockTrailer/Filler inside a block; ~0u if none
    Kind kind = Kind::Verbatim;
  };

  Variant variant() const { return variant_; }
  uint16_t crate() const { return crate_; }
  size_t nwords() const { return words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }      // copy of the input payload
  const std::vector<Item>& items() const { return items_; }
  const std::vector<BlockInfo>& blocks() const { return blocks_; }
  const std::vector<EventInfo>& events() const { return events_; }
  uint32_t nevents() const { return uint32_t(events_.size()); }
  uint32_t nframes() const { return uint32_t(frame_nhits_.size()); }
  uint32_t nhits_total() const { return nhits_total_; }
  const std::vector<uint32_t>& frame_nhits() const { return frame_nhits_; }
  void clear();

 private:
  friend class Decoder;
  friend class Encoder;
  Variant variant_ = Variant::Empty;
  uint16_t crate_ = 0;
  std::vector<uint32_t> words_;
  std::vector<Item> items_;
  std::vector<BlockInfo> blocks_;
  std::vector<EventInfo> events_;
  std::vector<uint32_t> frame_nhits_;
  std::vector<uint8_t> frame_protected_;
  std::vector<uint32_t> block_item_begin_, block_item_end_;  // item ranges (header..trailer)
  uint32_t nhits_total_ = 0;
};

// Result of decoding one data-bank payload (all events of the block).
struct DecodedBank {
  std::vector<ApvFrame> frames;        // all events, grouped: frames of event e are
                                       // [event_begin[e], event_begin[e+1])
  std::vector<uint32_t> event_begin;   // size nevents+1
  std::vector<FrameInfo> frame_info;   // parallel to frames
  GemBankLayout layout;

  uint32_t nevents() const { return event_begin.empty() ? 0u : uint32_t(event_begin.size() - 1); }
  // Copy of the frames of one event (convenience; prefer index ranges in hot loops).
  std::vector<ApvFrame> event_frames(uint32_t e) const;
  // All-true mask matching `frames`.
  KeepMask keep_all() const;
  void clear();
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
// Decode one data-bank payload (words after the 2-word EVIO bank header) from ROC `crate`.
DecodedBank decode(const uint32_t* payload, size_t nwords, uint16_t crate,
                   const DecodeOptions& opt = {});
inline DecodedBank decode(const std::vector<uint32_t>& payload, uint16_t crate,
                          const DecodeOptions& opt = {}) {
  return decode(payload.data(), payload.size(), crate, opt);
}
// Allocation-reusing form.
void decode_into(const uint32_t* payload, size_t nwords, uint16_t crate, DecodedBank& out,
                 const DecodeOptions& opt = {});

// Re-encode. `keep[f][j]` refers to frames[f].hits[j]; the original hit is
// frames[f].hits[j].src_index (hits may be reordered or removed from the vectors; a hit that
// is absent from frames[f].hits is dropped). Frame order/count must match the layout.
std::vector<uint32_t> encode(const GemBankLayout& layout, const std::vector<ApvFrame>& frames,
                             const KeepMask& keep, const EncodeOptions& opt = {},
                             EncodeStats* stats = nullptr);
void encode_into(const GemBankLayout& layout, const std::vector<ApvFrame>& frames,
                 const KeepMask& keep, std::vector<uint32_t>& out,
                 const EncodeOptions& opt = {}, EncodeStats* stats = nullptr);
// Mask indexed directly by src_index ([frame][src_index]); no frames needed (repack unavailable).
std::vector<uint32_t> encode(const GemBankLayout& layout, const KeepMask& keep_by_src,
                             const EncodeOptions& opt = {}, EncodeStats* stats = nullptr);

// Human-readable dump of a payload (one line per word) for debugging real data.
std::string dump(const uint32_t* payload, size_t nwords, const DecodeOptions& opt = {});

}  // namespace codec
}  // namespace gemfilter
