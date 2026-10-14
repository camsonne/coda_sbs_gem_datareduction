// gemfilter — minimal event source / sink abstraction for the file-mode driver.
//
// The evioio track provides the full EVIO event/bank layer; the driver only needs "give me
// the next top-level event" and "write this event", so it is written against these two
// interfaces.  EvioFileSource/EvioFileSink implement them directly with the evio C API
// (evOpen / evReadNoCopy / evWrite / evClose) and can be swapped for evioio-based classes
// without touching FileDriver.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gemfilter {

class EventSource {
 public:
  virtual ~EventSource() = default;
  // Copy the next top-level event into buf[0..nwords) (buf grows as needed and keeps at least
  // `headroom` extra words so processors/status bank may grow the event).  false at end.
  virtual bool next(std::vector<uint32_t>& buf, size_t& nwords, size_t headroom) = 0;
  virtual std::string describe() const = 0;
};

class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void write(const uint32_t* ev, size_t nwords) = 0;
  virtual void close() = 0;
};

// evio C library implementations (read: evio v1-4 and uncompressed v6; write: evio v6).
class EvioFileSource final : public EventSource {
 public:
  explicit EvioFileSource(const std::string& path);
  ~EvioFileSource() override;
  bool next(std::vector<uint32_t>& buf, size_t& nwords, size_t headroom) override;
  std::string describe() const override { return "evio:" + path_; }

 private:
  std::string path_;
  int handle_ = -1;
};

class EvioFileSink final : public EventSink {
 public:
  explicit EvioFileSink(const std::string& path);
  ~EvioFileSink() override;
  void write(const uint32_t* ev, size_t nwords) override;
  void close() override;

 private:
  std::string path_;
  int handle_ = -1;
};

// In-memory implementations (tests, embedding).
class MemorySource final : public EventSource {
 public:
  explicit MemorySource(std::vector<std::vector<uint32_t>> events) : ev_(std::move(events)) {}
  bool next(std::vector<uint32_t>& buf, size_t& nwords, size_t headroom) override;
  std::string describe() const override { return "memory"; }

 private:
  std::vector<std::vector<uint32_t>> ev_;
  size_t i_ = 0;
};

class MemorySink final : public EventSink {
 public:
  void write(const uint32_t* ev, size_t nwords) override { events.emplace_back(ev, ev + nwords); }
  void close() override {}
  std::vector<std::vector<uint32_t>> events;
};

}  // namespace gemfilter
