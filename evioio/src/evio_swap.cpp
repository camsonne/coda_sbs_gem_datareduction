// gemfilter evioio: type-aware EVIO byte swapping.
#include "gemfilter/evioio/evio_swap.h"

#include <cstring>
#include <string>

#include "gemfilter/evioio/evio_event.h"

namespace gemfilter {
namespace evioio {

namespace {

void swap_data(uint32_t* p, size_t n, uint8_t dt) {
  switch (dt) {
    case type::kShort16: case type::kUShort16: {
      auto* b = reinterpret_cast<unsigned char*>(p);
      for (size_t i = 0; i < 2 * n; ++i) std::swap(b[2 * i], b[2 * i + 1]);
      break;
    }
    case type::kDouble64: case type::kLong64: case type::kULong64: {
      auto* b = reinterpret_cast<unsigned char*>(p);
      for (size_t i = 0; i < n / 2; ++i)
        for (int k = 0; k < 4; ++k) std::swap(b[8 * i + k], b[8 * i + 7 - k]);
      break;
    }
    case type::kCharStar8: case type::kChar8: case type::kUChar8:
      break;   // bytes: no swap
    case type::kComposite:
      throw EvioError(EvioErrorCode::Unsupported, "swap of composite data not supported");
    default:   // 32-bit types (unknown32, uint32, float, int32)
      for (size_t i = 0; i < n; ++i) p[i] = bswap32(p[i]);
      break;
  }
}

// kind: 0 = bank (2-word header), 1 = segment, 2 = tagsegment (1-word headers)
size_t swap_struct(uint32_t* p, size_t avail, int kind, bool to_host);

void swap_children(uint32_t* p, size_t n, uint8_t dt, bool to_host) {
  int kind = -1;
  if (type::is_bank_container(dt)) kind = 0;
  else if (type::is_segment_container(dt)) kind = 1;
  else if (dt == type::kTagSegment) kind = 2;
  if (kind < 0) { swap_data(p, n, dt); return; }
  size_t i = 0;
  while (i < n) i += swap_struct(p + i, n - i, kind, to_host);
  if (i != n) throw EvioError(EvioErrorCode::BadStructure, "children do not tile parent while swapping");
}

size_t swap_struct(uint32_t* p, size_t avail, int kind, bool to_host) {
  const size_t hw = kind == 0 ? 2 : 1;
  if (avail < hw) throw EvioError(EvioErrorCode::BadLength, "truncated header while swapping");
  if (to_host) for (size_t k = 0; k < hw; ++k) p[k] = bswap32(p[k]);
  size_t len;      // words following the length-carrying word
  uint8_t dt;
  size_t data_off;
  if (kind == 0) { len = p[0]; dt = uint8_t((p[1] >> 8) & 0x3Fu); data_off = 2; }
  else if (kind == 1) { len = p[0] & 0xFFFFu; dt = uint8_t((p[0] >> 16) & 0x3Fu); data_off = 1; }
  else { len = p[0] & 0xFFFFu; dt = uint8_t((p[0] >> 16) & 0xFu); data_off = 1; }
  const size_t total = (kind == 0) ? len + 1 : len + 1;
  if (total > avail || total < data_off) throw EvioError(EvioErrorCode::BadLength, "structure overruns parent while swapping");
  swap_children(p + data_off, total - data_off, dt, to_host);
  if (!to_host) for (size_t k = 0; k < hw; ++k) p[k] = bswap32(p[k]);
  return total;
}

}  // namespace

void swap_event(uint32_t* buf, size_t nwords, bool to_host) {
  if (!buf || nwords < 2) throw EvioError(EvioErrorCode::BadLength, "buffer shorter than a bank header");
  const size_t len = (to_host ? bswap32(buf[0]) : buf[0]) + size_t(1);
  if (len > nwords) throw EvioError(EvioErrorCode::BadLength, "event length " + std::to_string(len) + " exceeds buffer");
  swap_struct(buf, len, 0, to_host);
}

}  // namespace evioio
}  // namespace gemfilter
