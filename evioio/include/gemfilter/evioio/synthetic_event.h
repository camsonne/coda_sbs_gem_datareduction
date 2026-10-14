// gemfilter evioio: builder for SYNTHETIC CODA3-like built physics events (tests only).
// Follows the layout in evio_event.h / Podd CodaDecoder; contents are synthetic.
#pragma once
#include <cstdint>
#include <vector>

namespace gemfilter {
namespace evioio {
namespace synth {

struct DataBankSpec { uint16_t tag = 0; uint8_t num = 0; uint8_t dtype = 0x01; std::vector<uint32_t> payload; };
struct RocSpec { uint16_t roc_id = 0; std::vector<DataBankSpec> banks; };
struct PhysicsEventSpec {
  uint16_t event_tag = 0xFF50;       // built physics event
  uint8_t block_level = 5;
  uint64_t first_event_number = 1;
  bool timestamps = true;            // trigger bank tag 0xFF21 (else 0xFF20)
  uint16_t tsroc = 21;               // ROC whose trigger segment carries trigger bits
  std::vector<RocSpec> rocs;
};

std::vector<uint32_t> make_physics_event(const PhysicsEventSpec& s);
// Simple control event (e.g. tag 0xFFD1 prestart): bank of uint32 {time, run, type}.
std::vector<uint32_t> make_control_event(uint16_t tag, uint32_t run, uint32_t run_type, uint32_t time);

}  // namespace synth
}  // namespace evioio
}  // namespace gemfilter
