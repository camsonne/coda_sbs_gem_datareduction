#include "gemfilter/et/evio_framing.h"

#include <cstring>

namespace gemfilter {
namespace et {

// Word positions / masks from evio 6.x src/libsrc/evio.c.
namespace {
constexpr size_t HD_BLKSIZ = 0, HD_BLKNUM = 1, HD_HDSIZ = 2, HD_COUNT = 3, HD_INDEXLEN = 4,
                 HD_VER = 5, HD_USERHDRLEN = 6, HD_MAGIC = 7, HD_UNCOMPLEN = 8, HD_COMPLEN = 9;
constexpr uint32_t VERSION_MASK = 0xff, DICT_MASK = 0x100, LAST_MASK = 0x200;
constexpr size_t V4_HDR = 8, V6_HDR = 14;

inline uint32_t type_bits(uint32_t w5) { return (w5 >> 10) & 0xf; }  // UNVERIFIED for v6 (see header)
inline uint32_t pad1(uint32_t w5) { return (w5 >> 20) & 0x3; }

// Some producers size ET events generously; tolerate all-zero padding after the last container.
bool zero_tail(const uint32_t* in, size_t nin, size_t pos) {
  if (pos >= nin || in[pos] != 0) return false;
  for (size_t i = pos; i < nin; ++i)
    if (in[i] != 0) return false;
  return true;
}

struct Container {
  Framing f = Framing::Unsupported;
  size_t start = 0, len = 0;        // words
  size_t data_start = 0, data_end = 0;  // event region (words, absolute)
  size_t index_start = 0;           // v6 index array (words, absolute)
  uint32_t count = 0;
  bool dict = false;
};

bool parse_container(const uint32_t* in, size_t nin, size_t pos, Container& c, std::string& why) {
  if (pos + V4_HDR > nin) { why = "truncated header"; return false; }
  const uint32_t* h = in + pos;
  if (h[HD_MAGIC] == kEvioMagicSwapped) { why = "byte-swapped buffer"; return false; }
  if (h[HD_MAGIC] != kEvioMagic) { why = "bad magic"; return false; }
  const uint32_t ver = h[HD_VER] & VERSION_MASK;
  c.start = pos;
  c.len = h[HD_BLKSIZ];
  c.count = h[HD_COUNT];
  c.dict = (h[HD_VER] & DICT_MASK) != 0;
  if (c.len < h[HD_HDSIZ] || pos + c.len > nin) { why = "container length exceeds buffer"; return false; }
  if (ver == 4) {
    if (h[HD_HDSIZ] != V4_HDR) { why = "v4 header size != 8"; return false; }
    c.f = Framing::EvioV4;
    c.data_start = pos + V4_HDR;
    c.data_end = pos + c.len;
    return true;
  }
  if (ver == 6) {
    if (h[HD_HDSIZ] != V6_HDR || pos + V6_HDR > nin) { why = "v6 header size != 14"; return false; }
    if ((h[HD_COMPLEN] >> 28) != 0) { why = "compressed v6 record"; return false; }
    if (h[HD_INDEXLEN] % 4 != 0) { why = "v6 index length not multiple of 4"; return false; }
    if (h[HD_INDEXLEN] != 0 && h[HD_INDEXLEN] / 4 != c.count) { why = "v6 index/count mismatch"; return false; }
    c.f = Framing::EvioV6;
    c.index_start = pos + V6_HDR;
    const size_t user_words = (h[HD_USERHDRLEN] + pad1(h[HD_VER]) + 3) / 4;
    c.data_start = c.index_start + h[HD_INDEXLEN] / 4 + user_words;
    c.data_end = pos + c.len;
    if (c.data_start > c.data_end) { why = "v6 header regions exceed record"; return false; }
    return true;
  }
  why = "unsupported evio version " + std::to_string(ver);
  return false;
}

// Event spans inside a container: (offset, words).  v6 with index uses index lengths.
bool event_spans(const uint32_t* in, const Container& c, std::vector<std::pair<size_t, size_t>>& ev,
                 std::string& why) {
  ev.clear();
  size_t p = c.data_start;
  if (c.f == Framing::EvioV6 && in[c.start + HD_INDEXLEN] != 0) {
    for (uint32_t i = 0; i < c.count; ++i) {
      const uint32_t bytes = in[c.index_start + i];
      if (bytes % 4 != 0 || bytes < 8) { why = "bad v6 index entry"; return false; }
      const size_t w = bytes / 4;
      if (p + w > c.data_end || in[p] + 1 != w) { why = "v6 index disagrees with bank length"; return false; }
      ev.emplace_back(p, w);
      p += w;
    }
    // remaining words (data padding) must be < 1 word; events are word aligned so p==end
    if (p != c.data_end) { why = "v6 trailing data in record"; return false; }
    return true;
  }
  while (p < c.data_end) {
    const size_t w = static_cast<size_t>(in[p]) + 1;
    if (w < 2 || p + w > c.data_end) { why = "event length exceeds container"; return false; }
    ev.emplace_back(p, w);
    p += w;
  }
  return true;
}
}  // namespace

const char* to_string(Framing f) {
  switch (f) {
    case Framing::EvioV4: return "evio4";
    case Framing::EvioV6: return "evio6";
    default: return "unsupported";
  }
}

Framing inspect_buffer(const uint32_t* in, size_t nwords, BufferInfo& info) {
  info = BufferInfo{};
  size_t pos = 0;
  std::vector<std::pair<size_t, size_t>> ev;
  while (pos < nwords) {
    if (info.ncontainers > 0 && zero_tail(in, nwords, pos)) break;
    Container c;
    if (!parse_container(in, nwords, pos, c, info.why) || !event_spans(in, c, ev, info.why)) {
      info.framing = Framing::Unsupported;
      return info.framing;
    }
    if (info.ncontainers == 0) {
      info.framing = c.f;
      info.type_bits = type_bits(in[pos + HD_VER]);
      info.has_dictionary = c.dict;
    } else if (c.f != info.framing) {
      info.why = "mixed evio versions";
      info.framing = Framing::Unsupported;
      return info.framing;
    }
    info.ncontainers++;
    size_t n = ev.size();
    if (c.f == Framing::EvioV4 && c.dict && n > 0) --n;  // dictionary bank is not an event
    info.nevents += static_cast<uint32_t>(n);
    if (c.len == 0) break;
    pos += c.len;
  }
  if (info.ncontainers == 0) { info.why = "empty buffer"; info.framing = Framing::Unsupported; }
  return info.framing;
}

RewriteResult rewrite_buffer(const uint32_t* in, size_t nin, uint32_t* out, size_t cap,
                             const EventCallback& cb) {
  RewriteResult r;
  if (inspect_buffer(in, nin, r.info) == Framing::Unsupported) {
    r.error = r.info.why;
    return r;
  }
  size_t pos = 0, o = 0;
  std::vector<std::pair<size_t, size_t>> ev;
  std::string why;
  while (pos < nin) {
    if (pos > 0 && zero_tail(in, nin, pos)) break;
    Container c;
    parse_container(in, nin, pos, c, why);
    event_spans(in, c, ev, why);
    const size_t hdr_words = c.data_start - c.start;  // header + index + user header
    if (o + hdr_words > cap) { r.error = "output capacity (headers)"; return r; }
    const size_t out_c = o;
    std::memcpy(out + o, in + c.start, hdr_words * sizeof(uint32_t));
    o += hdr_words;
    int64_t delta_words = 0;
    for (size_t i = 0; i < ev.size(); ++i) {
      const auto [off, w] = ev[i];
      if (o + w > cap) { r.error = "output capacity (event)"; return r; }
      std::memcpy(out + o, in + off, w * sizeof(uint32_t));
      size_t nw = w;
      const bool is_dict = (c.f == Framing::EvioV4 && c.dict && i == 0);
      if (!is_dict) {
        if (cb(out + o, nw, cap - o)) r.changed = true;
        if (nw < 2 || nw > cap - o || static_cast<size_t>(out[o]) + 1 != nw) {
          r.error = "callback returned invalid event";
          return r;
        }
        r.nevents++;
      }
      if (c.f == Framing::EvioV6 && in[c.start + HD_INDEXLEN] != 0)
        out[out_c + V6_HDR + i] = static_cast<uint32_t>(nw * 4);
      delta_words += static_cast<int64_t>(nw) - static_cast<int64_t>(w);
      o += nw;
    }
    out[out_c + HD_BLKSIZ] = static_cast<uint32_t>(static_cast<int64_t>(c.len) + delta_words);
    if (c.f == Framing::EvioV6)
      out[out_c + HD_UNCOMPLEN] =
          static_cast<uint32_t>(static_cast<int64_t>(in[c.start + HD_UNCOMPLEN]) + 4 * delta_words);
    if (c.len == 0) break;
    pos += c.len;
  }
  r.out_words = o;
  r.ok = true;
  return r;
}

std::vector<uint32_t> build_buffer(Framing f, const std::vector<std::vector<uint32_t>>& events,
                                   uint32_t container_number, BufferEventType type, bool last) {
  std::vector<uint32_t> b;
  size_t evwords = 0;
  for (const auto& e : events) evwords += e.size();
  const uint32_t w5_bits = (static_cast<uint32_t>(type) & 0xf) << 10 | (last ? LAST_MASK : 0u);
  if (f == Framing::EvioV4) {
    b.resize(V4_HDR);
    b[HD_BLKSIZ] = static_cast<uint32_t>(V4_HDR + evwords);
    b[HD_BLKNUM] = container_number;
    b[HD_HDSIZ] = V4_HDR;
    b[HD_COUNT] = static_cast<uint32_t>(events.size());
    b[4] = 0;
    b[HD_VER] = 4 | w5_bits;
    b[6] = 0;
    b[HD_MAGIC] = kEvioMagic;
  } else if (f == Framing::EvioV6) {
    b.resize(V6_HDR + events.size());
    b[HD_BLKSIZ] = static_cast<uint32_t>(V6_HDR + events.size() + evwords);
    b[HD_BLKNUM] = container_number;
    b[HD_HDSIZ] = V6_HDR;
    b[HD_COUNT] = static_cast<uint32_t>(events.size());
    b[HD_INDEXLEN] = static_cast<uint32_t>(4 * events.size());
    b[HD_VER] = 6 | w5_bits;
    b[HD_USERHDRLEN] = 0;
    b[HD_MAGIC] = kEvioMagic;
    b[HD_UNCOMPLEN] = static_cast<uint32_t>(4 * evwords);  // as evio C writeEventToBufferV6
    b[HD_COMPLEN] = 0;
    for (size_t i = 0; i < events.size(); ++i) b[V6_HDR + i] = static_cast<uint32_t>(4 * events[i].size());
  } else {
    return b;
  }
  for (const auto& e : events) b.insert(b.end(), e.begin(), e.end());
  return b;
}

bool split_buffer(const uint32_t* in, size_t nin, std::vector<std::vector<uint32_t>>& events,
                  BufferInfo* info) {
  events.clear();
  BufferInfo local;
  BufferInfo& bi = info ? *info : local;
  if (inspect_buffer(in, nin, bi) == Framing::Unsupported) return false;
  size_t pos = 0;
  std::vector<std::pair<size_t, size_t>> ev;
  std::string why;
  while (pos < nin) {
    if (pos > 0 && zero_tail(in, nin, pos)) break;
    Container c;
    parse_container(in, nin, pos, c, why);
    event_spans(in, c, ev, why);
    for (size_t i = 0; i < ev.size(); ++i) {
      if (c.f == Framing::EvioV4 && c.dict && i == 0) continue;
      events.emplace_back(in + ev[i].first, in + ev[i].first + ev[i].second);
    }
    if (c.len == 0) break;
    pos += c.len;
  }
  return true;
}

}  // namespace et
}  // namespace gemfilter
