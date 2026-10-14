// SYNTHETIC SSP/VTP MPD payload generator (not real detector data).
#include <map>
#include "gemfilter/codec/synthetic.h"

#include <algorithm>
#include <random>
#include <stdexcept>

#include "gemfilter/codec/mpd_format.h"

namespace gemfilter {
namespace codec {
namespace synth {

using namespace mpd;

namespace {

struct Gen {
  const Config& c;
  uint16_t crate;
  std::mt19937 rng;
  Generated g;

  Gen(const Config& cfg, uint16_t cr) : c(cfg), crate(cr), rng(cfg.seed) {}

  int16_t rand_sample() {
    // mostly moderate values, sometimes the 13-bit extremes
    std::uniform_int_distribution<int> pick(0, 19);
    const int k = pick(rng);
    if (k == 0) return -4096;
    if (k == 1) return 4095;
    std::uniform_int_distribution<int> d(-300, 2000);
    return int16_t(d(rng));
  }

  // MPD data of one event for one fiber; returns words appended to `w`.
  void mpd_frames(std::vector<uint32_t>& w, uint32_t ev, uint16_t slot, uint32_t fiber, uint64_t tstamp) {
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const bool full = u(rng) < c.full_readout_fraction;
    const uint32_t mpd_id = (fiber * 7u + 3u) & 0x1Fu;
    const uint32_t hdr = make_frame_header(fiber, mpd_id, c.enable_cm, full, false);
    if (!c.debug_words) w.push_back(hdr);
    for (uint32_t a = 0; a < c.apvs_per_mpd; ++a) {
      if (c.debug_words) w.push_back(hdr);   // one frame header per APV so the debug triple can follow
      for (uint32_t ch = 0; ch < 128; ++ch) {
        if (!full && !(u(rng) < c.occupancy)) continue;
        int16_t s[6];
        for (auto& x : s) x = rand_sample();
        uint32_t hw[3];
        make_hit(ch, a, s, hw);
        w.insert(w.end(), hw, hw + 3);
        TruthHit t;
        t.event = ev;
        t.addr.crate = crate; t.addr.slot = slot; t.addr.fiber = uint16_t(fiber);
        t.addr.mpd = uint16_t(mpd_id); t.addr.adc = uint16_t(a);
        t.chan = uint8_t(ch);
        std::copy(s, s + 6, t.adc.begin());
        t.full_readout = full;
        g.hits.push_back(t);
      }
      if (c.debug_words) {
        std::uniform_int_distribution<int> d(-200, 200);
        w.push_back(make_debug0(int16_t(d(rng)), int16_t(d(rng))));
        for (int k = 0; k < 2; ++k) {
          int16_t s[6] = {int16_t(d(rng)), int16_t(d(rng)), 0, 0, 0, 0};
          uint32_t hw[3];
          make_hit(0, 0, s, hw);
          w.push_back(hw[0]);   // continuation: two packed 13-bit CM values, bits 30..26 = 0
        }
      }
    }
    if (c.event_info) {
      const uint32_t coarse = uint32_t(tstamp / 6) & 0xFFFFFFFFu;
      const uint32_t fine = uint32_t(tstamp % 6);
      w.push_back(make_event_info0(((coarse & 0xFFFFu) << 8) | fine));
      w.push_back((coarse >> 16) & 0xFFFFFFu);
      w.push_back((c.first_trigger + ev) & 0xFFFFFu);
    }
  }

  void event_words(std::vector<uint32_t>& w, uint32_t ev, uint16_t slot, uint32_t fiber0, bool with_header) {
    const uint64_t t = 1000000ull + 1234ull * ev;
    if (with_header) {
      w.push_back(make_event_header(c.first_trigger + ev));
      w.push_back(make_trigger_time1(t));
      w.push_back(make_trigger_time2(t));
    }
    if (c.reserved_word) {
      w.push_back(make_defining(7, 0x123456u));
      w.push_back(0x0ABCDEFu);
    }
    for (uint32_t f = 0; f < c.nfibers; ++f) mpd_frames(w, ev, slot, fiber0 + f, t);
  }

  void pad(std::vector<uint32_t>& w, size_t region_len_with_trailer) {
    if (c.filler_align == 0) return;
    while (region_len_with_trailer % c.filler_align) {
      w.push_back(kFillerWord);
      ++region_len_with_trailer;
    }
  }

  void block(uint16_t slot, uint32_t fiber0, uint32_t blknum) {
    auto& w = g.payload;
    const size_t start = w.size();
    w.push_back(make_block_header(slot, blknum, c.events_per_block));
    for (uint32_t ev = 0; ev < c.events_per_block; ++ev) event_words(w, ev, slot, fiber0, true);
    pad(w, w.size() - start + 1);
    const size_t n = w.size() - start + 1;   // inclusive, with trailer
    w.push_back(make_block_trailer(slot, uint32_t(n)));
  }

  void run() {
    switch (c.layout) {
      case Layout::Ssp: {
        uint32_t fiber0 = c.fiber_offset;
        for (auto s : c.slots) {
          block(s, fiber0, c.block_number);
          fiber0 += c.nfibers;
        }
        g.nevents = c.events_per_block;
        break;
      }
      case Layout::VtpBlock:
        block(11, c.fiber_offset, c.block_number);
        g.nevents = c.events_per_block;
        break;
      case Layout::VtpBare:
        for (uint32_t ev = 0; ev < c.events_per_block; ++ev) event_words(g.payload, ev, 11, c.fiber_offset, true);
        pad(g.payload, g.payload.size());
        g.nevents = c.events_per_block;
        break;
      case Layout::VtpBareNoEventHeader:
        event_words(g.payload, 0, 11, c.fiber_offset, false);
        pad(g.payload, g.payload.size());
        g.nevents = 1;
        break;
    }
    if (c.data_not_valid) g.payload.push_back(kDataNotValidWord);
    std::stable_sort(g.hits.begin(), g.hits.end(),
                     [](const TruthHit& a, const TruthHit& b) { return a.event < b.event; });
  }
};

}  // namespace

Generated generate(const Config& cfg, uint16_t crate) {
  if (cfg.apvs_per_mpd > 16) throw std::invalid_argument("synth: apvs_per_mpd > 16");
  if (cfg.events_per_block == 0 || cfg.events_per_block > 255) throw std::invalid_argument("synth: events_per_block");
  if (cfg.fiber_offset + cfg.nfibers * std::max<size_t>(1, cfg.slots.size()) > 64)
    throw std::invalid_argument("synth: fiber numbers exceed 6 bits");
  Gen g(cfg, crate);
  g.run();
  return std::move(g.g);
}

}  // namespace synth
}  // namespace codec

namespace codec {
namespace synth {
std::vector<uint32_t> write_block(const std::vector<std::vector<ApvFrame>>& events, uint16_t slot,
                                  uint32_t first_trigger, uint32_t block_number, bool enable_cm) {
  std::vector<uint32_t> w;
  w.push_back(mpd::make_block_header(slot, block_number, uint32_t(events.size())));
  for (size_t e = 0; e < events.size(); ++e) {
    w.push_back(mpd::make_event_header(first_trigger + uint32_t(e)));
    uint64_t tt = 1000ull * (first_trigger + e);
    w.push_back(mpd::make_trigger_time1(tt));
    w.push_back(mpd::make_trigger_time2(tt));
    std::map<std::pair<uint16_t, uint16_t>, std::vector<const ApvFrame*>> by_fiber;
    for (const auto& f : events[e]) by_fiber[{f.addr.fiber, f.addr.mpd}].push_back(&f);
    for (const auto& [key, frs] : by_fiber) {
      bool any = false;
      for (auto* f : frs) any |= !f->hits.empty();
      if (!any) continue;
      w.push_back(mpd::make_frame_header(key.first, key.second, enable_cm, false, false));
      for (auto* f : frs)
        for (const auto& h : f->hits) {
          uint32_t o[3];
          int16_t s6[6];
          for (int i = 0; i < 6; ++i) s6[i] = h.adc[size_t(i)];
          mpd::make_hit(h.apv_chan, f->addr.adc, s6, o);
          w.insert(w.end(), o, o + 3);
        }
    }
  }
  w.push_back(mpd::make_block_trailer(slot, uint32_t(w.size() + 1)));
  if (w.size() % 2) w.push_back(mpd::kFillerWord);
  return w;
}
}  // namespace synth
}  // namespace codec
}  // namespace gemfilter
