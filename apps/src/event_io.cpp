#include "gemfilter/apps/event_io.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern "C" {
#include "evio.h"
}

namespace gemfilter {

namespace {
std::string everr(int st) {
  const char* m = evPerror(st);
  return m ? std::string(m) : ("evio status " + std::to_string(st));
}
}  // namespace

EvioFileSource::EvioFileSource(const std::string& path) : path_(path) {
  int h = -1;
  const int st = evOpen(const_cast<char*>(path.c_str()), const_cast<char*>("r"), &h);
  if (st != S_SUCCESS) throw std::runtime_error("evOpen(" + path + ", r): " + everr(st));
  handle_ = h;
}

EvioFileSource::~EvioFileSource() {
  if (handle_ >= 0) evClose(handle_);
}

bool EvioFileSource::next(std::vector<uint32_t>& buf, size_t& nwords, size_t headroom) {
  uint32_t* p = nullptr;
  uint32_t len = 0;
  // evReadAlloc works for all evio versions the C library reads (incl. < 4).
  const int st = evReadAlloc(handle_, &p, &len);
  if (st == EOF) return false;
  if (st != S_SUCCESS) {
    if (p) std::free(p);
    throw std::runtime_error("evReadAlloc(" + path_ + "): " + everr(st));
  }
  if (buf.size() < len + headroom) buf.resize(len + headroom);
  std::memcpy(buf.data(), p, static_cast<size_t>(len) * sizeof(uint32_t));
  std::free(p);
  nwords = len;
  return true;
}

EvioFileSink::EvioFileSink(const std::string& path) : path_(path) {
  int h = -1;
  const int st = evOpen(const_cast<char*>(path.c_str()), const_cast<char*>("w"), &h);
  if (st != S_SUCCESS) throw std::runtime_error("evOpen(" + path + ", w): " + everr(st));
  handle_ = h;
}

EvioFileSink::~EvioFileSink() {
  try {
    close();
  } catch (...) {
  }
}

void EvioFileSink::write(const uint32_t* ev, size_t nwords) {
  if (handle_ < 0) throw std::runtime_error("EvioFileSink: closed");
  if (nwords < 2 || static_cast<size_t>(ev[0]) + 1 != nwords)
    throw std::runtime_error("EvioFileSink: event length word inconsistent");
  const int st = evWrite(handle_, ev);
  if (st != S_SUCCESS) throw std::runtime_error("evWrite(" + path_ + "): " + everr(st));
}

void EvioFileSink::close() {
  if (handle_ < 0) return;
  const int st = evClose(handle_);
  handle_ = -1;
  if (st != S_SUCCESS) throw std::runtime_error("evClose(" + path_ + "): " + everr(st));
}

bool MemorySource::next(std::vector<uint32_t>& buf, size_t& nwords, size_t headroom) {
  if (i_ >= ev_.size()) return false;
  const auto& e = ev_[i_++];
  if (buf.size() < e.size() + headroom) buf.resize(e.size() + headroom);
  std::copy(e.begin(), e.end(), buf.begin());
  nwords = e.size();
  return true;
}

}  // namespace gemfilter
