# SBS GEM raw-data format: MPD readout through SSP / VTP (bit-level reference)

Track: GEM codec (gemfilter). Status: written from source code and manuals, **not yet
cross-checked against a real SBS raw EVIO file** (none was available to this track).
Items that could not be confirmed from the two primary sources are marked **UNVERIFIED**
and are listed again in §9.

## 0. Sources

| Tag | Source | What it fixes |
|-----|--------|---------------|
| [SSPDOC] | B. Raydo, *SSP_MPD Event Format*, JLab Fast Electronics Group, 4 Sep 2020, `coda.jlab.org/drupal/system/files/pdfs/HardwareManual/SSP/SSP_MPD_EventFormat.pdf` (7 pp.) | word categories, type list, block header/trailer, event header, trigger time, MPD frame header (FIBER 0..31, MPD_ID), 3-word APV hit, data-not-valid, filler |
| [MPDMOD] | `SBS-offline/MPDModule.cxx` (master), `Decoder::MPDModule`, module type **3561** — the decoder used in production for GMn, GEn-II, GEn-RP, GEp | production interpretation: ENABLE_CM / BUILD_ALL_SAMPLES / CM_OR flags in the frame header, 6-bit FIBER (40-MPD firmware), types 12 (MPD event info) and 13 (MPD debug / online CM), bare-VTP mode (no block header -> slot 11) |
| [V5SSP] | commented-out "v5 decoder with SSP online zero suppression" in `MPDModule.cxx` (J.C. Cornejo 2018) | word order inside an SSP block, filler position (before block trailer), NUM_WORDS = header..trailer inclusive check, 22-bit event-count mask |
| [GEMMOD] | `SBS-offline/SBSGEMModule.cxx` | meaning of the flags, time-stamp and debug-word unpacking, `chanmap` "mpd_id" == FIBER |
| [VMEV4] | `SBS-offline/MPDModuleVMEv4.cxx`, module type 3560 | legacy direct-VME MPD v4 format (non-ZS, test stands) |
| [PODD] | Podd `hana_decode/CodaDecoder.cxx`, `THaCrateMap.cxx` | CODA3 built-event structure, trigger bank, ROC/bank lookup |
| [CRATEMAP] | `SBS-replay/DB/db_cratemap.dat` (master) | which crates/banks carry MPD data |
| [MPD4] | MPD4 user manual (coda.jlab.org) via `manual_facts.txt` | APV/ADC context only (no SSP/VTP word format inside) |

Word numbering: bit 31 = MSB of a 32-bit word in host order (EVIO handles the byte order).

## 1. Word categories (common to SSP and VTP MPD readout) [SSPDOC p2, MPDMOD]

The SSP/VTP MPD format uses the FADC250 encoding:

| bit 31 | meaning | payload |
|--------|---------|---------|
| 1 | **data-type defining word** | type tag in bits 30..27, payload in 26..0 |
| 0 | **data-type continuation word** | payload in bits 30..0, belongs to the most recent defining word |

| type | name | words | source |
|------|------|-------|--------|
| 0  | Block header | 1 | SSPDOC |
| 1  | Block trailer | 1 | SSPDOC |
| 2  | Event header | 1 | SSPDOC |
| 3  | Trigger time | 2 (1 def + 1 cont) | SSPDOC |
| 4  | reserved | – | SSPDOC |
| 5  | MPD frame (header + 3·N hit words) | 1 + 3N | SSPDOC, MPDMOD |
| 6–11 | reserved | – | SSPDOC |
| 12 | MPD event info (time stamps, event count) | 3 (1 def + 2 cont) | MPDMOD only (reserved in SSPDOC 2020) |
| 13 | MPD debug header = online common-mode values | 3 (1 def + 2 cont) | MPDMOD/GEMMOD only (reserved in SSPDOC 2020) |
| 14 | Data not valid (empty module) | 1 | SSPDOC (doc writes "0x14", the tag is decimal 14 = `0xF0000000` prefix) |
| 15 | Filler | 1 | SSPDOC (typical value `0xF8000000`, [V5SSP]) |

## 2. Per-type bit layout

### 2.1 Block header — type 0 (`1 0000 ...`) [SSPDOC p2]

| bits | field | notes |
|------|-------|-------|
| 31 | 1 | |
| 30..27 | 0000 | |
| 26..22 | SLOTID | VME64x geographic slot |
| 21..18 | 0 | |
| 17..8 | BLOCK_NUMBER | 10 bits, block counter |
| 7..0 | BLOCK_SIZE | number of events in the block (block level, 5 in SBS) |

SBS-offline: `slot = (w & 0x07C00000) >> 22`; the presence of a block header is what
MPDModule calls "is_SSP".

### 2.2 Block trailer — type 1 [SSPDOC p3]

| bits | field | notes |
|------|-------|-------|
| 26..22 | SLOTID | |
| 21..0 | NUM_WORDS | "total number of words in block of events". [V5SSP] checks it equals the count from block header to block trailer **inclusive, including filler words that precede the trailer**. |

### 2.3 Event header — type 2 [SSPDOC p3]

| bits | field | notes |
|------|-------|-------|
| 26..0 | TRIGGER_NUMBER | doc text: 27-bit trigger number; the bit picture labels only 15..0; [V5SSP] masks 22 bits (`0x3FFFFF`). **UNVERIFIED width** — the codec keeps the raw word. |

### 2.4 Trigger time — type 3, 2 words [SSPDOC p4]

| word | bits | field |
|------|------|-------|
| 1 (defining) | 26..24 = 000, 23..0 | TRIGGER_TIME_L (low 24 bits of 48-bit counter, 250 MHz system clock) |
| 2 (continuation) | 30..24 = 0, 23..0 | TRIGGER_TIME_H (high 24 bits) |

MPDModule ignores these words (it neither decodes nor stores them).

### 2.5 MPD frame header — type 5 [SSPDOC p5, MPDMOD]

| bits | field | SSPDOC 2020 | MPDMOD (production firmware) |
|------|-------|-------------|------------------------------|
| 26 | ENABLE_CM | undefined | online common-mode subtraction enabled |
| 25 | BUILD_ALL_SAMPLES | undefined | 1 = **no** online zero suppression: all 128 strips of the APV are sent |
| 24 | CM_OR | undefined | common-mode "out of range" -> APV sent in full (forces offline CM) |
| 23..22 | – | undefined | not used |
| 21..16 | FIBER | bits 20..16, 0..31 | **bits 21..16 ("NEW firmware to support up to 40 MPDs per VTP")**; bit 21 is 0 on old firmware, so the 6-bit mask is backward compatible |
| 15..5 | – | undefined | not used |
| 4..0 | MPD_ID | programmable MPD id | decoded but ignored by MPDModule |

Followed by 3·N continuation words (N fired strips), §2.6. A frame with N = 0 is legal for
MPDModule (header only).

Flag semantics from [GEMMOD]: `cm_flags = 4*CM_OR + 2*ENABLE_CM + BUILD_ALL_SAMPLES` is stored
per APV in reference channel 640. BUILD_ALL_SAMPLES = 0 & ENABLE_CM = 0 is "should never
happen" (APV skipped). When BUILD_ALL_SAMPLES = 1, SBSGEMModule computes the common mode
offline from **all 128 strips** — so strips of such an APV must not be dropped by a filter
(the codec protects these frames by default, see `docs/codec.md`).

### 2.6 APV strip hit — 3 continuation words after a type-5 header [SSPDOC p5-6, MPDMOD]

| word | 31 | 30..26 | 25..13 | 12..0 |
|------|----|--------|--------|-------|
| h0 | 0 | APV_CHANNEL_NUM[4:0] | APV_SAMPLE1 | APV_SAMPLE0 |
| h1 | 0 | APV_CHANNEL_NUM[6:5] in bits 27..26 (bits 30..28: see note) | APV_SAMPLE3 | APV_SAMPLE2 |
| h2 | 0 | APV_ID (ADC input 0..15 of the MPD; 5-bit field) | APV_SAMPLE5 | APV_SAMPLE4 |

* Samples: 13-bit two's complement, range −4096..+4095, sign bit = bit 12 of the field
  (`s = (w & 0xFFF) | ((w & 0x1000) ? 0xFFFFF000 : 0)` in MPDModule).
* `apv_chan = ((h1 >> 26) & 0x3) << 5 | ((h0 >> 26) & 0x1F)` (0..127).
* `apv_id   = (h2 >> 26) & 0x1F`.
* Bits 30..28 of h1: drawn as part of a 5-bit field in SSPDOC but only 2 bits are defined;
  MPDModule masks `0x0C000000`. **UNVERIFIED** whether they are always 0 — the codec keeps
  them verbatim (bit-exact re-encode copies the original words).
* Always 6 samples per hit in this format (SBS runs 6 APV samples, 24 ns apart:
  APV clock 250 MHz/6 = 41.67 MHz).
* SBS-offline effective channel: `effChan = (fiber << 4) | apv_id`; the SBS DB `chanmap`
  column called "mpd_id" is matched against **FIBER**, not against the MPD_ID field.

### 2.7 MPD event info — type 12, 3 words [MPDMOD, GEMMOD]

| word | bits | field |
|------|------|-------|
| defining | 23..0 | TIMESTAMP_LO: bits 23..8 = coarse time stamp [15:0], bits 7..0 = fine time stamp |
| cont. 1 | 23..0 | TIMESTAMP_HI (upper 24 bits of coarse time stamp) |
| cont. 2 | 19..0 | EVENT_COUNT (20-bit MPD event counter) |

Keyed by the most recently seen FIBER. SBSGEMModule uses
`Tcoarse = (HI & 0xF) << 16 | (LO >> 8)`, `Tfine = LO & 0xFF`, time = 24 ns·Tcoarse + 4 ns·(Tfine % 6).
**UNVERIFIED**: the position of the type-12 group relative to the hit words of its frame
(after a type-12 defining word, continuation words can no longer be hits of the preceding
type-5 frame, so the group must come after the hits or before the next frame header).

### 2.8 MPD debug header (online common mode) — type 13, 3 words [MPDMOD, GEMMOD]

Each of the 3 words (the defining word included) packs two 13-bit signed CM values:
bits 12..0 = CM[2k], bits 25..13 = CM[2k+1], k = 0,1,2 -> 6 time samples. MPDModule
associates the triple with the APV of the **last decoded hit** (`effChan`), and
SBSGEMModule reads it only when ENABLE_CM = 1. **UNVERIFIED**: whether the debug triple
follows or precedes the hits of its APV, and whether an MPD frame with several APVs carries
several type-13 groups (if it does, a hit cannot follow a type-13 group without a new type-5
header). The codec does not depend on the answer: it keeps the words verbatim and in place.

### 2.9 Data not valid (type 14) and filler (type 15) [SSPDOC p7]

Single words; payload undefined. Fillers pad the read-out length to a multiple of 2 or 4
words (2eSST/64-bit DMA). [V5SSP] places them **before** the block trailer and counts them
in NUM_WORDS. MPDModule ignores both.

### 2.10 No MPD trailer in SSP/VTP format

Unlike the VME v4 format (§4) and the "v5 without SSP zero suppression" format in the
comments of MPDModule.cxx, the SSP/VTP MPD format has **no per-MPD or per-APV trailer and no
per-frame word count**: the frame ends at the next defining word. The only length field
inside the payload is the block-trailer NUM_WORDS (block-wrapped variant only). The EVIO
data-bank length is handled by the EVIO layer.

## 3. Payload variants and event grouping

| variant | where | structure of one ROC data bank (tag 3561) payload |
|---------|-------|---------------------------------------------------|
| **Block-wrapped** ("SSP" in MPDModule) | SSP readout (GMn era); also VTP if its firmware emits block headers | `BH [EH TT TT {MPD-frame}*]×BLOCK_SIZE [FILLER*] BT`, repeated once per SSP/slot read out in the bank |
| **Bare** ("VTP" in MPDModule) | VTP readout (GEn-II, GEn-RP, GEp, 2025 GEM VTP) | MPD frames (and, if present, EH/TT words) without block header/trailer; slot forced to `fSLOTID_VTP = 11` |

Event grouping inside a block-level-5 bank (as implemented in the codec):

* Block-wrapped: each type-2 event header starts event *i* (0-based within the block); the
  number of event headers must equal BLOCK_SIZE. With several blocks (several SSP slots) in
  one bank, event *i* of every block is the same trigger, so the codec merges them; all
  blocks must have the same BLOCK_SIZE.
* Bare: each type-2 event header starts a new event; if the payload contains no event
  header at all, the whole payload is one event. **UNVERIFIED**: whether VTP-era banks carry
  per-event headers in block-level > 1 running. Note that MPDModule itself is *not*
  multi-block aware (it never looks at event headers and does not implement
  `LoadNextEvBuffer`), so with block level > 1 it would merge all events of the block into
  one Podd event. Either SBS GEM ROCs ran with an effective block level 1 in the GEM banks
  or the SBS analysis tolerated this — **UNVERIFIED**, to be checked on a real file with
  `tests/codec`'s dump tool once one is available.

## 4. Legacy direct-VME MPD format (MPDModuleVMEv4, module 3560) — documented, not re-encoded

Non-zero-suppressed readout used on test stands (UVA/INFN cosmic stands); not used in
production runs. Fields are in the low 24 bits:

| bits 23..21 | meaning | other fields |
|-------------|---------|--------------|
| 000 | block header | bits 20..16 = MPD id (= "slot") |
| 100 | APV data | bits 20..19 = sub-type: 0 = APV header (bits 3..0 = ADC channel), 1 = sample (bits 11..0 = 12-bit raw ADC) |

128 × 6 samples per APV, ordered sample-major (`sample[ch + 128*s]`); the decoder rejects
an APV with any other count. Because every strip must be present, strip suppression cannot
be expressed in this format, so the codec does not support it. (A VMEv4 payload
fed to the SSP/VTP decoder fails strict decoding: its words have bit 31 = 0 and are
reported as `ErrorCode::OrphanContinuation`.)

## 5. Old "v4 MPD tags" format (historic, commented in MPDModule.cxx)

`0xE` version, `0x1` event, `0x2` module, `0x3` ADC, `0x4` APV header, `0x5` trailer,
`0x0` data (12-bit sample, 7-bit channel). Historic only, not supported.

## 6. Where the GEM data are in a CODA3 built event [PODD, CRATEMAP]

```
Physics event bank   tag 0xFF50 / 0xFF58 (sync) / 0xFF70 / 0xFF78, type 0x10 (bank of banks), num = block level
 ├─ Built trigger bank tag 0xFF20..0xFF27, type 0x20 (bank of segments), num = number of ROCs
 │    seg 1: uint64 event number [+ uint64 timestamp × block level if tag&1] [+ uint64 run info if tag&2]
 │    seg 2: uint16 event type × block level (padded)
 │    one uint32 segment per ROC (tag = ROC id): {uint64 ts [+ uint32 trigger bits]} × block level
 ├─ ROC bank            tag = ROC id (Podd: (w1 & 0x0FFF0000) >> 16), type 0x10, num = block level
 │    └─ data bank      tag = bank id (crate map "bank" column), type 0x01 (uint32), num = block level (not used by Podd)
 │         payload      module data (here: SSP/VTP MPD words)
 └─ ... one ROC bank per ROC; Podd requires #ROC banks == trigger bank num
```

Podd (`CodaDecoder::bank_decode`) splits a ROC bank into data banks keyed by
`(roc << 16) | tag` and passes the payload (after the 2-word bank header) to the module
registered for that `(crate, slot)`. For GEMs the crate map reads `slot 11  model 3561 bank 3561`,
i.e. **all GEM data of a crate are in the data bank with tag 3561 = 0x0DE9**, decoded by
`MPDModule` with slot 11.

GEM crates in `SBS-replay/DB/db_cratemap.dat` (master):

| ROC (crate) | detector | crate-map validity |
|-------------|----------|--------------------|
| 19, 20 | BigBite GEMs | from 2021-01-01 (beginning of SBS history) |
| 22, 23, 24, 25 | SBS-arm GEMs | from 2021-01-01 |
| 58 | "Adding new GEM VTP" (SBS GEMs) | from 2025-01-01 |
| 3 | test-lab CM test bench | test lab only |

All use `11 3561 3561`. Note: crate 19/20 lines are in a non-"Bank Decoding" section but
have exactly three columns, which `THaCrateMap::ParseSlotInfo` interprets as bank mode.
**UNVERIFIED**: whether GMn-era SSP block headers reported SLOTID = 11 (the same crate-map
line is valid for 2021); if not, MPDModule would ignore all GMn data in that bank, so it
almost certainly did, but this was not checked on data.

## 7. Size arithmetic (useful for budgets)

* one fired strip = 3 words = 12 bytes (6 × 13-bit samples + 7-bit channel + 5-bit APV id);
* per MPD frame overhead: 1 header word (+3 event-info words, +3 debug words per APV when enabled);
* per event: 1 event header + 2 trigger-time words per block; per block: header + trailer + 0–3 fillers.
* A full-readout APV (BUILD_ALL_SAMPLES = 1) costs 1 + 128·3 = 385 words.

## 8. Summary of the decode rules used by `gemfilter_codec`

1. Defining word -> type = bits 30..27. Continuation words are attached to the current type.
2. Type 0 opens a block (SLOTID, BLOCK_SIZE), type 1 closes it (NUM_WORDS).
3. Type 2 starts a new event within the current block (or payload, if bare).
4. Type 5 opens an MPD frame (flags, FIBER (6-bit by default), MPD_ID); its continuation
   words are grouped in threes into strip hits.
5. Types 3, 12, 13 have fixed continuation counts (1, 2, 2) checked in strict mode.
6. Types 14, 15 and reserved types are carried verbatim.
7. Every input word is owned by exactly one layout item, so `encode(all-true mask)` reproduces
   the input bit-for-bit.

## 9. UNVERIFIED items (format)

1. Width of TRIGGER_NUMBER in the event header (27 bits per SSPDOC text, 22 bits in V5SSP).
2. Bits 30..28 of hit word h1 (assumed 0; kept verbatim).
3. Bits 26..24 of the MPD frame header (ENABLE_CM/BUILD_ALL_SAMPLES/CM_OR) come only from
   MPDModule; SSPDOC 2020 shows them undefined.
4. Position of the type-12 and type-13 groups relative to the hit words of their frame;
   association of a type-13 triple with an APV (MPDModule convention: last decoded hit).
5. Whether VTP-era (bare) banks contain event headers / trigger-time words and how events
   of a block-level-5 readout are delimited there.
6. NUM_WORDS convention on real data (V5SSP: header..trailer inclusive incl. fillers). The
   codec updates NUM_WORDS by the number of removed words, which is correct for any
   convention that counts data words; it can optionally check the inclusive convention.
7. Filler placement (before vs. after the block trailer) and the alignment modulus (2 or 4).
8. SLOTID reported by SSP blocks in GMn data (crate map implies 11).
9. Whether the VTP firmware ever emits block headers/trailers for MPD data (the codec accepts
   both).
