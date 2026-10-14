// gemfilter_file — run the GEM filter pipeline on a raw CODA3 EVIO file.
//
//   gemfilter_file -i run.evio.0 -o run_filtered.evio --csv metrics.csv -p passthrough
//   gemfilter_file -i run.evio.0 --list-banks            # ROC/bank inventory (tag check)
#include <fstream>
#include <iostream>
#include <memory>
#include <unistd.h>

#include "cli.h"
#include "gemfilter/apps/event_io.h"
#include "gemfilter/apps/file_driver.h"
#include "gemfilter/apps/processor_registry.h"
#include "gemfilter/status/policy.h"

using namespace gemfilter;

static void usage() {
  std::cerr <<
R"(usage: gemfilter_file -i <in.evio> [options]
  -i, --input FILE        input EVIO file (CODA3 raw data)
  -o, --output FILE       output EVIO file (evio v6, written with the evio C library)
  --csv FILE              per-event metrics CSV
  -p, --processor SPEC    processor: passthrough | truncate[:frac] (test only) | <registered>
  --prescale N            raw pass-through of every Nth physics event (flagged), 0 = off
  --no-status             do not append the GEMFILTER status bank
  --no-status-prescaled   do not append the status bank to prescaled raw events
  --gem-banks LIST        comma-separated GEM bank tags locating the host ROC (default 3561)
  --instance-id N         value stored in status bank word p12 (default: pid)
  -n, --max-events N      process at most N events (0 = all)
  --skip N                skip first N events
  --list-banks            print (ROC, bank tag) inventory of physics events and exit
  -q, --quiet             no summary line
)";
}

int main(int argc, char** argv) {
  try {
    cli::Args a(argc, argv, {"--no-status", "--no-status-prescaled", "--list-banks", "-q",
                             "--quiet", "-h", "--help"});
    if (a.has("-h", "--help") || !a.has("-i", "--input")) {
      usage();
      return a.has("-h", "--help") ? 0 : 2;
    }
    const std::string in = a.str("-i", "--input", "");
    EvioFileSource src(in);

    if (a.has("--list-banks")) {
      BankInventory inv;
      std::vector<uint32_t> buf;
      size_t n = 0;
      const auto maxev = static_cast<uint64_t>(a.num("-n", "--max-events", 0));
      while (src.next(buf, n, 0)) {
        inv.add(buf.data(), n);
        if (maxev && inv.events >= maxev) break;
      }
      inv.print(std::cout);
      return 0;
    }

    PolicyConfig pc;
    pc.prescale = static_cast<uint32_t>(a.num("--prescale", "", 0));
    pc.append_status = !a.has("--no-status");
    pc.status_on_prescaled = !a.has("--no-status-prescaled");
    if (a.has("--gem-banks")) pc.gem_bank_tags = cli::parse_u16_list(a.str("--gem-banks", "", ""));
    pc.instance_id = static_cast<uint32_t>(a.num("--instance-id", "", static_cast<long long>(getpid())));
    auto factory = make_processor_factory(a.str("-p", "--processor", "passthrough"));
    FilterPolicy policy(factory(), pc);

    std::unique_ptr<EvioFileSink> sink;
    if (a.has("-o", "--output")) sink = std::make_unique<EvioFileSink>(a.str("-o", "--output", ""));
    std::ofstream csvf;
    std::unique_ptr<MetricsCsvWriter> csv;
    if (a.has("--csv")) {
      csvf.open(a.str("--csv", "", ""));
      if (!csvf) throw std::runtime_error("cannot open CSV output");
      csv = std::make_unique<MetricsCsvWriter>(csvf);
    }
    FileDriverConfig fc;
    fc.max_events = static_cast<uint64_t>(a.num("-n", "--max-events", 0));
    fc.skip = static_cast<uint64_t>(a.num("--skip", "", 0));
    FileDriver drv(src, sink.get(), csv.get(), policy, fc);
    const FileDriverSummary s = drv.run();
    if (!a.has("-q", "--quiet"))
      std::cerr << "gemfilter_file: " << policy.processor().name() << " config_hash=0x" << std::hex
                << policy.config_hash() << std::dec << " " << s.to_string() << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "gemfilter_file: error: " << e.what() << '\n';
    return 1;
  }
}
