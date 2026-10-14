// End-to-end test of the production pipeline on SYNTHETIC CODA3 events:
//   toy tracks (SBS GEM geometry from the SBS-replay DB snapshot) + background -> MPD blocks per
//   crate, HCal VTP cluster at the block the track points to -> built physics events (block level 5)
//   -> FilterPolicy(GemFilterProcessor) via FileDriver -> decode output, check every signal strip
//   survived, reduction, status bank, prescale pass-through, EVIO file round trip.
#include <cstdio>
#include <fstream>
#include <set>
#include <tuple>

#include "gemfilter/apps/event_io.h"
#include "gemfilter/apps/file_driver.h"
#include "gemfilter/codec/synthetic.h"
#include "gemfilter/evioio/evio_event.h"
#include "gemfilter/evioio/synthetic_event.h"
#include "gemfilter/pipeline/GemFilterProcessor.h"
#include "gemfilter/seeds/VtpCluster.h"
#include "gemfilter/status/policy.h"
#include "gemfilter/status/status_bank.h"
#include "gemfilter/track/Toy.h"
#include "testing.h"

using namespace gemfilter;

static const char* kCfg =
    "db_dir = " GF_TEST_DB "\n"
    "date = 2024-10-01 00:00:00\n"
    "arm = sbs\n"
    "gem_tracker = sbs.gem\n"
    "seed_source = vtp\n"
    "calo = sbs.hcal\n"
    "vtp_roc = 30\n"
    "vtp_bank_tag = 0x56\n"
    "mode = track\n"
    "min_layers = 4\n"
    "min_layers_confident = 7\n";

using Key = std::tuple<uint32_t, uint16_t, uint16_t, uint16_t, uint8_t>;   // event, crate, fiber, adc, chan

struct Sample {
  std::vector<std::vector<uint32_t>> events;
  std::set<Key> signal;
  uint64_t strips = 0;
};

static Sample makeSample(const pipeline::PipelineContext& ctx, int nblocks, double occ, uint32_t seed) {
  Sample S;
  track::ToyGenerator g(ctx.gem, ctx.apvmap, seed);
  track::ToyConfig tc; tc.occupancy = occ;
  std::uniform_int_distribution<int> rowd(6, 17), cold(3, 8);
  std::uniform_real_distribution<double> u(-0.015, 0.015);
  uint32_t evn = 1;
  for (int b = 0; b < nblocks; ++b) {
    std::map<uint16_t, std::vector<std::vector<ApvFrame>>> by_crate;
    std::vector<seeds::VtpEvent> vtp;
    for (int e = 0; e < 5; ++e) {
      int row = rowd(g.rng()), col = cold(g.rng());
      int bi = ctx.calo.blockAt(row, col);
      const auto& blk = ctx.calo.blocks[size_t(bi)];
      geom::Vec3 target{blk.x + ctx.calo.origin.x, blk.y + ctx.calo.origin.y, ctx.calo.origin.z};
      track::ToyTrack t;
      t.p0 = {u(g.rng()), u(g.rng()), 0};
      t.dir = (target - t.p0).unit();
      auto ev = g.generate({t}, tc);
      for (auto& [cr, v] : by_crate) v.emplace_back();
      for (size_t f = 0; f < ev.frames.size(); ++f) {
        auto& v = by_crate[ev.frames[f].addr.crate];
        v.resize(size_t(e) + 1);
        v[size_t(e)].push_back(ev.frames[f]);
        for (size_t h = 0; h < ev.frames[f].hits.size(); ++h) {
          const auto& a = ev.frames[f].addr;
          if (ev.truth[f][h]) S.signal.insert({evn + uint32_t(e), a.crate, a.fiber, a.adc, ev.frames[f].hits[h].apv_chan});
          ++S.strips;
        }
      }
      seeds::VtpEvent ve; ve.trigger_number = evn + uint32_t(e); ve.slot = 11;
      seeds::VtpCluster cl; cl.det = seeds::VtpDet::HCal; cl.subtype = 13; cl.energy = 500;
      cl.row = uint32_t(row); cl.col = uint32_t(col); cl.nhits = 3; cl.time = 50;
      ve.clusters.push_back(cl);
      vtp.push_back(ve);
    }
    evioio::synth::PhysicsEventSpec sp;
    sp.block_level = 5;
    sp.first_event_number = evn;
    for (auto& [cr, v] : by_crate) {
      v.resize(5);
      evioio::synth::RocSpec r; r.roc_id = cr;
      r.banks.push_back({3561, 11, 0x01, codec::synth::write_block(v, 11, evn, uint32_t(b + 1))});
      sp.rocs.push_back(r);
    }
    evioio::synth::RocSpec rv; rv.roc_id = 30;
    rv.banks.push_back({0x56, 0, 0x01, seeds::encodeVtpBlock(11, vtp)});
    sp.rocs.push_back(rv);
    S.events.push_back(evioio::synth::make_physics_event(sp));
    evn += 5;
  }
  return S;
}

static std::set<Key> decodeOut(const std::vector<std::vector<uint32_t>>& evs, uint64_t* nstrips) {
  std::set<Key> out;
  *nstrips = 0;
  for (const auto& ev : evs) {
    auto v = evioio::EventView::parse(ev.data(), ev.size());
    uint64_t first = v.trigger().first_event_number;
    for (const auto& b : v.banks()) {
      if (b.tag != 3561) continue;
      auto d = codec::decode(b.payload, b.payload_len, b.roc_id);
      for (uint32_t e = 0; e < d.nevents(); ++e)
        for (uint32_t fi = d.event_begin[e]; fi < d.event_begin[e + 1]; ++fi)
          for (const auto& h : d.frames[fi].hits) {
            const auto& a = d.frames[fi].addr;
            out.insert({uint32_t(first) + e, a.crate, a.fiber, a.adc, h.apv_chan});
            ++*nstrips;
          }
    }
  }
  return out;
}

static std::shared_ptr<const pipeline::PipelineContext> ctx() {
  static auto c = pipeline::PipelineContext::build(pipeline::PipelineConfig::parse(kCfg));
  return c;
}

TEST_CASE(context_builds) {
  auto c = ctx();
  CHECK_EQ(c->gem.nlayers, 10);
  CHECK(c->apvmap.size() > 500);
  CHECK_EQ(c->calo.nelem(), 288);
}

TEST_CASE(end_to_end_track_mode) {
  auto c = ctx();
  Sample S = makeSample(*c, 4, 0.15, 21);
  CHECK(!S.signal.empty());
  PolicyConfig pc;
  FilterPolicy pol(std::make_unique<pipeline::GemFilterProcessor>(c), pc);
  MemorySource src(S.events);
  MemorySink sink;
  FileDriver drv(src, &sink, nullptr, pol);
  auto sum = drv.run();
  CHECK_EQ(sum.events, uint64_t(4));
  CHECK_EQ(sum.modified, uint64_t(4));
  CHECK_EQ(sum.errors, uint64_t(0));
  uint64_t nout = 0;
  auto out = decodeOut(sink.events, &nout);
  size_t lost = 0;
  for (auto& k : S.signal) lost += !out.count(k);
  std::printf("e2e: strips in %llu out %llu (driver %llu->%llu), words %llu->%llu, signal lost %zu/%zu\n",
              (unsigned long long)S.strips, (unsigned long long)nout, (unsigned long long)sum.strips_in,
              (unsigned long long)sum.strips_out, (unsigned long long)sum.words_in, (unsigned long long)sum.words_out,
              lost, S.signal.size());
  CHECK_EQ(sum.strips_in, S.strips);
  CHECK_EQ(sum.strips_out, nout);
  CHECK(double(lost) / double(S.signal.size()) < 0.03);
  CHECK(nout * 5 < S.strips);
  // status bank present and consistent
  auto rec = find_status_bank(sink.events[0].data(), sink.events[0].size());
  CHECK(rec.has_value());
  CHECK(rec->stats.nseeds == 5);
  CHECK(rec->stats.strips_out < rec->stats.strips_in);
}

TEST_CASE(prescale_passes_raw) {
  auto c = ctx();
  Sample S = makeSample(*c, 4, 0.10, 22);
  PolicyConfig pc; pc.prescale = 2;
  FilterPolicy pol(std::make_unique<pipeline::GemFilterProcessor>(c), pc);
  MemorySource src(S.events);
  MemorySink sink;
  FileDriver drv(src, &sink, nullptr, pol);
  auto sum = drv.run();
  CHECK_EQ(sum.prescaled, uint64_t(2));
  int identical_gem = 0;
  for (size_t i = 0; i < 4; ++i) {
    uint64_t a = 0, b = 0;
    decodeOut({S.events[i]}, &a);
    decodeOut({sink.events[i]}, &b);
    identical_gem += (a == b);
  }
  CHECK_EQ(identical_gem, 2);
}

TEST_CASE(road_mode_lossless) {
  auto cfg = pipeline::PipelineConfig::parse(std::string(kCfg) + "mode = road\n");
  auto c = pipeline::PipelineContext::build(cfg);
  Sample S = makeSample(*c, 4, 0.15, 23);
  pipeline::GemFilterProcessor proc(c);
  uint64_t lost = 0, nout_all = 0;
  for (auto ev : S.events) {
    size_t n = ev.size();
    ev.resize(n + 4096);
    proc.process(ev.data(), n, ev.size());
    ev.resize(n);
    uint64_t nout = 0;
    auto out = decodeOut({ev}, &nout);
    nout_all += nout;
    auto v = evioio::EventView::parse(ev.data(), ev.size());
    uint32_t first = uint32_t(v.trigger().first_event_number);
    for (auto& k : S.signal)
      if (std::get<0>(k) >= first && std::get<0>(k) < first + 5) lost += !out.count(k);
  }
  std::printf("road mode: strips %llu -> %llu, signal lost %llu\n", (unsigned long long)S.strips,
              (unsigned long long)nout_all, (unsigned long long)lost);
  CHECK_EQ(lost, uint64_t(0));
  CHECK(nout_all < S.strips);
}

TEST_CASE(evio_file_roundtrip) {
  auto c = ctx();
  Sample S = makeSample(*c, 3, 0.10, 24);
  const std::string in = "gf_e2e_in.evio", outp = "gf_e2e_out.evio";
  {
    EvioFileSink w(in);
    for (auto& e : S.events) w.write(e.data(), e.size());
    w.close();
  }
  PolicyConfig pc;
  FilterPolicy pol(std::make_unique<pipeline::GemFilterProcessor>(c), pc);
  EvioFileSource src(in);
  EvioFileSink sink(outp);
  std::ofstream csvf("gf_e2e_metrics.csv");
  MetricsCsvWriter csv(csvf);
  FileDriver drv(src, &sink, &csv, pol);
  auto sum = drv.run();
  sink.close();
  CHECK_EQ(sum.events, uint64_t(3));
  CHECK_EQ(sum.modified, uint64_t(3));
  EvioFileSource rd(outp);
  std::vector<uint32_t> buf;
  size_t n = 0;
  int count = 0;
  while (rd.next(buf, n, 0)) {
    auto v = evioio::EventView::parse(buf.data(), n);
    CHECK(v.is_physics());
    ++count;
  }
  CHECK_EQ(count, 3);
}

TEST_MAIN()
