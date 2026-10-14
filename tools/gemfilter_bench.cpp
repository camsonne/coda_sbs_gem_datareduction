// gemfilter_bench : SYNTHETIC performance / reduction scan.
//
//   gemfilter_bench <SBS-replay DB dir> [nblocks=40] [out.csv]
//
// (a) "sbs_pipeline": full GemFilterProcessor (EVIO parse, MPD decode, VTP seeds, constraint
//     windows, filter, re-encode, event rebuild) on the SBS GEM geometry (DB 2024-10-01), HCal VTP
//     seed at the block the toy track points to.  Block level 5.
// (b) "bb_filter": GemEventFilter alone on the BigBite GEM geometry (DB 2022-01-10) with a road
//     built from the true track and the BigBite production constraint widths from the DB.
// Occupancy (fraction of strips fired by background) scanned 0.05..0.40; mode road and track.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include "gemfilter/apps/event_io.h"
#include "gemfilter/codec/synthetic.h"
#include "gemfilter/evioio/evio_event.h"
#include "gemfilter/evioio/synthetic_event.h"
#include "gemfilter/geom/ArmConfig.h"
#include "gemfilter/pipeline/GemFilterProcessor.h"
#include "gemfilter/seeds/VtpCluster.h"
#include "gemfilter/status/policy.h"
#include "gemfilter/track/Toy.h"

using namespace gemfilter;
using Key = std::tuple<uint32_t, uint16_t, uint16_t, uint16_t, uint8_t>;

struct Row {
  std::string geometry, mode;
  double occ = 0;
  uint64_t nphys = 0, strips_in = 0, strips_out = 0, words_in = 0, words_out = 0, sig = 0, sig_kept = 0;
  double cpu_us = 0;
  uint64_t fallback = 0, tracks = 0;
};

static void writeRow(std::ostream& o, const Row& r) {
  o << r.geometry << ',' << r.mode << ',' << r.occ << ',' << r.nphys << ',' << double(r.strips_in) / r.nphys << ','
    << double(r.strips_out) / r.nphys << ',' << double(r.strips_in) / std::max<uint64_t>(1, r.strips_out) << ','
    << double(r.words_in) / r.nphys << ',' << double(r.words_out) / r.nphys << ','
    << double(r.words_in) / std::max<uint64_t>(1, r.words_out) << ',' << double(r.sig_kept) / std::max<uint64_t>(1, r.sig)
    << ',' << r.cpu_us / r.nphys << ',' << double(r.fallback) / r.nphys << ',' << double(r.tracks) / r.nphys << '\n';
}

static Row runSbs(const std::string& db, double occ, bool road, int nblocks, uint32_t seed) {
  std::string cfg = "db_dir = " + db +
                    "\ndate = 2024-10-01 00:00:00\narm = sbs\ngem_tracker = sbs.gem\nseed_source = vtp\ncalo = sbs.hcal\n"
                    "vtp_roc = 30\nvtp_bank_tag = 0x56\nmin_layers = 4\nmin_layers_confident = 7\nmax_hits2d_track = 400\nmode = " +
                    std::string(road ? "road" : "track") + "\n";
  auto ctx = pipeline::PipelineContext::build(pipeline::PipelineConfig::parse(cfg));
  pipeline::GemFilterProcessor proc(ctx);
  track::ToyGenerator g(ctx->gem, ctx->apvmap, seed);
  track::ToyConfig tc; tc.occupancy = occ;
  std::uniform_int_distribution<int> rowd(6, 17), cold(3, 8);
  std::uniform_real_distribution<double> u(-0.015, 0.015);
  Row R; R.geometry = "sbs_pipeline"; R.mode = road ? "road" : "track"; R.occ = occ;
  uint32_t evn = 1;
  for (int b = 0; b < nblocks; ++b) {
    std::map<uint16_t, std::vector<std::vector<ApvFrame>>> by_crate;
    std::vector<seeds::VtpEvent> vtp;
    std::set<Key> signal;
    for (int e = 0; e < 5; ++e) {
      int row = rowd(g.rng()), col = cold(g.rng());
      const auto& blk = ctx->calo.blocks[size_t(ctx->calo.blockAt(row, col))];
      geom::Vec3 target{blk.x + ctx->calo.origin.x, blk.y + ctx->calo.origin.y, ctx->calo.origin.z};
      track::ToyTrack t; t.p0 = {u(g.rng()), u(g.rng()), 0}; t.dir = (target - t.p0).unit();
      auto ev = g.generate({t}, tc);
      for (size_t f = 0; f < ev.frames.size(); ++f) {
        auto& v = by_crate[ev.frames[f].addr.crate];
        v.resize(size_t(e) + 1);
        v[size_t(e)].push_back(ev.frames[f]);
        for (size_t h = 0; h < ev.frames[f].hits.size(); ++h)
          if (ev.truth[f][h]) {
            const auto& a = ev.frames[f].addr;
            signal.insert({uint32_t(e), a.crate, a.fiber, a.adc, ev.frames[f].hits[h].apv_chan});
          }
        R.strips_in += ev.frames[f].hits.size();
      }
      seeds::VtpEvent ve; ve.trigger_number = evn + uint32_t(e); ve.slot = 11;
      seeds::VtpCluster cl; cl.det = seeds::VtpDet::HCal; cl.subtype = 13; cl.energy = 500;
      cl.row = uint32_t(row); cl.col = uint32_t(col); cl.nhits = 3; cl.time = 50;
      ve.clusters.push_back(cl); vtp.push_back(ve);
    }
    evioio::synth::PhysicsEventSpec sp; sp.block_level = 5; sp.first_event_number = evn;
    for (auto& [cr, v] : by_crate) {
      v.resize(5);
      evioio::synth::RocSpec r; r.roc_id = cr;
      r.banks.push_back({3561, 11, 0x01, codec::synth::write_block(v, 11, evn, uint32_t(b + 1))});
      sp.rocs.push_back(r);
    }
    evioio::synth::RocSpec rv; rv.roc_id = 30;
    rv.banks.push_back({0x56, 0, 0x01, seeds::encodeVtpBlock(11, vtp)});
    sp.rocs.push_back(rv);
    auto ev = evioio::synth::make_physics_event(sp);
    size_t n = ev.size();
    ev.resize(n + 4096);
    uint64_t c0 = thread_cpu_us();
    FilterStats st = proc.process(ev.data(), n, ev.size());
    R.cpu_us += double(thread_cpu_us() - c0);
    ev.resize(n);
    R.words_in += st.bytes_in / 4; R.words_out += st.bytes_out / 4;
    for (auto& d : proc.last_details()) { R.fallback += d.fallback; R.tracks += d.ntracks; }
    // signal retention: decode output
    auto vw = evioio::EventView::parse(ev.data(), ev.size());
    std::set<Key> out;
    for (const auto& bk : vw.banks()) {
      if (bk.tag != 3561) continue;
      auto d = codec::decode(bk.payload, bk.payload_len, bk.roc_id);
      for (uint32_t e = 0; e < d.nevents(); ++e)
        for (uint32_t fi = d.event_begin[e]; fi < d.event_begin[e + 1]; ++fi)
          for (const auto& h : d.frames[fi].hits) {
            out.insert({e, d.frames[fi].addr.crate, d.frames[fi].addr.fiber, d.frames[fi].addr.adc, h.apv_chan});
            ++R.strips_out;
          }
    }
    for (auto& k : signal) { ++R.sig; R.sig_kept += out.count(k); }
    R.nphys += 5;
    evn += 5;
  }
  return R;
}

static Row runBb(const std::string& db, double occ, bool road, int nev, uint32_t seed) {
  auto T = geom::civilSeconds(2022, 1, 10);
  geom::DbDir dir(db);
  static geom::GemTracker trk = geom::loadGemTracker(dir, "bb.gem", T);
  static geom::ApvMap map(trk);
  geom::BigBiteConfig bb = geom::loadBigBiteConfig(dir, T);
  track::TrackerConfig tc;
  tc.mode = road ? track::TrackerConfig::Mode::RoadOnly : track::TrackerConfig::Mode::Track;
  tc.min_layers = 3; tc.min_layers_confident = 5; tc.max_hits2d_track = 150;
  track::GemEventFilter f(trk, map, tc);
  track::ToyGenerator g(trk, map, seed);
  track::ToyConfig toy; toy.occupancy = occ;
  std::normal_distribution<double> smear(0.0, 0.01);
  Row R; R.geometry = "bb_filter"; R.mode = road ? "road" : "track"; R.occ = occ;
  for (int i = 0; i < nev; ++i) {
    auto t = g.randomTrack(0.2, 0.1, 0.25, 0.08);
    auto ev = g.generate({t}, toy);
    std::vector<const ApvFrame*> p;
    for (auto& fr : ev.frames) p.push_back(&fr);
    seeds::ConstraintPoints cp;
    cp.front = t.p0 + geom::Vec3{smear(g.rng()), smear(g.rng()), 0};   // ~1 cm seed resolution
    cp.back = t.p0 + t.dir * (3.0 / t.dir.z) + geom::Vec3{smear(g.rng()), smear(g.rng()), 0};
    cp.fwx = bb.cw.front_wx; cp.fwy = bb.cw.front_wy; cp.bwx = bb.cw.back_wx; cp.bwy = bb.cw.back_wy;
    uint64_t c0 = thread_cpu_us();
    auto w = seeds::moduleWindows(trk, {cp});
    auto r = f.run(p, &w);
    R.cpu_us += double(thread_cpu_us() - c0);
    R.strips_in += r.strips_in; R.strips_out += r.strips_out;
    R.words_in += 3ull * r.strips_in; R.words_out += 3ull * r.strips_out;   // hit words only
    R.fallback += r.fallback; R.tracks += r.tracks.size();
    for (size_t fi = 0; fi < ev.frames.size(); ++fi)
      for (size_t h = 0; h < ev.frames[fi].hits.size(); ++h)
        if (ev.truth[fi][h]) { ++R.sig; R.sig_kept += r.keep[fi][h]; }
    ++R.nphys;
  }
  return R;
}

// --synth <DB> <out.evio> [nblocks=200] [occupancy=0.15]: write SYNTHETIC SBS events (with control
// events around them) to an EVIO file, plus a pipeline config file next to it (<out>.cfg).
static int writeSynth(const std::string& db, const std::string& out, int nblocks, double occ) {
  std::string cfg = "db_dir = " + db +
                    "\ndate = 2024-10-01 00:00:00\narm = sbs\ngem_tracker = sbs.gem\nseed_source = vtp\ncalo = sbs.hcal\n"
                    "vtp_roc = 30\nvtp_bank_tag = 0x56\nmode = road\n";
  auto ctx = pipeline::PipelineContext::build(pipeline::PipelineConfig::parse(cfg));
  track::ToyGenerator g(ctx->gem, ctx->apvmap, 4242);
  track::ToyConfig tc; tc.occupancy = occ;
  std::uniform_int_distribution<int> rowd(6, 17), cold(3, 8);
  std::uniform_real_distribution<double> u(-0.015, 0.015);
  EvioFileSink sink(out);
  auto ctl = [&](uint16_t tag) { std::vector<uint32_t> e{4, (uint32_t(tag) << 16) | (0x01u << 8) | 0xCC, 0x5f000000u, 4242, 1}; sink.write(e.data(), e.size()); };
  ctl(0xFFD1); ctl(0xFFD2);
  uint32_t evn = 1;
  for (int b = 0; b < nblocks; ++b) {
    std::map<uint16_t, std::vector<std::vector<ApvFrame>>> by_crate;
    std::vector<seeds::VtpEvent> vtp;
    for (int e = 0; e < 5; ++e) {
      int row = rowd(g.rng()), col = cold(g.rng());
      const auto& blk = ctx->calo.blocks[size_t(ctx->calo.blockAt(row, col))];
      geom::Vec3 target{blk.x + ctx->calo.origin.x, blk.y + ctx->calo.origin.y, ctx->calo.origin.z};
      track::ToyTrack t; t.p0 = {u(g.rng()), u(g.rng()), 0}; t.dir = (target - t.p0).unit();
      auto ev = g.generate({t}, tc);
      for (size_t f = 0; f < ev.frames.size(); ++f) {
        auto& v = by_crate[ev.frames[f].addr.crate];
        v.resize(size_t(e) + 1);
        v[size_t(e)].push_back(ev.frames[f]);
      }
      seeds::VtpEvent ve; ve.trigger_number = evn + uint32_t(e); ve.slot = 11;
      seeds::VtpCluster cl; cl.det = seeds::VtpDet::HCal; cl.subtype = 13; cl.energy = 500;
      cl.row = uint32_t(row); cl.col = uint32_t(col); cl.nhits = 3; cl.time = 50;
      ve.clusters.push_back(cl); vtp.push_back(ve);
    }
    evioio::synth::PhysicsEventSpec sp; sp.block_level = 5; sp.first_event_number = evn;
    for (auto& [cr, v] : by_crate) {
      v.resize(5);
      evioio::synth::RocSpec r; r.roc_id = cr;
      r.banks.push_back({3561, 11, 0x01, codec::synth::write_block(v, 11, evn, uint32_t(b + 1))});
      sp.rocs.push_back(r);
    }
    evioio::synth::RocSpec rv; rv.roc_id = 30;
    rv.banks.push_back({0x56, 0, 0x01, seeds::encodeVtpBlock(11, vtp)});
    sp.rocs.push_back(rv);
    auto ev = evioio::synth::make_physics_event(sp);
    sink.write(ev.data(), ev.size());
    evn += 5;
  }
  ctl(0xFFD4);
  sink.close();
  std::ofstream(out + ".cfg") << cfg;
  std::fprintf(stderr, "wrote %d physics blocks (%d events) to %s and config %s.cfg\n", nblocks, 5 * nblocks, out.c_str(), out.c_str());
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::string(argv[1]) == "--synth")
    return writeSynth(argv[2], argv[3], argc > 4 ? std::atoi(argv[4]) : 200, argc > 5 ? std::atof(argv[5]) : 0.15);
  if (argc < 2) { std::fprintf(stderr, "usage: gemfilter_bench <DB dir> [nblocks] [out.csv]\n"); return 2; }
  std::string db = argv[1];
  int nblocks = argc > 2 ? std::atoi(argv[2]) : 40;
  std::ofstream o(argc > 3 ? argv[3] : "gemfilter_bench.csv");
  o << "geometry,mode,occupancy,nphys,strips_in_per_ev,strips_out_per_ev,strip_reduction,words_in_per_ev,"
       "words_out_per_ev,word_reduction,signal_retention,cpu_us_per_ev,fallback_frac,tracks_per_ev\n";
  {
    auto bb = geom::loadBigBiteConfig(geom::DbDir(db), geom::civilSeconds(2022, 1, 10));
    std::fprintf(stderr, "BigBite constraint widths from DB: front %.3f/%.3f back %.3f/%.3f m\n", bb.cw.front_wx,
                 bb.cw.front_wy, bb.cw.back_wx, bb.cw.back_wy);
  }
  for (double occ : {0.05, 0.10, 0.20, 0.30, 0.40})
    for (bool road : {true, false}) {
      Row a = runSbs(db, occ, road, nblocks, 100 + uint32_t(occ * 100));
      writeRow(o, a);
      Row b = runBb(db, occ, road, 5 * nblocks, 200 + uint32_t(occ * 100));
      writeRow(o, b);
      std::fprintf(stderr, "occ %.2f %s: sbs red %.1f ret %.4f cpu %.0f us | bb red %.1f ret %.4f cpu %.0f us\n", occ,
                   road ? "road " : "track", double(a.strips_in) / a.strips_out, double(a.sig_kept) / a.sig,
                   a.cpu_us / a.nphys, double(b.strips_in) / b.strips_out, double(b.sig_kept) / b.sig, b.cpu_us / b.nphys);
    }
  return 0;
}
