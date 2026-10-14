// gf_et_feed / gf_et_drain : replay an EVIO file through a real ET system and collect the output.
//
//   gf_et_feed  -f <ET file> -i <in.evio>  [-n max] [--rate Hz]     (producer at GrandCentral,
//               each CODA event framed as a one-event EVIO v4 buffer, as written by the CODA EB)
//   gf_et_drain -f <ET file> -o <out.evio> [-n expected] [--station ER] [--position P] [--timeout s]
//               (blocking station at the END of the chain = stands in for the CODA ER)
//
// Use on a DAQ test machine: et_start -> gf_et_drain (ER) -> gemfilter_et (position 1) -> gf_et_feed.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "et.h"
}
#include "gemfilter/apps/event_io.h"
#include "gemfilter/et/evio_framing.h"
#include "gemfilter/status/coda_event.h"

using namespace gemfilter;

namespace {
std::string arg(int argc, char** argv, const char* k, const char* def) {
  for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], k)) return argv[i + 1];
  return def;
}
et_sys_id openEt(const std::string& file) {
  et_openconfig oc;
  et_open_config_init(&oc);
  et_open_config_setwait(oc, ET_OPEN_WAIT);
  struct timespec ts{10, 0};
  et_open_config_settimeout(oc, ts);
  et_sys_id id;
  int st = et_open(&id, file.c_str(), oc);
  et_open_config_destroy(oc);
  if (st != ET_OK) { std::fprintf(stderr, "et_open(%s) failed: %d\n", file.c_str(), st); std::exit(1); }
  return id;
}

int feed(int argc, char** argv) {
  std::string f = arg(argc, argv, "-f", ""), in = arg(argc, argv, "-i", "");
  long maxn = std::stol(arg(argc, argv, "-n", "0"));
  double rate = std::stod(arg(argc, argv, "--rate", "0"));
  if (f.empty() || in.empty()) { std::fprintf(stderr, "usage: gf_et_feed -f ETfile -i in.evio [-n N] [--rate Hz]\n"); return 2; }
  et_sys_id id = openEt(f);
  et_att_id att;
  if (et_station_attach(id, ET_GRANDCENTRAL, &att) != ET_OK) { std::fprintf(stderr, "attach GC failed\n"); return 1; }
  EvioFileSource src(in);
  std::vector<uint32_t> buf;
  size_t n = 0;
  long count = 0;
  uint32_t blk = 1;
  auto t0 = std::chrono::steady_clock::now();
  while ((maxn == 0 || count < maxn) && src.next(buf, n, 0)) {
    std::vector<uint32_t> ev(buf.begin(), buf.begin() + long(n));
    auto cls = coda::classify(ev.data(), ev.size());
    auto type = cls == coda::EventClass::Physics ? et::BufferEventType::Physics
              : cls == coda::EventClass::Control ? et::BufferEventType::Control : et::BufferEventType::User;
    auto framed = et::build_buffer(et::Framing::EvioV4, {ev}, blk++, type, false);
    et_event* pe = nullptr;
    int st = et_event_new(id, att, &pe, ET_SLEEP, nullptr, framed.size() * 4);
    if (st != ET_OK) { std::fprintf(stderr, "et_event_new failed: %d\n", st); return 1; }
    void* data = nullptr;
    et_event_getdata(pe, &data);
    std::memcpy(data, framed.data(), framed.size() * 4);
    et_event_setlength(pe, framed.size() * 4);
    et_event_put(id, att, pe);
    ++count;
    if (rate > 0) std::this_thread::sleep_until(t0 + std::chrono::microseconds(long(1e6 * count / rate)));
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::fprintf(stderr, "gf_et_feed: %ld events in %.2f s\n", count, s);
  et_station_detach(id, att);
  et_close(id);
  return 0;
}

volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_sig(int) { g_stop = 1; }

int drain(int argc, char** argv) {
  std::string f = arg(argc, argv, "-f", ""), out = arg(argc, argv, "-o", ""), name = arg(argc, argv, "--station", "ER");
  long expect = std::stol(arg(argc, argv, "-n", "0"));
  double tmo = std::stod(arg(argc, argv, "--timeout", "30"));
  const int pos = std::stoi(arg(argc, argv, "--position", "-1"));   // -1 = ET_END
  if (f.empty() || out.empty()) { std::fprintf(stderr, "usage: gf_et_drain -f ETfile -o out.evio [-n N] [--station ER] [--position P] [--timeout s]\n"); return 2; }
  std::signal(SIGINT, on_sig); std::signal(SIGTERM, on_sig);
  et_sys_id id = openEt(f);
  et_statconfig sc;
  et_station_config_init(&sc);
  et_station_config_setblock(sc, ET_STATION_BLOCKING);
  et_station_config_setselect(sc, ET_STATION_SELECT_ALL);
  et_stat_id stat;
  int st = et_station_create_at(id, &stat, name.c_str(), sc, pos < 0 ? ET_END : pos, ET_END);
  et_station_config_destroy(sc);
  if (st != ET_OK && st != ET_ERROR_EXISTS) { std::fprintf(stderr, "station create failed: %d\n", st); return 1; }
  et_att_id att;
  if (et_station_attach(id, stat, &att) != ET_OK) { std::fprintf(stderr, "attach failed\n"); return 1; }
  std::fprintf(stderr, "gf_et_drain: station %s ready\n", name.c_str());
  std::fflush(stderr);
  EvioFileSink sink(out);
  long count = 0;
  auto last = std::chrono::steady_clock::now();
  std::vector<et_event*> pe(64);
  while (!g_stop && (expect == 0 || count < expect)) {
    struct timespec ts{0, 200000000};
    int nread = 0;
    st = et_events_get(id, att, pe.data(), ET_TIMED, &ts, int(pe.size()), &nread);
    if (st == ET_ERROR_TIMEOUT || st == ET_ERROR_EMPTY) {
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count() > tmo) break;
      continue;
    }
    if (st != ET_OK) { std::fprintf(stderr, "et_events_get: %d\n", st); break; }
    last = std::chrono::steady_clock::now();
    for (int i = 0; i < nread; ++i) {
      void* data = nullptr; size_t len = 0;
      et_event_getdata(pe[size_t(i)], &data);
      et_event_getlength(pe[size_t(i)], &len);
      std::vector<std::vector<uint32_t>> evs;
      if (et::split_buffer(static_cast<uint32_t*>(data), len / 4, evs))
        for (auto& e : evs) { sink.write(e.data(), e.size()); ++count; }
    }
    et_events_put(id, att, pe.data(), nread);
  }
  sink.close();
  std::fprintf(stderr, "gf_et_drain: %ld events written to %s\n", count, out.c_str());
  et_station_detach(id, att);
  et_close(id);
  return expect == 0 || count == expect ? 0 : 3;
}
}  // namespace

int main(int argc, char** argv) {
  std::string me = argv[0];
  if (me.find("drain") != std::string::npos) return drain(argc, argv);
  return feed(argc, argv);
}
