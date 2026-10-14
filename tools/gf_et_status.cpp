// gf_et_status : probe an ET system for the gemfilter stations (used by deploy/coda/gemfilter_ctl
// and by the run-control hook to verify that the filters are attached and upstream of the ER).
//
//   gf_et_status -f <ET file> [--host H] [--port N] [--udp-port N] [--timeout S]
//                [--station A,B,...] [--er-station ER] [--quiet]
//
// For every station prints one line:
//   station=<name> exists=0|1 position=<n> parallel=<n> attachments=<n> status=idle|active|-
//                  in=<events in> out=<events out>
// plus "stations=<n in system>".  Exit codes:
//   0  every requested station exists, has >= 1 attachment and (with --er-station) sits upstream
//      of the ER station
//   1  a requested station is missing or has no attachment
//   2  ordering problem: a requested station is at or downstream of the ER station, or the ER
//      station itself is missing
//   3  ET system could not be opened
//   4  usage error
// With no --station only the ET system is probed (exit 0 if it opens).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "et.h"
}

namespace {
std::string arg(int argc, char** argv, const char* k, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], k)) return argv[i + 1];
  return def;
}
bool flag(int argc, char** argv, const char* k) {
  for (int i = 1; i < argc; ++i)
    if (!std::strcmp(argv[i], k)) return true;
  return false;
}
std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> v;
  std::stringstream ss(s);
  std::string t;
  while (std::getline(ss, t, ','))
    if (!t.empty()) v.push_back(t);
  return v;
}

struct StationInfo {
  bool exists = false;
  int position = -1, parallel = -1, natt = 0, status = -1, in = -1, out = -1;
};

StationInfo probe(et_sys_id id, const std::string& name) {
  StationInfo s;
  et_stat_id sid;
  if (et_station_name_to_id(id, &sid, name.c_str()) != ET_OK) return s;
  s.exists = true;
  et_station_getposition(id, sid, &s.position, &s.parallel);
  et_station_getattachments(id, sid, &s.natt);
  et_station_getstatus(id, sid, &s.status);
  et_station_getinputcount(id, sid, &s.in);
  et_station_getoutputcount(id, sid, &s.out);
  return s;
}

const char* status_name(int st) {
  if (st == ET_STATION_ACTIVE) return "active";
  if (st == ET_STATION_IDLE) return "idle";
  return "-";
}

void print(const std::string& name, const StationInfo& s) {
  std::printf("station=%s exists=%d position=%d parallel=%d attachments=%d status=%s in=%d out=%d\n",
              name.c_str(), s.exists ? 1 : 0, s.position, s.parallel, s.natt, status_name(s.status),
              s.in, s.out);
}
}  // namespace

int main(int argc, char** argv) {
  if (flag(argc, argv, "-h") || flag(argc, argv, "--help") || arg(argc, argv, "-f", "")[0] == '\0') {
    std::fprintf(stderr,
                 "usage: gf_et_status -f ETfile [--host H] [--port N] [--udp-port N] [--timeout S]\n"
                 "                    [--station A,B,...] [--er-station ER] [--quiet]\n");
    return 4;
  }
  const std::string file = arg(argc, argv, "-f", "");
  const std::string host = arg(argc, argv, "--host", "");
  const int port = std::atoi(arg(argc, argv, "--port", "0").c_str());
  const int udp = std::atoi(arg(argc, argv, "--udp-port", "0").c_str());
  const int tmo = std::atoi(arg(argc, argv, "--timeout", "3").c_str());
  const auto stations = split(arg(argc, argv, "--station", ""));
  const std::string er = arg(argc, argv, "--er-station", "");
  const bool quiet = flag(argc, argv, "--quiet");

  et_openconfig oc;
  et_open_config_init(&oc);
  et_open_config_setmode(oc, ET_HOST_AS_LOCAL);
  et_open_config_sethost(oc, host.empty() ? ET_HOST_LOCAL : host.c_str());
  if (!host.empty() && port > 0) {
    et_open_config_setcast(oc, ET_DIRECT);
    et_open_config_setserverport(oc, port);
  } else if (udp > 0) {
    et_open_config_setport(oc, udp);
  }
  if (tmo > 0) {
    et_open_config_setwait(oc, ET_OPEN_WAIT);
    struct timespec ts{tmo, 0};
    et_open_config_settimeout(oc, ts);
  } else {
    et_open_config_setwait(oc, ET_OPEN_NOWAIT);
  }
  et_open_config_setdebugdefault(oc, ET_DEBUG_NONE);
  et_sys_id id;
  const int err = et_open(&id, file.c_str(), oc);
  et_open_config_destroy(oc);
  if (err != ET_OK) {
    if (!quiet) std::printf("et=%s open=fail err=%d\n", file.c_str(), err);
    return 3;
  }
  int nst = -1;
  et_system_getstations(id, &nst);
  if (!quiet) std::printf("et=%s open=ok stations=%d\n", file.c_str(), nst);

  int rc = 0;
  StationInfo ers;
  if (!er.empty()) {
    ers = probe(id, er);
    if (!quiet) print(er, ers);
    if (!ers.exists) rc = 2;
  }
  for (const auto& name : stations) {
    const StationInfo s = probe(id, name);
    if (!quiet) print(name, s);
    if (!s.exists || s.natt < 1) {
      if (rc == 0) rc = 1;
      continue;
    }
    if (!er.empty() && ers.exists && s.position >= ers.position) rc = 2;
  }
  et_close(id);
  return rc;
}
