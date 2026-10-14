#include "gemfilter/status/status_bank.h"

#include <cstring>

#include "gemfilter/status/coda_event.h"

namespace gemfilter {

bool StatusRecord::operator==(const StatusRecord& o) const {
  const auto& a = stats;
  const auto& b = o.stats;
  return version == o.version && config_hash == o.config_hash && a.strips_in == b.strips_in &&
         a.strips_out == b.strips_out && a.bytes_in == b.bytes_in && a.bytes_out == b.bytes_out &&
         a.nseeds == b.nseeds && a.nroads == b.nroads && a.ntracks == b.ntracks &&
         a.flags == b.flags && prescale_counter == o.prescale_counter &&
         prescale_factor == o.prescale_factor && cpu_us == o.cpu_us && instance_id == o.instance_id;
}

size_t encode_status_bank(const StatusRecord& r, uint32_t* out, size_t cap) {
  if (cap < kStatusBankWordsV1) return 0;
  out[0] = static_cast<uint32_t>(kStatusPayloadWordsV1 + 1);
  out[1] = coda::make_bank_word1(kStatusBankTag, coda::kTypeUint32, kStatusBankVersion);
  uint32_t* p = out + 2;
  p[0] = kStatusBankVersion;
  p[1] = static_cast<uint32_t>(r.config_hash & 0xffffffffu);
  p[2] = static_cast<uint32_t>(r.config_hash >> 32);
  p[3] = r.stats.strips_in;
  p[4] = r.stats.strips_out;
  p[5] = r.stats.bytes_in;
  p[6] = r.stats.bytes_out;
  p[7] = static_cast<uint32_t>(r.stats.nseeds) | (static_cast<uint32_t>(r.stats.nroads) << 16);
  p[8] = static_cast<uint32_t>(r.stats.ntracks) | (static_cast<uint32_t>(r.stats.flags) << 16);
  p[9] = r.prescale_counter;
  p[10] = r.prescale_factor;
  p[11] = r.cpu_us;
  p[12] = r.instance_id;
  return kStatusBankWordsV1;
}

std::vector<uint32_t> encode_status_bank(const StatusRecord& r) {
  std::vector<uint32_t> v(kStatusBankWordsV1);
  encode_status_bank(r, v.data(), v.size());
  return v;
}

std::optional<StatusRecord> decode_status_bank(const uint32_t* bank, size_t nwords_avail) {
  if (!bank || nwords_avail < 2) return std::nullopt;
  const coda::BankHeader h = coda::parse_bank_header(bank);
  if (h.tag != kStatusBankTag || h.type != coda::kTypeUint32) return std::nullopt;
  if (h.length < 1 + 12 || static_cast<size_t>(h.length) + 1 > nwords_avail) return std::nullopt;
  const uint32_t* p = bank + 2;
  const size_t npay = h.length - 1;
  StatusRecord r;
  r.version = p[0];
  if (r.version < 1) return std::nullopt;
  r.config_hash = static_cast<uint64_t>(p[1]) | (static_cast<uint64_t>(p[2]) << 32);
  r.stats.strips_in = p[3];
  r.stats.strips_out = p[4];
  r.stats.bytes_in = p[5];
  r.stats.bytes_out = p[6];
  r.stats.nseeds = static_cast<uint16_t>(p[7] & 0xffff);
  r.stats.nroads = static_cast<uint16_t>(p[7] >> 16);
  r.stats.ntracks = static_cast<uint16_t>(p[8] & 0xffff);
  r.stats.flags = static_cast<uint16_t>(p[8] >> 16);
  r.prescale_counter = p[9];
  r.prescale_factor = p[10];
  r.cpu_us = p[11];
  r.instance_id = npay > 12 ? p[12] : 0;
  return r;
}

std::optional<StatusRecord> find_status_bank(const uint32_t* ev, size_t nwords,
                                             uint16_t* host_roc) {
  coda::PhysicsLayout lay;
  if (!coda::parse_physics(ev, nwords, lay)) return std::nullopt;
  std::vector<coda::BankRef> kids;
  for (const auto& roc : lay.rocs) {
    if (!coda::is_bank_of_banks(roc.hdr.type)) continue;
    if (!coda::list_children(ev, nwords, roc.offset, kids)) continue;
    for (const auto& k : kids) {
      if (k.hdr.tag != kStatusBankTag) continue;
      auto r = decode_status_bank(ev + k.offset, nwords - k.offset);
      if (r) {
        if (host_roc) *host_roc = coda::roc_id(roc.hdr);
        return r;
      }
    }
  }
  return std::nullopt;
}

bool append_status_to_roc(uint32_t* ev, size_t& nwords, size_t cap, size_t roc_offset,
                          const StatusRecord& r) {
  if (nwords < 2 || roc_offset + 1 >= nwords) return false;
  const size_t evlen = static_cast<size_t>(ev[0]) + 1;
  if (evlen > nwords) return false;
  const size_t add = kStatusBankWordsV1;
  if (evlen + add > cap) return false;
  const coda::BankHeader rh = coda::parse_bank_header(ev + roc_offset);
  if (!coda::is_bank_of_banks(rh.type)) return false;
  const size_t roc_end = roc_offset + 1 + rh.length;  // insertion point
  if (roc_end > evlen) return false;
  std::memmove(ev + roc_end + add, ev + roc_end, (evlen - roc_end) * sizeof(uint32_t));
  encode_status_bank(r, ev + roc_end, add);
  ev[roc_offset] += static_cast<uint32_t>(add);
  ev[0] += static_cast<uint32_t>(add);
  nwords = evlen + add;
  return true;
}

std::optional<size_t> find_gem_host_roc(const uint32_t* ev, size_t nwords,
                                        const std::vector<uint16_t>& gem_bank_tags) {
  coda::PhysicsLayout lay;
  if (!coda::parse_physics(ev, nwords, lay)) return std::nullopt;
  for (const auto& roc : lay.rocs)
    for (uint16_t t : gem_bank_tags)
      if (coda::roc_has_bank(ev, nwords, roc, t)) return roc.offset;
  return std::nullopt;
}

uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h;
}

}  // namespace gemfilter
