# gemfilter — module interfaces and pipeline contract

Goal: online GEM data suppression for SBS (next run: SIDIS, BigBite + SBS arms), as a CODA3
software filter between the Event Builder and the Event Recorder (ET station), with a file-mode
driver that runs on raw EVIO from previous SBS runs (GMn: SSP-era MPD data; GEn-II/GEp: VTP-era).

Language: C++17, CMake >= 3.16, no ROOT dependency in the core. Namespace `gemfilter`.
Builds on macOS (Apple clang 17) and Linux gcc >= 9. Third-party: JLab evio (C / C++ libs), JLab ET.

## Source tree
```
CMakeLists.txt              top-level build
include/gemfilter/types.h   shared types (given below; extend only by adding, never changing)
codec/      include + src   MPD/SSP/VTP decode + re-encode
evioio/     include + src   EVIO event/bank access & rewrite
geom/       include + src   Podd DB readers, module geometry, APV maps
seeds/      include + src   VTP cluster + FADC250 seeds, road corridors
track/      include + src   clustering, road tracker
pipeline/                   per-event processor (GemFilterProcessor)
apps/                       gemfilter_file, gemfilter_et
et/                         ET wrapper
status/                     GEMFILTER status bank encode/decode
tools/                      benchmark, ET test tools, sbsdig2evio converter
tests/<module>/             unit and integration tests, registered with CTest
docs/                       design and format notes
third_party/                build recipe for evio + et (no vendored sources)
```

## Pipeline (per built event)
1. evioio: parse built event -> trigger bank + list of ROC banks (crate id, bank tag/num, payload span).
2. codec: for each ROC bank holding GEM data -> `std::vector<ApvFrame>` (decoded) + an opaque
   `GemBankLayout` that lets it re-encode a *subset* of strips bit-exactly in the original format.
3. seeds: from VTP and/or FADC banks -> `std::vector<CaloSeed>`; geom+seeds -> `std::vector<Road>`.
4. track: ApvFrames + geometry + roads -> keep mask per strip (`KeepMask`).
5. codec re-encode -> new payload; evioio replaces ROC bank; status appends GEMFILTER bank.

## Error policy
Any decode/encode exception for an event => event passes through UNMODIFIED, status flag set.
The filter must never drop a whole event; it only removes strips. Prescaled raw pass-through.
