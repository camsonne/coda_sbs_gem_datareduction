// gemfilter evioio: type-aware EVIO byte-order conversion for raw event buffers (ET path).
// Equivalent in purpose to evio C `evioswap()`; implemented here so that the in-memory path
// has no evio dependency. Composite data (type 0xf) is not supported (throws Unsupported).
#pragma once
#include <cstddef>
#include <cstdint>

namespace gemfilter {
namespace evioio {

// Convert one EVIO bank (event) in place.
//   to_host = true : buffer is in foreign byte order, convert to host order
//   to_host = false: buffer is in host order, convert to foreign order
// `nwords` is the buffer capacity; the bank's own length is used (and checked against it).
void swap_event(uint32_t* buf, size_t nwords, bool to_host);

inline uint32_t bswap32(uint32_t x) {
  return (x >> 24) | ((x >> 8) & 0x0000FF00u) | ((x << 8) & 0x00FF0000u) | (x << 24);
}

}  // namespace evioio
}  // namespace gemfilter
