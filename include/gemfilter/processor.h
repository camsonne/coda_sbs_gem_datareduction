// gemfilter — processing-pipeline interface (owned by the CODA-integration track).
//
// The ET daemon (gemfilter_et) and the file driver (gemfilter_file) call an EventProcessor
// once per CODA3 *built physics event* (one top-level EVIO bank, host-endian, possibly a
// multi-event block with block level = top bank num).  Control, user (EPICS, scaler, config)
// and unknown events never reach a processor: they are filtered out by FilterPolicy
// (status/include/gemfilter/status/policy.h), which also implements prescaled raw
// pass-through, the error policy, timing, and the GEMFILTER status bank.
//
// Contract for implementers (codec/evioio/seeds/track pipeline):
//   * buf[0 .. nwords) holds the event; buf has room for `cap` words (cap >= nwords).
//   * On return, buf[0 .. nwords) must be a valid built event (lengths consistent).  The
//     processor may shrink or (within cap) grow the event.  Never write beyond cap.
//   * If the result would not fit in `cap`, leave the content unspecified and set
//     flags::kNoRoom in the returned stats, or throw: FilterPolicy works on a private copy
//     and restores the original event in both cases.
//   * Any exception => the event passes through UNMODIFIED with flags::kDecodeError set.
//   * One processor instance is used by exactly one thread at a time (the ET daemon creates
//     one instance per worker thread through a ProcessorFactory), so implementations may
//     keep scratch state without locking.
//   * Fill FilterStats strips/bytes/seeds/roads/tracks/flags (contract bits 0-2; see
//     status_bank.h namespace flags for the additional bits).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "gemfilter/types.h"

namespace gemfilter {

class EventProcessor {
 public:
  virtual ~EventProcessor() = default;
  virtual FilterStats process(uint32_t* buf, size_t& nwords, size_t cap) = 0;
  // Short name and a canonical configuration string; the latter is hashed (FNV-1a 64) into
  // the GEMFILTER status bank so every output event records which configuration made it.
  virtual std::string name() const = 0;
  virtual std::string config_string() const { return name(); }
};

using ProcessorFactory = std::function<std::unique_ptr<EventProcessor>()>;

}  // namespace gemfilter
