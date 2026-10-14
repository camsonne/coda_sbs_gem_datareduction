#include "gemfilter/status/processors.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include "gemfilter/status/coda_event.h"

namespace gemfilter {

FilterStats PassThroughProcessor::process(uint32_t* /*buf*/, size_t& nwords, size_t /*cap*/) {
  FilterStats s;
  s.bytes_in = s.bytes_out = static_cast<uint32_t>(nwords * 4);
  return s;
}

TruncateProcessor::TruncateProcessor(double keep_fraction, std::vector<uint16_t> bank_tags,
                                     uint32_t min_keep)
    : keep_(keep_fraction), tags_(std::move(bank_tags)), min_keep_(min_keep) {
  if (!(keep_ >= 0.0 && keep_ <= 1.0)) throw std::invalid_argument("keep_fraction not in [0,1]");
}

std::string TruncateProcessor::config_string() const {
  std::ostringstream os;
  os << "truncate;keep=" << keep_ << ";min=" << min_keep_ << ";tags=";
  for (auto t : tags_) os << t << ',';
  return os.str();
}

uint32_t TruncateProcessor::kept(uint32_t n) const {
  auto k = static_cast<uint32_t>(std::floor(keep_ * static_cast<double>(n)));
  k = std::max(k, std::min(n, min_keep_));
  return std::min(k, n);
}

FilterStats TruncateProcessor::process(uint32_t* buf, size_t& nwords, size_t /*cap*/) {
  FilterStats s;
  s.bytes_in = static_cast<uint32_t>(nwords * 4);
  coda::PhysicsLayout lay;
  if (!coda::parse_physics(buf, nwords, lay)) throw std::runtime_error("truncate: not physics");
  // Process ROCs from the last to the first so earlier offsets stay valid while shrinking.
  size_t evlen = static_cast<size_t>(buf[0]) + 1;
  for (auto r = lay.rocs.rbegin(); r != lay.rocs.rend(); ++r) {
    if (!coda::is_bank_of_banks(r->hdr.type)) continue;
    std::vector<coda::BankRef> kids;
    if (!coda::list_children(buf, evlen, r->offset, kids)) throw std::runtime_error("bad ROC");
    for (auto k = kids.rbegin(); k != kids.rend(); ++k) {
      if (std::find(tags_.begin(), tags_.end(), k->hdr.tag) == tags_.end()) continue;
      const uint32_t payload = k->hdr.length - 1;
      const uint32_t keep = kept(payload);
      s.strips_in += payload;
      s.strips_out += keep;
      const uint32_t cut = payload - keep;
      if (cut == 0) continue;
      const size_t from = k->offset + 2 + payload;  // first word after child
      const size_t to = from - cut;
      std::memmove(buf + to, buf + from, (evlen - from) * sizeof(uint32_t));
      buf[k->offset] -= cut;
      buf[r->offset] -= cut;
      buf[0] -= cut;
      evlen -= cut;
    }
  }
  nwords = evlen;
  s.bytes_out = static_cast<uint32_t>(nwords * 4);
  return s;
}

}  // namespace gemfilter
