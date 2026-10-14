// gemfilter — thin abstraction over the ET client calls used by the filter station.
//
// LibEtBackend (libet_backend.h) is the real implementation on top of JLab ET; tests use a
// fake implementation to exercise ET death / restart and drain paths deterministically.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gemfilter {
namespace et {

struct EtEventRef {
  void* handle = nullptr;      // backend handle (et_event*)
  uint32_t* data = nullptr;    // event data (host memory or shared memory)
  size_t length = 0;           // bytes of valid data
  size_t capacity = 0;         // bytes available (et_event::memsize)
  bool needs_swap = false;     // et_event_needtoswap()
  bool is_new = false;         // obtained with new_event()
};

enum class EtStatus { Ok, Timeout, Empty, Wakeup, Dead, Error };
const char* to_string(EtStatus s);
inline bool is_fatal(EtStatus s) { return s == EtStatus::Dead || s == EtStatus::Error; }

class EtBackend {
 public:
  virtual ~EtBackend() = default;
  // Open the ET system (if needed), create/attach the station.  Safe to call again after
  // disconnect().  msg receives a human-readable description or error.
  virtual EtStatus connect(std::string& msg) = 0;
  // Get up to `max` events; timeout_ms <= 0 means non-blocking (ET_ASYNC).
  virtual EtStatus get(std::vector<EtEventRef>& out, int max, int timeout_ms) = 0;
  // Put events back in the given order (ET preserves put order on the station output list).
  virtual EtStatus put(std::vector<EtEventRef>& evs) = 0;
  // Return events to GrandCentral without passing them downstream.
  virtual EtStatus dump(std::vector<EtEventRef>& evs) = 0;
  // Obtain a new (possibly temporary, larger-than-normal) event of `bytes` capacity.
  virtual EtStatus new_event(size_t bytes, int timeout_ms, EtEventRef& out) = 0;
  // Copy ET metadata (control words, byte order, priority, data status) from -> to.
  virtual void copy_meta(const EtEventRef& from, EtEventRef& to) = 0;
  virtual void set_length(EtEventRef& ev, size_t bytes) = 0;
  virtual bool alive() = 0;
  // Detach / close.  forced => et_forcedclose (ET dead); remove_station => remove the station
  // if no other attachment remains (graceful shutdown).
  virtual void disconnect(bool forced, bool remove_station) = 0;
  virtual std::string describe() const = 0;
};

}  // namespace et
}  // namespace gemfilter
