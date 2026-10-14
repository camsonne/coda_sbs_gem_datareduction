// gemfilter evioio: EVIO file reading/writing for the file-mode driver.
//
//  * EvioFileReader  - reads EVIO v4 and v6 files through the JLab evio C library
//                      (evOpen/evReadAlloc; events are returned in host byte order).
//                      Only available when built with evio (GEMFILTER_HAVE_EVIO=1).
//  * V4FileWriter    - built-in EVIO v4 writer (no evio dependency). v4 is what Podd's
//                      default evio 5.3 can read.
//  * EvioFileWriter  - V4 via V4FileWriter, V6 via evio C evWrite (needs evio).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gemfilter {
namespace evioio {

enum class EvioVersion : int { V4 = 4, V6 = 6 };

// True when the library was compiled against the evio C library.
bool evio_library_available();

class V4FileWriter {
 public:
  // target_block_words: block size target incl. the 8-word header (default 1 Mi words = 4 MB)
  explicit V4FileWriter(const std::string& path, uint32_t target_block_words = 1u << 20,
                        uint32_t max_events_per_block = 10000);
  ~V4FileWriter();
  V4FileWriter(const V4FileWriter&) = delete;
  V4FileWriter& operator=(const V4FileWriter&) = delete;
  // XML dictionary: written as the first (uncounted) event of the first block, dictionary bit set.
  // Must be called before the first write().
  void write_dictionary(const std::string& xml);
  void write(const uint32_t* event, size_t nwords);   // host order; nwords >= event[0]+1
  void close();                                       // flush + final empty block with last-block bit
  uint64_t events_written() const { return nev_; }

 private:
  void flush_block(bool last);
  std::FILE* f_ = nullptr;
  std::string path_;
  uint32_t target_, maxev_;
  std::vector<uint32_t> blk_;     // pending events
  uint32_t blk_count_ = 0;
  uint32_t blk_number_ = 1;
  bool dict_pending_ = false;
  std::vector<uint32_t> dict_;
  uint64_t nev_ = 0;
  bool closed_ = false;
};

#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
class EvioFileReader {
 public:
  explicit EvioFileReader(const std::string& path);
  ~EvioFileReader();
  EvioFileReader(const EvioFileReader&) = delete;
  EvioFileReader& operator=(const EvioFileReader&) = delete;
  // Read the next event; false at end of file. Throws EvioError(Io) on read errors.
  bool next();
  const uint32_t* data() const { return buf_; }
  size_t nwords() const { return len_; }
  int version() const { return version_; }
  const std::string& dictionary() const { return dict_; }   // v4 files only (evio C ignores v6 dictionaries)
  uint64_t events_read() const { return nread_; }
  void close();

 private:
  int handle_ = 0;
  uint32_t* buf_ = nullptr;
  size_t len_ = 0;
  int version_ = 0;
  std::string dict_;
  uint64_t nread_ = 0;
};
#endif

class EvioFileWriter {
 public:
  // V6 requires evio (throws EvioError(Unsupported) otherwise).
  EvioFileWriter(const std::string& path, EvioVersion version);
  ~EvioFileWriter();
  EvioFileWriter(const EvioFileWriter&) = delete;
  EvioFileWriter& operator=(const EvioFileWriter&) = delete;
  void write_dictionary(const std::string& xml);   // V4 only; V6: ignored (evio C 6.x has no dictionary writer)
  void write(const uint32_t* event, size_t nwords);
  void write(const std::vector<uint32_t>& ev) { write(ev.data(), ev.size()); }
  void close();
  EvioVersion version() const { return ver_; }
  uint64_t events_written() const { return nev_; }

 private:
  EvioVersion ver_;
  V4FileWriter* v4_ = nullptr;
  int handle_ = 0;
  uint64_t nev_ = 0;
};

}  // namespace evioio
}  // namespace gemfilter
