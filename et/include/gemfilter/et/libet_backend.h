// gemfilter — EtBackend implemented with the JLab ET C library.
//
// Station placement for CODA3 (see docs/coda.md):  GrandCentral(0) -> GEMFILTER(1) -> ... ->
// ER station.  The EB inserts built events into GrandCentral; stations see events in list
// order; the ER's station must be downstream of GEMFILTER.  Creating GEMFILTER at position 1
// inserts it right after GrandCentral and shifts any existing station (e.g. an ER station
// created earlier) down by one; `er_station` enables a position check (and optional fix).
#pragma once
#include <string>

#include "gemfilter/et/et_backend.h"

namespace gemfilter {
namespace et {

struct EtConnectionConfig {
  // --- ET system ---
  std::string et_file = "/tmp/et_sys_sbs";  // ET system file name (CODA: the EB's ET name)
  std::string host;                 // "" => local host (shared memory on Linux)
  int server_port = 0;              // >0 with host => ET_DIRECT connection to that TCP port
  int udp_port = 0;                 // >0 => broadcast/multicast port used to find the system
  int open_wait_s = 0;              // >0 => et_open waits up to this many seconds
  int debug = 2;                    // ET_DEBUG_ERROR
  // --- station ---
  std::string station = "GEMFILTER";
  int position = 1;                 // 1 = first after GrandCentral
  bool blocking = true;             // ET_STATION_BLOCKING (recommended, see docs)
  int cue = 100;                    // input cue for non-blocking mode
  std::string select = "all";       // all | rrobin | equalcue (latter two: parallel only)
  std::string restore = "out";      // out | in | gc | redist  (ET_STATION_RESTORE_*)
  bool multi_user = true;           // ET_STATION_USER_MULTI: several attachments may share it
  bool parallel = false;            // create as member of a parallel group
  int parallel_position = -2;       // ET_NEWHEAD (-2) or ET_END (-1) or index
  std::string er_station;           // ER station name for position check ("" = skip)
  bool fix_position = false;        // move GEMFILTER upstream of er_station if needed
};

class LibEtBackend final : public EtBackend {
 public:
  explicit LibEtBackend(EtConnectionConfig cfg);
  // Use an ET id obtained elsewhere (e.g. et_system_start in an in-process test, or an
  // embedding application).  The id is never closed by this object.
  LibEtBackend(EtConnectionConfig cfg, void* existing_et_sys_id);
  ~LibEtBackend() override;

  EtStatus connect(std::string& msg) override;
  EtStatus get(std::vector<EtEventRef>& out, int max, int timeout_ms) override;
  EtStatus put(std::vector<EtEventRef>& evs) override;
  EtStatus dump(std::vector<EtEventRef>& evs) override;
  EtStatus new_event(size_t bytes, int timeout_ms, EtEventRef& out) override;
  void copy_meta(const EtEventRef& from, EtEventRef& to) override;
  void set_length(EtEventRef& ev, size_t bytes) override;
  bool alive() override;
  void disconnect(bool forced, bool remove_station) override;
  std::string describe() const override;

  int station_id() const { return stat_; }
  int attachment_id() const { return att_; }
  // Station position (1-based list index) or -1.
  int position_of(const std::string& station_name);

 private:
  EtStatus map_err(int err) const;
  EtConnectionConfig cfg_;
  void* id_ = nullptr;
  bool external_id_ = false;
  bool open_ = false;
  int stat_ = -1;
  int att_ = -1;
  bool remote_ = false;
  std::vector<void*> ptrs_;  // scratch for et_events_get / put
};

}  // namespace et
}  // namespace gemfilter
