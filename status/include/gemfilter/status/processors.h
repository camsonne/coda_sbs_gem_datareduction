// gemfilter — trivial EventProcessor implementations used until the codec/track pipeline is
// plugged in, and for integration tests.
#pragma once
#include <vector>

#include "gemfilter/processor.h"

namespace gemfilter {

// Leaves the event unchanged; reports bytes in/out of the whole event.
class PassThroughProcessor final : public EventProcessor {
 public:
  FilterStats process(uint32_t* buf, size_t& nwords, size_t cap) override;
  std::string name() const override { return "passthrough"; }
};

// TEST-ONLY.  Structurally valid shrinking edit: inside every ROC bank (bank of banks), each
// direct child bank whose tag is in `bank_tags` keeps only floor(keep_fraction * payload)
// payload words (at least `min_keep`); all parent lengths are updated.  "strips" are counted
// as payload words.  This is NOT a physics algorithm; it exercises in-place shrinking,
// set-length, and downstream verification in the ET/file integration tests.
class TruncateProcessor final : public EventProcessor {
 public:
  explicit TruncateProcessor(double keep_fraction = 0.5,
                             std::vector<uint16_t> bank_tags = {3561}, uint32_t min_keep = 0);
  FilterStats process(uint32_t* buf, size_t& nwords, size_t cap) override;
  std::string name() const override { return "truncate"; }
  std::string config_string() const override;
  // Expected payload words kept for a payload of n words (for test verification).
  uint32_t kept(uint32_t n) const;

 private:
  double keep_;
  std::vector<uint16_t> tags_;
  uint32_t min_keep_;
};

}  // namespace gemfilter
