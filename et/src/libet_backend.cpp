#include "gemfilter/et/libet_backend.h"

#include <cstring>
#include <ctime>
#include <sstream>

extern "C" {
#include "et.h"
}

namespace gemfilter {
namespace et {


namespace {
timespec ms_to_ts(int ms) {
  timespec ts{};
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  return ts;
}
int select_mode(const std::string& s) {
  if (s == "rrobin") return ET_STATION_SELECT_RROBIN;
  if (s == "equalcue") return ET_STATION_SELECT_EQUALCUE;
  return ET_STATION_SELECT_ALL;
}
int restore_mode(const std::string& s) {
  if (s == "in") return ET_STATION_RESTORE_IN;
  if (s == "gc") return ET_STATION_RESTORE_GC;
  if (s == "redist") return ET_STATION_RESTORE_REDIST;
  return ET_STATION_RESTORE_OUT;
}
void fill_ref(et_event* pe, EtEventRef& r) {
  void* d = nullptr;
  size_t len = 0;
  int swap = 0;
  et_event_getdata(pe, &d);
  et_event_getlength(pe, &len);
  et_event_needtoswap(pe, &swap);
  r.handle = pe;
  r.data = static_cast<uint32_t*>(d);
  r.length = len;
  r.capacity = static_cast<size_t>(pe->memsize);
  r.needs_swap = swap != 0;
}
}  // namespace

LibEtBackend::LibEtBackend(EtConnectionConfig cfg) : cfg_(std::move(cfg)) {}
LibEtBackend::LibEtBackend(EtConnectionConfig cfg, void* existing)
    : cfg_(std::move(cfg)), id_(existing), external_id_(true) {}
LibEtBackend::~LibEtBackend() {
  if (open_ || att_ >= 0) disconnect(false, false);
}

EtStatus LibEtBackend::map_err(int err) const {
  switch (err) {
    case ET_OK: return EtStatus::Ok;
    case ET_ERROR_TIMEOUT: return EtStatus::Timeout;
    case ET_ERROR_EMPTY:
    case ET_ERROR_BUSY: return EtStatus::Empty;
    case ET_ERROR_WAKEUP: return EtStatus::Wakeup;
    case ET_ERROR_DEAD:
    case ET_ERROR_CLOSED:
    case ET_ERROR_READ:
    case ET_ERROR_WRITE:
    case ET_ERROR_NETWORK: return EtStatus::Dead;
    default: return EtStatus::Error;
  }
}

EtStatus LibEtBackend::connect(std::string& msg) {
  std::ostringstream os;
  if (!open_) {
    if (external_id_) {
      open_ = true;
    } else {
      et_openconfig oc;
      et_open_config_init(&oc);
      et_open_config_setmode(oc, ET_HOST_AS_LOCAL);
      if (cfg_.host.empty()) {
        et_open_config_sethost(oc, ET_HOST_LOCAL);
      } else {
        et_open_config_sethost(oc, cfg_.host.c_str());
      }
      if (!cfg_.host.empty() && cfg_.server_port > 0) {
        et_open_config_setcast(oc, ET_DIRECT);
        et_open_config_setserverport(oc, cfg_.server_port);
      } else if (cfg_.udp_port > 0) {
        et_open_config_setport(oc, cfg_.udp_port);
      }
      if (cfg_.open_wait_s > 0) {
        et_open_config_setwait(oc, ET_OPEN_WAIT);
        timespec ts{};
        ts.tv_sec = cfg_.open_wait_s;
        et_open_config_settimeout(oc, ts);
      } else {
        et_open_config_setwait(oc, ET_OPEN_NOWAIT);
      }
      et_open_config_setdebugdefault(oc, cfg_.debug);
      et_sys_id id = nullptr;
      const int err = et_open(&id, cfg_.et_file.c_str(), oc);
      et_open_config_destroy(oc);
      if (err != ET_OK) {
        os << "et_open(" << cfg_.et_file << ") failed: " << et_perror(err);
        msg = os.str();
        return EtStatus::Error;  // retried by caller
      }
      id_ = id;
      open_ = true;
      et_system_setdebug(id_, cfg_.debug);
    }
    int loc = ET_LOCAL;
    et_system_getlocality(id_, &loc);
    remote_ = (loc == ET_REMOTE);
  }

  et_statconfig sc;
  et_station_config_init(&sc);
  et_station_config_setblock(sc, cfg_.blocking ? ET_STATION_BLOCKING : ET_STATION_NONBLOCKING);
  if (!cfg_.blocking) et_station_config_setcue(sc, cfg_.cue);
  et_station_config_setselect(sc, select_mode(cfg_.select));
  et_station_config_setuser(sc, cfg_.multi_user ? ET_STATION_USER_MULTI : ET_STATION_USER_SINGLE);
  et_station_config_setrestore(sc, restore_mode(cfg_.restore));
  et_station_config_setflow(sc, cfg_.parallel ? ET_STATION_PARALLEL : ET_STATION_SERIAL);
  et_station_config_setprescale(sc, 1);
  et_stat_id stat = -1;
  int err = et_station_create_at(id_, &stat, cfg_.station.c_str(), sc, cfg_.position,
                                 cfg_.parallel ? cfg_.parallel_position : ET_END);
  et_station_config_destroy(sc);
  if (err == ET_ERROR_EXISTS) {
    os << "station " << cfg_.station << " exists with a different configuration; attaching. ";
  } else if (err != ET_OK) {
    os << "et_station_create_at(" << cfg_.station << ") failed: " << et_perror(err);
    msg = os.str();
    return map_err(err) == EtStatus::Dead ? EtStatus::Dead : EtStatus::Error;
  }
  stat_ = stat;
  et_att_id att = -1;
  err = et_station_attach(id_, stat_, &att);
  if (err != ET_OK) {
    os << "et_station_attach failed: " << et_perror(err);
    msg = os.str();
    return map_err(err) == EtStatus::Dead ? EtStatus::Dead : EtStatus::Error;
  }
  att_ = att;
  const int mypos = position_of(cfg_.station);
  os << "attached to station " << cfg_.station << " (id " << stat_ << ", position " << mypos
     << ", att " << att_ << (remote_ ? ", remote" : ", local") << ")";
  if (!cfg_.er_station.empty()) {
    int erpos = position_of(cfg_.er_station);
    if (erpos < 0) {
      os << "; ER station '" << cfg_.er_station << "' not present yet (will be downstream if "
         << "created at ET_END or position > " << mypos << ")";
    } else if (mypos > erpos) {
      os << "; WARNING: ER station '" << cfg_.er_station << "' at position " << erpos
         << " is UPSTREAM of the filter";
      if (cfg_.fix_position) {
        err = et_station_setposition(id_, stat_, erpos, ET_END);
        os << (err == ET_OK ? " -> moved filter to position " + std::to_string(erpos)
                            : std::string(" -> setposition failed: ") + et_perror(err));
      }
    } else {
      os << "; ER station '" << cfg_.er_station << "' at position " << erpos << " (downstream)";
    }
  }
  msg = os.str();
  return EtStatus::Ok;
}

int LibEtBackend::position_of(const std::string& name) {
  if (!open_) return -1;
  et_stat_id sid;
  if (et_station_name_to_id(id_, &sid, name.c_str()) != ET_OK) return -1;
  int pos = -1, ppos = -1;
  if (et_station_getposition(id_, sid, &pos, &ppos) != ET_OK) return -1;
  return pos;
}

EtStatus LibEtBackend::get(std::vector<EtEventRef>& out, int max, int timeout_ms) {
  out.clear();
  if (att_ < 0) return EtStatus::Dead;
  ptrs_.assign(static_cast<size_t>(max), nullptr);
  int mode = timeout_ms > 0 ? ET_TIMED : ET_ASYNC;
  if (remote_) mode |= ET_MODIFY;  // remote: ship modified data back on put
  timespec ts = ms_to_ts(timeout_ms);
  int nread = 0;
  const int err = et_events_get(id_, att_, reinterpret_cast<et_event**>(ptrs_.data()), mode,
                                timeout_ms > 0 ? &ts : nullptr, max, &nread);
  if (err != ET_OK) return map_err(err);
  out.resize(static_cast<size_t>(nread));
  for (int i = 0; i < nread; ++i) fill_ref(static_cast<et_event*>(ptrs_[i]), out[i]);
  return EtStatus::Ok;
}

EtStatus LibEtBackend::put(std::vector<EtEventRef>& evs) {
  if (evs.empty()) return EtStatus::Ok;
  ptrs_.resize(evs.size());
  for (size_t i = 0; i < evs.size(); ++i) ptrs_[i] = evs[i].handle;
  return map_err(et_events_put(id_, att_, reinterpret_cast<et_event**>(ptrs_.data()),
                               static_cast<int>(evs.size())));
}

EtStatus LibEtBackend::dump(std::vector<EtEventRef>& evs) {
  if (evs.empty()) return EtStatus::Ok;
  ptrs_.resize(evs.size());
  for (size_t i = 0; i < evs.size(); ++i) ptrs_[i] = evs[i].handle;
  return map_err(et_events_dump(id_, att_, reinterpret_cast<et_event**>(ptrs_.data()),
                                static_cast<int>(evs.size())));
}

EtStatus LibEtBackend::new_event(size_t bytes, int timeout_ms, EtEventRef& out) {
  et_event* pe = nullptr;
  timespec ts = ms_to_ts(timeout_ms > 0 ? timeout_ms : 1);
  const int err = et_event_new(id_, att_, &pe, ET_TIMED, &ts, bytes);
  if (err != ET_OK) return map_err(err);
  fill_ref(pe, out);
  out.is_new = true;
  return EtStatus::Ok;
}

void LibEtBackend::copy_meta(const EtEventRef& from, EtEventRef& to) {
  auto* a = static_cast<et_event*>(from.handle);
  auto* b = static_cast<et_event*>(to.handle);
  int con[ET_STATION_SELECT_INTS];
  et_event_getcontrol(a, con);
  et_event_setcontrol(b, con, ET_STATION_SELECT_INTS);
  int endian = ET_ENDIAN_LOCAL;
  et_event_getendian(a, &endian);
  et_event_setendian(b, endian);
  int pri = ET_LOW;
  et_event_getpriority(a, &pri);
  et_event_setpriority(b, pri);
  int ds = ET_DATA_OK;
  et_event_getdatastatus(a, &ds);
  et_event_setdatastatus(b, ds);
}

void LibEtBackend::set_length(EtEventRef& ev, size_t bytes) {
  et_event_setlength(static_cast<et_event*>(ev.handle), bytes);
  ev.length = bytes;
}

bool LibEtBackend::alive() { return open_ && et_alive(id_) == 1; }

void LibEtBackend::disconnect(bool forced, bool remove_station) {
  if (open_ && att_ >= 0 && !forced) et_station_detach(id_, att_);
  if (open_ && remove_station && !forced && stat_ > 0) {
    int natt = -1;
    if (et_station_getattachments(id_, stat_, &natt) == ET_OK && natt == 0)
      et_station_remove(id_, stat_);
  }
  att_ = -1;
  stat_ = -1;
  if (open_ && !external_id_) {
    if (forced)
      et_forcedclose(id_);
    else
      et_close(id_);
    id_ = nullptr;
  }
  if (!external_id_) open_ = false;
}

std::string LibEtBackend::describe() const {
  std::ostringstream os;
  os << "ET " << cfg_.et_file << (cfg_.host.empty() ? " (local)" : " @" + cfg_.host)
     << " station " << cfg_.station << " pos " << cfg_.position
     << (cfg_.blocking ? " blocking" : " nonblocking") << " restore=" << cfg_.restore
     << " select=" << cfg_.select << (cfg_.parallel ? " parallel" : " serial");
  return os.str();
}

}  // namespace et
}  // namespace gemfilter
