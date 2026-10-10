# Plan

Upstream's six phases, with phases 1 to 5 complete, are in
[ROADMAP.md](ROADMAP.md). This is what sits on top: the order of work from the
state of the tree on 2026-10-10 to the first game running, with each step
ending in something that runs and is measured.

## Milestone A: a homebrew module runs

The cheapest end-to-end proof that needs no game, no keys, and no GPU. The
vitasdk sample programs that draw through `sceDisplaySetFrameBuf` and read
`sceCtrlPeekBufferPositive` use about a dozen firmware functions, all shallow.

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

## Milestone B: NEON

The instruction set upstream stopped on. 0.58 % of *Uncharted*'s
instructions, but the first one is on the boot path.

1. Decoder: classify the Advanced SIMD data-processing, load/store
   (`VLD1..VLD4`, `VST1..VST4`), and `VMOV`/`VDUP` spaces; fix the A32 media
   space currently classed `A_UNDEF`.
2. Runtime: a 16 x 128-bit register file aliased with the existing
   single/double VFP file (`d0..d31`, `q0..q15`), and helpers per lane type.
   Portable C first; SSE intrinsics later only where a profile shows it.
3. Emitter: integer and single-precision lanes first, which is what game code
   uses; polynomial, saturating and fixed-point forms as named traps until a
   module needs them.
4. Verification: differential runs of individual functions against Vita3K or
   the touchHLE interpreter core from identical state (the UMK3 iOS method),
   plus synthetic `ctest` cases with expected values from the ARM ARM.

Exit criterion: *Uncharted* QA (if available) or any NEON-using homebrew
proceeds past its first SIMD instruction with lane results matching the
oracle.

## Milestone C: retail input

1. Accept FAGDec-decrypted `eboot.bin` and `*.suprx` (plain SCE ELF, the
   Vita3K input format) in `armrecomp info/extract`, in addition to fself.
2. Load a game's own `*.suprx` modules alongside the main module: the same
   export/import binding, with exports from one module satisfying imports of
   another before HLE is consulted (the order Vita3K uses).
3. `armrecomp funcs` across all of a title's modules gives the HLE work list
   for that title. Pick the first retail target from that list, not from
   preference: smallest list, 2D, low NEON density.

**Reference title: *Uncharted: Golden Abyss* (PCSA00029, USA, v1.03).** A
NoNpDrm dump of it (app + patch) is PFS-encrypted from byte zero: `eboot.bin`
is a 12 KB loader, the game is `uncharted.self` (4.6 MB) plus
`sce_module/libc.suprx`, `libfios2.suprx`, `libult.suprx`, all ciphertext,
with `sce_pfs/{files.db,unicv.db,pflist}` present. `armrecomp info` refuses it
by name, which is the "true negative" upstream's corpus lacked. The route to
a readable module is Vita3K's own decryption, driven by the dump's own
licence (`sce_sys/package/work.bin` carries the klicensee): install the dump
in Vita3K; its `decrypt_selfs` writes re-wrapped SELFs with plaintext
segments under its cache directory `decrypted_selfs/`, and those are ordinary
`armrecomp` inputs. A 2012 launch title, so expect `ET_SCE_EXEC` with no
relocations, heavy NEON and the full `sceGxm` surface: a measurement input
for `cover` and `funcs` long before it is a bring-up target.

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

- CI on Windows and Linux: build, `ctest`, and the smoke run on the VitaShell
  fixture.
- Keep upstream's rule: every untranslated instruction and every unimplemented
  import traps by name. No silent zeros.
- Measure whole modules, not prefixes (LESSONS.md, "A prefix is not a sample").
- Track upstream: `git fetch upstream` and merge if it moves again.
