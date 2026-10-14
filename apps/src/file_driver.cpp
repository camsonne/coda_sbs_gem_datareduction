#include "gemfilter/apps/file_driver.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace gemfilter {

MetricsCsvWriter::MetricsCsvWriter(std::ostream& os) : os_(os) { os_ << header() << '\n'; }

const char* MetricsCsvWriter::header() {
  return "index,event_number,block_level,class,strips_in,strips_out,bytes_in,bytes_out,"
         "gem_bytes_in,gem_bytes_out,nseeds,nroads,ntracks,flags,cpu_us,wall_us,modified,status";
}

void MetricsCsvWriter::write(uint64_t index, const EventResult& r) {
  os_ << index << ',' << r.event_number << ',' << unsigned(r.block_level) << ','
      << coda::to_string(r.cls) << ',' << r.stats.strips_in << ',' << r.stats.strips_out << ','
      << uint64_t(r.words_in) * 4 << ',' << uint64_t(r.words_out) * 4 << ',' << r.stats.bytes_in
      << ',' << r.stats.bytes_out << ',' << r.stats.nseeds << ',' << r.stats.nroads << ','
      << r.stats.ntracks << ",0x" << std::hex << std::setw(4) << std::setfill('0') << r.stats.flags
      << std::dec << std::setfill(' ') << ',' << r.cpu_us << ',' << std::fixed
      << std::setprecision(1) << r.wall_us << std::defaultfloat << ',' << (r.modified ? 1 : 0)
      << ',' << (r.status_appended ? 1 : 0) << '\n';
}

FileDriver::FileDriver(EventSource& src, EventSink* sink, MetricsCsvWriter* csv,
                       FilterPolicy& policy, FileDriverConfig cfg)
    : src_(src), sink_(sink), csv_(csv), pol_(policy), cfg_(cfg) {}

FileDriverSummary FileDriver::run() {
  FileDriverSummary s;
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<uint32_t> buf;
  size_t n = 0;
  uint64_t index = 0;
  while (src_.next(buf, n, cfg_.headroom_words)) {
    const uint64_t i = index++;
    if (i < cfg_.skip) continue;
    if (cfg_.max_events && s.events >= cfg_.max_events) break;
    const size_t cap = buf.size();
    EventResult r = pol_.apply(buf.data(), n, cap);
    s.events++;
    s.by_class[coda::to_string(r.cls)]++;
    s.words_in += r.words_in;
    s.words_out += r.words_out;
    if (r.cls == coda::EventClass::Physics) {
      s.physics++;
      s.strips_in += r.stats.strips_in;
      s.strips_out += r.stats.strips_out;
      s.cpu_us += r.cpu_us;
      if (r.modified) s.modified++;
      if (r.status_appended) s.status_appended++;
      if (r.stats.flags & flags::kPassthroughPrescale) s.prescaled++;
      if (r.stats.flags & (flags::kDecodeError | flags::kNoRoom)) s.errors++;
    }
    if (csv_) csv_->write(i, r);
    if (sink_) sink_->write(buf.data(), n);
  }
  if (sink_) sink_->close();
  s.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return s;
}

std::string FileDriverSummary::to_string() const {
  std::ostringstream os;
  os << "events=" << events << " physics=" << physics << " modified=" << modified
     << " prescaled=" << prescaled << " errors=" << errors << " status_banks=" << status_appended
     << " bytes_in=" << words_in * 4 << " bytes_out=" << words_out * 4 << " ratio="
     << (words_out ? double(words_in) / double(words_out) : 0.0) << " strips_in=" << strips_in
     << " strips_out=" << strips_out << " cpu_us/physics="
     << (physics ? double(cpu_us) / double(physics) : 0.0) << " wall_s=" << wall_s << " classes:";
  for (const auto& kv : by_class) os << ' ' << kv.first << '=' << kv.second;
  return os.str();
}

void BankInventory::add(const uint32_t* ev, size_t nwords) {
  events++;
  const auto cls = coda::classify(ev, nwords);
  event_classes[coda::to_string(cls)]++;
  if (nwords >= 2) top_tags[static_cast<uint16_t>(ev[1] >> 16)]++;
  coda::PhysicsLayout lay;
  if (cls != coda::EventClass::Physics || !coda::parse_physics(ev, nwords, lay)) return;
  std::vector<coda::BankRef> kids;
  for (const auto& roc : lay.rocs) {
    if (!coda::is_bank_of_banks(roc.hdr.type)) {
      auto& e = roc_bank[{coda::roc_id(roc.hdr), 0xFFFF}];  // non-bank ROC payload
      e.count++;
      e.words += roc.hdr.length + 1;
      continue;
    }
    if (!coda::list_children(ev, nwords, roc.offset, kids)) continue;
    for (const auto& k : kids) {
      auto& e = roc_bank[{coda::roc_id(roc.hdr), k.hdr.tag}];
      e.count++;
      e.words += k.hdr.length + 1;
    }
  }
}

void BankInventory::print(std::ostream& os) const {
  os << "# events " << events << "\n# classes:";
  for (const auto& kv : event_classes) os << ' ' << kv.first << '=' << kv.second;
  os << "\n# top-level tags:";
  for (const auto& kv : top_tags) os << " 0x" << std::hex << kv.first << std::dec << '=' << kv.second;
  os << "\nroc,bank_tag,bank_tag_hex,count,mean_words,collides_with_status_tag\n";
  for (const auto& kv : roc_bank) {
    const auto [roc, tag] = kv.first;
    os << roc << ',' << tag << ",0x" << std::hex << tag << std::dec << ',' << kv.second.count << ','
       << (kv.second.count ? double(kv.second.words) / double(kv.second.count) : 0.0) << ','
       << (tag == kStatusBankTag ? "YES" : "no") << '\n';
  }
}

}  // namespace gemfilter
