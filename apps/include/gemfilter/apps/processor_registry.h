// gemfilter — processor registry: maps a command-line spec "name[:args]" to a
// ProcessorFactory (one EventProcessor instance per worker thread).
//
// Built-ins:  passthrough            no change (status bank only)
//             truncate[:frac]        TEST-ONLY shrinking edit (keep frac of bank-3561 payload)
//
// Plugging in the real pipeline (codec + seeds + track): the pipeline track provides a CMake
// target `gemfilter_pipeline` and a header "gemfilter/pipeline_registration.h" declaring
//     namespace gemfilter { void register_pipeline_processors(); }
// which calls register_processor("gem", ...).  apps/CMakeLists.txt links that target and
// defines GEMFILTER_HAVE_PIPELINE when it exists; builtin registration then calls it.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gemfilter/processor.h"

namespace gemfilter {

using ProcessorMaker = std::function<std::unique_ptr<EventProcessor>(const std::string& args)>;

void register_processor(const std::string& name, ProcessorMaker maker);
std::vector<std::string> registered_processors();
// Throws std::invalid_argument for unknown names / bad args.  The returned factory is
// validated by constructing one instance.
ProcessorFactory make_processor_factory(const std::string& spec);

}  // namespace gemfilter
