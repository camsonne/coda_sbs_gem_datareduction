// gemfilter — EVIO *buffer* framing for ET events.
//
// A CODA3 ET event written by the Event Builder holds an EVIO buffer: one or more EVIO
// blocks (version 4, 8-word block header) or records (version 6, 14-word record header,
// event-length index array, optional user header), each containing one or more top-level
// events of a single CODA type (evio.c header comment: "only a single CODA event TYPE is
// placed into a single ET or cMsg buffer").  Header layouts are taken from evio 6.x
// src/libsrc/evio.c (EV_HD_* word positions and the v4/v6 header diagrams).
//
// rewrite_buffer() copies the headers, runs a callback on every top-level event in place in
// the output buffer (the callback may shrink or grow the event within the given capacity),
// and patches the lengths: v4 block size; v6 record length, index array entries and
// uncompressed data length (adjusted by the byte delta, which is independent of whether a
// writer counts the user header in that word).  The format version is preserved.
//
// UNVERIFIED: which version the SBS CODA3 EMU writes into ET (CODA 3.10+ uses evio 6
//   records, earlier CODA 3 used evio 4 blocks).  Both are supported; byte-swapped buffers
//   and compressed v6 records are reported Unsupported and must be passed through.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gemfilter {
namespace et {

constexpr uint32_t kEvioMagic = 0xc0da0100u;
constexpr uint32_t kEvioMagicSwapped = 0x0001dac0u;

enum class Framing : uint8_t { EvioV4, EvioV6, Unsupported };
const char* to_string(Framing f);

// Bit-info event type (bits 10-13 of word 5 counted from 0, i.e. "bits 11-14" in evio.c).
enum class BufferEventType : uint8_t {
  RocRaw = 0, Physics = 1, PartialPhysics = 2, DisentangledPhysics = 3, User = 4, Control = 5,
  Other = 15
};

struct BufferInfo {
  Framing framing = Framing::Unsupported;
  std::string why;                  // reason when Unsupported
  uint32_t ncontainers = 0;         // blocks or records
  uint32_t nevents = 0;             // top-level events found (excluding v4 dictionary)
  uint32_t type_bits = 0;           // event type of the first container
  bool has_dictionary = false;
};

// Inspect without modifying.  Returns framing; fills info.
Framing inspect_buffer(const uint32_t* in, size_t nwords, BufferInfo& info);

// Per-event callback: ev points into the OUTPUT buffer, holding a copy of the event
// (nwords).  cap = words available from ev to the end of the output buffer.  Return true if
// the event was changed (length or content).  Must leave a valid event of nwords <= cap.
using EventCallback = std::function<bool(uint32_t* ev, size_t& nwords, size_t cap)>;

struct RewriteResult {
  bool ok = false;                  // false => output invalid, caller must pass input through
  bool changed = false;             // any callback reported a change
  size_t out_words = 0;
  uint32_t nevents = 0;
  std::string error;
  BufferInfo info;
};

RewriteResult rewrite_buffer(const uint32_t* in, size_t nin, uint32_t* out, size_t cap,
                             const EventCallback& cb);

// Build a buffer (used by tests, the synthetic ET producer and file->ET replays).
// type = BufferEventType for the bit-info word; last = set the last-block/record bit.
std::vector<uint32_t> build_buffer(Framing f, const std::vector<std::vector<uint32_t>>& events,
                                   uint32_t container_number, BufferEventType type, bool last);

// Split a buffer into its top-level events (copies).  Returns false if malformed/unsupported.
bool split_buffer(const uint32_t* in, size_t nin, std::vector<std::vector<uint32_t>>& events,
                  BufferInfo* info = nullptr);

}  // namespace et
}  // namespace gemfilter
