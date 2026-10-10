# Plan

Upstream's six phases, with phases 1 to 5 complete, are in
[ROADMAP.md](ROADMAP.md). This is what sits on top: the order of work from the
state of the tree on 2026-10-10 to the first game running, with each step
ending in something that runs and is measured.

## Milestone A: a homebrew module runs — reached 2026-10-10

The cheapest end-to-end proof that needs no game, no keys, and no GPU. The
vitasdk sample programs that draw through `sceDisplaySetFrameBuf` and read
`sceCtrlPeekBufferPositive` use about a dozen firmware functions, all shallow.

**What landed.** `samples/fbdemo` is a freestanding program (no newlib, eight
imports) built with vitasdk; its SELF is tracked so CI needs no toolchain.
`armrecomp` translates every instruction of its own code. The runtime gained
`SceSysmem` memory blocks, `SceDisplay`, `SceCtrl` and `sceKernelExitProcess`
(`src/hle_*.c`), and a host contract (`include/vitarecomp/host.h`): three
functions a host supplies — present a frame, report buttons, exit. `host/` is
the headless one: scripted input, frames to PPM. `tools/e2e.sh` runs the
whole chain and hash-checks the frame; CI runs it on Windows and Linux.

Getting there found four translation bugs that the QA-build corpus had hidden
(docs/LESSONS.md, "Four bugs the first running homebrew found"): the entry's
Thumb bit, post-indexed write-back, IT blocks not applied, and RSB's second
operand. Each was invisible in a translation percentage and obvious in a
frame.

**Done the same day: the newlib-linked sample.** `samples/hello` is what
every real homebrew is: a C `main` over vitasdk's newlib. Its import table
(54 functions, 11 libraries) is the C runtime's surface, and all of it on
the sample's path is implemented: mutexes and lightweight mutexes, thread
id, TLS slots, console file descriptors, `sceClib*`, the clock. It also
brought the first NEON subset (GCC vectorises newlib itself), jump tables,
interworking veneers, tail-call discovery, and nine emitter fixes found with
the bring-up tools built alongside (function trace, instruction trace,
watchpoints, PC on bad accesses). The steps below are kept as the record of
the plan; all six are done.

1. **Relocations. Done 2026-10-10.** Homebrew and every 2015+ title are
   `ET_SCE_RELEXEC`. `tools/armrecomp/reloc.c` decodes `PT_SCE_RELA`
   (formats 0 to 9, per [VITA3K-REFERENCE.md](VITA3K-REFERENCE.md)), applies
   it at the link addresses as a check, and seeds discovery from every
   relocated word holding a Thumb-tagged `.text` address. On VitaShell:
   functions 8,074 → 9,195, coverage 65.3 % → 71.5 %, the shape heuristic's
   share 44 % → 18 %. Details in [ARCHITECTURE.md](ARCHITECTURE.md), stage 4.
   Left open: the 14,532 even-aligned relocated pointers into `.text`, which
   are ARM code or literals and nothing in the entry says which.
2. **Fixture corpus.** `tools/fetch-deps.sh` already pulls VitaShell. Add a
   vitasdk framebuffer sample built from source (or its release VPK) so the
   milestone has a module small enough to reach 100 % translation.
3. **Runtime loader for relocated modules**, and the stack/heap/TLS layout the
   kernel HLE needs. Guest memory stays flat as upstream built it.
4. **Kernel HLE, minimum set.** `sceKernelCreateThread/StartThread/
   WaitThreadEnd/ExitDeleteThread`, `sceKernelAllocMemBlock/GetMemBlockBase/
   FreeMemBlock`, `sceKernelGetProcessTimeWide`, `sceKernelDelayThread`,
   mutex/sema/event-flag create and wait. One host thread per guest thread,
   CPU globals made thread-local.
5. **`SceLibc` remainder and a real allocator.** Upstream has 24 of 68 and a
   bump heap whose `free` is a no-op. Forward the rest to the host C library
   through the existing bounds-checked guest/host boundary; replace the heap.
6. **`SceDisplay` + `SceCtrl` + `SceTouch`** onto an SDL window and gamepad.
   `sceDisplaySetFrameBuf` blits the guest framebuffer; `sceDisplayWaitVblankStart`
   paces to 60 Hz.

Exit criterion: the sample's own pixels on screen, input working, zero
traps in a one-minute run.

## Milestone B: NEON — the retail census pass done 2026-10-10

The instruction set upstream stopped on. On *Golden Abyss* it was 85 % of
every trap left, so the order of work came from the title itself: tally the
raw encodings behind every trap in its emit, disassemble the most frequent
with the vitasdk `objdump`, and translate families in that order. Two
passes took the module from 96.23 % to 99.7 % translated.

Done, in census order: `VDUP` by lane (the single most frequent trap; every
vector-by-scalar multiply starts with one), the f32 three-register set
(`VADD/VSUB/VMUL/VPADD/VABD/VMLA/VMLS/VMAX/VMIN/VCEQ/VCGE/VCGT/VRECPS/
VRSQRTS`), VFP `VMOV` immediate (45,000 of them), `LDREX/STREX` and the
barriers, `VRECPE/VRSQRTE`, the two-register miscellaneous group (`VREV`,
`VCLZ`, `VCNT`, `VMVN`, compares with zero, `VABS/VNEG`, `VSWP`, `VMOVN`,
`VCVT`), `VEXT`, shifts by an immediate (`VSHR/VSRA/VRSHR/VRSRA/VSHL/VSLI/
VSRI`, `VMOVL/VSHLL`, `VSHRN/VRSHRN`), multiply by a scalar, the integer
`VMUL/VMLA/VMLS/VMAX/VMIN/VCGT/VCGE`, the 64-bit core-VFP transfers, `VLDM`
without write-back, general `LDM/STM`, the TLS-pointer `MRC`, and scalar
`VNMLA/VNMLS` and `VCVT` between precisions. All lane-by-lane in portable C
(`neon.h`), with `ctest` cases from the ARM ARM.

Left, as named traps until a module needs them: the saturating forms
(`VQADD`, `VQSHL`, `VQMOVN`, `VQDMULH`), the long multiplies, `VLD2`-`VLD4`
and the lane loads, `VTRN/VUZP/VZIP`, `VTBL`, polynomial and fixed-point
forms, the integer estimates. What the emit showed after that was not a
NEON problem at all but the size of the output, which was duplication, and
is dealt with under housekeeping below.

Still open from the original plan:

4. Verification: differential runs of individual functions against Vita3K or
   the touchHLE interpreter core from identical state (the UMK3 iOS method).
   Two deliberate differences to keep in mind when reading a diff: NEON on
   the console runs flush-to-zero with default NaNs, and `VRECPE/VRSQRTE`
   are eight-bit estimates where the helpers compute exactly.

Exit criterion: *Uncharted* QA (if available) or any NEON-using homebrew
proceeds past its first SIMD instruction with lane results matching the
oracle.

## Milestone C: retail input — bring-up begun 2026-10-10

What a retail title needs before its first frame, learned by running
*Golden Abyss* and implementing what trapped, in the order it trapped:

- **Threads.** The title creates worker threads before it reads a file.
  `src/sched.c`: one host thread per guest thread, one running at a time,
  switching at the kernel's waits, delays, thread ends and the vertical
  blank; the thread manager, SceUlt and the vblank wait sit on it.
- **Files.** Mounts the host declares (`--mount app0=DIR`, repeatable as an
  overlay), real files behind `sceIo*`, and SceFios2 with every operation
  finished before it is handed back. The dump's data is PFS-encrypted; the
  copy Vita3K installs is not, and is what gets mounted.
- **Libraries.** SceLibm over the host's math library in the hard-float
  convention; `sceClibMspace*` heaps inside the title's own memory blocks;
  the printf family on one formatter; `bsearch` and `qsort` calling the
  title's comparator through the dispatch table; `ldiv` with its result in
  memory; `strtok`; the tick clock; sysmodule, power, AppUtil init;
  `setjmp` (records, returns 0) and `longjmp` (a named trap); the C++
  runtime's `type_info` and a named trap for `__cxa_throw`.
- **Memory.** 416 MiB of guest memory, the console's main memory and CDRAM
  together: the title's first five blocks are 120 MiB.
- **A null GPU.** `sceGxm` accepted in full and drawn nowhere: contexts,
  the shader patcher, render targets, surfaces, textures, the display
  queue calling the title back at once. Milestone D replaces its insides.
- **Variable imports.** The C library's case tables, `_Ctype` and the
  standard streams, bound by the loader at every site that names them; the
  C++ runtime's vtables get zeroed pages until the runtime has them.
- **Three decoder faults and one in discovery** that the title's own code
  found: the 32-bit extends, the structure loads, the table-branch guards
  (docs/LESSONS.md).

Where it stands: `module_start` through archive index, resources, worker
threads and `sceGxm` setup to the parsing of its input tables.

1. Accept FAGDec-decrypted `eboot.bin` and `*.suprx` (plain SCE ELF, the
   Vita3K input format) in `armrecomp info/extract`, in addition to fself.
2. Load a game's own `*.suprx` modules alongside the main module: the same
   export/import binding, with exports from one module satisfying imports of
   another before HLE is consulted (the order Vita3K uses).
3. `armrecomp funcs` across all of a title's modules gives the HLE work list
   for that title. Pick the first retail target from that list, not from
   preference: smallest list, 2D, low NEON density.

**Reference title: *Uncharted: Golden Abyss* (PCSA00029, USA, v1.03) — in
the toolkit since 2026-10-10.** A NoNpDrm dump of it is PFS-encrypted from
byte zero, and `armrecomp info` refuses it by name (the "true negative"
upstream's corpus lacked). The route that works, all with the dump's own
licence and no key material in this repository:

1. `Vita3K.exe <dump>\app\PCSA00029` installs the title: the PFS layer is
   removed on the way into Vita3K's `ux0/app`. No firmware needed; the run
   that follows fails, harmlessly.
2. `tools/vita3k-decrypt.ps1 -TitleId PCSA00029` opens Vita3K's interface
   and, through Windows UI Automation, right-clicks the title and picks
   *Other > Decrypt All SELF*, the only place that action exists. The
   plaintext modules land in `<Vita3K>\cache\decrypted_selfs\PCSA00029\`.
3. `tools/import-vita3k.sh <that dir> goldenabyss` copies them into
   `fixtures/goldenabyss/` (gitignored), extracts, and measures.

What the measurement says about a flagship launch title:

| | `uncharted.self` |
|---|---|
| Container | `ET_SCE_EXEC`, static, 9.43 MB code segment, no relocations |
| Decode | 3.33 M Thumb instructions, 0.18 % unknown, **5.34 % SIMD** (6.22 % of reached instructions): the densest NEON in the corpus |
| Discovery | 70,967 functions (60,093 from seeds, 10,874 from shape), 75.0 % covered, 416 jump tables, 30,291 unresolved indirect sites |
| Imports | 618 functions across 42 libraries, 585 named (94.7 %) |
| Largest libraries | SceGxm 122, SceLibc 79, SceFios2 42, SceLibm 39, SceLibKernel 39, SceNgs 34, SceCommonDialog 27, SceNet 22, SceMp4 19, SceUlt 18 |
| Discovery, second pass | 77,455 functions: 19,222 more than before, from MOVW/MOVT pairs that build a Thumb address in a register; 7,322 weak pointer targets (no prologue, no table) walked but not roots, 621 of them promoted by a clean body |
| Emit, whole module | 23,436 functions emitted, the closure of entry point, exports and stored pointers under calls and tail calls; 2.04 M instructions, **99.98 % translated**, 194 MB of C in 12 parts. Before the reachability pass every discovered function was emitted: 13.07 M instructions at 99.7 %, 1.12 GB of C, four copies of every distinct instruction (96.23 % translated before milestone B's census pass) |
| What traps | 455 instructions: branches to addresses discovery never registered, and the last NEON forms |
| Thread-local image | 84 bytes initialised of 1,140 per thread; the runtime builds each thread's block as the console does (0x800 bytes of kernel slots, then the image) |

So the shape is the one upstream predicted for the launch window: no
relocation stream, so a seventh of the functions rest on the shape
heuristic; the full `sceGxm` surface plus audio (SceNgs), video (SceMp4),
networking and dialogs; and NEON on the hot paths. It is the measurement
input for milestones B, D and E, not the next bring-up. The bundled
`sce_module/libc.suprx` (relocatable, 42 imports) is Sony's own SceLibc,
which a per-title host could load instead of writing that HLE.

## Milestone D: `sceGxm` without a shader compiler

Upstream measured 107 `SceGxm` imports of which about 85 are struct field
writes. Order:

1. The shallow 92: state setters, texture accessors, render-target and
   surface init, memory mapping, sync and display queue. Struct layouts come
   from vita-headers (MIT).
2. GXP reflection: parse the program container enough to answer
   `sceGxmProgramFindParameterByName` and friends.
3. The pipeline (`BeginScene`, `Draw`, `EndScene`, `MidSceneFlush`) on a host
   API with a *fixed* substitute shader per program, so geometry and state
   can be verified before any shader is translated.

Host API: see [DECISIONS.md](DECISIONS.md). The pipeline is written once
against a small internal interface so the choice can change.

## Milestone E: GXP shader translation

The compiler. The USSE instruction set of the SGX543MP4+ is documented only by
Vita3K's translator (~300 KB of C++, years of work) and by the homebrew
community's notes. Options are in [DECISIONS.md](DECISIONS.md); this is the
decision that sets the project's licence and its timeline, so it is taken
before milestone D's step 3 is finished, not after.

## Milestone F: first title repo

A per-game repo consuming this one as a submodule, as upstream intended:
host executable, game-specific HLE overrides, patches, packaging. The toolkit
stays game-agnostic.

## Housekeeping, ongoing

- The output was four copies of the module. Done 2026-10-10: the emitter
  writes only the functions an entry point, an export or a stored pointer
  can reach, directly or through calls and tail calls (docs/ARCHITECTURE.md,
  "Only what can run is emitted"). *Golden Abyss* went from 1.12 GB of C to
  194 MB, VitaShell from 7.28 M emitted instructions to 4.47 M, the hello
  sample from 94,005 to 15,705, all three still running or translating as
  before. Still open: a walk that falls through a call that never returns
  (`exit`, `abort`, `sceKernelExitProcess`) into whatever follows; and the
  581 (VitaShell) and 557 (*Golden Abyss*) branches to addresses
  discovery never registered, each a function reached only by a jump from
  the middle of another.

- CI on Windows and Linux: build, `ctest`, and the smoke run on the VitaShell
  fixture.
- Keep upstream's rule: every untranslated instruction and every unimplemented
  import traps by name. No silent zeros.
- Measure whole modules, not prefixes (LESSONS.md, "A prefix is not a sample").
- Track upstream: `git fetch upstream` and merge if it moves again.
