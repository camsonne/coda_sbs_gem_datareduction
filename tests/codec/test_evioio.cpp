// Unit tests for gemfilter_evioio in-memory layer (parse, edit, swap). SYNTHETIC events only.
#include <algorithm>
#include <cstring>
#include <vector>

#include "gemfilter/codec/gem_codec.h"
#include "gemfilter/codec/synthetic.h"
#include "gemfilter/evioio/evio_event.h"
#include "gemfilter/evioio/evio_swap.h"
#include "gemfilter/evioio/synthetic_event.h"
#include "testing.h"

#if GEMFILTER_HAVE_EVIO
extern "C" {
#include "evio.h"
}
#endif

using namespace gemfilter;
using namespace gemfilter::evioio;

namespace {

constexpr uint16_t kGemBank = 3561;   // MPDModule bank tag (db_cratemap.dat)

synth::PhysicsEventSpec make_spec(uint32_t seed, uint8_t bl = 5) {
  synth::PhysicsEventSpec s;
  s.block_level = bl;
  s.first_event_number = 100 + seed * bl;
  codec::synth::Config c;
  c.events_per_block = bl;
  c.seed = seed;
  c.layout = codec::synth::Layout::VtpBare;
  auto g19 = codec::synth::generate(c, 19);
  c.seed = seed + 1000;
  c.layout = codec::synth::Layout::Ssp;
  c.slots = {11};
  auto g20 = codec::synth::generate(c, 20);
  s.rocs.push_back({21, {{0x0C, bl, 0x01, std::vector<uint32_t>{0xDEADBEEF, 1, 2, 3}}}});      // TS ROC (opaque)
  s.rocs.push_back({19, {{kGemBank, bl, 0x01, g19.payload}, {4, bl, 0x01, {7, 8, 9}}}});
  s.rocs.push_back({20, {{kGemBank, bl, 0x01, g20.payload}}});
  return s;
}

}  // namespace

TEST_CASE(parse_physics_event_structure) {
  auto spec = make_spec(1);
  auto ev = synth::make_physics_event(spec);
  EventView v = EventView::parse(ev.data(), ev.size());
  CHECK(v.is_physics());
  CHECK_EQ(int(v.block_level()), 5);
  CHECK_EQ(v.nwords(), ev.size());
  CHECK(v.trigger().present);
  CHECK_EQ(v.trigger().tag, 0xFF21);
  CHECK_EQ(int(v.trigger().nrocs), 3);
  CHECK_EQ(v.trigger().first_event_number, uint64_t(105));
  CHECK_EQ(v.trigger().timestamps.size(), size_t(5));
  CHECK_EQ(v.trigger().event_types.size(), size_t(5));
  CHECK_EQ(int(v.trigger().event_types[1]), 2);
  CHECK_EQ(v.trigger().roc_segment_ids.size(), size_t(3));
  CHECK_EQ(v.rocs().size(), size_t(3));
  CHECK_EQ(v.banks().size(), size_t(4));
  auto gem = v.find_banks(-1, kGemBank);
  CHECK_EQ(gem.size(), size_t(2));
  const DataBankRef& b19 = v.banks()[gem[0]];
  CHECK_EQ(b19.roc_id, 19);
  CHECK_EQ(b19.payload_len, uint32_t(spec.rocs[1].banks[0].payload.size()));
  CHECK(std::equal(b19.payload, b19.payload + b19.payload_len, spec.rocs[1].banks[0].payload.begin()));
  CHECK_EQ(v.find_roc(20), 2);
  CHECK_EQ(v.find_roc(99), -1);
  // the payload decodes with the codec
  auto db = codec::decode(b19.payload, b19.payload_len, b19.roc_id);
  CHECK_EQ(db.nevents(), 5u);
}

TEST_CASE(control_and_malformed_events) {
  auto c = synth::make_control_event(0xFFD1, 1234, 5, 999);
  EventView v = EventView::parse(c.data(), c.size());
  CHECK(v.is_control());
  CHECK(v.rocs().empty());
  EventEditor ed(v);
  CHECK(ed.build() == c);
  // malformed
  auto ev = synth::make_physics_event(make_spec(2));
  {
    auto q = ev; q[0] += 5;   // event length beyond buffer
    CHECK_THROWS_CODE(EventView::parse(q.data(), q.size()), EvioError, EvioErrorCode::BadLength);
  }
  {
    auto q = ev;
    EventView vv = EventView::parse(ev.data(), ev.size());
    q[vv.rocs()[1].offset] += 1;   // ROC length overruns event / mis-tiles
    bool thrown = false;
    try { EventView::parse(q.data(), q.size()); } catch (const EvioError&) { thrown = true; }
    CHECK(thrown);
  }
  {
    auto q = ev;
    EventView vv = EventView::parse(ev.data(), ev.size());
    q[vv.trigger().offset + 2] += 2;   // segment-1 length wrong
    bool thrown = false;
    try { EventView::parse(q.data(), q.size()); } catch (const EvioError& e) { thrown = (e.code() == EvioErrorCode::BadTriggerBank || e.code() == EvioErrorCode::BadLength); }
    CHECK(thrown);
  }
}

TEST_CASE(editor_identity_shrink_append) {
  auto spec = make_spec(3);
  auto ev = synth::make_physics_event(spec);
  EventView v = EventView::parse(ev.data(), ev.size());
  {
    EventEditor ed(v);
    CHECK(!ed.modified());
    CHECK(ed.build() == ev);
    CHECK_EQ(ed.built_words(), ev.size());
  }
  // filter both GEM banks with the codec: keep every other strip
  EventEditor ed(v);
  std::vector<std::vector<uint32_t>> newp;
  for (size_t bi : v.find_banks(-1, kGemBank)) {
    const auto& b = v.banks()[bi];
    auto db = codec::decode(b.payload, b.payload_len, b.roc_id);
    KeepMask m = db.keep_all();
    for (auto& f : m) for (size_t j = 0; j < f.size(); ++j) f[j] = (j % 2 == 0);
    auto out = codec::encode(db.layout, db.frames, m);
    CHECK(out.size() < b.payload_len);
    newp.push_back(out);
    ed.replace_payload(bi, out.data(), out.size());
  }
  const std::vector<uint32_t> status = {0x47454D46, 1, 2, 3};   // test content (not the CODA-track status format)
  const int iroc = v.find_roc(19);
  ed.append_bank_in_roc(size_t(iroc), 0xE1F0, 0, status.data(), status.size());
  auto out = ed.build();
  CHECK_EQ(out.size(), ed.built_words());
  CHECK(out.size() < ev.size());
  EventView v2 = EventView::parse(out.data(), out.size());
  CHECK_EQ(v2.rocs().size(), size_t(3));
  CHECK_EQ(v2.banks().size(), size_t(5));
  CHECK_EQ(v2.trigger().first_event_number, v.trigger().first_event_number);
  // trigger bank and TS ROC untouched, byte for byte
  CHECK(std::equal(out.begin() + v2.trigger().offset, out.begin() + v2.trigger().offset + v2.trigger().total_words,
                   ev.begin() + v.trigger().offset));
  CHECK(std::equal(out.begin() + v2.rocs()[0].offset, out.begin() + v2.rocs()[0].offset + v2.rocs()[0].total_words,
                   ev.begin() + v.rocs()[0].offset));
  auto gem2 = v2.find_banks(-1, kGemBank);
  CHECK_EQ(gem2.size(), size_t(2));
  for (size_t k = 0; k < 2; ++k) {
    const auto& b = v2.banks()[gem2[k]];
    CHECK_EQ(size_t(b.payload_len), newp[k].size());
    CHECK(std::equal(b.payload, b.payload + b.payload_len, newp[k].begin()));
    CHECK_EQ(int(b.num), 5);
  }
  auto st = v2.find_banks(19, 0xE1F0);
  CHECK_EQ(st.size(), size_t(1));
  CHECK(std::equal(v2.banks()[st[0]].payload, v2.banks()[st[0]].payload + 4, status.begin()));
  // untouched non-GEM bank in ROC 19 is intact
  auto b4 = v2.find_banks(19, 4);
  CHECK_EQ(b4.size(), size_t(1));
  CHECK_EQ(v2.banks()[b4[0]].payload[2], 9u);
  // raw-buffer build (ET path) and aliasing / capacity checks
  std::vector<uint32_t> dst(out.size() + 10, 0);
  CHECK_EQ(ed.build_into(dst.data(), dst.size()), out.size());
  CHECK(std::equal(out.begin(), out.end(), dst.begin()));
  CHECK_THROWS_CODE(ed.build_into(dst.data(), out.size() - 1), EvioError, EvioErrorCode::BadArgument);
  CHECK_THROWS_CODE(ed.build_into(ev.data(), ev.size()), EvioError, EvioErrorCode::BadArgument);
  // replacement with a LONGER payload and with an empty payload
  EventEditor ed2(v);
  std::vector<uint32_t> longer(5000, 0xF8000000u);
  ed2.replace_payload(v.find_banks(20, kGemBank)[0], longer.data(), longer.size());
  ed2.replace_payload(v.find_banks(19, 4)[0], nullptr, 0);
  auto out2 = ed2.build();
  EventView v3 = EventView::parse(out2.data(), out2.size());
  CHECK_EQ(v3.banks()[v3.find_banks(20, kGemBank)[0]].payload_len, 5000u);
  CHECK_EQ(v3.banks()[v3.find_banks(19, 4)[0]].payload_len, 0u);
  // top-level append (documented as Podd-incompatible) still yields a valid EVIO structure
  EventEditor ed3(v);
  ed3.append_top_level_bank(0xE1F1, 0, status.data(), status.size());
  auto out3 = ed3.build();
  EventView v4 = EventView::parse(out3.data(), out3.size());
  CHECK_EQ(v4.rocs().size(), size_t(4));   // appears as an extra "ROC"
  CHECK_THROWS_CODE(ed.replace_payload(99, nullptr, 0), EvioError, EvioErrorCode::BadArgument);
}

TEST_CASE(swap_roundtrip_and_vs_evio) {
  auto ev = synth::make_physics_event(make_spec(4, 3));   // odd block level: padded uint16 segment
  auto sw = ev;
  swap_event(sw.data(), sw.size(), /*to_host=*/false);
  CHECK(sw != ev);
  CHECK_EQ(sw[0], bswap32(ev[0]));
  // uint64 timestamps are swapped as 64-bit units
  EventView v = EventView::parse(ev.data(), ev.size());
  auto back = sw;
  swap_event(back.data(), back.size(), /*to_host=*/true);
  CHECK(back == ev);
#if GEMFILTER_HAVE_EVIO
  // independent check against the evio C library's evioswap()
  std::vector<uint32_t> e1 = ev, d1(ev.size(), 0);
  evioswap(e1.data(), 0 /*buffer is local*/, d1.data());
  CHECK(d1 == sw);
  std::vector<uint32_t> e2 = sw, d2(ev.size(), 0);
  evioswap(e2.data(), 1 /*to local*/, d2.data());
  CHECK(d2 == ev);
#endif
  (void)v;
}

TEST_MAIN()
