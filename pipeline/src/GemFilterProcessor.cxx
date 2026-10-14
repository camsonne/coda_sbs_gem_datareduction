// gemfilter / pipeline : see GemFilterProcessor.h
#include "gemfilter/pipeline/GemFilterProcessor.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "gemfilter/evioio/evio_event.h"
#include "gemfilter/seeds/Fadc250.h"

namespace gemfilter {
namespace pipeline {

namespace {
std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> v; std::string t; std::stringstream ss(s);
  while (std::getline(ss, t, sep)) { t = trim(t); if (!t.empty()) v.push_back(t); }
  return v;
}
bool toBool(const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; }
}  // namespace

PipelineConfig PipelineConfig::parse(const std::string& text) {
  PipelineConfig c;
  bool cluster_override = false;
  std::stringstream ss(text);
  std::string line;
  int ln = 0;
  while (std::getline(ss, line)) {
    ++ln;
    auto h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    line = trim(line);
    if (line.empty()) continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) throw std::invalid_argument("pipeline config line " + std::to_string(ln) + ": no '='");
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    auto D = [&] { return std::stod(v); };
    auto I = [&] { return std::stoi(v); };
    if (k == "db_dir") c.db_dir = v;
    else if (k == "date") {
      auto t = geom::parseDateTime(v);
      if (!t) throw std::invalid_argument("bad date '" + v + "'");
      c.date = *t;
    } else if (k == "arm") {
      if (v == "bb" || v == "bigbite") c.arm = Arm::BigBite;
      else if (v == "sbs") c.arm = Arm::SBS;
      else throw std::invalid_argument("arm must be bb|sbs");
    } else if (k == "gem_tracker") c.gem_tracker = v;
    else if (k == "gem_bank_tags") { c.gem_bank_tags.clear(); for (auto& t : split(v, ',')) c.gem_bank_tags.push_back(uint16_t(std::stoul(t, nullptr, 0))); }
    else if (k == "seed_source") {
      if (v == "fadc") c.seed_source = SeedSource::FADC;
      else if (v == "vtp") c.seed_source = SeedSource::VTP;
      else if (v == "none") c.seed_source = SeedSource::None;
      else throw std::invalid_argument("seed_source must be fadc|vtp|none");
    } else if (k == "calo") c.calo = v;
    else if (k == "calo_ps") c.calo_ps = (v == "none") ? "" : v;
    else if (k == "vtp_roc") c.vtp_roc = I();
    else if (k == "vtp_bank_tag") c.vtp_bank_tag = uint16_t(std::stoul(v, nullptr, 0));
    else if (k == "vtp_energy_scale") c.vtp.energy_scale = D();
    else if (k == "vtp_row_offset") c.vtp.row_offset = I();
    else if (k == "vtp_col_offset") c.vtp.col_offset = I();
    else if (k == "seed_emin") c.seed_emin = D();
    else if (k == "max_seeds") c.max_seeds = I();
    else if (k == "cluster_emin_seed") { c.cluster.emin_seed = D(); cluster_override = true; }
    else if (k == "cluster_emin_block") { c.cluster.emin_block = D(); cluster_override = true; }
    else if (k == "cluster_tmax") { c.cluster.tmax = D(); cluster_override = true; }
    else if (k == "no_seed") {
      if (v == "keep") c.no_seed = NoSeed::KeepAll;
      else if (v == "drop") c.no_seed = NoSeed::DropAll;
      else if (v == "global") c.no_seed = NoSeed::Global;
      else throw std::invalid_argument("no_seed must be keep|drop|global");
    } else if (k == "road_width_scale") c.road.width_scale = D();
    else if (k == "road_margin") c.road.margin = D();
    else if (k == "mode") {
      if (v == "road") c.tracker.mode = track::TrackerConfig::Mode::RoadOnly;
      else if (v == "track") c.tracker.mode = track::TrackerConfig::Mode::Track;
      else throw std::invalid_argument("mode must be road|track");
    } else if (k == "min_layers") c.tracker.min_layers = I();
    else if (k == "search_window") c.tracker.search_window = D();
    else if (k == "sigma_hit") c.tracker.sigma_hit = D();
    else if (k == "max_chi2ndf") c.tracker.max_chi2ndf = D();
    else if (k == "max_tracks") c.tracker.max_tracks = I();
    else if (k == "keep_margin_strips") c.tracker.keep_margin_strips = I();
    else if (k == "keep_track_tol") c.tracker.keep_track_tol = D();
    else if (k == "max_asym") c.tracker.max_asym = D();
    else if (k == "max_dt_ns") c.tracker.max_dt_ns = D();
    else if (k == "max_strip_gap") c.tracker.max_strip_gap = I();
    else if (k == "max_cluster_size") c.tracker.max_cluster_size = I();
    else if (k == "peak_sample_min") c.tracker.peak_sample_min = I();
    else if (k == "peak_sample_max") c.tracker.peak_sample_max = I();
    else if (k == "fallback_road") c.tracker.fallback_road_if_no_track = toBool(v);
    else if (k == "max_hits2d_track") c.tracker.max_hits2d_track = I();
    else if (k == "min_layers_confident") c.tracker.min_layers_confident = I();
    else if (k == "filler") c.encode.filler_policy = (v == "realign") ? codec::FillerPolicy::Realign : codec::FillerPolicy::Preserve;
    else if (k == "fiber_bits") c.decode.fiber_bits = I();
    else if (k == "strict_counts") c.decode.strict_counts = toBool(v);
    else if (k == "apv_key") c.apv_key = (v == "mpd") ? geom::ApvKeyField::Mpd : geom::ApvKeyField::Fiber;
    else if (k == "apv_match_slot") c.apv_match_slot = toBool(v);
    else throw std::invalid_argument("pipeline config: unknown key '" + k + "'");
  }
  c.cluster_from_db = !cluster_override;
  return c;
}

PipelineConfig PipelineConfig::load(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::invalid_argument("cannot open pipeline config '" + path + "'");
  std::stringstream ss; ss << f.rdbuf();
  return parse(ss.str());
}

std::string PipelineConfig::to_string() const {
  std::ostringstream o;
  o << "db_dir=" << db_dir << "\ndate=" << geom::formatDateTime(date) << "\narm=" << (arm == Arm::BigBite ? "bb" : "sbs")
    << "\ngem_tracker=" << gem_tracker << "\ngem_bank_tags=";
  for (size_t i = 0; i < gem_bank_tags.size(); ++i) o << (i ? "," : "") << gem_bank_tags[i];
  o << "\nseed_source=" << int(seed_source) << "\ncalo=" << calo << "\ncalo_ps=" << calo_ps
    << "\nvtp=" << vtp_roc << "/" << vtp_bank_tag << "/" << vtp.energy_scale << "\nseed_emin=" << seed_emin
    << "\nmax_seeds=" << max_seeds << "\nno_seed=" << int(no_seed) << "\nroad=" << road.width_scale << "/" << road.margin
    << "\nmode=" << int(tracker.mode) << "\nmin_layers=" << tracker.min_layers << "\nwindow=" << tracker.search_window
    << "\nchi2=" << tracker.max_chi2ndf << "\nmargin=" << tracker.keep_margin_strips << "\ntol=" << tracker.keep_track_tol
    << "\nasym=" << tracker.max_asym << "\ndt=" << tracker.max_dt_ns << "\nfallback=" << tracker.fallback_road_if_no_track << "/" << tracker.min_layers_confident << "/" << tracker.max_hits2d_track
    << "\npeak=" << tracker.peak_sample_min << "-" << tracker.peak_sample_max << "\nfiller=" << int(encode.filler_policy) << "\n";
  return o.str();
}

// ---------------------------------------------------------------- context
void PipelineContext::finish() {
  apvmap = geom::ApvMap(gem, cfg.apv_key, cfg.apv_match_slot);
  if (cfg.cluster_from_db && !calo.blocks.empty()) cfg.cluster = seeds::ClusterOptions::fromDb(calo);
  // FADC banks of the calorimeter crates
  std::set<std::pair<uint16_t, uint16_t>> fb;
  auto addCalo = [&](const geom::CaloGeometry& g) {
    std::set<int> crates;
    for (auto& b : g.blocks) if (b.adc_crate >= 0) crates.insert(b.adc_crate);
    for (int cr : crates)
      for (int sl : cratemap.slotsWithModel(cr, 250)) {
        int bk = cratemap.bank(cr, sl);
        fb.insert({uint16_t(cr), uint16_t(bk >= 0 ? bk : 250)});
      }
  };
  if (!calo.blocks.empty()) addCalo(calo);
  if (have_ps) addCalo(calo_ps);
  fadc_banks.assign(fb.begin(), fb.end());
}

std::shared_ptr<const PipelineContext> PipelineContext::build(const PipelineConfig& cfg) {
  auto c = std::make_shared<PipelineContext>();
  c->cfg = cfg;
  geom::DbDir dir(cfg.db_dir);
  c->gem = geom::loadGemTracker(dir, cfg.gem_tracker, cfg.date);
  if (cfg.seed_source != PipelineConfig::SeedSource::None) {
    c->cratemap = geom::CrateMap::load(dir, cfg.date);
    geom::CaloLoadOptions lo;
    c->calo = geom::loadCalorimeter(dir, cfg.calo, cfg.date, &c->cratemap, lo);
    if (cfg.arm == PipelineConfig::Arm::BigBite && !cfg.calo_ps.empty() && cfg.seed_source == PipelineConfig::SeedSource::FADC) {
      c->calo_ps = geom::loadCalorimeter(dir, cfg.calo_ps, cfg.date, &c->cratemap, lo);
      c->have_ps = true;
    }
    if (cfg.arm == PipelineConfig::Arm::BigBite) c->bb = geom::loadBigBiteConfig(dir, cfg.date);
    else c->sbs = geom::loadSbsArmConfig(dir, cfg.date);
  }
  c->finish();
  return c;
}

std::shared_ptr<const PipelineContext> PipelineContext::build(const PipelineConfig& cfg, geom::GemTracker gem,
                                                              geom::CaloGeometry calo) {
  auto c = std::make_shared<PipelineContext>();
  c->cfg = cfg;
  c->gem = std::move(gem);
  c->calo = std::move(calo);
  c->finish();
  return c;
}

// ---------------------------------------------------------------- processor
GemFilterProcessor::GemFilterProcessor(std::shared_ptr<const PipelineContext> ctx)
    : ctx_(std::move(ctx)), filter_(ctx_->gem, ctx_->apvmap, ctx_->cfg.tracker) {}

std::vector<seeds::ConstraintPoints> GemFilterProcessor::constraints(const std::vector<CaloSeed>& sh,
                                                                     const std::vector<CaloSeed>& ps) const {
  const auto& c = ctx_->cfg;
  std::vector<size_t> order;
  for (size_t i = 0; i < sh.size(); ++i) if (sh[i].E >= c.seed_emin) order.push_back(i);
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sh[a].E > sh[b].E; });
  if (int(order.size()) > c.max_seeds) order.resize(size_t(std::max(0, c.max_seeds)));
  std::vector<seeds::ConstraintPoints> out;
  for (size_t i : order) {
    seeds::ConstraintPoints cp;
    if (c.arm == PipelineConfig::Arm::BigBite) {
      const CaloSeed* p = (i < ps.size() && ps[i].nblocks > 0) ? &ps[i] : nullptr;
      cp = seeds::bigbiteConstraint(ctx_->bb, ctx_->calo, sh[i], ctx_->have_ps ? &ctx_->calo_ps : nullptr, p);
    } else {
      cp = seeds::sbsConstraint(ctx_->sbs, ctx_->calo, sh[i], 0);
    }
    cp.seed_index = int(i);
    out.push_back(cp);
  }
  return out;
}

FilterStats GemFilterProcessor::process(uint32_t* buf, size_t& nwords, size_t cap) {
  FilterStats st;
  details_.clear();
  const auto& C = ctx_->cfg;
  evioio::EventView view = evioio::EventView::parse(buf, nwords);
  const uint32_t nev = std::max<uint32_t>(1, view.block_level());

  // 1. GEM banks
  std::vector<size_t> gem_idx;
  for (size_t i = 0; i < view.banks().size(); ++i) {
    const auto& b = view.banks()[i];
    if (std::find(C.gem_bank_tags.begin(), C.gem_bank_tags.end(), b.tag) != C.gem_bank_tags.end()) gem_idx.push_back(i);
  }
  if (gem_idx.empty()) return st;
  banks_.resize(gem_idx.size());
  masks_.resize(gem_idx.size());
  for (size_t k = 0; k < gem_idx.size(); ++k) {
    const auto& b = view.banks()[gem_idx[k]];
    codec::decode_into(b.payload, b.payload_len, b.roc_id, banks_[k], C.decode);
    masks_[k] = banks_[k].keep_all();
    st.bytes_in += uint32_t(4 * b.payload_len);
    for (auto& f : banks_[k].frames) st.strips_in += uint32_t(f.hits.size());
  }

  // 2. seed data for the block
  std::vector<seeds::FadcDecodeResult> fadc;
  std::vector<uint16_t> fadc_crate;
  seeds::VtpDecodeResult vtp;
  if (C.seed_source == PipelineConfig::SeedSource::FADC) {
    for (auto& [roc, tag] : ctx_->fadc_banks)
      for (size_t bi : view.find_banks(roc, tag)) {
        const auto& b = view.banks()[bi];
        fadc.push_back(seeds::decodeFadc250(b.payload, b.payload_len));
        fadc_crate.push_back(roc);
      }
  } else if (C.seed_source == PipelineConfig::SeedSource::VTP && C.vtp_roc >= 0) {
    for (size_t bi : view.find_banks(C.vtp_roc, C.vtp_bank_tag)) {
      const auto& b = view.banks()[bi];
      vtp = seeds::decodeVtp(b.payload, b.payload_len);
    }
  }

  // 3. per physics event
  std::vector<const ApvFrame*> frames;
  std::vector<std::pair<uint32_t, uint32_t>> where;   // (bank k, frame index in bank)
  uint16_t nroads = 0, ntracks = 0, nseeds = 0;
  bool any_noseed = false;
  for (uint32_t e = 0; e < nev; ++e) {
    frames.clear(); where.clear();
    for (size_t k = 0; k < banks_.size(); ++k) {
      const auto& B = banks_[k];
      if (e >= B.nevents()) continue;
      for (uint32_t fi = B.event_begin[e]; fi < B.event_begin[e + 1]; ++fi) {
        frames.push_back(&B.frames[fi]);
        where.push_back({uint32_t(k), fi});
      }
    }
    EventDetail d;
    for (auto* f : frames) d.strips_in += uint32_t(f->hits.size());
    // seeds
    std::vector<CaloSeed> sh, ps;
    if (C.seed_source == PipelineConfig::SeedSource::FADC) {
      seeds::FadcEventStore store;
      for (size_t j = 0; j < fadc.size(); ++j)
        if (e < fadc[j].events.size()) store.add(fadc_crate[j], fadc[j].events[e]);
      auto hits = seeds::blockHitsFromFadc(ctx_->calo, store);
      auto arm = C.arm == PipelineConfig::Arm::BigBite ? CaloSeed::Arm::BigBite : CaloSeed::Arm::SBS;
      sh = seeds::clusterBlocks(ctx_->calo, hits, C.cluster, arm);
      if (ctx_->have_ps && !sh.empty()) {
        auto ph = seeds::blockHitsFromFadc(ctx_->calo_ps, store);
        auto m = seeds::matchPreshower(sh, ctx_->calo_ps, ph, 0.3, 0.4, C.cluster.tmax);
        ps.resize(sh.size());
        for (size_t i = 0; i < m.size(); ++i) if (m[i].matched) ps[i] = m[i].ps;
      }
    } else if (C.seed_source == PipelineConfig::SeedSource::VTP && e < vtp.events.size()) {
      auto arm = C.arm == PipelineConfig::Arm::BigBite ? CaloSeed::Arm::BigBite : CaloSeed::Arm::SBS;
      sh = seeds::vtpSeeds(vtp.events[e], ctx_->calo, arm, C.vtp);
    }
    auto cps = constraints(sh, ps);
    d.nseeds = uint32_t(cps.size());
    nseeds = uint16_t(std::min<uint32_t>(0xFFFF, nseeds + d.nseeds));
    track::FilterResult r;
    bool have = false;
    if (!cps.empty()) {
      auto wins = seeds::moduleWindows(ctx_->gem, cps, C.road);
      r = filter_.run(frames, &wins);
      have = true;
      nroads = uint16_t(nroads + cps.size());
    } else {
      d.no_seed = true; any_noseed = true;
      if (C.no_seed == PipelineConfig::NoSeed::Global) { r = filter_.run(frames, nullptr); have = true; }
    }
    for (size_t j = 0; j < frames.size(); ++j) {
      auto& m = masks_[where[j].first][where[j].second];
      if (have) m = r.keep[j];
      else std::fill(m.begin(), m.end(), C.no_seed != PipelineConfig::NoSeed::DropAll);
    }
    if (have) { d.ntracks = uint32_t(r.tracks.size()); d.fallback = r.fallback; ntracks = uint16_t(ntracks + r.tracks.size()); }
    for (size_t j = 0; j < frames.size(); ++j) for (bool b : masks_[where[j].first][where[j].second]) d.strips_out += b;
    details_.push_back(d);
  }

  // 4. re-encode and rebuild
  evioio::EventEditor ed(view);
  for (size_t k = 0; k < banks_.size(); ++k) {
    codec::EncodeStats es;
    codec::encode_into(banks_[k].layout, banks_[k].frames, masks_[k], out_, C.encode, &es);
    st.strips_out += es.strips_out;
    st.bytes_out += uint32_t(4 * out_.size());
    ed.replace_payload(gem_idx[k], out_.data(), out_.size());
  }
  st.nseeds = nseeds; st.nroads = nroads; st.ntracks = ntracks;
  if (any_noseed) st.flags |= 1u << 2;   // contract: bit2 no seed
  std::vector<uint32_t> rebuilt;
  ed.build_into(rebuilt);
  if (rebuilt.size() > cap) throw std::runtime_error("gem pipeline: rebuilt event exceeds capacity");
  std::memcpy(buf, rebuilt.data(), rebuilt.size() * 4);
  nwords = rebuilt.size();
  return st;
}

ProcessorMaker gem_processor_maker() {
  return [](const std::string& args) -> std::unique_ptr<EventProcessor> {
    if (args.empty()) throw std::invalid_argument("gem processor needs a config file: -p gem:<file>");
    // contexts are cached per config file so all worker threads share one (immutable) context
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<const PipelineContext>> cache;
    std::shared_ptr<const PipelineContext> ctx;
    {
      std::lock_guard<std::mutex> l(mu);
      auto& slot = cache[args];
      if (!slot) slot = PipelineContext::build(PipelineConfig::load(args));
      ctx = slot;
    }
    return std::unique_ptr<EventProcessor>(new GemFilterProcessor(ctx));
  };
}

}  // namespace pipeline

}  // namespace gemfilter
