// gemfilter evioio: EVIO file I/O (evio C library + built-in v4 writer).
#include "gemfilter/evioio/evio_file.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "gemfilter/evioio/evio_event.h"

#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
extern "C" {
#include "evio.h"
}
#endif

namespace gemfilter {
namespace evioio {

bool evio_library_available() {
#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
  return true;
#else
  return false;
#endif
}

namespace {
constexpr uint32_t kV4Magic = 0xc0da0100u;
constexpr uint32_t kV4HeaderWords = 8;
constexpr uint32_t kDictBit = 0x100u;
constexpr uint32_t kLastBit = 0x200u;
[[noreturn]] void io(const std::string& m) { throw EvioError(EvioErrorCode::Io, m); }
}  // namespace

// ---------------------------------------------------------------------------
// V4FileWriter
// ---------------------------------------------------------------------------
V4FileWriter::V4FileWriter(const std::string& path, uint32_t target_block_words, uint32_t max_events_per_block)
    : path_(path), target_(target_block_words), maxev_(max_events_per_block) {
  if (target_ < 1024) target_ = 1024;
  if (maxev_ == 0) maxev_ = 1;
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) io("cannot open " + path + " for writing");
}

V4FileWriter::~V4FileWriter() {
  try { close(); } catch (...) {}
}

void V4FileWriter::write_dictionary(const std::string& xml) {
  if (nev_ > 0 || blk_count_ > 0) throw EvioError(EvioErrorCode::BadArgument, "dictionary must precede events");
  // char8 bank, tag 0, num 0, padded to 4 bytes (padding count in header bits 15..14)
  const size_t nbytes = xml.size();
  const size_t nw = (nbytes + 3) / 4;
  const uint32_t pad = uint32_t(nw * 4 - nbytes);
  dict_.assign(nw + 2, 0);
  dict_[0] = uint32_t(nw + 1);
  BankHeader h; h.tag = 0; h.dtype = type::kCharStar8; h.num = 0; h.pad = uint8_t(pad);
  dict_[1] = h.word1();
  std::memcpy(dict_.data() + 2, xml.data(), nbytes);
  dict_pending_ = true;
}

void V4FileWriter::write(const uint32_t* ev, size_t nwords) {
  if (closed_) throw EvioError(EvioErrorCode::BadArgument, "write after close");
  if (!ev || nwords < 2 || size_t(ev[0]) + 1 > nwords) throw EvioError(EvioErrorCode::BadLength, "bad event buffer");
  const size_t len = size_t(ev[0]) + 1;
  const size_t cur = kV4HeaderWords + blk_.size() + (dict_pending_ ? dict_.size() : 0);
  if (blk_count_ > 0 && (cur + len > target_ || blk_count_ >= maxev_)) flush_block(false);
  blk_.insert(blk_.end(), ev, ev + len);
  ++blk_count_;
  ++nev_;
}

void V4FileWriter::flush_block(bool last) {
  const bool dict = dict_pending_;
  const size_t payload = blk_.size() + (dict ? dict_.size() : 0);
  uint32_t h[kV4HeaderWords];
  h[0] = uint32_t(kV4HeaderWords + payload);
  h[1] = blk_number_++;
  h[2] = kV4HeaderWords;
  h[3] = blk_count_;            // dictionary is not counted (evio convention)
  h[4] = 0;
  h[5] = 4u | (dict ? kDictBit : 0u) | (last ? kLastBit : 0u);
  h[6] = 0;
  h[7] = kV4Magic;
  if (std::fwrite(h, sizeof(uint32_t), kV4HeaderWords, f_) != kV4HeaderWords) io("write error on " + path_);
  if (dict) {
    if (std::fwrite(dict_.data(), sizeof(uint32_t), dict_.size(), f_) != dict_.size()) io("write error on " + path_);
    dict_pending_ = false;
  }
  if (!blk_.empty() && std::fwrite(blk_.data(), sizeof(uint32_t), blk_.size(), f_) != blk_.size())
    io("write error on " + path_);
  blk_.clear();
  blk_count_ = 0;
}

void V4FileWriter::close() {
  if (closed_ || !f_) return;
  if (blk_count_ > 0 || dict_pending_) flush_block(false);
  flush_block(true);   // empty last block (header only, last-block bit), as evio 4 does
  closed_ = true;
  if (std::fclose(f_) != 0) { f_ = nullptr; io("close error on " + path_); }
  f_ = nullptr;
}

#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
// ---------------------------------------------------------------------------
// EvioFileReader (evio C)
// ---------------------------------------------------------------------------
EvioFileReader::EvioFileReader(const std::string& path) {
  std::string p = path;
  char mode[] = "r";
  const int st = evOpen(&p[0], mode, &handle_);
  if (st != S_SUCCESS) { handle_ = 0; io("evOpen(" + path + "): " + std::string(evPerror(st))); }
  int32_t v = 0;
  char req[] = "v";
  if (evIoctl(handle_, req, &v) == S_SUCCESS) version_ = v;
  char* d = nullptr;
  uint32_t dl = 0;
  if (evGetDictionary(handle_, &d, &dl) == S_SUCCESS && d) {
    dict_.assign(d, dl);
    std::free(d);
  }
}

EvioFileReader::~EvioFileReader() { close(); }

bool EvioFileReader::next() {
  if (!handle_) return false;
  if (buf_) { std::free(buf_); buf_ = nullptr; len_ = 0; }
  uint32_t* b = nullptr;
  uint32_t n = 0;
  const int st = evReadAlloc(handle_, &b, &n);
  if (st == EOF) return false;
  if (st != S_SUCCESS) io("evReadAlloc: " + std::string(evPerror(st)));
  buf_ = b;
  len_ = n;
  ++nread_;
  return true;
}

void EvioFileReader::close() {
  if (buf_) { std::free(buf_); buf_ = nullptr; len_ = 0; }
  if (handle_) { evClose(handle_); handle_ = 0; }
}
#endif

// ---------------------------------------------------------------------------
// EvioFileWriter
// ---------------------------------------------------------------------------
EvioFileWriter::EvioFileWriter(const std::string& path, EvioVersion version) : ver_(version) {
  if (ver_ == EvioVersion::V4) {
    v4_ = new V4FileWriter(path);
    return;
  }
#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
  std::string p = path;
  char mode[] = "w";
  const int st = evOpen(&p[0], mode, &handle_);
  if (st != S_SUCCESS) { handle_ = 0; io("evOpen(" + path + ", w): " + std::string(evPerror(st))); }
#else
  throw EvioError(EvioErrorCode::Unsupported, "EVIO v6 output requires the evio C library");
#endif
}

EvioFileWriter::~EvioFileWriter() {
  try { close(); } catch (...) {}
  delete v4_;
}

void EvioFileWriter::write_dictionary(const std::string& xml) {
  if (v4_) v4_->write_dictionary(xml);
}

void EvioFileWriter::write(const uint32_t* ev, size_t nwords) {
  if (v4_) { v4_->write(ev, nwords); ++nev_; return; }
#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
  if (!handle_) throw EvioError(EvioErrorCode::BadArgument, "write on closed writer");
  if (!ev || nwords < 2 || size_t(ev[0]) + 1 > nwords) throw EvioError(EvioErrorCode::BadLength, "bad event buffer");
  const int st = evWrite(handle_, ev);
  if (st != S_SUCCESS) io("evWrite: " + std::string(evPerror(st)));
  ++nev_;
#else
  (void)ev; (void)nwords;
#endif
}

void EvioFileWriter::close() {
  if (v4_) { v4_->close(); return; }
#if defined(GEMFILTER_HAVE_EVIO) && GEMFILTER_HAVE_EVIO
  if (handle_) {
    const int st = evClose(handle_);
    handle_ = 0;
    if (st != S_SUCCESS) io("evClose: " + std::string(evPerror(st)));
  }
#endif
}

}  // namespace evioio
}  // namespace gemfilter
