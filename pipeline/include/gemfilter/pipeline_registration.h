// Included only by apps/src/processor_registry.cpp when GEMFILTER_HAVE_PIPELINE is defined.
#pragma once
#include "gemfilter/apps/processor_registry.h"
#include "gemfilter/pipeline/GemFilterProcessor.h"
namespace gemfilter {
inline void register_pipeline_processors() { register_processor("gem", pipeline::gem_processor_maker()); }
}  // namespace gemfilter
