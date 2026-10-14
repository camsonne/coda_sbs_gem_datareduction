// gemfilter / pipeline : the production EventProcessor.
//
// Per ET/EVIO event (a CODA3 built event holding a block of N physics events):
//   1. parse the event (evioio::EventView), find all GEM banks (tag gem_bank_tag, any ROC);
//   2. decode each GEM bank (codec) -> ApvFrames for the N events;
//   3. per physics event e: calorimeter seeds (FADC250 clusters or VTP clusters) -> constraint
//      points (BigBite or SBS arm logic, seeds/Roads.h) -> per-module U/V windows;
//   4. GemEventFilter (RoadOnly or Track) -> keep mask; no-seed policy if no seed;
//   5. re-encode every GEM bank with its mask, rebuild the event in place (only if it shrinks).
// Any exception leaves the event untouched (flags::kDecodeError via the policy layer).
//
// Shared, immutable state (DB geometry, APV map, calorimeter geometry, crate map) lives in a
// PipelineContext shared by all worker threads; each worker owns one GemFilterProcessor.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gemfilter/codec/gem_codec.h"
#include "gemfilter/geom/ArmConfig.h"
#include "gemfilter/geom/CaloGeometry.h"
#include "gemfilter/geom/GemGeometry.h"
#include "gemfilter/processor.h"
#include "gemfilter/seeds/CaloClustering.h"
#include "gemfilter/seeds/Roads.h"
#include "gemfilter/seeds/VtpCluster.h"
#include "gemfilter/track/GemEventFilter.h"

namespace gemfilter {
namespace pipeline {

struct PipelineConfig {
  enum class Arm : uint8_t { BigBite = 0, SBS = 1 };
  enum class SeedSource : uint8_t { FADC = 0, VTP = 1, None = 2 };
  enum class NoSeed : uint8_t { KeepAll = 0, DropAll = 1, Global = 2 };

  std::string db_dir = "DB";
  geom::DbTime date = geom::kLatest;
  Arm arm = Arm::BigBite;
  std::string gem_tracker = "bb.gem";          // DB prefix of the GEM tracker
  std::vector<uint16_t> gem_bank_tags{3561};
  SeedSource seed_source = SeedSource::FADC;
  std::string calo = "bb.sh";                  // shower / HCal DB prefix
  std::string calo_ps = "bb.ps";               // BigBite preshower ("" = none)
  int vtp_roc = -1;                            // VTP cluster bank location (SeedSource::VTP)
  uint16_t vtp_bank_tag = 0;
  seeds::VtpSeedOptions vtp;
  seeds::ClusterOptions cluster;               // filled from the calorimeter DB, overridable
  bool cluster_from_db = true;
  double seed_emin = 0.0;                      // minimum seed energy (calibrated units)
  int max_seeds = 4;                           // highest-energy seeds turned into roads
  NoSeed no_seed = NoSeed::KeepAll;
  seeds::RoadOptions road;
  track::TrackerConfig tracker;
  codec::DecodeOptions decode;
  codec::EncodeOptions encode;
  geom::ApvKeyField apv_key = geom::ApvKeyField::Fiber;
  bool apv_match_slot = true;

  // key = value text ('#' comments).  Unknown keys throw std::invalid_argument.
  static PipelineConfig parse(const std::string& text);
  static PipelineConfig load(const std::string& path);
  std::string to_string() const;               // canonical form (feeds the status-bank config hash)
};

struct PipelineContext {
  PipelineConfig cfg;
  geom::GemTracker gem;
  geom::ApvMap apvmap;                         // points into gem
  geom::CrateMap cratemap;
  geom::CaloGeometry calo, calo_ps;
  bool have_ps = false;
  geom::BigBiteConfig bb;
  geom::SbsArmConfig sbs;
  std::vector<std::pair<uint16_t, uint16_t>> fadc_banks;   // (roc, bank tag) holding calorimeter FADCs

  static std::shared_ptr<const PipelineContext> build(const PipelineConfig& cfg);
  // for tests: geometry supplied directly
  static std::shared_ptr<const PipelineContext> build(const PipelineConfig& cfg, geom::GemTracker gem,
                                                      geom::CaloGeometry calo);
 private:
  void finish();
};

struct EventDetail {                            // per physics event (monitoring / tests)
  uint32_t strips_in = 0, strips_out = 0, nseeds = 0, ntracks = 0;
  bool fallback = false, no_seed = false;
};

class GemFilterProcessor final : public EventProcessor {
 public:
  explicit GemFilterProcessor(std::shared_ptr<const PipelineContext> ctx);
  FilterStats process(uint32_t* buf, size_t& nwords, size_t cap) override;
  std::string name() const override { return "gem"; }
  std::string config_string() const override { return "gem\n" + ctx_->cfg.to_string(); }
  const std::vector<EventDetail>& last_details() const { return details_; }

  // seeds -> constraint points for one physics event (exposed for tests)
  std::vector<seeds::ConstraintPoints> constraints(const std::vector<CaloSeed>& sh,
                                                   const std::vector<CaloSeed>& ps) const;

 private:
  std::shared_ptr<const PipelineContext> ctx_;
  track::GemEventFilter filter_;
  std::vector<codec::DecodedBank> banks_;
  std::vector<KeepMask> masks_;
  std::vector<EventDetail> details_;
  std::vector<uint32_t> out_;
};

// Maker for "gem:<config file>" (registered by the apps processor registry).
using ProcessorMaker = std::function<std::unique_ptr<EventProcessor>(const std::string& args)>;
ProcessorMaker gem_processor_maker();

}  // namespace pipeline
}  // namespace gemfilter
