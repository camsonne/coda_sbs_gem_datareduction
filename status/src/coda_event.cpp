#include "gemfilter/status/coda_event.h"

#include <cstring>

namespace gemfilter {
namespace coda {

const char* to_string(EventClass c) {
  switch (c) {
    case EventClass::Physics: return "physics";
    case EventClass::Control: return "control";
    case EventClass::User: return "user";
    case EventClass::OtherReserved: return "other-reserved";
    case EventClass::Malformed: return "malformed";
  }
  return "?";
}

BankHeader parse_bank_header(const uint32_t* p) {
  BankHeader h;
  h.length = p[0];
  h.tag = static_cast<uint16_t>(p[1] >> 16);
  h.pad = static_cast<uint8_t>((p[1] >> 14) & 0x3);
  h.type = static_cast<uint8_t>((p[1] >> 8) & 0x3f);
  h.num = static_cast<uint8_t>(p[1] & 0xff);
  return h;
}

uint32_t make_bank_word1(uint16_t tag, uint8_t type, uint8_t num, uint8_t pad) {
  return (static_cast<uint32_t>(tag) << 16) | (static_cast<uint32_t>(pad & 0x3) << 14) |
         (static_cast<uint32_t>(type & 0x3f) << 8) | num;
}

EventClass classify(const uint32_t* buf, size_t nwords) {
  if (!buf || nwords < 2) return EventClass::Malformed;
  const BankHeader h = parse_bank_header(buf);
  if (h.length < 1 || static_cast<size_t>(h.length) + 1 > nwords) return EventClass::Malformed;
  const uint16_t t = h.tag;
  if (t < 0xFF00) return EventClass::User;
  if (t >= 0xFFD0 && t <= 0xFFDF) return EventClass::Control;
  if (t == kTagPhysics || t == kTagPhysicsSync || t == kTagPhysicsStream || t == kTagPhysicsSync2) {
    if (!is_bank_of_banks(h.type)) return EventClass::Malformed;
    return EventClass::Physics;
  }
  return EventClass::OtherReserved;
}

bool list_children(const uint32_t* buf, size_t nwords, size_t parent, std::vector<BankRef>& out) {
  out.clear();
  if (parent + 1 >= nwords) return false;
  const size_t end = parent + 1 + buf[parent];  // one past last word of parent
  if (end > nwords) return false;
  size_t pos = parent + 2;
  while (pos < end) {
    if (pos + 1 >= end) return false;
    BankRef r;
    r.offset = pos;
    r.hdr = parse_bank_header(buf + pos);
    const size_t next = pos + 1 + r.hdr.length;
    if (r.hdr.length < 1 || next > end) return false;
    out.push_back(r);
    pos = next;
  }
  return pos == end;
}

bool parse_physics(const uint32_t* buf, size_t nwords, PhysicsLayout& out) {
  out = PhysicsLayout{};
  if (classify(buf, nwords) != EventClass::Physics) return false;
  out.top = parse_bank_header(buf);
  out.block_level = out.top.num;
  std::vector<BankRef> kids;
  if (!list_children(buf, nwords, 0, kids)) return false;
  size_t i0 = 0;
  if (!kids.empty() && kids[0].hdr.tag >= kTrigBankMin && kids[0].hdr.tag <= kTrigBankMax) {
    out.trigger = kids[0];
    i0 = 1;
    // Segment 1 of the trigger bank: 64-bit event number (Podd trigBankDecode).
    const size_t seg = kids[0].offset + 2;
    const size_t tb_end = kids[0].offset + 1 + kids[0].hdr.length;
    if (seg < tb_end) {
      const uint32_t sw = buf[seg];
      const uint32_t seglen = sw & 0xffff;
      const uint32_t segtype = (sw >> 16) & 0x3f;
      if (seglen >= 2 && seg + seglen < tb_end && (segtype == 0x0a || segtype == 0x09)) {
        uint64_t ev = 0;
        std::memcpy(&ev, buf + seg + 1, sizeof(ev));  // native order, as Podd does
        out.first_event_number = ev;
      }
    }
  }
  for (size_t i = i0; i < kids.size(); ++i) out.rocs.push_back(kids[i]);
  return true;
}

bool roc_has_bank(const uint32_t* buf, size_t nwords, const BankRef& roc, uint16_t bank_tag) {
  if (!is_bank_of_banks(roc.hdr.type)) return false;
  std::vector<BankRef> kids;
  if (!list_children(buf, nwords, roc.offset, kids)) return false;
  for (const auto& k : kids)
    if (k.hdr.tag == bank_tag) return true;
  return false;
}

}  // namespace coda
}  // namespace gemfilter
