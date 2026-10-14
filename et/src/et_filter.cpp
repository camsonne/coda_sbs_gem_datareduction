#include "gemfilter/et/et_filter.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "gemfilter/et/evio_framing.h"

namespace gemfilter {
namespace et {

const char* to_string(EtStatus s) {
  switch (s) {
    case EtStatus::Ok: return "ok";
    case EtStatus::Timeout: return "timeout";
    case EtStatus::Empty: return "empty";
    case EtStatus::Wakeup: return "wakeup";
    case EtStatus::Dead: return "dead";
    case EtStatus::Error: return "error";
  }
  return "?";
}


namespace {
using Clock = std::chrono::steady_clock;

struct Outcome {
  enum Kind : uint8_t { Untouched, InPlace, NeedNew } kind = Untouched;
  size_t new_bytes = 0;
  std::vector<uint32_t> newdata;
};

struct Chunk {
  uint64_t seq = 0;
  std::vector<EtEventRef> evs;
  std::vector<Outcome> out;
  std::atomic<int> remaining{0};
};

struct Counters {
  std::atomic<uint64_t> et_events{0}, chunks{0}, coda_events{0}, physics{0}, modified{0},
      prescaled{0}, errors{0}, unsupported{0}, oversize_pt{0}, new_events{0}, inplace{0},
      bytes_in{0}, bytes_out{0}, strips_in{0}, strips_out{0}, cpu_us{0}, reconnects{0},
      discarded{0};
};
}  // namespace

struct EtFilter::Impl {
  EtBackend& be;
  ProcessorFactory factory;
  PolicyConfig pcfg;
  EtFilterConfig cfg;
  EtFilter* owner;
  std::shared_ptr<std::atomic<uint64_t>> counter = std::make_shared<std::atomic<uint64_t>>(0);

  std::vector<std::thread> workers;
  std::mutex qm;
  std::condition_variable qcv;
  std::deque<std::pair<Chunk*, size_t>> tasks;
  bool quit = false;

  std::mutex dm;
  std::condition_variable dcv;
  std::deque<std::unique_ptr<Chunk>> inflight;

  Counters c;
  std::atomic<int> n_inflight{0};
  std::atomic<bool> connected{false};
  Clock::time_point t0 = Clock::now();
  mutable std::mutex msgm;
  std::string last_msg;

  Impl(EtBackend& b, ProcessorFactory f, PolicyConfig p, EtFilterConfig cf, EtFilter* o)
      : be(b), factory(std::move(f)), pcfg(std::move(p)), cfg(cf), owner(o) {
    if (cfg.nthreads < 1) cfg.nthreads = 1;
    if (cfg.chunk < 1) cfg.chunk = 1;
    if (cfg.max_inflight <= 0) cfg.max_inflight = 2 * cfg.nthreads;
  }

  void set_msg(const std::string& m) {
    std::lock_guard<std::mutex> l(msgm);
    last_msg = m;
  }

  void start_workers() {
    quit = false;
    for (int i = 0; i < cfg.nthreads; ++i) workers.emplace_back([this] { worker_main(); });
  }
  void stop_workers() {
    {
      std::lock_guard<std::mutex> l(qm);
      quit = true;
    }
    qcv.notify_all();
    for (auto& t : workers) t.join();
    workers.clear();
  }

  void worker_main() {
    FilterPolicy pol(factory(), pcfg, counter);
    std::vector<uint32_t> scratch;
    for (;;) {
      std::pair<Chunk*, size_t> task;
      {
        std::unique_lock<std::mutex> l(qm);
        qcv.wait(l, [&] { return quit || !tasks.empty(); });
        if (tasks.empty()) return;  // quit
        task = tasks.front();
        tasks.pop_front();
      }
      Chunk* ch = task.first;
      process_event(pol, scratch, ch->evs[task.second], ch->out[task.second]);
      if (ch->remaining.fetch_sub(1) == 1) {
        std::lock_guard<std::mutex> l(dm);
        dcv.notify_all();
      }
    }
  }

  void process_event(FilterPolicy& pol, std::vector<uint32_t>& scratch, EtEventRef& ev, Outcome& out) {
    c.et_events++;
    c.bytes_in += ev.length;
    if (!ev.data || ev.needs_swap || ev.length % 4 != 0 || ev.length < 32) {
      c.unsupported++;
      c.bytes_out += ev.length;
      return;
    }
    const size_t nw = ev.length / 4;
    const size_t capw = ev.capacity / 4;
    size_t scap = std::max(capw, nw + nw / 4 + 4096);
    scap = std::min(scap, std::max(nw + 64, cfg.max_event_bytes / 4));
    if (scratch.size() < scap) scratch.resize(scap);
    struct Local {
      uint64_t coda = 0, phys = 0, mod = 0, pres = 0, err = 0, sin = 0, sout = 0, cpu = 0;
    } loc;
    auto cb = [&](uint32_t* e, size_t& n, size_t cap) {
      EventResult r = pol.apply(e, n, cap);
      loc.coda++;
      if (r.cls == coda::EventClass::Physics) {
        loc.phys++;
        if (r.modified) loc.mod++;
        if (r.stats.flags & flags::kPassthroughPrescale) loc.pres++;
        if (r.stats.flags & (flags::kDecodeError | flags::kNoRoom)) loc.err++;
        loc.sin += r.stats.strips_in;
        loc.sout += r.stats.strips_out;
        loc.cpu += r.cpu_us;
      }
      if (owner->on_event) owner->on_event(r);
      return r.modified || r.status_appended;
    };
    RewriteResult rr = rewrite_buffer(ev.data, nw, scratch.data(), scap, cb);
    c.coda_events += loc.coda;
    c.physics += loc.phys;
    c.prescaled += loc.pres;
    c.errors += loc.err;
    c.cpu_us += loc.cpu;
    if (!rr.ok) {
      c.unsupported++;
      c.bytes_out += ev.length;
      return;
    }
    bool committed = false;
    if (!rr.changed) {
      c.bytes_out += ev.length;
    } else if (rr.out_words <= capw) {
      std::memcpy(ev.data, scratch.data(), rr.out_words * sizeof(uint32_t));
      out.kind = Outcome::InPlace;
      out.new_bytes = rr.out_words * 4;
      c.bytes_out += out.new_bytes;
      c.inplace++;
      committed = true;
    } else if (cfg.oversize == OversizePolicy::NewEvent && rr.out_words * 4 <= cfg.max_event_bytes) {
      out.kind = Outcome::NeedNew;
      out.newdata.assign(scratch.begin(), scratch.begin() + static_cast<long>(rr.out_words));
      out.new_bytes = rr.out_words * 4;
      c.bytes_out += out.new_bytes;
      committed = true;
    } else {
      c.oversize_pt++;
      c.bytes_out += ev.length;
    }
    if (committed) {
      c.modified += loc.mod;
      c.strips_in += loc.sin;
      c.strips_out += loc.sout;
    } else {
      c.strips_in += loc.sin;
      c.strips_out += loc.sin;
    }
  }

  void dispatch(std::unique_ptr<Chunk> ch) {
    {
      std::lock_guard<std::mutex> l(qm);
      for (size_t i = 0; i < ch->evs.size(); ++i) tasks.emplace_back(ch.get(), i);
    }
    qcv.notify_all();
    inflight.push_back(std::move(ch));
    n_inflight = static_cast<int>(inflight.size());
  }

  bool head_done() { return !inflight.empty() && inflight.front()->remaining.load() == 0; }

  void wait_all_done() {
    std::unique_lock<std::mutex> l(dm);
    dcv.wait(l, [&] {
      for (auto& ch : inflight)
        if (ch->remaining.load() != 0) return false;
      return true;
    });
  }

  // Apply outcomes, put the chunk, dump replaced originals.
  EtStatus finalize(Chunk& ch) {
    std::vector<EtEventRef> to_dump;
    for (size_t i = 0; i < ch.evs.size(); ++i) {
      Outcome& o = ch.out[i];
      EtEventRef& ev = ch.evs[i];
      if (o.kind == Outcome::InPlace) {
        be.set_length(ev, o.new_bytes);
      } else if (o.kind == Outcome::NeedNew) {
        EtEventRef ne;
        const EtStatus st = be.new_event(o.new_bytes, 1000, ne);
        if (is_fatal(st)) return st;
        if (st == EtStatus::Ok && ne.capacity >= o.new_bytes && ne.data) {
          std::memcpy(ne.data, o.newdata.data(), o.new_bytes);
          be.set_length(ne, o.new_bytes);
          be.copy_meta(ev, ne);
          to_dump.push_back(ev);
          ev = ne;
          c.new_events++;
        } else {
          if (st == EtStatus::Ok) {
            std::vector<EtEventRef> d{ne};
            be.dump(d);
          }
          c.oversize_pt++;
        }
        o.newdata.clear();
        o.newdata.shrink_to_fit();
      }
    }
    EtStatus st = be.put(ch.evs);
    if (st != EtStatus::Ok) return is_fatal(st) ? st : EtStatus::Error;
    if (!to_dump.empty()) {
      st = be.dump(to_dump);
      if (is_fatal(st)) return st;
    }
    return EtStatus::Ok;
  }

  EtStatus flush_completed() {
    while (head_done()) {
      const EtStatus st = finalize(*inflight.front());
      if (st != EtStatus::Ok) return st;
      inflight.pop_front();
      n_inflight = static_cast<int>(inflight.size());
    }
    return EtStatus::Ok;
  }

  void handle_death(const std::string& why) {
    wait_all_done();
    uint64_t lost = 0;
    for (auto& ch : inflight) lost += ch->evs.size();
    inflight.clear();
    n_inflight = 0;
    c.discarded += lost;
    be.disconnect(true, false);
    connected = false;
    c.reconnects++;
    set_msg("ET lost (" + why + "); discarded " + std::to_string(lost) +
            " in-flight events; reconnecting");
  }

  EtFilterSnapshot snap() const {
    EtFilterSnapshot s;
    s.elapsed_s = std::chrono::duration<double>(Clock::now() - t0).count();
    s.et_events = c.et_events;
    s.chunks = c.chunks;
    s.coda_events = c.coda_events;
    s.physics = c.physics;
    s.modified = c.modified;
    s.prescaled = c.prescaled;
    s.errors = c.errors;
    s.untouched_unsupported = c.unsupported;
    s.oversize_passthrough = c.oversize_pt;
    s.new_events = c.new_events;
    s.inplace = c.inplace;
    s.bytes_in = c.bytes_in;
    s.bytes_out = c.bytes_out;
    s.strips_in = c.strips_in;
    s.strips_out = c.strips_out;
    s.cpu_us = c.cpu_us;
    s.reconnects = c.reconnects;
    s.discarded_on_death = c.discarded;
    s.inflight = n_inflight;
    s.connected = connected;
    std::lock_guard<std::mutex> l(msgm);
    s.last_message = last_msg;
    return s;
  }

  int run(const std::atomic<bool>& stop) {
    t0 = Clock::now();
    start_workers();
    int result = 0;
    int backoff = cfg.reconnect_min_ms;
    int fail_streak = 0;
    uint64_t seq = 0;
    auto last_alive = Clock::now();
    auto next_stats = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                         std::chrono::duration<double>(cfg.stats_interval_s));
    std::vector<EtEventRef> evs;
    while (!stop.load()) {
      if (!connected) {
        std::string msg;
        const EtStatus st = be.connect(msg);
        set_msg(msg);
        if (st == EtStatus::Ok) {
          connected = true;
          backoff = cfg.reconnect_min_ms;
          fail_streak = 0;
          last_alive = Clock::now();
          if (owner->on_connect) owner->on_connect(msg);
        } else {
          be.disconnect(true, false);
          if (cfg.max_reconnects >= 0 && fail_streak >= cfg.max_reconnects) {
            result = 1;
            break;
          }
          ++fail_streak;
          const auto until = Clock::now() + std::chrono::milliseconds(backoff);
          while (!stop.load() && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          backoff = std::min(backoff * 2, cfg.reconnect_max_ms);
          continue;
        }
      }
      EtStatus st = flush_completed();
      if (st != EtStatus::Ok) {
        handle_death(std::string("put: ") + to_string(st));
        continue;
      }
      if (static_cast<int>(inflight.size()) < cfg.max_inflight) {
        const int to = inflight.empty() ? cfg.get_timeout_ms : cfg.busy_poll_ms;
        st = be.get(evs, cfg.chunk, to);
        if (st == EtStatus::Ok && !evs.empty()) {
          auto ch = std::make_unique<Chunk>();
          ch->seq = seq++;
          ch->evs = std::move(evs);
          evs = {};
          ch->out.resize(ch->evs.size());
          ch->remaining = static_cast<int>(ch->evs.size());
          c.chunks++;
          dispatch(std::move(ch));
          last_alive = Clock::now();
        } else if (is_fatal(st)) {
          handle_death(std::string("get: ") + to_string(st));
          continue;
        } else if (inflight.empty() &&
                   Clock::now() - last_alive > std::chrono::milliseconds(cfg.alive_check_ms)) {
          if (!be.alive()) {
            handle_death("et_alive()==0");
            continue;
          }
          last_alive = Clock::now();
        }
      } else {
        std::unique_lock<std::mutex> l(dm);
        dcv.wait_for(l, std::chrono::milliseconds(50), [&] { return head_done(); });
      }
      if (cfg.stats_interval_s > 0 && Clock::now() >= next_stats) {
        if (owner->on_stats) owner->on_stats(snap());
        next_stats = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                        std::chrono::duration<double>(cfg.stats_interval_s));
      }
    }
    // Drain.
    if (connected) {
      wait_all_done();
      const EtStatus st = flush_completed();
      if (st != EtStatus::Ok) {
        handle_death(std::string("drain put: ") + to_string(st));
      } else {
        be.disconnect(false, cfg.remove_station_on_exit);
        connected = false;
        set_msg("drained and detached");
      }
    } else {
      wait_all_done();
      inflight.clear();
    }
    stop_workers();
    if (owner->on_stats) owner->on_stats(snap());
    return result;
  }
};

EtFilter::EtFilter(EtBackend& backend, ProcessorFactory factory, PolicyConfig policy, EtFilterConfig cfg)
    : impl_(std::make_unique<Impl>(backend, std::move(factory), std::move(policy), cfg, this)) {}
EtFilter::~EtFilter() = default;
int EtFilter::run(const std::atomic<bool>& stop) { return impl_->run(stop); }
EtFilterSnapshot EtFilter::snapshot() const { return impl_->snap(); }

std::string format_snapshot(const EtFilterSnapshot& s, const EtFilterSnapshot* prev) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1);
  const double dt = prev ? s.elapsed_s - prev->elapsed_s : s.elapsed_s;
  const auto d = [&](uint64_t a, uint64_t b) { return static_cast<double>(a - (prev ? b : 0)); };
  const double ev_rate = dt > 0 ? d(s.coda_events, prev ? prev->coda_events : 0) / dt : 0;
  const double mb_in = dt > 0 ? d(s.bytes_in, prev ? prev->bytes_in : 0) / dt / 1e6 : 0;
  const double mb_out = dt > 0 ? d(s.bytes_out, prev ? prev->bytes_out : 0) / dt / 1e6 : 0;
  const double phys = d(s.physics, prev ? prev->physics : 0);
  const double cpu = phys > 0 ? d(s.cpu_us, prev ? prev->cpu_us : 0) / phys : 0;
  os << "[gemfilter_et t=" << s.elapsed_s << "s] " << (s.connected ? "connected" : "DISCONNECTED")
     << " events=" << s.coda_events << " (" << ev_rate << "/s) physics=" << s.physics
     << " modified=" << s.modified << " prescaled=" << s.prescaled << " errors=" << s.errors
     << " in=" << mb_in << "MB/s out=" << mb_out << "MB/s"
     << " reduction=" << (s.bytes_out ? static_cast<double>(s.bytes_in) / static_cast<double>(s.bytes_out) : 0.0)
     << " cpu/ev=" << cpu << "us inflight=" << s.inflight << " unsupported=" << s.untouched_unsupported
     << " oversize_pt=" << s.oversize_passthrough << " new=" << s.new_events
     << " reconnects=" << s.reconnects << " discarded=" << s.discarded_on_death;
  return os.str();
}

}  // namespace et
}  // namespace gemfilter
