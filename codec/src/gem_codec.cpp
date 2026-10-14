// gemfilter codec implementation. See docs/gem_format.md for the bit-level format.
#include "gemfilter/codec/gem_codec.h"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <unordered_map>

#include "gemfilter/codec/mpd_format.h"

namespace gemfilter {
namespace codec {

using namespace mpd;

const char* to_string(ErrorCode c) {
  switch (c) {
    case ErrorCode::OrphanContinuation: return "OrphanContinuation";
    case ErrorCode::IncompleteHit: return "IncompleteHit";
    case ErrorCode::BadContinuationCount: return "BadContinuationCount";
    case ErrorCode::HitOutsideEvent: return "HitOutsideEvent";
    case ErrorCode::BlockNotClosed: return "BlockNotClosed";
    case ErrorCode::TrailerWithoutHeader: return "TrailerWithoutHeader";
    case ErrorCode::NestedBlock: return "NestedBlock";
    case ErrorCode::BlockSizeMismatch: return "BlockSizeMismatch";
    case ErrorCode::BlockCountMismatch: return "BlockCountMismatch";
    case ErrorCode::SlotMismatch: return "SlotMismatch";
    case ErrorCode::UnknownType: return "UnknownType";
    case ErrorCode::TooManyEvents: return "TooManyEvents";
    case ErrorCode::LayoutMismatch: return "LayoutMismatch";
    case ErrorCode::SampleOutOfRange: return "SampleOutOfRange";
    case ErrorCode::CountUnderflow: return "CountUnderflow";
  }
  return "Unknown";
}

CodecError::CodecError(ErrorCode code, size_t word_index, const std::string& what)
    : std::runtime_error(std::string("gemfilter::codec ") + to_string(code) + " at word " +
                         std::to_string(word_index) + ": " + what),
      code_(code), word_index_(word_index) {}

void GemBankLayout::clear() {
  variant_ = Variant::Empty;
  crate_ = 0;
  words_.clear();
  items_.clear();
  blocks_.clear();
  events_.clear();
  frame_nhits_.clear();
  frame_protected_.clear();
  block_item_begin_.clear();
  block_item_end_.clear();
  nhits_total_ = 0;
}

void DecodedBank::clear() {
  frames.clear();
  event_begin.clear();
  frame_info.clear();
  layout.clear();
}

std::vector<ApvFrame> DecodedBank::event_frames(uint32_t e) const {
  if (e + 1 >= event_begin.size()) return {};
  return std::vector<ApvFrame>(frames.begin() + event_begin[e], frames.begin() + event_begin[e + 1]);
}

KeepMask DecodedBank::keep_all() const {
  KeepMask m(frames.size());
  for (size_t f = 0; f < frames.size(); ++f) m[f].assign(frames[f].hits.size(), true);
  return m;
}

// ===========================================================================
// Decoder
// ===========================================================================
namespace {
constexpr uint32_t kNone = ~0u;

struct TmpFrame {
  ApvFrame frame;
  FrameInfo info;
  bool prot = false;
};
}  // namespace

class Decoder {
 public:
  Decoder(const uint32_t* p, size_t n, uint16_t crate, const DecodeOptions& o, DecodedBank& out)
      : p_(p), n_(n), crate_(crate), opt_(o), out_(out), L_(out.layout) {}

  void run();

 private:
  [[noreturn]] void fail(ErrorCode c, size_t i, const std::string& msg) { throw DecodeError(c, i, msg); }

  void add_verbatim(uint32_t i) {
    auto& items = L_.items_;
    if (!items.empty() && items.back().kind == GemBankLayout::Kind::Verbatim &&
        items.back().begin + items.back().len == i) {
      ++items.back().len;
      return;
    }
    GemBankLayout::Item it;
    it.begin = i; it.len = 1; it.kind = GemBankLayout::Kind::Verbatim; it.block = cur_block_;
    items.push_back(it);
  }
  void add_single(uint32_t i, GemBankLayout::Kind k) {
    auto& items = L_.items_;
    if (k == GemBankLayout::Kind::Filler && !items.empty() && items.back().kind == k &&
        items.back().begin + items.back().len == i && items.back().block == cur_block_) {
      ++items.back().len;
      return;
    }
    GemBankLayout::Item it;
    it.begin = i; it.len = 1; it.kind = k; it.block = cur_block_;
    items.push_back(it);
  }

  uint32_t ensure_event(uint32_t e, size_t i) {
    if (e >= opt_.max_events) fail(ErrorCode::TooManyEvents, i, "event index " + std::to_string(e));
    if (e >= ev_frames_.size()) {
      ev_frames_.resize(e + 1);
      L_.events_.resize(e + 1);
    }
    return e;
  }

  // Close the current defining-word group: validate continuation counts.
  void close_group(size_t /*i*/) {
    if (grp_type_ == kNone) return;
    if (grp_type_ == kMpdFrame) {
      if (hit_fill_ != 0)
        fail(ErrorCode::IncompleteHit, grp_start_, "MPD frame has " + std::to_string(grp_conts_) +
                                                     " continuation words (not a multiple of 3)");
    } else {
      const int exp = expected_continuations(grp_type_);
      if (exp >= 0 && int(grp_conts_) != exp && opt_.strict_counts)
        fail(ErrorCode::BadContinuationCount, grp_start_,
             "type " + std::to_string(grp_type_) + " has " + std::to_string(grp_conts_) +
                 " continuation words, expected " + std::to_string(exp));
      if (grp_type_ == kMpdDebug && grp_conts_ == 2 && last_hit_frame_ != kNone) {
        // SBS-offline convention: associate debug triple with the APV of the last decoded hit.
        auto& ef = ev_frames_[last_hit_event_][last_hit_frame_].frame;
        ef.extra_words.insert(ef.extra_words.end(), p_ + grp_start_, p_ + grp_start_ + 3);
      }
    }
    grp_type_ = kNone;
    grp_conts_ = 0;
  }

  void on_hit(uint32_t i0) {
    const uint32_t h0 = p_[i0], h1 = p_[i0 + 1], h2 = p_[i0 + 2];
    const uint32_t chan = hit_apv_chan(h0, h1);
    const uint32_t apv = hit_apv_id(h2);
    const uint32_t e = cur_event_;
    const uint64_t key = (uint64_t(e) << 40) | (uint64_t(cur_slot_) << 24) | (uint64_t(fiber_) << 16) |
                         (uint64_t(mpd_id_) << 8) | apv;
    uint32_t lf;
    if (key == last_key_) {
      lf = last_lf_;
    } else {
      auto it = frame_index_.find(key);
      if (it == frame_index_.end()) {
        auto& vec = ev_frames_[e];
        lf = uint32_t(vec.size());
        vec.emplace_back();
        TmpFrame& tf = vec.back();
        tf.frame.addr.crate = crate_;
        tf.frame.addr.slot = cur_slot_;
        tf.frame.addr.fiber = uint16_t(fiber_);
        tf.frame.addr.mpd = uint16_t(mpd_id_);
        tf.frame.addr.adc = uint16_t(apv);
        tf.info.event = e;
        tf.info.block = cur_block_ == kNone ? 0u : cur_block_;
        tf.info.frame_header = frame_hdr_;
        tf.info.enable_cm = (frame_hdr_ & kEnableCmBit) != 0;
        tf.info.build_all_samples = (frame_hdr_ & kBuildAllSamplesBit) != 0;
        tf.info.cm_or = (frame_hdr_ & kCmOrBit) != 0;
        frame_index_.emplace(key, lf);
      } else {
        lf = it->second;
      }
      last_key_ = key;
      last_lf_ = lf;
    }
    TmpFrame& tf = ev_frames_[e][lf];
    if (frame_hdr_ & (kBuildAllSamplesBit | kCmOrBit)) tf.prot = true;
    StripHit h;
    h.apv_chan = uint8_t(chan);
    h.adc[0] = sample13(hit_lo_sample(h0)); h.adc[1] = sample13(hit_hi_sample(h0));
    h.adc[2] = sample13(hit_lo_sample(h1)); h.adc[3] = sample13(hit_hi_sample(h1));
    h.adc[4] = sample13(hit_lo_sample(h2)); h.adc[5] = sample13(hit_hi_sample(h2));
    h.nsamples = kMaxSamples;
    h.src_index = uint32_t(tf.frame.hits.size());
    tf.frame.hits.push_back(h);

    GemBankLayout::Item it;
    it.begin = i0; it.len = 3; it.kind = GemBankLayout::Kind::Hit;
    it.frame = lf; it.hit = h.src_index; it.block = cur_block_;
    L_.items_.push_back(it);
    hit_item_event_.push_back(e);
    last_hit_frame_ = lf;
    last_hit_event_ = e;
  }

  const uint32_t* p_;
  size_t n_;
  uint16_t crate_;
  const DecodeOptions& opt_;
  DecodedBank& out_;
  GemBankLayout& L_;

  // state
  uint32_t grp_type_ = kNone, grp_start_ = 0, grp_conts_ = 0;
  uint32_t hit_fill_ = 0, hit_start_ = 0;
  uint32_t cur_block_ = kNone;
  uint16_t cur_slot_ = 0;
  uint32_t cur_event_ = kNone;      // event index of the current event (kNone = none yet)
  uint32_t block_events_ = 0;       // event headers seen in current block
  uint32_t bare_events_ = 0;        // event headers seen outside blocks
  uint32_t frame_hdr_ = 0, fiber_ = 0, mpd_id_ = 0;
  uint32_t last_hit_frame_ = kNone, last_hit_event_ = 0;
  uint64_t last_key_ = ~0ull;
  uint32_t last_lf_ = 0;
  uint32_t tt_lo_ = 0;
  std::vector<std::vector<TmpFrame>> ev_frames_;
  std::unordered_map<uint64_t, uint32_t> frame_index_;
  std::vector<uint32_t> hit_item_event_;
};

void Decoder::run() {
  out_.clear();
  L_.crate_ = crate_;
  L_.words_.assign(p_, p_ + n_);
  if (n_ == 0) {
    out_.event_begin.push_back(0);
    return;
  }
  // Pre-scan: block-wrapped if any block header; bare payloads: event headers present?
  bool any_block = false, any_eh = false;
  for (size_t i = 0; i < n_; ++i) {
    if (!is_defining(p_[i])) continue;
    const uint32_t t = type_of(p_[i]);
    if (t == kBlockHeader) any_block = true;
    if (t == kEventHeader) any_eh = true;
  }
  L_.variant_ = any_block ? Variant::BlockWrapped : Variant::Bare;
  if (!any_block) {
    cur_slot_ = opt_.default_slot;
    if (!any_eh) cur_event_ = ensure_event(0, 0);   // whole payload = one event
  }

  for (uint32_t i = 0; i < n_; ++i) {
    const uint32_t w = p_[i];
    if (!is_defining(w)) {
      // ---- continuation word ----
      if (grp_type_ == kNone) {
        if (!opt_.allow_orphan_words)
          fail(ErrorCode::OrphanContinuation, i, "continuation word before any defining word");
        add_verbatim(i);
        continue;
      }
      ++grp_conts_;
      switch (grp_type_) {
        case kMpdFrame:
          if (hit_fill_ == 0) hit_start_ = i;
          if (++hit_fill_ == 3) {
            hit_fill_ = 0;
            on_hit(hit_start_);
          }
          break;
        case kTriggerTime:
          if (grp_conts_ == 1 && cur_event_ != kNone) {
            EventInfo& ev = L_.events_[cur_event_];
            if (!ev.has_trigger_time) {
              ev.trigger_time = (uint64_t(w & 0xFFFFFFu) << 24) | tt_lo_;
              ev.has_trigger_time = true;
            }
          }
          add_verbatim(i);
          break;
        case kBlockHeader: case kBlockTrailer: case kEventHeader:
        case kDataNotValid: case kFiller:
          if (opt_.strict_counts)
            fail(ErrorCode::BadContinuationCount, i,
                 "continuation word after single-word type " + std::to_string(grp_type_));
          add_verbatim(i);
          break;
        default:
          add_verbatim(i);
          break;
      }
      continue;
    }
    // ---- defining word ----
    close_group(i);
    const uint32_t t = type_of(w);
    grp_type_ = t;
    grp_start_ = i;
    grp_conts_ = 0;
    if (t != kMpdFrame && t != kMpdEventInfo && t != kMpdDebug && t != kDataNotValid && t != kFiller) {
      // structural words end the MPD-frame context used for debug-word association
      last_hit_frame_ = kNone;
      last_key_ = ~0ull;
    }
    switch (t) {
      case kBlockHeader: {
        if (cur_block_ != kNone) fail(ErrorCode::NestedBlock, i, "block header inside open block");
        BlockInfo b;
        b.header_word = w;
        b.header_pos = i;
        b.slot = uint16_t(bh_slot(w));
        b.block_number = uint16_t(bh_block_number(w));
        b.block_size = uint16_t(bh_block_size(w));
        cur_block_ = uint32_t(L_.blocks_.size());
        L_.blocks_.push_back(b);
        L_.block_item_begin_.push_back(uint32_t(L_.items_.size()));
        L_.block_item_end_.push_back(kNone);
        cur_slot_ = b.slot;
        block_events_ = 0;
        cur_event_ = kNone;
        add_single(i, GemBankLayout::Kind::BlockHeader);
        break;
      }
      case kBlockTrailer: {
        if (cur_block_ == kNone) fail(ErrorCode::TrailerWithoutHeader, i, "block trailer outside block");
        BlockInfo& b = L_.blocks_[cur_block_];
        b.trailer_word = w;
        b.trailer_pos = i;
        b.num_words = bt_num_words(w);
        b.count_is_inclusive = (b.num_words == i - b.header_pos + 1);
        if (opt_.strict_slot && bt_slot(w) != b.slot)
          fail(ErrorCode::SlotMismatch, i, "trailer slot " + std::to_string(bt_slot(w)) + " != header slot " +
                                               std::to_string(b.slot));
        if (opt_.strict_block_count && !b.count_is_inclusive)
          fail(ErrorCode::BlockCountMismatch, i, "NUM_WORDS " + std::to_string(b.num_words) + " != " +
                                                     std::to_string(i - b.header_pos + 1));
        if (block_events_ != b.block_size)
          fail(ErrorCode::BlockSizeMismatch, i, std::to_string(block_events_) + " event headers, BLOCK_SIZE " +
                                                    std::to_string(b.block_size));
        add_single(i, GemBankLayout::Kind::BlockTrailer);
        L_.block_item_end_[cur_block_] = uint32_t(L_.items_.size() - 1);
        cur_block_ = kNone;
        cur_event_ = kNone;
        break;
      }
      case kEventHeader: {
        uint32_t e;
        if (cur_block_ != kNone) e = block_events_++;
        else e = bare_events_++;
        cur_event_ = ensure_event(e, i);
        EventInfo& ev = L_.events_[e];
        if (!ev.has_header) {
          ev.has_header = true;
          ev.event_header = w;
          ev.trigger_number = eh_trigger_number(w);
        }
        add_verbatim(i);
        break;
      }
      case kTriggerTime:
        tt_lo_ = w & 0xFFFFFFu;
        add_verbatim(i);
        break;
      case kMpdFrame:
        if (cur_event_ == kNone)
          fail(ErrorCode::HitOutsideEvent, i, "MPD frame header before any event header");
        frame_hdr_ = w;
        fiber_ = fh_fiber(w, opt_.fiber_bits);
        mpd_id_ = fh_mpd_id(w);
        last_hit_frame_ = kNone;
        last_key_ = ~0ull;
        hit_fill_ = 0;
        add_verbatim(i);
        break;
      case kFiller:
        add_single(i, GemBankLayout::Kind::Filler);
        break;
      case kMpdEventInfo: case kMpdDebug: case kDataNotValid:
        add_verbatim(i);
        break;
      default:
        if (opt_.strict_unknown_types)
          fail(ErrorCode::UnknownType, i, "reserved data type " + std::to_string(t));
        add_verbatim(i);
        break;
    }
  }
  close_group(uint32_t(n_));
  if (cur_block_ != kNone) fail(ErrorCode::BlockNotClosed, n_, "block header without trailer");

  // number of events
  uint32_t nev = uint32_t(ev_frames_.size());
  if (L_.variant_ == Variant::BlockWrapped) {
    const uint32_t bs = L_.blocks_.front().block_size;
    for (const auto& b : L_.blocks_)
      if (b.block_size != bs) fail(ErrorCode::BlockSizeMismatch, b.header_pos, "blocks disagree on BLOCK_SIZE");
    if (ev_frames_.size() > bs)
      fail(ErrorCode::BlockSizeMismatch, 0, "event headers outside blocks in a block-wrapped payload");
    nev = bs;
  }
  if (nev > ev_frames_.size()) {
    ensure_event(nev - 1, 0);
  }

  // flatten frames event by event
  out_.event_begin.assign(1, 0);
  std::vector<uint32_t> ev_off(nev, 0);
  uint32_t nf = 0;
  for (uint32_t e = 0; e < nev; ++e) {
    ev_off[e] = nf;
    nf += uint32_t(ev_frames_[e].size());
    out_.event_begin.push_back(nf);
  }
  out_.frames.reserve(nf);
  out_.frame_info.reserve(nf);
  L_.frame_nhits_.reserve(nf);
  L_.frame_protected_.reserve(nf);
  for (uint32_t e = 0; e < nev; ++e) {
    for (auto& tf : ev_frames_[e]) {
      L_.frame_nhits_.push_back(uint32_t(tf.frame.hits.size()));
      L_.nhits_total_ += uint32_t(tf.frame.hits.size());
      L_.frame_protected_.push_back(tf.prot ? 1 : 0);
      out_.frame_info.push_back(tf.info);
      out_.frames.push_back(std::move(tf.frame));
    }
  }
  // remap hit items to global frame index
  size_t k = 0;
  for (auto& it : L_.items_) {
    if (it.kind != GemBankLayout::Kind::Hit) continue;
    it.frame += ev_off[hit_item_event_[k++]];
  }
#ifndef NDEBUG
  size_t covered = 0;
  for (const auto& it : L_.items_) covered += it.len;
  if (covered != n_) throw std::logic_error("gemfilter::codec internal: layout does not cover payload");
#endif
}

void decode_into(const uint32_t* payload, size_t nwords, uint16_t crate, DecodedBank& out,
                 const DecodeOptions& opt) {
  Decoder d(payload, nwords, crate, opt, out);
  d.run();
}

DecodedBank decode(const uint32_t* payload, size_t nwords, uint16_t crate, const DecodeOptions& opt) {
  DecodedBank out;
  decode_into(payload, nwords, crate, out, opt);
  return out;
}

// ===========================================================================
// Encoder
// ===========================================================================
class Encoder {
 public:
  static void run(const GemBankLayout& L, const std::vector<ApvFrame>* frames, const KeepMask& keep,
                  bool by_src, std::vector<uint32_t>& out, const EncodeOptions& opt, EncodeStats* stats);
};

void Encoder::run(const GemBankLayout& L, const std::vector<ApvFrame>* frames, const KeepMask& keep,
                  bool by_src, std::vector<uint32_t>& out, const EncodeOptions& opt, EncodeStats* stats) {
  using Kind = GemBankLayout::Kind;
  const uint32_t nf = L.nframes();
  if (keep.size() != nf)
    throw EncodeError(ErrorCode::LayoutMismatch, 0,
                      "mask has " + std::to_string(keep.size()) + " frames, layout " + std::to_string(nf));
  if (frames && frames->size() != nf)
    throw EncodeError(ErrorCode::LayoutMismatch, 0,
                      "frames vector has " + std::to_string(frames->size()) + " frames, layout " + std::to_string(nf));
  if (opt.repack_samples && !frames)
    throw EncodeError(ErrorCode::LayoutMismatch, 0, "repack_samples requires frames");
  if (opt.filler_policy == FillerPolicy::Realign && opt.align == 0)
    throw EncodeError(ErrorCode::LayoutMismatch, 0, "align must be > 0");

  // keep flags indexed by (frame offset + src_index)
  std::vector<uint32_t> off(nf + 1, 0);
  for (uint32_t f = 0; f < nf; ++f) off[f + 1] = off[f] + L.frame_nhits_[f];
  std::vector<uint8_t> ks(off[nf], 0);
  std::vector<int32_t> jidx;
  if (opt.repack_samples) jidx.assign(off[nf], -1);
  uint32_t nprot = 0;
  for (uint32_t f = 0; f < nf; ++f) {
    const uint32_t nh = L.frame_nhits_[f];
    if (opt.protect_full_readout && L.frame_protected_[f]) {
      std::fill(ks.begin() + off[f], ks.begin() + off[f + 1], 1);
      ++nprot;
    }
    if (by_src) {
      if (keep[f].size() != nh)
        throw EncodeError(ErrorCode::LayoutMismatch, f, "mask size " + std::to_string(keep[f].size()) +
                                                            " != hits " + std::to_string(nh));
      for (uint32_t h = 0; h < nh; ++h)
        if (keep[f][h]) ks[off[f] + h] = 1;
    } else {
      const auto& hits = (*frames)[f].hits;
      if (keep[f].size() != hits.size())
        throw EncodeError(ErrorCode::LayoutMismatch, f, "mask size " + std::to_string(keep[f].size()) +
                                                            " != frame hits " + std::to_string(hits.size()));
      for (size_t j = 0; j < hits.size(); ++j) {
        const uint32_t s = hits[j].src_index;
        if (s >= nh)
          throw EncodeError(ErrorCode::LayoutMismatch, f, "src_index " + std::to_string(s) + " out of range");
        if (keep[f][j]) ks[off[f] + s] = 1;
        if (opt.repack_samples) jidx[off[f] + s] = int32_t(j);
      }
    }
  }

  const auto& W = L.words_;
  const auto& items = L.items_;
  const bool realign = opt.filler_policy == FillerPolicy::Realign;
  out.clear();
  out.reserve(W.size());

  auto kept_len = [&](size_t a, size_t b) {   // non-filler kept words in items [a,b)
    uint64_t s = 0;
    for (size_t k = a; k < b; ++k) {
      const auto& it = items[k];
      if (it.kind == Kind::Filler) continue;
      if (it.kind == Kind::Hit && !ks[off[it.frame] + it.hit]) continue;
      s += it.len;
    }
    return s;
  };
  auto filler_value = [&](size_t a, size_t b) {
    for (size_t k = a; k < b; ++k)
      if (items[k].kind == Kind::Filler) return W[items[k].begin];
    return kFillerWord;
  };

  // Bare payload realignment: one region = whole payload.
  uint32_t bare_nfill = 0, bare_fillw = kFillerWord;
  bool bare_emitted = false;
  if (realign && L.variant_ == Variant::Bare) {
    const uint64_t s = kept_len(0, items.size());
    bare_nfill = uint32_t((opt.align - s % opt.align) % opt.align);
    bare_fillw = filler_value(0, items.size());
  }

  size_t hdr_out = 0;
  uint32_t blk_nfill = 0, blk_fillw = kFillerWord;
  bool blk_emitted = false;
  uint32_t kept_hits = 0;

  for (size_t k = 0; k < items.size(); ++k) {
    const auto& it = items[k];
    switch (it.kind) {
      case Kind::Verbatim:
        out.insert(out.end(), W.begin() + it.begin, W.begin() + it.begin + it.len);
        break;
      case Kind::Hit: {
        if (!ks[off[it.frame] + it.hit]) break;
        ++kept_hits;
        if (opt.repack_samples) {
          const int32_t j = jidx[off[it.frame] + it.hit];
          if (j < 0) {   // kept only through protection but absent from frames: copy verbatim
            out.insert(out.end(), W.begin() + it.begin, W.begin() + it.begin + 3);
            break;
          }
          const StripHit& sh = (*frames)[it.frame].hits[size_t(j)];
          for (int q = 0; q < 6; ++q)
            if (sh.adc[q] < -4096 || sh.adc[q] > 4095)
              throw EncodeError(ErrorCode::SampleOutOfRange, it.begin, "sample " + std::to_string(sh.adc[q]));
          for (int q = 0; q < 3; ++q)
            out.push_back(repack_hit_word(W[it.begin + q], sh.adc[2 * q], sh.adc[2 * q + 1]));
        } else {
          out.insert(out.end(), W.begin() + it.begin, W.begin() + it.begin + 3);
        }
        break;
      }
      case Kind::BlockHeader: {
        hdr_out = out.size();
        out.push_back(W[it.begin]);
        if (realign) {
          const size_t a = L.block_item_begin_[it.block], b = L.block_item_end_[it.block];
          const uint64_t s = 2 + kept_len(a + 1, b);
          blk_nfill = uint32_t((opt.align - s % opt.align) % opt.align);
          blk_fillw = filler_value(a + 1, b);
          blk_emitted = false;
        }
        break;
      }
      case Kind::Filler:
        if (!realign) {
          out.insert(out.end(), W.begin() + it.begin, W.begin() + it.begin + it.len);
        } else if (it.block != kNone) {
          if (!blk_emitted) { out.insert(out.end(), blk_nfill, blk_fillw); blk_emitted = true; }
        } else if (L.variant_ == Variant::Bare) {
          if (!bare_emitted) { out.insert(out.end(), bare_nfill, bare_fillw); bare_emitted = true; }
        } else {
          out.insert(out.end(), W.begin() + it.begin, W.begin() + it.begin + it.len);   // between blocks
        }
        break;
      case Kind::BlockTrailer: {
        if (realign && !blk_emitted) { out.insert(out.end(), blk_nfill, blk_fillw); blk_emitted = true; }
        const BlockInfo& b = L.blocks_[it.block];
        const int64_t old_len = int64_t(b.trailer_pos) - int64_t(b.header_pos) + 1;
        const int64_t new_len = int64_t(out.size()) + 1 - int64_t(hdr_out);
        const int64_t nc = int64_t(b.num_words) - (old_len - new_len);
        if (nc < 0 || nc > int64_t(kNumWordsMask))
          throw EncodeError(ErrorCode::CountUnderflow, b.trailer_pos, "NUM_WORDS would be " + std::to_string(nc));
        out.push_back((b.trailer_word & ~kNumWordsMask) | uint32_t(nc));
        break;
      }
    }
  }
  if (realign && L.variant_ == Variant::Bare && !bare_emitted) out.insert(out.end(), bare_nfill, bare_fillw);

  if (stats) {
    stats->strips_in = L.nhits_total_;
    stats->strips_out = kept_hits;
    stats->words_in = uint32_t(W.size());
    stats->words_out = uint32_t(out.size());
    stats->protected_frames = nprot;
  }
}

void encode_into(const GemBankLayout& layout, const std::vector<ApvFrame>& frames, const KeepMask& keep,
                 std::vector<uint32_t>& out, const EncodeOptions& opt, EncodeStats* stats) {
  Encoder::run(layout, &frames, keep, false, out, opt, stats);
}

std::vector<uint32_t> encode(const GemBankLayout& layout, const std::vector<ApvFrame>& frames,
                             const KeepMask& keep, const EncodeOptions& opt, EncodeStats* stats) {
  std::vector<uint32_t> out;
  Encoder::run(layout, &frames, keep, false, out, opt, stats);
  return out;
}

std::vector<uint32_t> encode(const GemBankLayout& layout, const KeepMask& keep_by_src,
                             const EncodeOptions& opt, EncodeStats* stats) {
  std::vector<uint32_t> out;
  Encoder::run(layout, nullptr, keep_by_src, true, out, opt, stats);
  return out;
}

// ===========================================================================
// dump
// ===========================================================================
std::string dump(const uint32_t* p, size_t n, const DecodeOptions& opt) {
  std::ostringstream os;
  char buf[160];
  uint32_t cur = kNone, conts = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t w = p[i];
    std::snprintf(buf, sizeof buf, "%6zu  0x%08x  ", i, w);
    os << buf;
    if (is_defining(w)) {
      cur = type_of(w);
      conts = 0;
      switch (cur) {
        case kBlockHeader:
          std::snprintf(buf, sizeof buf, "BLOCK HEADER slot=%u blk#=%u blksize=%u", bh_slot(w), bh_block_number(w),
                        bh_block_size(w));
          break;
        case kBlockTrailer:
          std::snprintf(buf, sizeof buf, "BLOCK TRAILER slot=%u nwords=%u", bt_slot(w), bt_num_words(w));
          break;
        case kEventHeader: std::snprintf(buf, sizeof buf, "EVENT HEADER trig=%u", eh_trigger_number(w)); break;
        case kTriggerTime: std::snprintf(buf, sizeof buf, "TRIGGER TIME lo=0x%06x", w & 0xFFFFFFu); break;
        case kMpdFrame:
          std::snprintf(buf, sizeof buf, "MPD FRAME fiber=%u mpd_id=%u CM=%d ALL=%d CMOR=%d", fh_fiber(w, opt.fiber_bits),
                        fh_mpd_id(w), int((w >> 26) & 1), int((w >> 25) & 1), int((w >> 24) & 1));
          break;
        case kMpdEventInfo: std::snprintf(buf, sizeof buf, "MPD EVENT INFO ts_lo=0x%06x", w & 0xFFFFFFu); break;
        case kMpdDebug:
          std::snprintf(buf, sizeof buf, "MPD DEBUG cm=%d,%d", sample13(hit_lo_sample(w)), sample13(hit_hi_sample(w)));
          break;
        case kDataNotValid: std::snprintf(buf, sizeof buf, "DATA NOT VALID"); break;
        case kFiller: std::snprintf(buf, sizeof buf, "FILLER"); break;
        default: std::snprintf(buf, sizeof buf, "RESERVED TYPE %u", cur); break;
      }
    } else {
      ++conts;
      if (cur == kMpdFrame) {
        const uint32_t q = (conts - 1) % 3;
        if (q == 0)
          std::snprintf(buf, sizeof buf, "  hit.w0 ch[4:0]=%u s0=%d s1=%d", (w >> 26) & 0x1F, sample13(hit_lo_sample(w)),
                        sample13(hit_hi_sample(w)));
        else if (q == 1)
          std::snprintf(buf, sizeof buf, "  hit.w1 ch[6:5]=%u s2=%d s3=%d", (w >> 26) & 0x3, sample13(hit_lo_sample(w)),
                        sample13(hit_hi_sample(w)));
        else
          std::snprintf(buf, sizeof buf, "  hit.w2 apv=%u s4=%d s5=%d", (w >> 26) & 0x1F, sample13(hit_lo_sample(w)),
                        sample13(hit_hi_sample(w)));
      } else if (cur == kNone) {
        std::snprintf(buf, sizeof buf, "ORPHAN CONTINUATION");
      } else {
        std::snprintf(buf, sizeof buf, "  cont #%u of type %u", conts, cur);
      }
    }
    os << buf << '\n';
  }
  return os.str();
}

}  // namespace codec
}  // namespace gemfilter
