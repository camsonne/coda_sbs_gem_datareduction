// gemfilter / tools / sbsdig2evio : libsbsdig digitized events (.sdg, from sbsdig_extract.py)
// -> CODA3 built physics events in REAL readout format.
//
//   * GEM (Earm.BBGEM.dighit): sim plane p -> DB module m = module_map[p/2], axis p%2, strip s
//     -> (crate, slot, fiber, adc, apv_chan) by inverting the real decode map (chanmap rows +
//     APV channel map, geom::ApvMap), samples baseline-subtracted to signed 13 bit, written as
//     MPD bank 3561 in the VTP-era block layout by codec::synth::write_block (one block per
//     slot, block level = --block-level).
//   * BBSH / BBPS / HCal (dighit FADC samples): sim chan -> logical channel as SBSSimDecoder ->
//     calorimeter element -> (crate, slot, chan) from detmap/chanmap -> FADC250 window raw
//     data (type 4) by seeds::encodeFadc250Block, bank tag from db_cratemap ("bank" column, else 250).
//   * Control events: prestart (0xFFD1), go (0xFFD2) before, end (0xFFD4) after.
//   * Truth sidecar (TSV, one line per GEM strip): event crate slot fiber adc chan module axis
//     strip sig maxadc entry.  sig: 1 signal, 0 background, -1 unknown (see docs/sbsdig_mapping.md).
//
// The trigger bank (event numbers, timestamps, event types) and the MPD/FADC trigger-time words
// are SYNTHETIC placeholders (simulation has no TS); HCal/hodoscope TDCs are not written.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "gemfilter/apps/event_io.h"
#include "gemfilter/codec/synthetic.h"
#include "gemfilter/evioio/synthetic_event.h"
#include "gemfilter/seeds/Fadc250.h"
#include "readout.h"

using namespace gemfilter;
using namespace gemfilter::sbsdig;

namespace {

struct ChanRef { ApvAddress addr; uint8_t chan = 0; bool ok = false; };

void usage() {
  std::fprintf(stderr,
      "usage: sbsdig2evio -i in.sdg -o out.evio --db <Podd DB dir> [options]\n"
      "  --date \"YYYY-MM-DD hh:mm:ss\"  DB time stamp (default 2023-06-01 00:00:00 = GEn-II layout)\n"
      "  --gem bb.gem                  GEM tracker DB name\n"
      "  --calo bb.sh,bb.ps,sbs.hcal   calorimeters to write (subset; \"\" none)\n"
      "  --module-map 0,1,...          sim module (plane/2) -> DB module index (default identity)\n"
      "  --gem-baseline 1500           common-mode baseline subtracted from sim GEM samples\n"
      "  --block-level 5               physics events per CODA event\n"
      "  --run 1  --first-event 1  --max-events N\n"
      "  --truth out.truth.tsv         GEM truth sidecar (default <out>.truth.tsv)\n"
      "  --dump-map map.tsv            write the full sim->readout GEM strip map and exit\n");
}

}  // namespace

int main(int argc, char** argv) {
  ReadoutConfig rc;
  std::string in, out, truth_path, dump_map;
  int block_level = 5;
  uint32_t run = 1, first_event = 1;
  long max_events = -1;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) { usage(); std::exit(2); }
      return argv[++i];
    };
    if (a == "-i") in = val();
    else if (a == "-o") out = val();
    else if (a == "--db") rc.db_dir = val();
    else if (a == "--date") rc.date = val();
    else if (a == "--gem") rc.gem = val();
    else if (a == "--calo") rc.calo = parseStrList(val());
    else if (a == "--module-map") rc.module_map = parseIntList(val());
    else if (a == "--gem-baseline") rc.gem_baseline = std::stoi(val());
    else if (a == "--block-level") block_level = std::stoi(val());
    else if (a == "--run") run = uint32_t(std::stoul(val()));
    else if (a == "--first-event") first_event = uint32_t(std::stoul(val()));
    else if (a == "--max-events") max_events = std::stol(val());
    else if (a == "--truth") truth_path = val();
    else if (a == "--dump-map") dump_map = val();
    else { usage(); return 2; }
  }
  if (rc.db_dir.empty() || (dump_map.empty() && (in.empty() || out.empty()))) { usage(); return 2; }
  if (block_level < 1 || block_level > 255) { std::fprintf(stderr, "bad --block-level\n"); return 2; }
  if (truth_path.empty() && !out.empty()) truth_path = out + ".truth.tsv";

  std::unique_ptr<Readout> R;
  try { R = Readout::load(rc); }
  catch (const std::exception& e) { std::fprintf(stderr, "DB error: %s\n", e.what()); return 1; }
  const auto& trk = *R->gem;

  // ---- inverse GEM map: [db module][axis][strip] -> readout channel
  std::vector<std::vector<ChanRef>> inv(trk.modules.size() * 2);
  for (size_t m = 0; m < trk.modules.size(); ++m)
    for (int a = 0; a < 2; ++a) inv[2 * m + size_t(a)].assign(size_t(std::max(0, trk.modules[m].nstrips[a])), ChanRef{});
  size_t ndup = 0;
  for (size_t m = 0; m < trk.modules.size(); ++m)
    for (const auto& e : trk.modules[m].apvs) {
      ApvAddress ad;
      ad.crate = e.crate; ad.slot = e.slot; ad.fiber = e.mpd_id; ad.mpd = e.mpd_id; ad.adc = e.adc_id;
      for (int ch = 0; ch < geom::kApvChannels; ++ch) {
        geom::StripRef s = R->apvmap->strip(ad, ch);
        if (!s.valid()) continue;
        auto& v = inv[size_t(2 * s.module + int(s.axis))];
        if (s.strip >= int(v.size())) continue;
        if (v[size_t(s.strip)].ok) ++ndup;
        v[size_t(s.strip)] = {ad, uint8_t(ch), true};
      }
    }
  size_t nmapped = 0, ntot = 0;
  for (auto& v : inv) for (auto& c : v) { ++ntot; nmapped += c.ok; }
  std::fprintf(stderr, "sbsdig2evio: %s: %zu modules, %zu/%zu strips mapped to readout, %zu duplicate strip assignments\n",
               trk.dbfile.c_str(), trk.modules.size(), nmapped, ntot, ndup);

  if (!dump_map.empty()) {
    std::ofstream mf(dump_map);
    mf << "# sim_plane\tstrip\tdb_module\tmodule_name\taxis\tcrate\tslot\tfiber\tadc\tapv_chan\n";
    for (size_t m = 0; m < trk.modules.size(); ++m) {
      int sm = R->simModule(int(m));
      for (int a = 0; a < 2; ++a) {
        const auto& v = inv[2 * m + size_t(a)];
        for (size_t s = 0; s < v.size(); ++s) {
          const auto& c = v[s];
          mf << (sm >= 0 ? 2 * sm + a : -1) << '\t' << s << '\t' << m << '\t' << trk.modules[m].name << '\t' << a << '\t';
          if (c.ok) mf << c.addr.crate << '\t' << c.addr.slot << '\t' << c.addr.fiber << '\t' << c.addr.adc << '\t' << int(c.chan) << '\n';
          else mf << "-1\t-1\t-1\t-1\t-1\n";
        }
      }
    }
    // calorimeters: sim chan -> logical element -> FADC (crate, slot, chan)
    mf << "# CALO\tdet\tsim_chan\telement\trow\tcol\tcrate\tslot\tchan\tbank_tag\n";
    for (int k = 0; k < kNPmtDet; ++k) {
      if (!R->have_calo[k]) continue;
      const auto& G = R->calo[k];
      for (int c = 0; c < G.nelem(); ++c) {
        int l = simChanToLogical(k, c);
        mf << "CALO\t" << pmtDetName(k) << '\t' << c << '\t' << l << '\t';
        if (l >= 0 && l < G.nelem() && G.blocks[size_t(l)].adc_crate >= 0) {
          const auto& b = G.blocks[size_t(l)];
          int bk = R->cmap.bank(b.adc_crate, b.adc_slot);
          mf << b.row << '\t' << b.col << '\t' << b.adc_crate << '\t' << b.adc_slot << '\t' << b.adc_chan << '\t' << (bk >= 0 ? bk : 250) << '\n';
        } else {
          mf << "-1\t-1\t-1\t-1\t-1\t-1\n";
        }
      }
    }
    std::fprintf(stderr, "wrote %s\n", dump_map.c_str());
    if (in.empty()) return 0;
  }

  // ---- FADC bank tag per (crate, slot)
  auto fadcBankTag = [&](int cr, int sl) {
    int bk = R->cmap.bank(cr, sl);
    return uint16_t(bk >= 0 ? bk : 250);
  };
  // all FADC slots per crate that host any calorimeter channel (written even when empty)
  std::map<uint16_t, std::set<uint16_t>> fadc_slots;
  for (int k = 0; k < kNPmtDet; ++k)
    if (R->have_calo[k])
      for (auto& b : R->calo[k].blocks)
        if (b.adc_crate >= 0) fadc_slots[uint16_t(b.adc_crate)].insert(uint16_t(b.adc_slot));
  // GEM (crate, slot) pairs, written even when empty
  std::map<uint16_t, std::set<uint16_t>> gem_slots;
  for (auto& m : trk.modules) for (auto& e : m.apvs) gem_slots[e.crate].insert(e.slot);

  SdgReader rd(in);
  EvioFileSink sink(out);
  std::ofstream tf(truth_path);
  const char* tsrc = (rd.flags() & 1u) ? "dighit_sig_branch" : (rd.flags() & 2u) ? "g4sbs_hit_geometry" : "none";
  tf << "# sbsdig2evio GEM truth sidecar. truth_source=" << tsrc << " db=" << trk.dbfile << " date=" << rc.date
     << " gem_baseline=" << rc.gem_baseline << "\n"
     << "# event\tcrate\tslot\tfiber\tadc\tchan\tmodule\taxis\tstrip\tsig\tmaxadc\tentry\n";

  auto ctl = [&](uint16_t tag) { auto e = evioio::synth::make_control_event(tag, run, 0, 0); sink.write(e.data(), e.size()); };
  ctl(0xFFD1);
  ctl(0xFFD2);

  struct Stats { uint64_t events = 0, blocks = 0, gem_strips = 0, gem_unmapped = 0, gem_badplane = 0, pmt_ch = 0, pmt_unmapped = 0,
                 sig = 0, bkg = 0, unk = 0, words = 0; } st;
  uint32_t evn = first_event, block_number = 0;
  bool more = true;
  SdgEvent ev;
  while (more) {
    // ---- collect one CODA block
    using FrameKey = std::tuple<uint16_t, uint16_t, uint16_t, uint16_t>;   // crate slot fiber adc
    std::vector<std::map<FrameKey, ApvFrame>> gem_ev;
    // fadc[crate][slot][event] -> channels
    std::map<uint16_t, std::map<uint16_t, std::vector<std::vector<seeds::FadcSynthChannel>>>> fadc;
    int nev = 0;
    while (nev < block_level) {
      if (max_events >= 0 && long(st.events) >= max_events) { more = false; break; }
      if (!rd.next(ev)) { more = false; break; }
      const uint32_t this_evn = evn + uint32_t(nev);
      gem_ev.emplace_back();
      auto& fr = gem_ev.back();
      for (const auto& g : ev.gem) {
        ++st.gem_strips;
        int dbm = R->dbModule(g.plane / 2), ax = g.plane % 2;
        const ChanRef* c = nullptr;
        if (dbm >= 0 && size_t(dbm) < trk.modules.size()) {
          const auto& v = inv[size_t(2 * dbm + ax)];
          if (g.strip < v.size() && v[g.strip].ok) c = &v[g.strip];
        } else {
          ++st.gem_badplane;
        }
        int maxadc = -99999;
        StripHit h;
        h.nsamples = uint8_t(std::min<size_t>(g.s.size(), kMaxSamples));
        for (size_t i = 0; i < size_t(kMaxSamples); ++i) {
          int16_t v = i < g.s.size() ? gemSampleToMpd(g.s[i], rc.gem_baseline) : int16_t(0);
          h.adc[i] = v;
          maxadc = std::max<int>(maxadc, v);
        }
        if (g.truth > 0) ++st.sig; else if (g.truth == 0) ++st.bkg; else ++st.unk;
        if (!c) {
          ++st.gem_unmapped;
          tf << this_evn << "\t-1\t-1\t-1\t-1\t-1\t" << dbm << '\t' << ax << '\t' << g.strip << '\t' << int(g.truth) << '\t' << maxadc << '\t' << ev.entry << '\n';
          continue;
        }
        FrameKey k{c->addr.crate, c->addr.slot, c->addr.fiber, c->addr.adc};
        auto& f = fr[k];
        f.addr = c->addr;
        h.apv_chan = c->chan;
        f.hits.push_back(h);
        tf << this_evn << '\t' << c->addr.crate << '\t' << c->addr.slot << '\t' << c->addr.fiber << '\t' << c->addr.adc << '\t'
           << int(c->chan) << '\t' << dbm << '\t' << ax << '\t' << g.strip << '\t' << int(g.truth) << '\t' << maxadc << '\t' << ev.entry << '\n';
      }
      for (const auto& p : ev.pmt) {
        ++st.pmt_ch;
        if (p.det >= kNPmtDet || !R->have_calo[p.det] || p.s.empty()) { ++st.pmt_unmapped; continue; }
        const auto& G = R->calo[p.det];
        int l = simChanToLogical(p.det, p.chan);
        if (l < 0 || l >= G.nelem() || G.blocks[size_t(l)].adc_crate < 0) { ++st.pmt_unmapped; continue; }
        const auto& b = G.blocks[size_t(l)];
        auto& slotv = fadc[uint16_t(b.adc_crate)][uint16_t(b.adc_slot)];
        slotv.resize(size_t(nev) + 1);
        seeds::FadcSynthChannel ch;
        ch.chan = b.adc_chan;
        ch.samples.reserve(p.s.size());
        for (uint16_t s : p.s) ch.samples.push_back(std::min<uint16_t>(s, 4095));
        slotv[size_t(nev)].push_back(std::move(ch));
      }
      ++nev;
      ++st.events;
    }
    if (nev == 0) break;
    ++block_number;
    // ---- build ROC banks
    std::map<uint16_t, evioio::synth::RocSpec> rocs;
    for (auto& [cr, slots] : gem_slots) {
      std::vector<uint32_t> payload;
      for (uint16_t sl : slots) {
        std::vector<std::vector<ApvFrame>> evs(static_cast<size_t>(nev));
        for (int e = 0; e < nev; ++e)
          for (auto& [k, f] : gem_ev[size_t(e)])
            if (std::get<0>(k) == cr && std::get<1>(k) == sl) {
              ApvFrame fc = f;
              std::sort(fc.hits.begin(), fc.hits.end(), [](const StripHit& x, const StripHit& y) { return x.apv_chan < y.apv_chan; });
              for (size_t j = 0; j < fc.hits.size(); ++j) fc.hits[j].src_index = uint32_t(j);
              evs[size_t(e)].push_back(std::move(fc));
            }
        auto w = codec::synth::write_block(evs, sl, evn, block_number, true);
        payload.insert(payload.end(), w.begin(), w.end());
      }
      auto& r = rocs[cr];
      r.roc_id = cr;
      r.banks.push_back({3561, uint8_t(*slots.begin()), 0x01, std::move(payload)});
    }
    for (auto& [cr, slots] : fadc_slots) {
      std::map<uint16_t, std::vector<seeds::FadcSynthSlot>> by_bank;
      for (uint16_t sl : slots) {
        seeds::FadcSynthSlot s;
        s.slot = sl;
        s.events.assign(size_t(nev), {});
        auto it = fadc.find(cr);
        if (it != fadc.end()) {
          auto jt = it->second.find(sl);
          if (jt != it->second.end())
            for (size_t e = 0; e < jt->second.size() && e < size_t(nev); ++e) {
              auto chs = jt->second[e];
              std::sort(chs.begin(), chs.end(), [](const auto& x, const auto& y) { return x.chan < y.chan; });
              s.events[e] = std::move(chs);
            }
        }
        by_bank[fadcBankTag(cr, sl)].push_back(std::move(s));
      }
      auto& r = rocs[cr];
      r.roc_id = cr;
      for (auto& [tag, sv] : by_bank) r.banks.push_back({tag, 0, 0x01, seeds::encodeFadc250Block(sv, evn, block_number)});
    }
    evioio::synth::PhysicsEventSpec sp;
    sp.block_level = uint8_t(nev);
    sp.first_event_number = evn;
    sp.tsroc = uint16_t(R->cmap.tsroc());
    for (auto& [cr, r] : rocs) sp.rocs.push_back(std::move(r));
    auto out_ev = evioio::synth::make_physics_event(sp);
    sink.write(out_ev.data(), out_ev.size());
    st.words += out_ev.size();
    ++st.blocks;
    evn += uint32_t(nev);
  }
  ctl(0xFFD4);
  sink.close();
  std::fprintf(stderr,
      "sbsdig2evio: %llu events in %llu CODA blocks (%.1f MB physics), GEM strips %llu (unmapped %llu, bad plane %llu; "
      "truth sig %llu bkg %llu unknown %llu [%s]), PMT channels %llu (unmapped %llu)\n  -> %s, truth %s\n",
      (unsigned long long)st.events, (unsigned long long)st.blocks, st.words * 4.0 / 1e6, (unsigned long long)st.gem_strips,
      (unsigned long long)st.gem_unmapped, (unsigned long long)st.gem_badplane, (unsigned long long)st.sig,
      (unsigned long long)st.bkg, (unsigned long long)st.unk, tsrc, (unsigned long long)st.pmt_ch,
      (unsigned long long)st.pmt_unmapped, out.c_str(), truth_path.c_str());
  return 0;
}
