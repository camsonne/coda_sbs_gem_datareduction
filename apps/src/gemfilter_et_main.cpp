// gemfilter_et — CODA3 ET filter station between the Event Builder and the Event Recorder.
//
//   gemfilter_et -f /et/sbs_et --station GEMFILTER --position 1 --er-station ER1 -t 8 \
//                -p gem --prescale 100
// See docs/coda.md for station configuration and failure modes.
#include <atomic>
#include <csignal>
#include <iostream>
#include <unistd.h>

#include "cli.h"
#include "gemfilter/apps/processor_registry.h"
#include "gemfilter/et/et_filter.h"
#include "gemfilter/et/libet_backend.h"

using namespace gemfilter;

namespace {
std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

void usage() {
  std::cerr <<
R"(usage: gemfilter_et -f <ET file> [options]
 ET system:
  -f, --et-file NAME      ET system file (the EB output ET of the CODA configuration)
  --host HOST             ET host ("" = local; remote => TCP, events copied over network)
  --port N                ET server TCP port (with --host: direct connection)
  --udp-port N            ET broadcast/multicast port used to locate the system
  --open-wait S           wait up to S seconds for the ET system in each open attempt
 station:
  --station NAME          station name (default GEMFILTER)
  --position N            station position, 1 = right after GrandCentral (default 1)
  --er-station NAME       ER station name: check that the filter is upstream of it
  --fix-position          if the ER station is upstream, move the filter in front of it
  --nonblocking           non-blocking station (events bypass when input cue is full)
  --cue N                 input cue for --nonblocking (default 100)
  --restore MODE          out|in|gc|redist: where ET puts events of a dead filter (default out)
  --parallel              create as member of a parallel station group (see docs)
  --select MODE           all|rrobin|equalcue (rrobin/equalcue for parallel groups)
  --keep-station          do not remove the station on exit
 processing:
  -t, --threads N         worker threads (default 4)
  --chunk N               events per et_events_get (default 64)
  --inflight N            chunks in flight (default 2*threads)
  -p, --processor SPEC    passthrough | truncate[:frac] (test only) | <registered>
  --prescale N            raw pass-through of every Nth physics event (flagged)
  --no-status             do not append the GEMFILTER status bank
  --gem-banks LIST        GEM bank tags locating the status-bank host ROC (default 3561)
  --oversize MODE         pass|new: event grown beyond ET event size (default pass)
  --instance-id N         status bank p12 (default pid)
  --stats S               stats print interval in seconds (default 10, 0 = off)
  --max-reconnects N      give up after N failed reconnect attempts (default: never)
  --et-debug N            ET debug level 0-4 (default 2)
)";
}
}  // namespace

int main(int argc, char** argv) {
  try {
    cli::Args a(argc, argv, {"--fix-position", "--nonblocking", "--parallel", "--keep-station",
                             "--no-status", "-h", "--help"});
    if (a.has("-h", "--help") || !a.has("-f", "--et-file")) {
      usage();
      return a.has("-h", "--help") ? 0 : 2;
    }
    et::EtConnectionConfig ec;
    ec.et_file = a.str("-f", "--et-file", ec.et_file);
    ec.host = a.str("--host", "", "");
    ec.server_port = static_cast<int>(a.num("--port", "", 0));
    ec.udp_port = static_cast<int>(a.num("--udp-port", "", 0));
    ec.open_wait_s = static_cast<int>(a.num("--open-wait", "", 0));
    ec.debug = static_cast<int>(a.num("--et-debug", "", 2));
    ec.station = a.str("--station", "", ec.station);
    ec.position = static_cast<int>(a.num("--position", "", 1));
    ec.er_station = a.str("--er-station", "", "");
    ec.fix_position = a.has("--fix-position");
    ec.blocking = !a.has("--nonblocking");
    ec.cue = static_cast<int>(a.num("--cue", "", 100));
    ec.restore = a.str("--restore", "", "out");
    ec.parallel = a.has("--parallel");
    ec.select = a.str("--select", "", "all");

    PolicyConfig pc;
    pc.prescale = static_cast<uint32_t>(a.num("--prescale", "", 0));
    pc.append_status = !a.has("--no-status");
    if (a.has("--gem-banks")) pc.gem_bank_tags = cli::parse_u16_list(a.str("--gem-banks", "", ""));
    pc.instance_id = static_cast<uint32_t>(a.num("--instance-id", "", static_cast<long long>(getpid())));

    et::EtFilterConfig fc;
    fc.nthreads = static_cast<int>(a.num("-t", "--threads", 4));
    fc.chunk = static_cast<int>(a.num("--chunk", "", 64));
    fc.max_inflight = static_cast<int>(a.num("--inflight", "", 0));
    fc.oversize = a.str("--oversize", "", "pass") == "new" ? et::OversizePolicy::NewEvent
                                                           : et::OversizePolicy::PassThrough;
    fc.stats_interval_s = a.real("--stats", "", 10.0);
    fc.max_reconnects = static_cast<int>(a.num("--max-reconnects", "", -1));
    fc.remove_station_on_exit = !a.has("--keep-station");

    auto factory = make_processor_factory(a.str("-p", "--processor", "passthrough"));

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // remote ET: broken socket must surface as an ET error

    et::LibEtBackend be(ec);
    et::EtFilter filter(be, factory, pc, fc);
    et::EtFilterSnapshot prev;
    bool have_prev = false;
    filter.on_stats = [&](const et::EtFilterSnapshot& s) {
      std::cerr << et::format_snapshot(s, have_prev ? &prev : nullptr) << '\n';
      if (!s.last_message.empty() && (!have_prev || s.last_message != prev.last_message))
        std::cerr << "[gemfilter_et] " << s.last_message << '\n';
      prev = s;
      have_prev = true;
    };
    filter.on_connect = [](const std::string& m) { std::cerr << "[gemfilter_et] " << m << '\n'; };
    std::cerr << "[gemfilter_et] " << be.describe() << ", " << fc.nthreads << " threads, chunk "
              << fc.chunk << "\n";
    const int rc = filter.run(g_stop);
    std::cerr << "[gemfilter_et] exit: " << filter.snapshot().last_message << '\n';
    return rc;
  } catch (const std::exception& e) {
    std::cerr << "gemfilter_et: error: " << e.what() << '\n';
    return 1;
  }
}
