// Unit tests for gemfilter_codec (SSP/VTP MPD decode + bit-exact re-encode).
// All payloads here are SYNTHETIC (generated or hand-written), not real SBS data.
#include <algorithm>
#include <random>
#include <string>
#include <vector>

#include "gemfilter/codec/gem_codec.h"
#include "gemfilter/codec/mpd_format.h"
#include "gemfilter/codec/synthetic.h"
#include "testing.h"

using namespace gemfilter;
using namespace gemfilter::codec;
using synth::Config;
using synth::Layout;

namespace {

// Flatten decoded frames into truth-comparable records (decode order).
std::vector<synth::TruthHit> flatten(const DecodedBank& db) {
  std::vector<synth::TruthHit> v;
  for (uint32_t e = 0; e < db.nevents(); ++e)
    for (uint32_t f = db.event_begin[e]; f < db.event_begin[e + 1]; ++f)
      for (const auto& h : db.frames[f].hits) {
        synth::TruthHit t;
        t.event = e;
        t.addr = db.frames[f].addr;
        t.chan = h.apv_chan;
        t.adc = h.adc;
        t.full_readout = db.frame_info[f].full_readout();
        v.push_back(t);
      }
  return v;
}

bool same_hit(const synth::TruthHit& a, const synth::TruthHit& b) {
  return a.event == b.event && a.addr == b.addr && a.chan == b.chan && a.adc == b.adc &&
         a.full_readout == b.full_readout;
}

void check_truth(const DecodedBank& db, const std::vector<synth::TruthHit>& truth) {
  auto got = flatten(db);
  CHECK_EQ(got.size(), truth.size());
  for (size_t i = 0; i < got.size(); ++i) {
    if (!same_hit(got[i], truth[i]))
      throw gftest::Failure("hit " + std::to_string(i) + " differs from truth");
  }
  for (uint32_t f = 0; f < db.frames.size(); ++f)
    for (uint32_t j = 0; j < db.frames[f].hits.size(); ++j) CHECK_EQ(db.frames[f].hits[j].src_index, j);
}

std::vector<Config> all_configs() {
  std::vector<Config> v;
  for (Layout L : {Layout::Ssp, Layout::VtpBlock, Layout::VtpBare, Layout::VtpBareNoEventHeader}) {
    for (uint32_t seed : {1u, 2u, 3u}) {
      for (uint32_t bl : {1u, 5u}) {
        Config c;
        c.layout = L;
        c.seed = seed;
        c.events_per_block = bl;
        c.occupancy = 0.05 + 0.1 * seed;
        c.filler_align = (seed == 1) ? 0 : (seed == 2 ? 2 : 4);
        c.debug_words = (seed == 2);
        c.event_info = (seed != 3);
        c.full_readout_fraction = (seed == 3) ? 0.3 : 0.0;
        c.reserved_word = (seed == 3);
        c.data_not_valid = (seed == 2) && L != Layout::VtpBareNoEventHeader;
        c.fiber_offset = (seed == 2) ? 32 : 0;     // fibers 32..39: 40-MPD firmware 6-bit field
        if (L == Layout::Ssp) c.slots = {3, 4, 11};
        v.push_back(c);
      }
    }
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------
TEST_CASE(golden_handwritten_words) {
  // Literal words built by hand from SSP_MPD_EventFormat (2020) + MPDModule bit positions.
  const std::vector<uint32_t> p = {
      0x82C00101,  // block header slot 11, block 1, block size 1
      0x90000007,  // event header trigger 7
      0x98123456,  // trigger time lo
      0x00000001,  // trigger time hi
      0xAC210005,  // MPD frame: ENABLE_CM, fiber 33, mpd_id 5
      0x1000BFFF,  // ch[4:0]=4, s1=5, s0=-1
      0x0DFFF000,  // ch[6:5]=3, s3=4095, s2=-4096
      0x27FFC000,  // apv 9, s5=-2, s4=0
      0xF8000000,  // filler
      0x8AC0000A,  // block trailer slot 11, NUM_WORDS 10
  };
  DecodedBank db = decode(p, 21);
  CHECK(db.layout.variant() == Variant::BlockWrapped);
  CHECK_EQ(db.nevents(), 1u);
  CHECK_EQ(db.frames.size(), size_t(1));
  const ApvFrame& f = db.frames[0];
  CHECK_EQ(f.addr.crate, 21); CHECK_EQ(f.addr.slot, 11); CHECK_EQ(f.addr.fiber, 33);
  CHECK_EQ(f.addr.mpd, 5); CHECK_EQ(f.addr.adc, 9);
  CHECK_EQ(f.hits.size(), size_t(1));
  CHECK_EQ(int(f.hits[0].apv_chan), 100);
  const int16_t exp[6] = {-1, 5, -4096, 4095, 0, -2};
  for (int i = 0; i < 6; ++i) CHECK_EQ(f.hits[0].adc[i], exp[i]);
  CHECK(db.frame_info[0].enable_cm);
  CHECK(!db.frame_info[0].build_all_samples);
  CHECK_EQ(db.layout.blocks()[0].num_words, 10u);
  CHECK(db.layout.blocks()[0].count_is_inclusive);
  CHECK_EQ(db.layout.events()[0].trigger_number, 7u);
  CHECK_EQ(db.layout.events()[0].trigger_time, (uint64_t(1) << 24) | 0x123456u);
  // identity
  CHECK(encode(db.layout, db.frames, db.keep_all()) == p);
  // drop the only strip: hit words removed, NUM_WORDS 10 -> 7, everything else identical
  KeepMask none = {{false}};
  auto out = encode(db.layout, db.frames, none);
  const std::vector<uint32_t> exp_out = {0x82C00101, 0x90000007, 0x98123456, 0x00000001,
                                         0xAC210005, 0xF8000000, 0x8AC00007};
  CHECK(out == exp_out);
  // realign fillers to 2: 6 non-filler words + trailer... header..trailer = 6 words -> 0 fillers
  EncodeOptions ro; ro.filler_policy = FillerPolicy::Realign; ro.align = 2;
  auto out2 = encode(db.layout, db.frames, none, ro);
  const std::vector<uint32_t> exp_out2 = {0x82C00101, 0x90000007, 0x98123456, 0x00000001,
                                          0xAC210005, 0x8AC00006};
  CHECK(out2 == exp_out2);
  // 5-bit fiber decoding (SSP doc 2020): 33 & 31 = 1
  DecodeOptions o5; o5.fiber_bits = 5;
  CHECK_EQ(decode(p, 21, o5).frames[0].addr.fiber, 1);
}

TEST_CASE(roundtrip_identity_all_variants) {
  int ncfg = 0;
  for (const Config& c : all_configs()) {
    auto g = synth::generate(c, 19);
    DecodedBank db = decode(g.payload, 19);
    CHECK_EQ(db.nevents(), g.nevents);
    check_truth(db, g.hits);
    EncodeStats st;
    auto out = encode(db.layout, db.frames, db.keep_all(), {}, &st);
    CHECK(out == g.payload);
    CHECK_EQ(st.strips_in, st.strips_out);
    CHECK_EQ(st.words_in, st.words_out);
    // by-src overload and protection off must also be identity
    EncodeOptions np; np.protect_full_readout = false;
    std::vector<std::vector<bool>> ks(db.layout.nframes());
    for (uint32_t f = 0; f < ks.size(); ++f) ks[f].assign(db.layout.frame_nhits()[f], true);
    CHECK(encode(db.layout, ks, np) == g.payload);
    // repack with unmodified samples is identity as well
    EncodeOptions rp; rp.repack_samples = true;
    CHECK(encode(db.layout, db.frames, db.keep_all(), rp) == g.payload);
    // decode_into reuse
    DecodedBank db2;
    decode_into(g.payload.data(), g.payload.size(), 19, db2);
    decode_into(g.payload.data(), g.payload.size(), 19, db2);
    CHECK_EQ(db2.frames.size(), db.frames.size());
    ++ncfg;
  }
  CHECK_EQ(ncfg, 24);
}

TEST_CASE(subset_reencode_gives_exactly_kept_strips) {
  std::mt19937 rng(777);
  for (const Config& c : all_configs()) {
    auto g = synth::generate(c, 20);
    DecodedBank db = decode(g.payload, 20);
    for (double pkeep : {0.0, 0.3, 0.9}) {
      for (bool prot : {false, true}) {
        std::bernoulli_distribution keep(pkeep);
        KeepMask m(db.frames.size());
        std::vector<synth::TruthHit> expect;
        auto truth = flatten(db);
        size_t k = 0;
        for (size_t f = 0; f < db.frames.size(); ++f) {
          m[f].resize(db.frames[f].hits.size());
          for (size_t j = 0; j < m[f].size(); ++j, ++k) {
            m[f][j] = keep(rng);
            const bool kept = m[f][j] || (prot && db.frame_info[f].full_readout());
            if (kept) expect.push_back(truth[k]);
          }
        }
        EncodeOptions eo; eo.protect_full_readout = prot;
        EncodeStats st;
        auto out = encode(db.layout, db.frames, m, eo, &st);
        CHECK_EQ(size_t(st.strips_out), expect.size());
        CHECK_EQ(size_t(st.words_in - st.words_out), size_t(3 * (st.strips_in - st.strips_out)));
        DecodeOptions strict; strict.strict_block_count = true;   // generator writes inclusive NUM_WORDS
        DecodedBank d2 = decode(out, 20, strict);
        CHECK_EQ(d2.nevents(), db.nevents());
        auto got = flatten(d2);
        CHECK_EQ(got.size(), expect.size());
        for (size_t i = 0; i < got.size(); ++i)
          if (!same_hit(got[i], expect[i])) throw gftest::Failure("kept hit mismatch at " + std::to_string(i));
        // everything that is not a strip hit is preserved: event info / trigger numbers
        for (uint32_t e = 0; e < d2.nevents(); ++e)
          CHECK_EQ(d2.layout.events()[e].trigger_number, db.layout.events()[e].trigger_number);
        // realigned fillers: block length multiple of 4 and NUM_WORDS still inclusive
        EncodeOptions ra = eo; ra.filler_policy = FillerPolicy::Realign; ra.align = 4;
        auto out4 = encode(db.layout, db.frames, m, ra);
        DecodedBank d4 = decode(out4, 20, strict);
        CHECK_EQ(flatten(d4).size(), expect.size());
        for (const auto& b : d4.layout.blocks()) CHECK_EQ((b.trailer_pos - b.header_pos + 1) % 4, 0u);
        if (d4.layout.variant() == Variant::Bare) CHECK_EQ(out4.size() % 4, size_t(0));
      }
    }
  }
}

TEST_CASE(protect_full_readout_frames) {
  Config c; c.layout = Layout::VtpBare; c.full_readout_fraction = 0.5; c.seed = 9; c.nfibers = 8;
  auto g = synth::generate(c, 22);
  DecodedBank db = decode(g.payload, 22);
  uint32_t nfull = 0, nfull_hits = 0;
  for (size_t f = 0; f < db.frames.size(); ++f)
    if (db.frame_info[f].full_readout()) { ++nfull; nfull_hits += uint32_t(db.frames[f].hits.size()); CHECK_EQ(db.frames[f].hits.size(), size_t(128)); }
  CHECK(nfull > 0);
  KeepMask none(db.frames.size());
  for (size_t f = 0; f < db.frames.size(); ++f) none[f].assign(db.frames[f].hits.size(), false);
  EncodeStats st;
  auto out = encode(db.layout, db.frames, none, {}, &st);
  CHECK_EQ(st.protected_frames, nfull);
  CHECK_EQ(st.strips_out, nfull_hits);
  EncodeOptions np; np.protect_full_readout = false;
  encode(db.layout, db.frames, none, np, &st);
  CHECK_EQ(st.strips_out, 0u);
}

TEST_CASE(repack_modified_samples_and_reordered_hits) {
  Config c; c.layout = Layout::Ssp; c.seed = 4; c.slots = {5, 6};
  auto g = synth::generate(c, 23);
  DecodedBank db = decode(g.payload, 23);
  std::vector<ApvFrame> fr = db.frames;
  KeepMask m(fr.size());
  // reverse hit order, drop every 3rd hit from the vectors entirely, modify samples
  for (size_t f = 0; f < fr.size(); ++f) {
    std::reverse(fr[f].hits.begin(), fr[f].hits.end());
    std::vector<StripHit> v;
    for (size_t j = 0; j < fr[f].hits.size(); ++j)
      if (j % 3 != 2) { StripHit h = fr[f].hits[j]; h.adc[0] = int16_t(-h.adc[5] / 2); h.adc[5] = 17; v.push_back(h); }
    fr[f].hits = v;
    m[f].assign(v.size(), true);
  }
  EncodeOptions rp; rp.repack_samples = true;
  auto out = encode(db.layout, fr, m, rp);
  DecodedBank d2 = decode(out, 23);
  CHECK_EQ(d2.frames.size(), db.frames.size());
  for (size_t f = 0; f < fr.size(); ++f) {
    // decoded order is payload order = original order; compare as sets keyed by channel
    CHECK_EQ(d2.frames[f].hits.size(), fr[f].hits.size());
    for (const auto& h : fr[f].hits) {
      auto it = std::find_if(d2.frames[f].hits.begin(), d2.frames[f].hits.end(),
                             [&](const StripHit& x) { return x.apv_chan == h.apv_chan; });
      CHECK(it != d2.frames[f].hits.end());
      CHECK(it->adc == h.adc);
    }
  }
  // without repack the original samples are copied verbatim
  auto out_v = encode(db.layout, fr, m);
  DecodedBank d3 = decode(out_v, 23);
  for (size_t f = 0; f < fr.size(); ++f)
    for (const auto& h : d3.frames[f].hits) {
      const auto& orig = db.frames[f].hits;
      auto it = std::find_if(orig.begin(), orig.end(), [&](const StripHit& x) { return x.apv_chan == h.apv_chan; });
      CHECK(it != orig.end() && it->adc == h.adc);
    }
  // out-of-range sample
  if (!fr.empty() && !fr[0].hits.empty()) {
    fr[0].hits[0].adc[2] = 5000;
    CHECK_THROWS_CODE(encode(db.layout, fr, m, rp), EncodeError, ErrorCode::SampleOutOfRange);
  }
}

TEST_CASE(debug_words_attached_to_last_apv) {
  Config c; c.layout = Layout::VtpBare; c.debug_words = true; c.occupancy = 0.5; c.seed = 11;
  auto g = synth::generate(c, 24);
  DecodedBank db = decode(g.payload, 24);
  CHECK(!db.frames.empty());
  for (const auto& f : db.frames) {
    CHECK_EQ(f.extra_words.size(), size_t(3));
    CHECK(mpd::is_defining(f.extra_words[0]) && mpd::type_of(f.extra_words[0]) == mpd::kMpdDebug);
  }
}

TEST_CASE(empty_and_trivial_payloads) {
  DecodedBank db = decode(nullptr, 0, 1);
  CHECK(db.layout.variant() == Variant::Empty);
  CHECK_EQ(db.nevents(), 0u);
  CHECK(encode(db.layout, db.frames, db.keep_all()).empty());
  const std::vector<uint32_t> fill = {0xF8000000, 0xF8000000};
  DecodedBank d2 = decode(fill, 1);
  CHECK(d2.layout.variant() == Variant::Bare);
  CHECK_EQ(d2.nevents(), 1u);
  CHECK(encode(d2.layout, d2.frames, d2.keep_all()) == fill);
  // frame header with zero hits is legal
  const std::vector<uint32_t> p = {0x90000001, 0xA8000000, 0x90000002, 0xA8010001};
  DecodedBank d3 = decode(p, 1);
  CHECK_EQ(d3.nevents(), 2u);
  CHECK(d3.frames.empty());
  CHECK(encode(d3.layout, d3.frames, d3.keep_all()) == p);
}

TEST_CASE(malformed_input_throws_typed_errors) {
  Config c; c.layout = Layout::Ssp; c.seed = 5; c.slots = {11}; c.events_per_block = 2; c.filler_align = 0;
  auto g = synth::generate(c, 25);
  const auto& P = g.payload;
  // locate structure
  size_t first_hit = 0, first_fh = 0, first_tt = 0;
  for (size_t i = 0; i < P.size(); ++i) {
    if (mpd::is_defining(P[i]) && mpd::type_of(P[i]) == mpd::kMpdFrame && !first_fh) first_fh = i;
    if (mpd::is_defining(P[i]) && mpd::type_of(P[i]) == mpd::kTriggerTime && !first_tt) first_tt = i;
  }
  for (size_t i = first_fh + 1; i < P.size(); ++i)
    if (!mpd::is_defining(P[i])) { first_hit = i; break; }
  CHECK(first_fh > 0 && first_hit > first_fh && first_tt > 0);

  {  // orphan continuation at start
    std::vector<uint32_t> q = P; q.insert(q.begin(), 0x00001234u);
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::OrphanContinuation);
    DecodeOptions lo; lo.allow_orphan_words = true;
    DecodedBank d = decode(q, 1, lo);
    CHECK(encode(d.layout, d.frames, d.keep_all()) == q);
  }
  {  // incomplete hit: remove one hit word
    std::vector<uint32_t> q = P; q.erase(q.begin() + long(first_hit));
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::IncompleteHit);
  }
  {  // missing trailer
    std::vector<uint32_t> q = P; q.pop_back();
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::BlockNotClosed);
  }
  {  // trailer without header
    std::vector<uint32_t> q = P; q.erase(q.begin());
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::TrailerWithoutHeader);
  }
  {  // nested block
    std::vector<uint32_t> q = P; q.insert(q.begin() + 1, P[0]);
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::NestedBlock);
  }
  {  // BLOCK_SIZE disagrees with number of event headers
    std::vector<uint32_t> q = P; q[0] = mpd::make_block_header(11, 1, 3);
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::BlockSizeMismatch);
  }
  {  // trigger time without its continuation word
    std::vector<uint32_t> q = P; q.erase(q.begin() + long(first_tt) + 1);
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::BadContinuationCount);
  }
  {  // trailer slot mismatch
    std::vector<uint32_t> q = P; q.back() = mpd::make_block_trailer(12, mpd::bt_num_words(P.back()));
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::SlotMismatch);
  }
  {  // NUM_WORDS wrong: tolerated by default, rejected in strict mode
    std::vector<uint32_t> q = P; q.back() = mpd::make_block_trailer(11, mpd::bt_num_words(P.back()) + 1);
    DecodedBank d = decode(q, 1);
    CHECK(!d.layout.blocks()[0].count_is_inclusive);
    CHECK(encode(d.layout, d.frames, d.keep_all()) == q);
    DecodeOptions s; s.strict_block_count = true;
    CHECK_THROWS_CODE(decode(q, 1, s), DecodeError, ErrorCode::BlockCountMismatch);
  }
  {  // reserved type: carried verbatim by default, rejected when strict
    std::vector<uint32_t> q = P; q.insert(q.begin() + long(first_fh), mpd::make_defining(9, 0));
    DecodeOptions lax; lax.strict_counts = true;
    // a reserved word between frame header groups: next MPD continuation words would belong to it,
    // so insert it right before a frame header (allowed)
    DecodedBank d = decode(q, 1, lax);
    CHECK(encode(d.layout, d.frames, d.keep_all()) == q);
    DecodeOptions s; s.strict_unknown_types = true;
    CHECK_THROWS_CODE(decode(q, 1, s), DecodeError, ErrorCode::UnknownType);
  }
  {  // MPD frame before any event header (bare payload with event headers later)
    const std::vector<uint32_t> q = {0xA8000000, 0x90000001};
    CHECK_THROWS_CODE(decode(q, 1), DecodeError, ErrorCode::HitOutsideEvent);
  }
  {  // encode with inconsistent mask / frames
    DecodedBank d = decode(P, 1);
    KeepMask bad = d.keep_all(); bad.pop_back();
    CHECK_THROWS_CODE(encode(d.layout, d.frames, bad), EncodeError, ErrorCode::LayoutMismatch);
    KeepMask bad2 = d.keep_all(); bad2[0].push_back(true);
    CHECK_THROWS_CODE(encode(d.layout, d.frames, bad2), EncodeError, ErrorCode::LayoutMismatch);
    std::vector<ApvFrame> fr = d.frames; fr[0].hits[0].src_index = 100000;
    CHECK_THROWS_CODE(encode(d.layout, fr, d.keep_all()), EncodeError, ErrorCode::LayoutMismatch);
  }
  {  // too many events
    DecodeOptions o; o.max_events = 1;
    CHECK_THROWS_CODE(decode(P, 1, o), DecodeError, ErrorCode::TooManyEvents);
  }
}

TEST_CASE(dump_is_readable) {
  Config c; c.layout = Layout::Ssp; c.events_per_block = 1; c.nfibers = 1; c.apvs_per_mpd = 1; c.occupancy = 0.02;
  auto g = synth::generate(c, 1);
  std::string s = dump(g.payload.data(), g.payload.size());
  CHECK(s.find("BLOCK HEADER slot=11") != std::string::npos);
  CHECK(s.find("BLOCK TRAILER") != std::string::npos);
  CHECK(s.find("MPD FRAME") != std::string::npos);
}

TEST_MAIN()
