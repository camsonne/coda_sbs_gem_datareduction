// gemfilter — file-mode driver: EventSource -> FilterPolicy -> EventSink + metrics CSV.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "gemfilter/apps/event_io.h"
#include "gemfilter/status/policy.h"

namespace gemfilter {

// Per-event metrics CSV.  One row per input event (all classes, so the CSV can be joined
// with the output file event by event).  Columns:
//   index,event_number,block_level,class,strips_in,strips_out,bytes_in,bytes_out,
//   gem_bytes_in,gem_bytes_out,nseeds,nroads,ntracks,flags,cpu_us,wall_us,modified,status
// bytes_in/out = whole event (incl. status bank); gem_bytes_* = as reported by the processor.
class MetricsCsvWriter {
 public:
  explicit MetricsCsvWriter(std::ostream& os);
  void write(uint64_t index, const EventResult& r);
  static const char* header();

 private:
  std::ostream& os_;
};

struct FileDriverConfig {
  uint64_t max_events = 0;      // 0 = all
  uint64_t skip = 0;            // skip first N input events (not written)
  size_t headroom_words = 4096; // extra capacity given to the processor per event
};

struct FileDriverSummary {
  uint64_t events = 0, physics = 0, modified = 0, prescaled = 0, errors = 0, status_appended = 0;
  uint64_t words_in = 0, words_out = 0, strips_in = 0, strips_out = 0, cpu_us = 0;
  double wall_s = 0;
  std::map<std::string, uint64_t> by_class;
  std::string to_string() const;
};

class FileDriver {
 public:
  FileDriver(EventSource& src, EventSink* sink, MetricsCsvWriter* csv, FilterPolicy& policy,
             FileDriverConfig cfg = {});
  FileDriverSummary run();

 private:
  EventSource& src_;
  EventSink* sink_;
  MetricsCsvWriter* csv_;
  FilterPolicy& pol_;
  FileDriverConfig cfg_;
};

// Inventory of (ROC id, child bank tag) in built physics events — used to check that the
// GEMFILTER status-bank tag does not collide with any bank in the real data, and to find the
// GEM ROCs/banks of a run.
struct BankInventory {
  struct Entry { uint64_t count = 0, words = 0; };
  std::map<std::pair<uint16_t, uint16_t>, Entry> roc_bank;   // (roc, bank tag)
  std::map<std::string, uint64_t> event_classes;
  std::map<uint16_t, uint64_t> top_tags;
  uint64_t events = 0;
  void add(const uint32_t* ev, size_t nwords);
  void print(std::ostream& os) const;
};

}  // namespace gemfilter
