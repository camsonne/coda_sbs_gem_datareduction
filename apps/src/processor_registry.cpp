#include "gemfilter/apps/processor_registry.h"

#include <map>
#include <mutex>
#include <stdexcept>

#include "gemfilter/status/processors.h"
#ifdef GEMFILTER_HAVE_PIPELINE
#include "gemfilter/pipeline_registration.h"
#endif

namespace gemfilter {

namespace {
std::mutex& reg_mutex() {
  static std::mutex m;
  return m;
}
std::map<std::string, ProcessorMaker>& reg() {
  static std::map<std::string, ProcessorMaker> r;
  return r;
}
void ensure_builtins() {
  static std::once_flag once;
  std::call_once(once, [] {
    register_processor("passthrough", [](const std::string&) {
      return std::unique_ptr<EventProcessor>(new PassThroughProcessor());
    });
    register_processor("truncate", [](const std::string& a) {
      const double f = a.empty() ? 0.5 : std::stod(a);
      return std::unique_ptr<EventProcessor>(new TruncateProcessor(f));
    });
#ifdef GEMFILTER_HAVE_PIPELINE
    register_pipeline_processors();
#endif
  });
}
}  // namespace

void register_processor(const std::string& name, ProcessorMaker maker) {
  std::lock_guard<std::mutex> l(reg_mutex());
  reg()[name] = std::move(maker);
}

std::vector<std::string> registered_processors() {
  ensure_builtins();
  std::lock_guard<std::mutex> l(reg_mutex());
  std::vector<std::string> v;
  for (const auto& kv : reg()) v.push_back(kv.first);
  return v;
}

ProcessorFactory make_processor_factory(const std::string& spec) {
  ensure_builtins();
  const auto colon = spec.find(':');
  const std::string name = spec.substr(0, colon);
  const std::string args = colon == std::string::npos ? "" : spec.substr(colon + 1);
  ProcessorMaker maker;
  {
    std::lock_guard<std::mutex> l(reg_mutex());
    auto it = reg().find(name);
    if (it == reg().end()) throw std::invalid_argument("unknown processor '" + name + "'");
    maker = it->second;
  }
  if (!maker(args)) throw std::invalid_argument("processor '" + name + "' returned null");
  return [maker, args]() { return maker(args); };
}

}  // namespace gemfilter
