// gemfilter — ET filter-station engine (the core of the gemfilter_et daemon).
//
// One I/O thread owns the ET attachment and does all ET calls (get, put, new, dump); a pool
// of worker threads processes events.  Each worker owns its own FilterPolicy (and thus its
// own EventProcessor instance from the ProcessorFactory) and scratch buffers.
//
//   I/O thread:  et_events_get(chunk) -> chunk #k queued -> workers process events of the
//                chunk in parallel -> when the OLDEST in-flight chunk is complete it is put
//                back with et_events_put.  Chunks are always put in get order, so a single
//                attachment preserves event order end to end.  Up to `max_inflight` chunks
//                are held at once (pipelining get / process / put).
//   per event:   the ET buffer (EVIO v4 blocks / v6 records) is rewritten into a worker
//                scratch buffer with FilterPolicy applied to each CODA event; if anything
//                changed and the result fits the ET event (memsize), it is copied back and
//                et_event_setlength() is called (in-place).  The ET buffer is untouched until
//                that final copy, which keeps the window in which a crash could leave a
//                half-written event to a single memcpy.  If the result is LARGER than the
//                event memory: OversizePolicy::PassThrough leaves the original event (counted);
//                OversizePolicy::NewEvent obtains a (temp) event with et_event_new, copies data
//                and metadata, puts the new event in the original's place in the same ordered
//                put, and dumps the original afterwards.
//   ET death:    any Dead/Error status from ET (or et_alive()==0 while idle) -> wait for
//                workers to finish in-flight chunks, discard them (they cannot be returned to a
//                dead system), force-close, and reconnect with exponential back-off until the
//                ET system is back (CODA restarts the EB's ET at Download/Prestart).
//   stop flag:   (SIGINT/SIGTERM) -> stop getting, drain: finish and put all in-flight chunks,
//                detach, remove the station if no attachments remain, return.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "gemfilter/et/et_backend.h"
#include "gemfilter/processor.h"
#include "gemfilter/status/policy.h"

namespace gemfilter {
namespace et {

enum class OversizePolicy : uint8_t { PassThrough, NewEvent };

struct EtFilterConfig {
  int nthreads = 4;
  int chunk = 64;                   // events per et_events_get
  int max_inflight = 0;             // chunks held at once; 0 => 2 * nthreads
  int get_timeout_ms = 200;         // timed get when nothing is in flight
  int busy_poll_ms = 2;             // timed get while chunks are in flight
  OversizePolicy oversize = OversizePolicy::PassThrough;
  size_t max_event_bytes = 64u << 20;  // upper bound for NewEvent / scratch buffers
  int reconnect_min_ms = 250;
  int reconnect_max_ms = 8000;
  int max_reconnects = -1;          // <0: unlimited (tests use small values)
  double stats_interval_s = 10.0;   // 0 disables periodic stats callback
  int alive_check_ms = 2000;        // et_alive() poll period while idle
  bool remove_station_on_exit = true;
};

struct EtFilterSnapshot {
  double elapsed_s = 0;
  uint64_t et_events = 0, chunks = 0, coda_events = 0, physics = 0, modified = 0;
  uint64_t prescaled = 0, errors = 0, untouched_unsupported = 0, oversize_passthrough = 0;
  uint64_t new_events = 0, inplace = 0, bytes_in = 0, bytes_out = 0, strips_in = 0, strips_out = 0;
  uint64_t cpu_us = 0, reconnects = 0, discarded_on_death = 0;
  int inflight = 0;
  bool connected = false;
  std::string last_message;
};
std::string format_snapshot(const EtFilterSnapshot& s, const EtFilterSnapshot* prev = nullptr);

class EtFilter {
 public:
  EtFilter(EtBackend& backend, ProcessorFactory factory, PolicyConfig policy, EtFilterConfig cfg);
  ~EtFilter();
  EtFilter(const EtFilter&) = delete;
  EtFilter& operator=(const EtFilter&) = delete;

  // Blocks until `stop` becomes true (then drains) or max_reconnects is exceeded.
  // Returns 0 on graceful stop, 1 if it gave up reconnecting.
  int run(const std::atomic<bool>& stop);

  EtFilterSnapshot snapshot() const;
  // Called from the I/O thread every stats_interval_s, and once at the end.
  std::function<void(const EtFilterSnapshot&)> on_stats;
  // Optional, called from WORKER threads for every CODA event (must be thread-safe).
  std::function<void(const EventResult&)> on_event;
  // Optional, called from the I/O thread after each successful (re)connect.
  std::function<void(const std::string&)> on_connect;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace et
}  // namespace gemfilter
