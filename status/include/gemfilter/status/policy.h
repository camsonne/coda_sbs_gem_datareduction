// gemfilter — per-event policy wrapper around an EventProcessor.
//
// FilterPolicy::apply() is what both drivers (file mode, ET mode) call for every CODA event:
//   1. classify by top-level tag (Podd CodaDecoder rules): only built physics events
//      (0xFF50/0xFF58/0xFF70/0xFF78) are candidates.  Control (0xFFD0-0xFFDF: sync, prestart,
//      go, pause, end), user events (tag < 0xFF00: EPICS 131, scalers 140/141, prescale
//      120/133, DAQ config 137/138, ...) and any other reserved tag are NEVER touched.
//   2. prescaled raw pass-through: every Nth physics event (N = prescale) is left unmodified
//      and flagged (flags::kPassthroughPrescale) in its status bank.
//   3. otherwise run the processor on a private copy; commit only if it returns a valid
//      event that fits `cap`.  Exceptions / kNoRoom / invalid output => original event kept,
//      flags::kDecodeError or kNoRoom set (contract error policy: never drop an event).
//   4. append the GEMFILTER status bank to the GEM host ROC (status_bank.h).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "gemfilter/processor.h"
#include "gemfilter/status/coda_event.h"
#include "gemfilter/status/status_bank.h"

namespace gemfilter {

struct PolicyConfig {
  uint32_t prescale = 0;               // raw pass-through of every Nth physics event (0 = off)
  bool append_status = true;           // append GEMFILTER bank to processed physics events
  bool status_on_prescaled = true;     // ... and to prescaled raw events (flag carrier)
  std::vector<uint16_t> gem_bank_tags{coda::kSbsMpdBankTag};
  uint32_t instance_id = 0;            // stored in status bank p12
};

struct EventResult {
  coda::EventClass cls = coda::EventClass::Malformed;
  uint64_t event_number = 0;           // first event number of the block (trigger bank)
  uint8_t block_level = 0;
  FilterStats stats;                   // processor stats with policy flags OR-ed in
  uint32_t words_in = 0, words_out = 0;  // whole event, before / after (incl. status bank)
  uint32_t cpu_us = 0;                 // processor thread-CPU time
  double wall_us = 0;                  // wall time of apply()
  uint32_t prescale_counter = 0;
  bool modified = false;               // processor output committed
  bool status_appended = false;
};

class FilterPolicy {
 public:
  // `physics_counter` may be shared between several FilterPolicy instances (worker threads)
  // so that the prescale counts physics events globally.  nullptr => private counter.
  FilterPolicy(std::unique_ptr<EventProcessor> proc, PolicyConfig cfg,
               std::shared_ptr<std::atomic<uint64_t>> physics_counter = nullptr);
  EventResult apply(uint32_t* buf, size_t& nwords, size_t cap);
  uint64_t config_hash() const { return hash_; }
  const PolicyConfig& config() const { return cfg_; }
  EventProcessor& processor() { return *proc_; }

 private:
  std::unique_ptr<EventProcessor> proc_;
  PolicyConfig cfg_;
  std::shared_ptr<std::atomic<uint64_t>> counter_;
  uint64_t hash_ = 0;
  std::vector<uint32_t> scratch_;
};

// Thread CPU time in microseconds (CLOCK_THREAD_CPUTIME_ID).
uint64_t thread_cpu_us();

}  // namespace gemfilter
