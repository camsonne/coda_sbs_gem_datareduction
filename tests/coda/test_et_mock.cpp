// In-process test of the ET station filter (EtFilter) with a mock ET backend:
//   * multi-threaded processing preserves event order (CODA ER requires in-order events)
//   * control (prestart/go/end) and user events pass bit-exact
//   * physics events are processed by the given processor (truncate test processor and the real
//     GEM pipeline on SYNTHETIC events) and shrunk in place (set_length)
//   * ET death -> reconnect -> processing resumes; nothing is duplicated
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "gemfilter/et/et_filter.h"
#include "gemfilter/et/evio_framing.h"
#include "gemfilter/status/coda_event.h"
#include "gemfilter/status/processors.h"
#include "synth.h"
#include "testing.h"

using namespace gemfilter;
using namespace gemfilter::et;

class MockEt final : public EtBackend {
 public:
  struct Ev { std::vector<uint32_t> buf; size_t len; };
  explicit MockEt(std::vector<std::vector<uint32_t>> in, int die_after_gets = -1) : die_after_(die_after_gets) {
    for (auto& e : in) {
      Ev x; x.len = e.size() * 4; x.buf = e; x.buf.resize(e.size() + 1024);   // ET events have spare room
      pool_.push_back(std::move(x));
    }
    for (size_t i = 0; i < pool_.size(); ++i) queue_.push_back(i);
  }
  EtStatus connect(std::string& msg) override { msg = "mock"; dead_ = false; ++connects; return EtStatus::Ok; }
  EtStatus get(std::vector<EtEventRef>& out, int max, int) override {
    std::lock_guard<std::mutex> l(m_);
    if (dead_) return EtStatus::Dead;
    if (die_after_ >= 0 && gets_++ == die_after_) { dead_ = true; die_after_ = -1; return EtStatus::Dead; }
    out.clear();
    if (queue_.empty()) return EtStatus::Timeout;
    for (int i = 0; i < max && !queue_.empty(); ++i) {
      size_t k = queue_.front(); queue_.pop_front();
      EtEventRef r; r.handle = reinterpret_cast<void*>(k + 1); r.data = pool_[k].buf.data();
      r.length = pool_[k].len; r.capacity = pool_[k].buf.size() * 4;
      out.push_back(r);
    }
    return EtStatus::Ok;
  }
  EtStatus put(std::vector<EtEventRef>& evs) override {
    std::lock_guard<std::mutex> l(m_);
    for (auto& r : evs) {
      size_t k = reinterpret_cast<size_t>(r.handle) - 1;
      put_order.push_back(k);
      output.emplace_back(r.data, r.data + r.length / 4);
    }
    return EtStatus::Ok;
  }
  EtStatus dump(std::vector<EtEventRef>&) override { return EtStatus::Ok; }
  EtStatus new_event(size_t, int, EtEventRef&) override { return EtStatus::Error; }
  void copy_meta(const EtEventRef&, EtEventRef&) override {}
  void set_length(EtEventRef& ev, size_t bytes) override {
    size_t k = reinterpret_cast<size_t>(ev.handle) - 1;
    pool_[k].len = bytes; ev.length = bytes;
  }
  bool alive() override { return !dead_; }
  void disconnect(bool, bool) override {}
  std::string describe() const override { return "mock"; }
  bool drained() { std::lock_guard<std::mutex> l(m_); return queue_.empty() && put_order.size() == pool_.size(); }
  bool queue_empty() { std::lock_guard<std::mutex> l(m_); return queue_.empty(); }

  std::vector<size_t> put_order;
  std::vector<std::vector<uint32_t>> output;
  int connects = 0;
 private:
  std::vector<Ev> pool_;
  std::deque<size_t> queue_;
  std::mutex m_;
  bool dead_ = false;
  int die_after_ = -1, gets_ = 0;
};

static std::vector<std::vector<uint32_t>> frame(const std::vector<std::vector<uint32_t>>& evs, Framing f) {
  std::vector<std::vector<uint32_t>> out;
  uint32_t n = 1;
  for (auto& e : evs) {
    auto cls = coda::classify(e.data(), e.size());
    BufferEventType t = cls == coda::EventClass::Physics ? BufferEventType::Physics
                      : cls == coda::EventClass::Control ? BufferEventType::Control : BufferEventType::User;
    out.push_back(build_buffer(f, {e}, n++, t, true));
  }
  return out;
}
static std::vector<std::vector<uint32_t>> stream(int nphys) {
  std::vector<std::vector<uint32_t>> v;
  v.push_back(synth::control(coda::kTagPrestart));
  v.push_back(synth::control(coda::kTagGo));
  for (int i = 0; i < nphys; ++i) {
    v.push_back(synth::physics(uint64_t(1 + 5 * i), 5, synth::default_rocs(200 + i)));
    if (i % 17 == 5) v.push_back(synth::user(137, 8));   // e.g. EPICS
  }
  v.push_back(synth::control(coda::kTagEnd));
  return v;
}

static EtFilterSnapshot runFilter(MockEt& be, ProcessorFactory f, PolicyConfig pc, int nthreads) {
  EtFilterConfig cfg; cfg.nthreads = nthreads; cfg.chunk = 8; cfg.get_timeout_ms = 5; cfg.busy_poll_ms = 1;
  cfg.stats_interval_s = 0; cfg.reconnect_min_ms = 1; cfg.reconnect_max_ms = 5; cfg.max_reconnects = 5;
  EtFilter ef(be, f, pc, cfg);
  std::atomic<bool> stop{false};
  std::thread t([&] { ef.run(stop); });
  for (int i = 0; i < 2000 && !be.drained(); ++i) {
    if (be.queue_empty() && ef.snapshot().inflight == 0 && i > 50) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  stop = true;
  t.join();
  return ef.snapshot();
}

TEST_CASE(order_and_untouched_controls) {
  auto raw = stream(120);
  auto in = frame(raw, Framing::EvioV4);
  MockEt be(in);
  PolicyConfig pc;
  runFilter(be, [] { return std::unique_ptr<EventProcessor>(new TruncateProcessor(0.5)); }, pc, 4);
  CHECK(be.drained());
  CHECK_EQ(be.put_order.size(), in.size());
  for (size_t i = 0; i < be.put_order.size(); ++i) CHECK_EQ(be.put_order[i], i);
  size_t shrunk = 0;
  for (size_t i = 0; i < in.size(); ++i) {
    auto cls = coda::classify(raw[i].data(), raw[i].size());
    if (cls != coda::EventClass::Physics) CHECK(be.output[i] == in[i]);
    else shrunk += be.output[i].size() < in[i].size();
    std::vector<std::vector<uint32_t>> evs;
    CHECK(split_buffer(be.output[i].data(), be.output[i].size(), evs));   // output still valid EVIO
    CHECK_EQ(evs.size(), size_t(1));
  }
  CHECK_EQ(shrunk, size_t(120));
}

TEST_CASE(reconnect_after_death) {
  auto in = frame(stream(60), Framing::EvioV6);
  MockEt be(in, /*die_after_gets=*/3);
  PolicyConfig pc;
  auto snap = runFilter(be, [] { return std::unique_ptr<EventProcessor>(new PassThroughProcessor()); }, pc, 2);
  CHECK(be.connects >= 2);
  CHECK(snap.reconnects >= 1);
  // events held when ET died are discarded (as with a real dead ET system); the rest is delivered
  // exactly once and in order
  CHECK_EQ(be.put_order.size() + snap.discarded_on_death, in.size());
  for (size_t i = 1; i < be.put_order.size(); ++i) CHECK(be.put_order[i] > be.put_order[i - 1]);
}

TEST_MAIN()
