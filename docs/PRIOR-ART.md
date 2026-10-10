# Prior art

What exists for Vita recompilation and for ARM/Thumb-2 static recompilation in
general, as surveyed on 2026-10-10. Each entry says what it is and what, if
anything, this repo takes from it.

## Vita to PC

### sp00nznet/vitarecomp (the base of this repo)

<https://github.com/sp00nznet/vitarecomp> - C, MIT, created 2026-07-20, last
push 2026-08-20, 38 commits. The only Vita-to-PC static recompiler found.

- Own SELF/ELF container parser, own zlib inflate, own ARMv7-A + Thumb-2
  decoder, `.sce_module_info` walker, function discovery, ARM-to-C emitter,
  NID resolver against vita-headers, and a C runtime (CPU globals, flat guest
  memory, sorted dispatch table, scalar VFP, trap-by-name imports).
- Measured on *Uncharted: Fight for Fortune* (QA build, `ET_SCE_EXEC`):
  21,495 functions, 98.95 % of instructions translated, the full module
  compiles and runs to the first NEON instruction.
- Stopped on: NEON (0.58 % of instructions but on the boot path), `SceLibc`
  remainder, kernel HLE, `sceGxm`, GXP shader translation, `PT_SCE_RELA`
  relocation seeding for 2015+ modules.
- Stance: QA builds only (no keys), Vita3K as oracle only, MIT.

Taken: everything. This repo is its continuation.

### Catalogues

- Recompendium <https://nio03.github.io/unricopie/en/> and PortsDR
  <https://portsdr.com/> list recomp projects by platform. Vita appears only
  as a *target* (N64 titles ported to Vita, Banjo-Kazooie Lighthouse Vita).
- mirou1611/AstraRecomp <https://github.com/mirou1611/AstraRecomp>: PS2
  static recompilation hosted *on* a Vita. Not relevant to the source side.
- hammerill/ps-vita-porter: PC to Vita. The other direction.

## The emulator

### Vita3K

<https://github.com/Vita3K/Vita3K> - C++, GPL-2.0, 5.8 k stars, active
(pushed 2026-10-09). The only mature Vita emulator and the best public
description of Vita behaviour. CPU via dynarmic (JIT); renderer via OpenGL and
Vulkan; GXP shaders translated to SPIR-V/GLSL in-process.

Taken: facts only (NIDs, struct layouts, relocation formats, calling
conventions, which functions matter). No code. The map of where each fact
lives is in [VITA3K-REFERENCE.md](VITA3K-REFERENCE.md); the licence question
is in [DECISIONS.md](DECISIONS.md).

## ARM / Thumb-2 static recompilers

### sp00nznet's ARM family

The same author has four more 32/64-bit ARM lifters, all MIT. They are worth
reading when a Vita problem turns out to be a general ARM one:

| Repo | Input | Notes |
|---|---|---|
| [iparecomp](https://github.com/sp00nznet/iparecomp) | 32-bit ARM Mach-O, iPhone OS 2.x/3.x | C++ runtime with `arm32_context.h`, `arm32_runtime.c`, Objective-C host shims; Python lifter + `lift_verify.py` differential checks |
| [androidrecomp](https://github.com/sp00nznet/androidrecomp) | AArch64 Android native engines | Loader, Bionic shim, aarch64-to-C lifter |
| [ngagerecomp](https://github.com/sp00nznet/ngagerecomp) | Symbian ARMv4 E32Image | "No GPU to emulate; the project is the Symbian HLE layer" |
| [nokia-recomp](https://github.com/sp00nznet/nokia-recomp) | ARM7 DCT4 firmware | Python |
| [psprecomp](https://github.com/sp00nznet/psprecomp) | PSP Allegrex MIPS | Same "module_start is not the program" shape; PPSSPP as oracle |

### MaryNCRT/Ultimate-Mortal-Kombat-3-iOS-Recomp

<https://github.com/MaryNCRT/Ultimate-Mortal-Kombat-3-iOS-Recomp> - C, MIT,
active (October 2026). The retail armv7 binary kept its STABS table (4,342
named functions). Its own literal ARM/Thumb-to-C recompiler
(`tools/armrecomp/recomp.py`, Capstone-based, explicit `arm_ctx` struct) is
used as an oracle for hand-written C: a decompiled function and its
recompiled original run from the same randomised state in one 32-bit process
and the return value plus the whole data image are compared. About 80
transcription bugs and several recompiler bugs were found that way.

Taken: the differential-testing shape. Running the recompiled function against
Vita3K (or against a second lifter) from identical state is the verification
this repo should adopt for NEON and VFP work. Also the warning that Ghidra
misreads soft-float conventions and models NEON as opaque intrinsics.

### GBA (ARM7TDMI, Thumb v1)

- mstan/gbarecomp <https://github.com/mstan/gbarecomp>: ARM + Thumb to C++
  with an interpreter fallback for unresolved code.
- JRickey/gba-recomp <https://github.com/JRickey/gba-recomp>: Rust, "zero
  interpreter" goal.

Thumb v1 only, no Thumb-2, no VFP/NEON. Useful for the ARM-mode ALU and
flag semantics, nothing else.

### Lifters and interpreters

- lifting-bits/remill <https://github.com/lifting-bits/remill>: machine code
  to LLVM bitcode, tagged armv7 and thumb2. An alternative lifter path if the
  in-tree emitter stalls; heavy dependency.
- moleworld-dev/touchhle-arm-interpreter: pure-Rust ARMv7-A (ARM, Thumb-2,
  VFP) interpreter extracted from touchHLE. A possible second oracle for
  instruction semantics.
- suyu (Switch) shipped an ahead-of-time ARM64-to-x86 tool in its final
  build; AArch64, so only the idea carries.

## Vita binary format references

- vitasdk/vita-toolchain `src/sce-elf-defs.h` (MIT): `sce_module_info`
  (0x5C bytes), `sce_module_exports` (0x20), `sce_module_imports` (0x34) and
  the 0x24 short form, `sce_process_param` v5/v6, `sce_libc_param`.
  `src/self.h`: the SCE header as the toolchain writes it for fself.
- vitasdk/vita-headers `db/` (MIT): NID databases per firmware, directories
  `0.931`, `0.990`, `360`, `363`; `360` has 154 YAML files, 9,274 functions.
- Vita3K `kernel/src/relocation.cpp`: the complete `PT_SCE_RELA` entry format
  (formats 0 to 9) and the ARM relocation codes applied.
- Vita3K `kernel/src/load_self.cpp`: `e_entry` encoding, import record sizes,
  stub patching, special export NIDs.
- CreepNT/VitaLoaderRedux <https://github.com/CreepNT/VitaLoaderRedux>: Ghidra
  loader for Vita ELF-PRX modules, NID resolution analyzer that accepts an
  external database. The reverse-engineering workbench for any title.
- BinaryNinja-PSVitaLoader (2024): the Binary Ninja equivalent.
- wiki.henkaku.xyz: the community wiki for SELF, ELF and NIDs. Behind a
  bot-protection page when fetched by tools; read it in a browser.

## Input pipeline on a real console

- VitaShell dumps a game; FAGDec decrypts `eboot.bin` and `*.suprx` on the
  console to plain SCE ELF; the result is what Vita3K loads. Decryption
  needs the game to be runnable on that console (retail or NoNpDrm).
- NoNpDrm alone leaves the PFS layer in place; such dumps are refused here.
- Homebrew `.vpk` files contain an `eboot.bin` produced by vita-make-fself:
  SELF with plaintext zlib segments. These are the free regression inputs
  (VitaShell 2.02 is the first fixture).
