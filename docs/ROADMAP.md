# Roadmap

Six phases. Each one ends with something runnable and measured, not a milestone
that is declared done.

## Phase 1 — the container stack ✅

Identify any layer from dump to module and report what stands between here and
decodable ARM, with no key material.

- [x] SCE/SELF header, appinfo, ELF32 header, program headers, segment table
- [x] Bounds-checking on every offset read out of the file
- [x] Named refusal for PFS-encrypted retail dumps
- [x] Encryption flag cross-checked against observed zlib headers
- [x] Synthetic `ctest` suite, with a guard against `NDEBUG` silently disabling it

**Established:** QA/prototype builds carry plaintext, zlib-compressed segments.
The corpus needs no decryption. See [`DECRYPT.md`](DECRYPT.md).

## Phase 2 — inflate and reassemble ✅

Turn a SELF into a plain ELF32 on disk.

- [x] DEFLATE + zlib, written rather than vendored (RFC 1951/1950), keeping the
      "no external dependencies" promise intact
- [x] Segment reassembly into a valid ELF32 using the plaintext program headers
- [x] Verify: inflated size matches `p_filesz`, Adler-32 matches, total matches
      `elf_filesize`, and `e_entry` resolves into a `PF_X` segment
- [x] `armrecomp extract` — SELF in, ELF out
- [x] **15/15 modules in the corpus extract and round-trip**, from 236 KB
      (`libfios2.suprx`) to 53 MB (*Volume*)

**Not in scope, now or later:** key derivation, key extraction, the PFS layer.

## Phase 3 — the decoder ✅ (classification; operands land in phase 5)

- [x] ARMv7-A and Thumb-2, both instruction sets, with correct **width**
      determination — the output that must never be wrong, since one bad width
      desynchronises every instruction after it
- [x] Control flow: branch/call/return/indirect, targets computed, conditionality
      and interworking flagged
- [x] IT blocks distinguished from the NOP hints sharing their encoding space
- [x] VFP / NEON identified as a class even where not translated
- [x] `armrecomp cover` — coverage, class histogram, and instruction-set
      determination
- [ ] Full operand decoding (deferred to phase 5, where the emitter needs it)
- [ ] Literal-pool identification — currently counted as unknown rather than
      recognised as data, which is why the unknown rate is an upper bound

**Established across 16 modules:** every one is Thumb-2 dominant, and
**NEON/SIMD is 0.71–5.34%** of instructions. The vector unit is not the
obstacle on this platform.

## Phase 4 — function discovery ✅ (seeded; boundaries need work)

- [x] `.sce_module_info` parsing — `e_entry` points at this structure, **not at
      code**
- [x] Import/export table walking; `armrecomp funcs` reports the HLE work list
- [x] Recursive descent carrying instruction-set state, since a seed without a
      mode is worthless
- [x] Linear harvest of `BL`/`BLX` targets
- [x] Pointer-shape recovery, prologue-filtered, counted separately as the
      heuristic it is
- [x] `armrecomp discover`
- [x] **`SCE_RELA` relocation seeding** — not applicable to `ET_SCE_EXEC`
      modules, which is the whole launch-window catalogue, but needed for
      2015+ titles and every homebrew module. All ten entry formats decoded
      (`reloc.c`), applied at the link addresses as a check (zero bytes
      changed on the fixture), and every relocated word holding a
      Thumb-tagged `.text` address seeded, with a text filter that pointer
      tables bypass. On VitaShell: 63,334 entries, 6,735 such pointers,
      functions 8,074 → 9,195, coverage 65.3% → 71.5%, shape's share of the
      functions 44% → 18%. `armrecomp relocs` reports the stream
- [x] **Scan the DATA segment for function pointers**, and read pointer tables
      as tables. The C++ `.init_array` lives in the rw segment, so all 551
      static constructors were invisible; a run of 4+ Thumb-tagged in-range
      words is a table, and inside one the prologue filter is skipped.
      0 → 551 of 551
- [x] **Refuse unconfirmed ARM seeds.** A mode-switching call is followed into
      ARM only when its target is a known import stub. The ARM decoder assigns
      a class to every op value and so never stops on a runaway walk — 304 bad
      seeds were producing 152,919 instructions of data-as-code
- [ ] Jump-table recognition — 17,970 indirect call sites remain unresolved
- [ ] Function *boundary* quality: extents currently absorb tail calls

## Phase 5 — the emitter ✅

- [x] Full operand decoding for the 16-bit Thumb core (the phase 3 gap): shifts,
      immediate and register ALU, high-register forms, every load/store
      addressing mode, PUSH/POP register lists, extends, `REV`, `CBZ`/`CBNZ`,
      `IT`
- [x] ARM → readable C, one function at a time, with the disassembly as comments
- [x] Conditional execution and flag semantics, in the runtime rather than
      inlined at each site
- [x] Literal-pool loads folded to constants at translation time
- [x] Anything untranslated emits a **named run-time trap**, never silence
- [x] **Verified: generated C compiles, links against the runtime, and runs**
- [x] **32-bit Thumb operand decoding** — data processing (shifted register and
      modified immediate), `MOVW`/`ADDW`/`SUBW`, load/store, shift-by-register,
      `MUL`, `PUSH.W`/`POP.W`. Translation **69.80% → 86.18%**
- [x] **`MOVT` and `LDRD`/`STRD`** — translation **86.18% → 89.44%**
- [x] **A host**: ELF segment loader, guest memory, stack, and `module_start`
      called for real. `VITARECOMP_TRACE=1` logs and continues instead of
      aborting, so one run names many missing pieces
- [x] **Runtime dispatch** — a sorted address→function table, so `BX Rm` and
      `BLX Rm` become real transfers. The module has ~18,000 indirect sites;
      without this the program cannot leave `module_start`
- [x] **Scalar VFP** — the float register file with single/double aliasing,
      `VLDR`/`VSTR`, arithmetic, `VABS`/`VNEG`/`VSQRT`, `VCMP` with proper
      unordered-NaN semantics, `VMRS`, `VMOV` core↔VFP, and `VCVT`.
      Translation **92.04% → 95.64%**
- [x] **Wide branches** — `b.w` and `b<cond>.w` decoded their targets correctly
      and then trapped anyway, because the decoder never set `op`. Translation
      **95.65% → 97.47%**
- [ ] Branch targets promoted to functions — 4,736 remaining (0.31%)
- [x] **Bitfield ops** — `SBFX`/`UBFX`/`BFI`/`BFC`, 655 (0.47%). The group's
      5-bit op field is shared with `MOVW`/`MOVT`/`ADDW`, and the field
      position is encoded three ways across the four instructions. `SSAT`/
      `USAT` share the group and are named separately, since they are not
      bitfield work. Out-of-range extracts and `msb < lsb` keep trapping
      rather than being clamped
- [x] **The scalar VFP remainder** — `VPUSH`/`VPOP`/`VLDM`/`VSTM` and the
      multiply-accumulate forms. Translation **97.47% → 97.87%**
- [x] **`VLDR` from a literal pool** — it emitted a bare `pc`, so the generated
      C did not compile at all once a VFP literal appeared. Fixed at the shared
      `address()` helper, which also cured the U bit being ignored for the T32
      `[rN, #-imm]` form. Found only by building at scale
- [x] **Verified at scale**: 1,500 functions → 15 MB of C → compiles, links,
      and runs, reaching the first indirect transfer at `0x8100B936`
- [x] **`BLX` immediate was decoded as `B.W`, and `B.W` as a conditional
      branch.** Four encodings share the 32-bit branch group and are selected
      by bits 14 and 12 of the second halfword; masking 14:12 together
      conflated them. BLX also computes its target from `Align(PC,4)`, not
      `PC`. Import calls bound 3,917 → 18,563; coverage 68.6% → 83.3%
- [ ] Advanced SIMD (NEON) — 10,716 instructions (0.58%), and now not just the
      largest remaining bucket but the thing the RUNNING program stops on:
      everything earlier in the boot path is translated. *Advanced SIMD and NEON are the same instruction
      set — ARM's formal name and the marketing one — so this is one job.*
- [ ] ARM (A32) operand decoding — a small minority of this corpus. The A32
      media space is also currently classed `A_UNDEF`, which is wrong: `op == 3`
      with bit 4 set is media, not an undefined encoding
- [ ] `MLA`/`MLS`, general `LDM`/`STM` — long tail

**Whole-module translation: 98.86%** (19,120 functions, 1,532,679 instructions).
Measure the whole module, not a prefix: the first 1,500 functions report 98.33%,
and functions are emitted in address order, so a prefix is not a sample.

## Phase 6 — the runtime and HLE (in progress)

- [x] CPU state, memory, semantic helpers (landed with phase 5)
- [x] **NID resolution against the MIT `vita-headers` database** — 519 of 524
      imports resolved (99.0%) from 9,274 known functions
- [x] The HLE work list sized and shaped: see [`HLE.md`](HLE.md)
- [x] **Bind imports.** A call to a stub emits `vita_hle_sceGxmDraw()` rather
      than `vita_func_814BB75C()`, and a generated companion file gives every
      import a default that traps by name — so the output links from the first
      build and each firmware call announces itself
- [x] **Route indirect transfers through the import table.** Imports bound on
      a direct `BL`, but `module_start` reaches its first firmware call
      *through a pointer*, so that path never saw it. Stubs are now merged into
      the same sorted dispatch table as functions (19,644 entries: 19,120 + 524)
      rather than getting a second lookup. `vita_dispatch_init` refuses an
      unsorted table, since an unsorted one silently fails to find entries that
      are present — indistinguishable from missing coverage
- [x] **Traps name the firmware function**, not just its NID. A NID is a hash;
      "implement 0xBFE02B3A" is not a task anyone can start
- [x] The shallow ~92 of `SceGxm`: state setters, texture accessors, mapping
- [x] **The link arrangement.** Import defaults are one file per import, built
      as a static library listed after the runtime. A linker pulls an archive
      member only for a symbol still undefined, so implementing an import is
      just defining it. One translation unit does NOT work: the unimplemented
      stubs drag the object in carrying the implemented ones
- [x] **`SceLibc` batch 1 — 24 of 68.** C++/CRT init (`__cxa_*`, TLS, errno),
      the mem/str family, and a bump-allocator guest heap. The semantics are
      free — SceLibc is the standard C library — the *boundary* is the work:
      every pointer argument is a guest address, and a string's length is in
      the data, so `vita_mem_avail()` bounds the scan a host `strlen` would
      otherwise run off the end of
- [ ] `SceLibc` remainder (44) and `SceLibm` (28), largely host-forwardable
- [ ] **A real allocator.** The heap is a bump allocator; `free` is a no-op
- [ ] `sceKernel` — threads, memory blocks, sync primitives
- [x] GXP reflection, then the scene pipeline: a software one, in `src/gxm_draw.c`
- [x] GXP shader translation, as an interpreter of the bytecode rather than a
      compiler (`src/usse.c`), one pixel at a time and sixty-four at a time;
      a compiled path is what speed will need after that
- [x] Touch: the front panel, from a script or the mouse (`src/hle_touch.c`)
- [ ] Movies: SceMp4, `sceAvcdec`, `sceAudiodec`. Until then the movie
      library is reported absent and titles skip their movies

**Measured, not estimated:** `SceGxm` is 107 functions, but ~85 of them are
struct field writes. The real work is ~15 functions, one of which is a shader
compiler. Deferring the online-only libraries takes the whole target from 524 to
roughly 412.

## Per-title repos

The toolkit is the product; a game brought up on it lives in its own repo
consuming this one as a submodule. Selection criteria, now that phase 1 has
established what the input path actually is:

1. **A QA/prototype build exists** — plaintext segments, no keys, no PFS.
2. **Small `.text`** — less to reach 100% decode coverage on.
3. **2D** — `sceGxm` is the deepest part of phase 6; a title that leans on it
   lightly gets to "runs" sooner.
4. **Low NEON density** — measurable with `cover` before committing.

## Beyond the six phases — psvitarecomp, from 2026-10-10

Upstream's phases stand as written above. What this repository added on top,
measured on its own fixtures:

### Discovery and translation

- [x] **`PT_SCE_RELA` decoded and seeded** (phase 4's open item): all ten
      entry formats, applied at the link addresses as a check, relocated
      Thumb-tagged pointers seeded with a text filter that pointer tables
      bypass. VitaShell: functions 8,074 → 9,195, coverage 65.3% → 71.5%
- [x] **`module_start` with its Thumb bit masked.** vita-elf-create sets it;
      SNC did not; seeding the odd address decoded the entry one byte in
- [x] **Post- and pre-indexed write-back** on the T32 8-bit immediate loads
      and stores and on LDRD/STRD, with the P/U/W bits read rather than the
      sign alone
- [x] **IT blocks applied.** The emitter and discovery carry IT state through
      a block; predicated instructions are guarded, a predicated return or
      branch no longer ends the flow
- [x] **`RSB` with a real operand2**; the 16-bit negate is the special case
- [x] **`MLA`/`MLS`, `UMULL`/`SMULL`/`UMLAL`/`SMLAL`**; `mul.w` reads `rn`
- [x] **Jump tables**: `tbb`/`tbh` with the count from the guarding compare,
      followed by discovery and emitted as a `switch`
- [x] **Veneers and tail calls**: `bx pc` PIC veneers resolved in the
      decoder; unconditional and backward-conditional branch targets
      registered as functions. VitaShell: 9,195 → 21,593 functions, 71.5% →
      78.6% coverage
- [x] **`CLZ`, `RBIT`, `REV16`/`REVSH`, `UADD8`/`USUB8`/`SEL`** with the GE
      bits — the C library's strlen
- [x] **A first NEON subset on D registers**: `VMOV`/`VORR`/`VBIC` immediate,
      the bitwise ops, `VADD`/`VSUB`/`VCEQ`/`VTST`/`VPADD`, `VDUP`, the 32-bit
      lane moves, `VLD1`/`VST1`; D16-D31 exist
- [x] The emitter bugs the newlib sample found (LESSONS.md): `pop.w` with
      PC, `bx lr`, the ITE mask bit and IT flag suppression, scaled register
      offsets, shift-by-register's `rn`, `pld`, backward `bgt.w`/`ble.w`
- [x] Whole-module translation on VitaShell: **98.79%** of 7.28 M emitted
      instructions (the count grew with the reach; 99.19% of the old 6.07 M)
- [ ] Saturating ops (`SSAT`/`USAT`/`QADD`...) — 0.39% of VitaShell, the
      largest remaining bucket there
- [ ] General `LDM`/`STM` against a non-SP base — 0.16%
- [ ] NEON — 0.20% of VitaShell; still the milestone B item
- [ ] The 14,532 even-aligned relocated pointers into `.text` (ARM code or
      literals): unseeded, reported

### Runtime and HLE

- [x] **`SceSysmem`**: `sceKernelAllocMemBlock` / `GetMemBlockBase` /
      `FreeMemBlock` on a bump arena with the console's granularity
- [x] **`SceDisplay`**: `sceDisplaySetFrameBuf` with the console's checks,
      `sceDisplayWaitVblankStart` as the host's turn
- [x] **`SceCtrl`**: `sceCtrlSetSamplingMode`, `sceCtrlPeekBufferPositive`
- [x] **`sceKernelExitProcess`**
- [x] **The host contract** (`host.h`): present, buttons, exit. The headless
      `vita_host`: scripted input, frames to PPM
- [x] **Emitted entry by name**: `vita_module_start()`
- [x] **End to end in CI**: `samples/fbdemo` from SELF to a hash-checked frame
- [x] **A newlib-linked sample runs** (`samples/hello`): `SceThreadmgr`
      mutexes and lightweight mutexes, thread id, TLS slots; `SceIofilemgr`
      open/write/close/stat for the console streams; `sceClib*` memory
      routines and a guest-aware `sceClibPrintf`; `SceRtc`; printf,
      snprintf, malloc and a 5x7 font on screen
- [x] **Bring-up tools in the runtime**: function-entry trace with names
      from an `nm` listing, per-instruction register trace in a window,
      write watchpoints, bad accesses reported with their PC, a sampling
      profile per thread and function; `VITARECOMP_WHY` for what discovery
      made of one address
- [x] **Threads that wait by spinning** (`samples/threads`): a time slice
      counted at backward branches, and an immediate hand-over in a loop
      that can only be ended by another thread
- [ ] A windowed host (SDL2 behind an option)
- [ ] Threads for real: `sceKernelCreateThread`, and a scheduler model

### Retail input

- [x] **A retail title in the toolkit**: *Uncharted: Golden Abyss* from a
      PFS-encrypted NoNpDrm dump, through Vita3K's own decryption driven by
      the dump's licence (`tools/vita3k-decrypt.ps1` drives the Qt-only
      action; `tools/import-vita3k.sh` imports and measures). 9.43 MB static
      module, 70,967 functions, 618 imports across 42 libraries, 96.23 %
      translated with NEON/VFP as 85 % of the remainder: milestone B is the
      retail blocker, as upstream said it would be
- [ ] Load a title's bundled `sce_module/*.suprx` alongside its main module,
      exports before HLE
