// gemfilter / seeds : VTP cluster decoding (see VtpCluster.h).
#include "gemfilter/seeds/VtpCluster.h"

#include <algorithm>

namespace gemfilter {
namespace seeds {

VtpDecodeResult decodeVtp(const uint32_t* words, size_t n, const VtpDecodeOptions& opt) {
  VtpDecodeResult res;
  uint32_t type = 15;
  int slot = -1;
  int ievt = -1;
  uint32_t tt_w1 = 0;
  bool have_word1 = false;
  VtpCluster pending;
  auto event = [&]() -> VtpEvent& {
    int i = std::max(ievt, 0);
    if (static_cast<int>(res.events.size()) <= i) res.events.resize(i + 1);
    return res.events[i];
  };
  for (size_t iw = 0; iw < n; ++iw) {
    const uint32_t w = words[iw];
    const bool defining = (w >> 31) & 1u;
    if (defining) type = (w >> 27) & 0xFu;
    switch (type) {
      case 0:
        if (defining) {
          slot = static_cast<int>((w >> 22) & 0x1F);
          ievt = -1;
        }
        break;
      case 1:
        ievt = -1;
        break;
      case 2: {
        ++ievt;
        VtpEvent& e = event();
        e.trigger_number = w & 0x3FFFFF;
        e.slot = slot;
        break;
      }
      case 3:
        if (defining)
          tt_w1 = w & 0xFFFFFF;
        else
          event().trigger_time = (static_cast<uint64_t>(w & 0xFFFFFF) << 24) | tt_w1;
        break;
      case 12:
        if (defining) {
          pending = VtpCluster{};
          pending.subtype = (w >> 23) & 0xF;
          if (pending.subtype == opt.hcal_subtype_a || pending.subtype == opt.hcal_subtype_b)
            pending.det = VtpDet::HCal;
          else if (pending.subtype == opt.ecal_subtype)
            pending.det = VtpDet::ECal;
          pending.energy = w & opt.energy_mask;
          have_word1 = true;
        } else {
          if (!have_word1) { ++res.nerrors; break; }
          if (pending.det == VtpDet::HCal) {
            pending.col = (w >> 20) & 0xF;
            pending.row = (w >> 15) & 0x1F;
          } else {  // SBS-offline: every non-HCAL id uses the ECal layout
            pending.col = (w >> 22) & 0x1F;
            pending.row = (w >> 15) & 0x7F;
          }
          pending.nhits = (w >> 11) & 0xF;
          pending.time = w & 0x7FF;
          event().clusters.push_back(pending);
          have_word1 = false;
        }
        break;
      case 13:
        if (defining) event().decisions.push_back({w & 0xFFFF, (w >> 16) & 0x7F});
        break;
      default:
        break;
    }
  }
  if (have_word1) ++res.nerrors;  // dangling first cluster word
  return res;
}

std::vector<CaloSeed> vtpSeeds(const VtpEvent& ev, const geom::CaloGeometry& calo, CaloSeed::Arm arm,
                               const VtpSeedOptions& opt) {
  std::vector<CaloSeed> out;
  for (const auto& c : ev.clusters) {
    int row = static_cast<int>(c.row) + opt.row_offset;
    int col = static_cast<int>(c.col) + opt.col_offset;
    int b = calo.blockAt(row, col);
    if (b < 0) continue;
    double E = c.energy * opt.energy_scale;
    if (E < opt.min_energy) continue;
    CaloSeed s;
    s.arm = arm;
    s.source = CaloSeed::Source::VTP;
    s.x = calo.blocks[b].x;
    s.y = calo.blocks[b].y;
    s.z = calo.origin.z;
    s.E = E;
    s.t = c.time * opt.time_ns_per_count;
    s.nblocks = static_cast<int>(c.nhits);
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const CaloSeed& a, const CaloSeed& b) { return a.E > b.E; });
  return out;
}

std::vector<uint32_t> encodeVtpBlock(int slot, const std::vector<VtpEvent>& events, const VtpDecodeOptions& opt) {
  std::vector<uint32_t> out;
  size_t start = out.size();
  out.push_back((1u << 31) | (0u << 27) | ((slot & 0x1Fu) << 22) | ((events.size() & 0x3FFu) << 8));
  for (const auto& e : events) {
    out.push_back((1u << 31) | (2u << 27) | (e.trigger_number & 0x3FFFFFu));
    out.push_back((1u << 31) | (3u << 27) | static_cast<uint32_t>(e.trigger_time & 0xFFFFFF));
    out.push_back(static_cast<uint32_t>((e.trigger_time >> 24) & 0xFFFFFF));
    for (const auto& c : e.clusters) {
      uint32_t sub = c.subtype ? c.subtype : (c.det == VtpDet::HCal ? opt.hcal_subtype_a : opt.ecal_subtype);
      out.push_back((1u << 31) | (12u << 27) | ((sub & 0xFu) << 23) | (c.energy & opt.energy_mask));
      uint32_t w2;
      if (c.det == VtpDet::HCal)
        w2 = ((c.col & 0xFu) << 20) | ((c.row & 0x1Fu) << 15);
      else
        w2 = ((c.col & 0x1Fu) << 22) | ((c.row & 0x7Fu) << 15);
      w2 |= ((c.nhits & 0xFu) << 11) | (c.time & 0x7FFu);
      out.push_back(w2);
    }
  }
  uint32_t nwords = static_cast<uint32_t>(out.size() - start + 1);
  out.push_back((1u << 31) | (1u << 27) | ((slot & 0x1Fu) << 22) | (nwords & 0x3FFFFFu));
  return out;
}

}  // namespace seeds
}  // namespace gemfilter
