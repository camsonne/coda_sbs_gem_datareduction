// SYNTHETIC CODA3-like event builder (tests only).
#include "gemfilter/evioio/synthetic_event.h"

#include <cstring>

#include "gemfilter/evioio/evio_event.h"

namespace gemfilter {
namespace evioio {
namespace synth {

namespace {
uint32_t seg_header(uint8_t tag, uint8_t dtype, uint8_t pad, uint32_t len) {
  return (uint32_t(tag) << 24) | (uint32_t(pad & 3u) << 22) | (uint32_t(dtype & 0x3Fu) << 16) | (len & 0xFFFFu);
}
void put64(std::vector<uint32_t>& v, uint64_t x) {
  uint32_t w[2];
  std::memcpy(w, &x, 8);   // host-order 64-bit value as stored by evio after swapping
  v.push_back(w[0]);
  v.push_back(w[1]);
}
}  // namespace

std::vector<uint32_t> make_physics_event(const PhysicsEventSpec& s) {
  const uint32_t bl = s.block_level;
  // ---- trigger bank ----
  std::vector<uint32_t> tb;
  const uint16_t ttag = s.timestamps ? 0xFF21 : 0xFF20;
  tb.push_back(0);   // length placeholder
  BankHeader th; th.tag = ttag; th.dtype = type::kSegment2; th.num = uint8_t(s.rocs.size());
  tb.push_back(th.word1());
  // seg 1: event number (+ timestamps)
  const uint32_t l1 = 2u * (1u + (s.timestamps ? bl : 0u));
  tb.push_back(seg_header(0x01 /*EB id*/, type::kULong64, 0, l1));
  put64(tb, s.first_event_number);
  if (s.timestamps)
    for (uint32_t i = 0; i < bl; ++i) put64(tb, 1000000ull + 250ull * (s.first_event_number + i));
  // seg 2: event types (uint16), padded
  const uint32_t l2 = (bl - 1u) / 2u + 1u;
  tb.push_back(seg_header(0x01, type::kUShort16, (bl % 2) ? 2 : 0, l2));
  std::vector<uint16_t> et(2 * l2, 0);
  for (uint32_t i = 0; i < bl; ++i) et[i] = uint16_t(1 + (i % 3));
  const size_t base = tb.size();
  tb.resize(base + l2);
  std::memcpy(tb.data() + base, et.data(), l2 * 4);
  // per-ROC segments: {uint64 ts [+ uint32 trigger bits for TS ROC]} x bl, as uint32 data
  for (const auto& r : s.rocs) {
    const bool ts = (r.roc_id == s.tsroc);
    const uint32_t per = ts ? 3u : 2u;
    tb.push_back(seg_header(uint8_t(r.roc_id), type::kUInt32, 0, per * bl));
    for (uint32_t i = 0; i < bl; ++i) {
      put64(tb, 1000000ull + 250ull * (s.first_event_number + i) + r.roc_id);
      if (ts) tb.push_back(0x10u + i);
    }
  }
  tb[0] = uint32_t(tb.size() - 1);

  // ---- event ----
  std::vector<uint32_t> ev;
  ev.push_back(0);
  BankHeader eh; eh.tag = s.event_tag; eh.dtype = type::kBank2; eh.num = uint8_t(bl);
  ev.push_back(eh.word1());
  ev.insert(ev.end(), tb.begin(), tb.end());
  for (const auto& r : s.rocs) {
    const size_t rs = ev.size();
    ev.push_back(0);
    BankHeader rh; rh.tag = r.roc_id; rh.dtype = type::kBank2; rh.num = uint8_t(bl);
    ev.push_back(rh.word1());
    for (const auto& b : r.banks) {
      ev.push_back(uint32_t(b.payload.size() + 1));
      BankHeader bh; bh.tag = b.tag; bh.dtype = b.dtype; bh.num = b.num;
      ev.push_back(bh.word1());
      ev.insert(ev.end(), b.payload.begin(), b.payload.end());
    }
    ev[rs] = uint32_t(ev.size() - rs - 1);
  }
  ev[0] = uint32_t(ev.size() - 1);
  return ev;
}

std::vector<uint32_t> make_control_event(uint16_t tag, uint32_t run, uint32_t run_type, uint32_t time) {
  BankHeader h; h.tag = tag; h.dtype = type::kUInt32; h.num = 0;
  return {4u, h.word1(), time, run, run_type};
}

}  // namespace synth
}  // namespace evioio
}  // namespace gemfilter
