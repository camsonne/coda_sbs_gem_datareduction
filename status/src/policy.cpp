#include "gemfilter/status/policy.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <sstream>
#include <stdexcept>

namespace gemfilter {

uint64_t thread_cpu_us() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
}

FilterPolicy::FilterPolicy(std::unique_ptr<EventProcessor> proc, PolicyConfig cfg,
                           std::shared_ptr<std::atomic<uint64_t>> physics_counter)
    : proc_(std::move(proc)), cfg_(std::move(cfg)), counter_(std::move(physics_counter)) {
  if (!proc_) throw std::invalid_argument("FilterPolicy: null processor");
  if (!counter_) counter_ = std::make_shared<std::atomic<uint64_t>>(0);
  std::ostringstream os;
  os << "gemfilter-status-v" << int(kStatusBankVersion) << ';' << proc_->config_string()
     << ";prescale=" << cfg_.prescale << ";gemtags=";
  for (auto t : cfg_.gem_bank_tags) os << t << ',';
  hash_ = fnv1a64(os.str());
}

namespace {
bool valid_physics_output(const uint32_t* b, size_t n, size_t cap) {
  if (n < 2 || n > cap) return false;
  if (static_cast<size_t>(b[0]) + 1 != n) return false;
  coda::PhysicsLayout lay;
  return coda::parse_physics(b, n, lay);
}
}  // namespace

EventResult FilterPolicy::apply(uint32_t* buf, size_t& nwords, size_t cap) {
  const auto t0 = std::chrono::steady_clock::now();
  EventResult res;
  res.words_in = static_cast<uint32_t>(nwords);
  res.cls = coda::classify(buf, nwords);
  auto finish = [&]() {
    res.words_out = static_cast<uint32_t>(nwords);
    res.wall_us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return res;
  };
  if (res.cls != coda::EventClass::Physics) {
    res.stats.flags |= (res.cls == coda::EventClass::Malformed) ? flags::kMalformed : flags::kNotPhysics;
    return finish();
  }
  coda::PhysicsLayout lay;
  if (!coda::parse_physics(buf, nwords, lay)) {
    res.cls = coda::EventClass::Malformed;
    res.stats.flags |= flags::kMalformed;
    return finish();
  }
  // Only operate on the event proper (ignore trailing words beyond the bank).
  nwords = static_cast<size_t>(buf[0]) + 1;
  res.words_in = static_cast<uint32_t>(nwords);
  res.event_number = lay.first_event_number;
  res.block_level = lay.block_level;
  const uint64_t count = ++(*counter_);
  res.prescale_counter = static_cast<uint32_t>(count);
  const bool raw = cfg_.prescale > 0 && (count % cfg_.prescale) == 0;

  if (raw) {
    res.stats.flags |= flags::kPassthroughPrescale;
    res.stats.bytes_in = res.stats.bytes_out = static_cast<uint32_t>(nwords * 4);
  } else {
    scratch_.resize(cap);
    std::memcpy(scratch_.data(), buf, nwords * sizeof(uint32_t));
    size_t n2 = nwords;
    const uint64_t c0 = thread_cpu_us();
    bool ok = true;
    try {
      res.stats = proc_->process(scratch_.data(), n2, cap);
    } catch (...) {
      ok = false;
      res.stats = FilterStats{};
      res.stats.flags |= flags::kDecodeError;
    }
    res.cpu_us = static_cast<uint32_t>(thread_cpu_us() - c0);
    if (ok && (res.stats.flags & flags::kNoRoom)) ok = false;
    if (ok && !valid_physics_output(scratch_.data(), n2, cap)) {
      ok = false;
      res.stats.flags |= flags::kDecodeError;
    }
    if (ok) {
      std::memcpy(buf, scratch_.data(), n2 * sizeof(uint32_t));
      nwords = n2;
      res.modified = true;
    } else {
      // Unmodified event: report the event as passed through.
      res.stats.strips_out = res.stats.strips_in;
      res.stats.bytes_in = res.stats.bytes_out = static_cast<uint32_t>(nwords * 4);
    }
  }

  if (cfg_.append_status && (!raw || cfg_.status_on_prescaled)) {
    auto host = find_gem_host_roc(buf, nwords, cfg_.gem_bank_tags);
    if (!host) {
      res.stats.flags |= flags::kNoGemRoc;
    } else {
      StatusRecord r;
      r.config_hash = hash_;
      r.stats = res.stats;
      r.prescale_counter = res.prescale_counter;
      r.prescale_factor = cfg_.prescale;
      r.cpu_us = res.cpu_us;
      r.instance_id = cfg_.instance_id;
      if (append_status_to_roc(buf, nwords, cap, *host, r))
        res.status_appended = true;
      else
        res.stats.flags |= flags::kStatusNotAppended;
    }
  }
  return finish();
}

}  // namespace gemfilter
