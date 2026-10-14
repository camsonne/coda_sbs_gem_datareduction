// gemfilter / tools / sbsdig2evio_verify : round-trip check of sbsdig2evio output.
// Decodes the EVIO file with the gemfilter production decoders (codec::decode for MPD banks,
// seeds::decodeFadc250 for FADC banks) and the FORWARD readout maps (geom::ApvMap: readout ->
// strip; CaloGeometry::blockForAdc: readout -> element), and compares with the .sdg input:
//   GEM : every (event, plane, strip) present exactly once, all 6 samples equal (after the
//         baseline subtraction / 13-bit clamp applied by the converter), no extra strips;
//   FADC: every (event, detector, sim channel) present, all samples equal, sample sums equal;
//   truth sidecar line count == number of GEM strips.
// Exit code 0 only if everything matches.
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "gemfilter/apps/event_io.h"
#include "gemfilter/codec/gem_codec.h"
#include "gemfilter/evioio/evio_event.h"
#include "gemfilter/seeds/Fadc250.h"
#include "readout.h"

using namespace gemfilter;
using namespace gemfilter::sbsdig;

int main(int argc, char** argv) {
  ReadoutConfig rc;
  std::string evio, sdg, truth;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> std::string { if (i + 1 >= argc) std::exit(2); return argv[++i]; };
    if (a == "-i") evio = val();
    else if (a == "--sdg") sdg = val();
    else if (a == "--truth") truth = val();
    else if (a == "--db") rc.db_dir = val();
    else if (a == "--date") rc.date = val();
    else if (a == "--gem") rc.gem = val();
    else if (a == "--calo") rc.calo = parseStrList(val());
    else if (a == "--module-map") rc.module_map = parseIntList(val());
    else if (a == "--gem-baseline") rc.gem_baseline = std::stoi(val());
    else { std::fprintf(stderr, "usage: sbsdig2evio_verify -i out.evio --sdg in.sdg --db DB [--date ..] [--truth f]\n"); return 2; }
  }
  auto R = Readout::load(rc);
  const auto& trk = *R->gem;
  std::set<uint16_t> fadc_tags;
  for (int k = 0; k < kNPmtDet; ++k)
    if (R->have_calo[k])
      for (auto& b : R->calo[k].blocks)
        if (b.adc_crate >= 0) { int bk = R->cmap.bank(b.adc_crate, b.adc_slot); fadc_tags.insert(uint16_t(bk >= 0 ? bk : 250)); }

  SdgReader rd(sdg);
  EvioFileSource src(evio);
  std::vector<uint32_t> buf;
  size_t nw = 0;
  struct { uint64_t events = 0, gem_exp = 0, gem_found = 0, gem_missing = 0, gem_extra = 0, gem_dup = 0, gem_samp_bad = 0,
           gem_unmapped_in = 0, pmt_exp = 0, pmt_found = 0, pmt_missing = 0, pmt_extra = 0, pmt_samp_bad = 0, pmt_sum_bad = 0,
           evnum_bad = 0, ctl = 0, decode_err = 0; } S;
  uint64_t expect_evn = 0;
  SdgEvent ev;
  bool sdg_done = false;
  while (src.next(buf, nw, 0)) {
    auto view = evioio::EventView::parse(buf.data(), nw);
    if (view.is_control()) { ++S.ctl; continue; }
    if (!view.is_physics()) continue;
    const int nev = view.block_level();
    if (expect_evn == 0) expect_evn = view.trigger().first_event_number;
    if (view.trigger().first_event_number != expect_evn) ++S.evnum_bad;
    expect_evn += uint64_t(nev);
    // decoded GEM: [event] (plane, strip) -> samples ; FADC: [event] (det, logical) -> samples
    std::vector<std::map<std::pair<int, int>, std::array<int16_t, 6>>> gem(static_cast<size_t>(nev));
    std::vector<std::map<std::pair<int, int>, std::vector<uint16_t>>> pmt(static_cast<size_t>(nev));
    for (const auto& b : view.banks()) {
      if (b.tag == 3561) {
        codec::DecodedBank db;
        try { db = codec::decode(b.payload, b.payload_len, b.roc_id); }
        catch (const std::exception& e) { ++S.decode_err; std::fprintf(stderr, "MPD decode error: %s\n", e.what()); continue; }
        for (uint32_t e = 0; e < db.nevents() && e < uint32_t(nev); ++e)
          for (uint32_t f = db.event_begin[e]; f < db.event_begin[e + 1]; ++f)
            for (const auto& h : db.frames[f].hits) {
              geom::StripRef s = R->apvmap->strip(db.frames[f].addr, h.apv_chan);
              if (!s.valid()) { ++S.gem_extra; continue; }
              int sm = R->simModule(s.module);
              auto key = std::make_pair(2 * sm + int(s.axis), s.strip);
              auto& m = gem[e];
              if (m.count(key)) ++S.gem_dup;
              std::array<int16_t, 6> a{};
              for (int i = 0; i < 6; ++i) a[size_t(i)] = h.adc[size_t(i)];
              m[key] = a;
            }
      } else if (fadc_tags.count(b.tag)) {
        auto fr = seeds::decodeFadc250(b.payload, b.payload_len);
        S.decode_err += fr.nerrors;
        for (size_t e = 0; e < fr.events.size() && e < size_t(nev); ++e)
          for (const auto& c : fr.events[e].channels) {
            if (c.samples.empty()) continue;
            int det = -1, l = -1;
            for (int k = 0; k < kNPmtDet && det < 0; ++k)
              if (R->have_calo[k]) {
                int bl = R->calo[k].blockForAdc(b.roc_id, c.slot, c.chan);
                if (bl >= 0) { det = k; l = bl; }
              }
            if (det < 0) { ++S.pmt_extra; continue; }
            pmt[e][{det, l}] = c.samples;
          }
      }
    }
    // compare with the .sdg events of this block
    for (int e = 0; e < nev; ++e) {
      if (!rd.next(ev)) { sdg_done = true; std::fprintf(stderr, "EVIO has more events than .sdg\n"); return 1; }
      ++S.events;
      auto& gm = gem[size_t(e)];
      for (const auto& g : ev.gem) {
        int dbm = R->dbModule(g.plane / 2);
        bool mapped = dbm >= 0 && size_t(dbm) < trk.modules.size() && g.strip < trk.modules[size_t(dbm)].nstrips[g.plane % 2];
        auto it = gm.find({g.plane, g.strip});
        if (!mapped || it == gm.end()) {
          if (!mapped) ++S.gem_unmapped_in; else ++S.gem_missing;
          continue;
        }
        ++S.gem_exp;
        ++S.gem_found;
        for (size_t i = 0; i < 6; ++i) {
          int16_t want = i < g.s.size() ? gemSampleToMpd(g.s[i], rc.gem_baseline) : int16_t(0);
          if (it->second[i] != want) { ++S.gem_samp_bad; break; }
        }
        gm.erase(it);
      }
      S.gem_extra += gm.size();
      auto& pm = pmt[size_t(e)];
      for (const auto& p : ev.pmt) {
        if (p.det >= kNPmtDet || !R->have_calo[p.det] || p.s.empty()) continue;
        ++S.pmt_exp;
        int l = simChanToLogical(p.det, p.chan);
        auto it = pm.find({int(p.det), l});
        if (it == pm.end()) { ++S.pmt_missing; continue; }
        ++S.pmt_found;
        long s_in = 0, s_out = 0;
        bool bad = it->second.size() != p.s.size();
        for (size_t i = 0; i < p.s.size(); ++i) {
          uint16_t want = std::min<uint16_t>(p.s[i], 4095);
          s_in += want;
          if (i < it->second.size()) { s_out += (it->second[i] & 0xFFF); bad |= ((it->second[i] & 0x1FFF) != want); }
        }
        S.pmt_samp_bad += bad;
        S.pmt_sum_bad += (s_in != s_out);
        pm.erase(it);
      }
      S.pmt_extra += pm.size();
    }
  }
  if (!sdg_done && rd.next(ev)) { std::fprintf(stderr, ".sdg has more events than EVIO\n"); return 1; }
  long truth_lines = -1;
  if (!truth.empty()) {
    std::ifstream tf(truth);
    std::string line;
    truth_lines = 0;
    while (std::getline(tf, line)) if (!line.empty() && line[0] != '#') ++truth_lines;
  }
  uint64_t gem_total = S.gem_found + S.gem_missing + S.gem_unmapped_in;
  std::printf("verify: events %llu, control %llu | GEM strips expected %llu found %llu missing %llu extra %llu dup %llu "
              "sample-mismatch %llu unmapped-in-input %llu | FADC channels expected %llu found %llu missing %llu extra %llu "
              "sample-mismatch %llu sum-mismatch %llu | evnum errors %llu decode errors %llu | truth lines %ld (strips %llu)\n",
              (unsigned long long)S.events, (unsigned long long)S.ctl, (unsigned long long)S.gem_exp, (unsigned long long)S.gem_found,
              (unsigned long long)S.gem_missing, (unsigned long long)S.gem_extra, (unsigned long long)S.gem_dup,
              (unsigned long long)S.gem_samp_bad, (unsigned long long)S.gem_unmapped_in, (unsigned long long)S.pmt_exp,
              (unsigned long long)S.pmt_found, (unsigned long long)S.pmt_missing, (unsigned long long)S.pmt_extra,
              (unsigned long long)S.pmt_samp_bad, (unsigned long long)S.pmt_sum_bad, (unsigned long long)S.evnum_bad,
              (unsigned long long)S.decode_err, truth_lines, (unsigned long long)gem_total);
  bool ok = S.events > 0 && S.gem_missing == 0 && S.gem_extra == 0 && S.gem_dup == 0 && S.gem_samp_bad == 0 &&
            S.pmt_missing == 0 && S.pmt_extra == 0 && S.pmt_samp_bad == 0 && S.pmt_sum_bad == 0 && S.evnum_bad == 0 &&
            S.decode_err == 0 && S.ctl == 3 && (truth_lines < 0 || uint64_t(truth_lines) == gem_total);
  std::printf("verify: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
