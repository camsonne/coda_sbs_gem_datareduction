// gemfilter evioio: in-memory CODA3 event parsing and rewriting.
#include "gemfilter/evioio/evio_event.h"

#include <algorithm>
#include <cstring>

namespace gemfilter {
namespace evioio {

const char* to_string(EvioErrorCode c) {
  switch (c) {
    case EvioErrorCode::BadLength: return "BadLength";
    case EvioErrorCode::BadStructure: return "BadStructure";
    case EvioErrorCode::BadTriggerBank: return "BadTriggerBank";
    case EvioErrorCode::BadArgument: return "BadArgument";
    case EvioErrorCode::Unsupported: return "Unsupported";
    case EvioErrorCode::Io: return "Io";
  }
  return "Unknown";
}

EvioError::EvioError(EvioErrorCode c, const std::string& msg)
    : std::runtime_error(std::string("gemfilter::evioio ") + to_string(c) + ": " + msg), code_(c) {}

bool is_physics_tag(uint16_t tag) { return (tag & 0xFFF0u) == 0xFF50u || (tag & 0xFFF0u) == 0xFF70u; }
bool is_control_tag(uint16_t tag) { return (tag & 0xFFF0u) == 0xFFD0u; }
bool is_trigger_bank_tag(uint16_t tag) { return tag >= 0xFF10u && tag <= 0xFF4Fu; }

namespace {
[[noreturn]] void bad(EvioErrorCode c, const std::string& m) { throw EvioError(c, m); }

uint64_t rd64(const uint32_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

// Decode the built trigger bank (bank of segments) at buf[off].
void parse_trigger(const uint32_t* buf, uint32_t off, uint8_t block_level, TriggerBankInfo& t) {
  const BankHeader h = BankHeader::decode(buf + off);
  t.present = true;
  t.tag = h.tag;
  t.nrocs = h.num;
  t.offset = off;
  t.total_words = h.length + 1;
  if (h.tag < 0xFF20u || h.tag > 0xFF27u) return;   // not a built trigger bank: keep opaque
  if (!type::is_segment_container(h.dtype)) bad(EvioErrorCode::BadTriggerBank, "trigger bank is not a bank of segments");
  if (block_level == 0) bad(EvioErrorCode::BadTriggerBank, "block level 0");
  t.has_timestamps = (h.tag & 1u) != 0;
  t.has_run_info = (h.tag & 2u) != 0;
  uint32_t p = off + 2;
  const uint32_t end = off + 1 + h.length;
  int iseg = 0;
  while (p < end) {
    const uint32_t w = buf[p];
    const uint8_t stag = uint8_t(w >> 24);
    const uint8_t sdt = uint8_t((w >> 16) & 0x3Fu);
    const uint32_t slen = w & 0xFFFFu;
    if (p + 1 + slen > end) bad(EvioErrorCode::BadTriggerBank, "segment overruns trigger bank");
    const uint32_t* d = buf + p + 1;
    if (iseg == 0) {
      const uint32_t need = 2u * (1u + (t.has_timestamps ? block_level : 0u) + (t.has_run_info ? 1u : 0u));
      if (sdt != type::kULong64 && sdt != type::kLong64) bad(EvioErrorCode::BadTriggerBank, "segment 1 not 64-bit");
      if (slen != need) bad(EvioErrorCode::BadTriggerBank, "segment 1 length " + std::to_string(slen) + " != " + std::to_string(need));
      t.first_event_number = rd64(d);
      if (t.has_timestamps) {
        t.timestamps.resize(block_level);
        for (uint32_t i = 0; i < block_level; ++i) t.timestamps[i] = rd64(d + 2 + 2 * i);
      }
      if (t.has_run_info) t.run_info = rd64(d + 2 + (t.has_timestamps ? 2u * block_level : 0u));
    } else if (iseg == 1) {
      if (sdt != type::kUShort16 && sdt != type::kShort16) bad(EvioErrorCode::BadTriggerBank, "segment 2 not 16-bit");
      if (slen != (block_level - 1u) / 2u + 1u) bad(EvioErrorCode::BadTriggerBank, "segment 2 length");
      t.event_types.resize(block_level);
      std::memcpy(t.event_types.data(), d, block_level * sizeof(uint16_t));
    } else {
      t.roc_segment_ids.push_back(stag);
    }
    p += 1 + slen;
    ++iseg;
  }
  if (p != end) bad(EvioErrorCode::BadTriggerBank, "segments do not tile the trigger bank");
  if (iseg < 2) bad(EvioErrorCode::BadTriggerBank, "fewer than 2 segments");
}
}  // namespace

EventView EventView::parse(const uint32_t* buf, size_t nwords) {
  if (!buf || nwords < 2) bad(EvioErrorCode::BadLength, "buffer shorter than a bank header");
  EventView v;
  v.buf_ = buf;
  v.hdr_ = BankHeader::decode(buf);
  if (v.hdr_.length == 0 || size_t(v.hdr_.length) + 1 > nwords)
    bad(EvioErrorCode::BadLength, "event length " + std::to_string(v.hdr_.length) + " exceeds buffer " + std::to_string(nwords));
  v.nwords_ = size_t(v.hdr_.length) + 1;
  if (!type::is_bank_container(v.hdr_.dtype)) return v;   // leaf event (e.g. control event data)

  const uint32_t end = uint32_t(v.nwords_);
  uint32_t p = 2;
  const bool phys = is_physics_tag(v.hdr_.tag);
  bool first = true;
  while (p < end) {
    if (p + 1 >= end) bad(EvioErrorCode::BadStructure, "truncated child bank header at word " + std::to_string(p));
    const BankHeader ch = BankHeader::decode(buf + p);
    if (ch.length == 0 || p + 1 + ch.length > end)
      bad(EvioErrorCode::BadLength, "child bank at word " + std::to_string(p) + " overruns event");
    Child c{p, ch.length + 1, -1};
    if (phys && first && is_trigger_bank_tag(ch.tag)) {
      parse_trigger(buf, p, v.hdr_.num, v.trig_);
    } else if (phys) {
      RocBankRef r;
      r.roc_id = uint16_t(ch.tag & 0x0FFFu);
      r.tag = ch.tag;
      r.num = ch.num;
      r.dtype = ch.dtype;
      r.offset = p;
      r.total_words = ch.length + 1;
      r.bank_begin = r.bank_end = uint32_t(v.banks_.size());
      const uint32_t rend = p + 1 + ch.length;
      if (type::is_bank_container(ch.dtype)) {
        uint32_t q = p + 2;
        while (q < rend) {
          if (q + 1 >= rend) bad(EvioErrorCode::BadStructure, "truncated data bank header in ROC " + std::to_string(r.roc_id));
          const BankHeader d = BankHeader::decode(buf + q);
          if (d.length == 0 || q + 1 + d.length > rend)
            bad(EvioErrorCode::BadLength, "data bank at word " + std::to_string(q) + " overruns ROC " + std::to_string(r.roc_id));
          DataBankRef b;
          b.roc_index = uint32_t(v.rocs_.size());
          b.roc_id = r.roc_id;
          b.tag = d.tag;
          b.num = d.num;
          b.dtype = d.dtype;
          b.pad = d.pad;
          b.offset = q;
          b.payload_offset = q + 2;
          b.payload_len = d.length - 1;
          b.payload = buf + q + 2;
          v.banks_.push_back(b);
          q += 1 + d.length;
        }
        r.bank_end = uint32_t(v.banks_.size());
      }
      c.roc = int(v.rocs_.size());
      v.rocs_.push_back(r);
    }
    v.children_.push_back(c);
    p += 1 + ch.length;
    first = false;
  }
  return v;
}

std::vector<size_t> EventView::find_banks(int roc_id, uint16_t tag) const {
  std::vector<size_t> r;
  for (size_t i = 0; i < banks_.size(); ++i)
    if (banks_[i].tag == tag && (roc_id < 0 || banks_[i].roc_id == uint16_t(roc_id))) r.push_back(i);
  return r;
}

int EventView::find_roc(uint16_t roc_id) const {
  for (size_t i = 0; i < rocs_.size(); ++i)
    if (rocs_[i].roc_id == roc_id) return int(i);
  return -1;
}

// ---------------------------------------------------------------------------
// EventEditor
// ---------------------------------------------------------------------------
EventEditor::EventEditor(const EventView& view) : v_(view) {
  if (!view.data()) bad(EvioErrorCode::BadArgument, "EventEditor on empty view");
}

void EventEditor::replace_payload(size_t bank_index, const uint32_t* p, size_t n) {
  replace_payload(bank_index, std::vector<uint32_t>(p, p + n));
}

void EventEditor::replace_payload(size_t bank_index, std::vector<uint32_t>&& w) {
  if (bank_index >= v_.banks().size()) bad(EvioErrorCode::BadArgument, "bank index out of range");
  for (auto& r : repl_)
    if (r.bank == bank_index) { r.words = std::move(w); return; }
  repl_.push_back({uint32_t(bank_index), std::move(w)});
}

void EventEditor::append_bank_in_roc(size_t roc_index, uint16_t tag, uint8_t num, const uint32_t* p, size_t n,
                                     uint8_t dtype) {
  if (roc_index >= v_.rocs().size()) bad(EvioErrorCode::BadArgument, "ROC index out of range");
  if (!type::is_bank_container(v_.rocs()[roc_index].dtype))
    bad(EvioErrorCode::BadArgument, "ROC bank is not a bank of banks");
  roc_app_.push_back({uint32_t(roc_index), tag, num, dtype, std::vector<uint32_t>(p, p + n)});
}

void EventEditor::append_top_level_bank(uint16_t tag, uint8_t num, const uint32_t* p, size_t n, uint8_t dtype) {
  if (!type::is_bank_container(v_.header().dtype)) bad(EvioErrorCode::BadArgument, "event is not a bank of banks");
  top_app_.push_back({0, tag, num, dtype, std::vector<uint32_t>(p, p + n)});
}

namespace {
struct VecSink {
  std::vector<uint32_t>& o;
  void put(uint32_t w) { o.push_back(w); }
  void put(const uint32_t* p, size_t n) { o.insert(o.end(), p, p + n); }
};
struct RawSink {
  uint32_t* d;
  size_t i = 0;
  void put(uint32_t w) { d[i++] = w; }
  void put(const uint32_t* p, size_t n) { std::memcpy(d + i, p, n * sizeof(uint32_t)); i += n; }
};
}  // namespace

template <class Sink>
void EventEditor::emit(Sink& s) const {
  const uint32_t* buf = v_.data();
  const auto& banks = v_.banks();
  const auto& rocs = v_.rocs();
  std::vector<int> ridx(banks.size(), -1);
  for (size_t i = 0; i < repl_.size(); ++i) ridx[repl_[i].bank] = int(i);
  std::vector<uint32_t> roc_new(rocs.size(), 0);
  std::vector<uint8_t> roc_mod(rocs.size(), 0);
  for (size_t r = 0; r < rocs.size(); ++r) {
    uint64_t len = rocs[r].total_words;
    for (uint32_t b = rocs[r].bank_begin; b < rocs[r].bank_end; ++b)
      if (ridx[b] >= 0) { len = len - banks[b].payload_len + repl_[size_t(ridx[b])].words.size(); roc_mod[r] = 1; }
    for (const auto& a : roc_app_)
      if (a.parent == r) { len += 2 + a.words.size(); roc_mod[r] = 1; }
    roc_new[r] = uint32_t(len);
  }
  uint64_t total = 2;
  if (v_.children_.empty()) total = v_.nwords();
  for (const auto& c : v_.children_) total += c.roc >= 0 ? roc_new[size_t(c.roc)] : c.total_words;
  for (const auto& a : top_app_) total += 2 + a.words.size();

  s.put(uint32_t(total - 1));
  s.put(buf[1]);
  if (v_.children_.empty()) {
    s.put(buf + 2, v_.nwords() - 2);
  }
  for (const auto& c : v_.children_) {
    if (c.roc < 0 || !roc_mod[size_t(c.roc)]) { s.put(buf + c.offset, c.total_words); continue; }
    const RocBankRef& r = rocs[size_t(c.roc)];
    s.put(roc_new[size_t(c.roc)] - 1);
    s.put(buf[r.offset + 1]);
    for (uint32_t b = r.bank_begin; b < r.bank_end; ++b) {
      const DataBankRef& d = banks[b];
      if (ridx[b] < 0) { s.put(buf + d.offset, d.payload_len + 2); continue; }
      const auto& w = repl_[size_t(ridx[b])].words;
      s.put(uint32_t(w.size() + 1));
      s.put(buf[d.offset + 1] & ~(0x3u << 14));   // clear padding: payload is uint32
      s.put(w.data(), w.size());
    }
    for (const auto& a : roc_app_) {
      if (a.parent != uint32_t(c.roc)) continue;
      BankHeader h; h.tag = a.tag; h.dtype = a.dtype; h.num = a.num;
      s.put(uint32_t(a.words.size() + 1));
      s.put(h.word1());
      s.put(a.words.data(), a.words.size());
    }
  }
  for (const auto& a : top_app_) {
    BankHeader h; h.tag = a.tag; h.dtype = a.dtype; h.num = a.num;
    s.put(uint32_t(a.words.size() + 1));
    s.put(h.word1());
    s.put(a.words.data(), a.words.size());
  }
}

size_t EventEditor::built_words() const {
  struct Count { size_t n = 0; void put(uint32_t) { ++n; } void put(const uint32_t*, size_t k) { n += k; } } c;
  emit(c);
  return c.n;
}

void EventEditor::build_into(std::vector<uint32_t>& out) const {
  out.clear();
  out.reserve(v_.nwords() + 64);
  VecSink s{out};
  emit(s);
}

size_t EventEditor::build_into(uint32_t* dst, size_t capacity) const {
  const size_t n = built_words();
  if (n > capacity) bad(EvioErrorCode::BadArgument, "destination too small: need " + std::to_string(n));
  const uint32_t* a = v_.data();
  if (dst < a + v_.nwords() && a < dst + n) bad(EvioErrorCode::BadArgument, "destination overlaps source buffer");
  RawSink s{dst};
  emit(s);
  return s.i;
}

std::vector<uint32_t> make_bank(uint16_t tag, uint8_t dtype, uint8_t num, const std::vector<uint32_t>& payload) {
  BankHeader h; h.tag = tag; h.dtype = dtype; h.num = num;
  std::vector<uint32_t> v;
  v.reserve(payload.size() + 2);
  v.push_back(uint32_t(payload.size() + 1));
  v.push_back(h.word1());
  v.insert(v.end(), payload.begin(), payload.end());
  return v;
}

}  // namespace evioio
}  // namespace gemfilter
