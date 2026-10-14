// Status bank and policy unit tests (SYNTHETIC events).
#include "gemfilter/status/policy.h"
#include "gemfilter/status/processors.h"
#include "gemfilter/status/status_bank.h"
#include "synth.h"
#include "../codec/testing.h"
using namespace gemfilter;

TEST_CASE(status_bank_roundtrip) {
  StatusRecord r; r.config_hash = 0x1234567890abcdefull; r.stats.strips_in = 1000; r.stats.strips_out = 40;
  r.stats.bytes_in = 12000; r.stats.bytes_out = 480; r.stats.nseeds = 3; r.stats.nroads = 3; r.stats.ntracks = 2;
  r.stats.flags = 0x4; r.prescale_counter = 77; r.prescale_factor = 100; r.cpu_us = 321; r.instance_id = 2;
  auto w = encode_status_bank(r);
  CHECK_EQ(w.size(), kStatusBankWordsV1);
  auto d = decode_status_bank(w.data(), w.size());
  CHECK(d.has_value());
  CHECK(*d == r);
}

TEST_CASE(policy_controls_untouched_and_status_appended) {
  PolicyConfig pc;
  FilterPolicy pol(std::unique_ptr<EventProcessor>(new TruncateProcessor(0.5)), pc);
  auto ctl = synth::control(coda::kTagPrestart);
  auto c0 = ctl; size_t n = ctl.size(); ctl.resize(n + 64);
  auto r = pol.apply(ctl.data(), n, ctl.size());
  CHECK(!r.modified); CHECK_EQ(n, c0.size());
  CHECK(std::equal(c0.begin(), c0.end(), ctl.begin()));
  auto ev = synth::physics(11, 5, synth::default_rocs(300));
  size_t m = ev.size(); ev.resize(m + 64);
  auto r2 = pol.apply(ev.data(), m, ev.size());
  CHECK(r2.modified); CHECK(r2.status_appended);
  auto st = find_status_bank(ev.data(), m);
  CHECK(st.has_value());
  CHECK_EQ(st->config_hash, pol.config_hash());
}

TEST_CASE(prescale_every_nth) {
  PolicyConfig pc; pc.prescale = 4;
  FilterPolicy pol(std::unique_ptr<EventProcessor>(new TruncateProcessor(0.5)), pc);
  int raw = 0;
  for (int i = 0; i < 40; ++i) {
    auto ev = synth::physics(uint64_t(1 + 5 * i), 5, synth::default_rocs(100));
    size_t m = ev.size(); ev.resize(m + 64);
    auto r = pol.apply(ev.data(), m, ev.size());
    raw += (r.stats.flags & flags::kPassthroughPrescale) ? 1 : 0;
  }
  CHECK_EQ(raw, 10);
}
TEST_MAIN()
