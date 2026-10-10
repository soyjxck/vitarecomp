# psvitarecomp

> A library of static-recompilation tools for PlayStation Vita games:
> ARMv7 / Thumb-2 to native C, with the firmware surface implemented as HLE
> rather than emulated.

This repository carries forward [sp00nznet/vitarecomp](https://github.com/sp00nznet/vitarecomp)
(MIT, 38 commits, July to August 2026), the only Vita-to-PC static
recompiler found anywhere. Its history is preserved here unchanged and the
`upstream` remote still points at it. Everything that toolkit established is
kept; this repo exists to finish what it stopped on and to widen it from one
QA build to retail titles.

**Milestone A reached 2026-10-10: a recompiled Vita homebrew module runs
natively.** `samples/fbdemo` (freestanding, eight firmware imports) goes
through `armrecomp emit`, links against the runtime's HLE, and runs in the
headless host for 200 frames with zero traps: its gradient and the square the
scripted d-pad moves come out pixel-exact, and START exits through
`sceKernelExitProcess`. `tools/e2e.sh` repeats that in CI on Windows and
Linux and hash-checks the frame.

**Status, measured on this machine (2026-10-10):**

| Check | Result |
|---|---|
| `tools/e2e.sh`: `samples/fbdemo` SELF to pixels | extract, emit (179 instructions, every one of the program's own translated; the 8 traps left are import stub bodies HLE replaces), build `vita_host`, run 200 frames: 0 traps, 0 bad accesses, frame hash matches |
| `armrecomp emit` on VitaShell, whole module | 99.19 % of instructions translated; what still traps: saturating ops 0.39 %, NEON/VFP 0.20 %, general LDM/STM 0.16 % |
| Build (MSVC 14.50, CMake 4.3, Ninja) | `armrecomp.exe` + `vitarecomp.lib` build clean |
| `ctest` | 4 / 4 suites pass (container, inflate, decode, runtime) |
| `armrecomp info / extract / cover` on VitaShell 2.02 `eboot.bin` (homebrew, no keys) | 3 segments inflated, Adler-32 ok, ELF 4,882,182 bytes as declared; Thumb-2 dominant, 0.22 % unknown, 2.01 % SIMD |
| `armrecomp relocs` on the same module (`ET_SCE_RELEXEC`) | 63,334 entries decoded, all format 0; 6,735 relocated words hold Thumb-tagged `.text` addresses; applying the stream at the link addresses changes 0 bytes |
| `armrecomp discover` with relocation seeding | 9,195 functions (4,523 from seeds, 2,975 only the relocations named, 1,697 from shape), 71.5 % of the segment covered; was 8,074 and 65.3 % before the stream was read |
| `armrecomp funcs` against vita-headers `db/360` (156 files, 9,596 names) | 294 imports across 40 libraries, 287 resolved (97.6 %); `SceGxm` alone is 57 of them |

Upstream's last measurement on *Uncharted: Fight for Fortune* (a QA build):
98.95 % of 1.8 M instructions translated, the whole module compiles to a 39 MB
executable, boots through 551 C++ static constructors and stops on the first
NEON instruction. NEON and the HLE layer (kernel, `SceLibc`, `sceGxm`) are
where the work is. See [docs/PLAN.md](docs/PLAN.md).

## Why this exists

- **Nobody has shipped a Vita recomp.** Vita appears in recomp catalogues only
  as a *target* (N64 ports to Vita) or as a host for PS2 recomp experiments.
  The one source-to-PC toolkit stalled seven weeks before this repo was made.
- **The Vita is a good recomp target.** One documented CPU (Cortex-A9,
  ARMv7-A, Thumb-2 dominant), a documented OS boundary reached only through
  NID-keyed import tables, and an MIT-licensed NID database
  ([vita-headers](https://github.com/vitasdk/vita-headers)).
- **Vita3K is the reference, not a dependency.** It has ~160 HLE module
  directories, the loader and relocation code, and a GXP-to-SPIR-V shader
  recompiler. It is GPLv2, so it is used as documentation and as a
  behavioural oracle. What that means for licensing is in
  [docs/DECISIONS.md](docs/DECISIONS.md).

## Layout

```
include/vitarecomp/   runtime API: cpu, mem, dispatch, vfp, hle, host, loader
src/                  runtime: CPU state, memory, traps, dispatch, and the HLE
                      (SceLibc batch 1, SceSysmem, SceDisplay, SceCtrl, SceLibKernel)
host/                 vita_host: the generic headless host (scripted input, frames to files)
samples/fbdemo/       the first fixture: a freestanding homebrew program, source and prebuilt SELF
tools/armrecomp/      the recompiler: container, inflate, decode, reloc, module, analyze, nids, emit
tools/*.sh            helpers: MSVC env import, dependency fetch, smoke, end-to-end, vitasdk in WSL
tests/                synthetic ctest suites, no game data
docs/                 upstream's design notes plus this repo's research, plan and decisions
external/             (gitignored) vita-headers NID database, fetched by tools/fetch-deps.sh
fixtures/             (gitignored) homebrew modules used as key-free regression inputs
```

## Build

```sh
# Windows, Git Bash: import the MSVC x64 environment first
source tools/msvc_env.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

# NID database and a key-free fixture (VitaShell eboot.bin)
tools/fetch-deps.sh
tools/smoke.sh          # info, extract, cover, funcs on the fixture
tools/e2e.sh            # samples/fbdemo: SELF -> C -> vita_host -> a hash-checked frame

# To rebuild the sample itself (needs vitasdk; the script installs it in a
# Linux shell, WSL included):
#   wsl -d <distro> -- bash /mnt/f/psvitarecomp/tools/wsl-vitasdk.sh
```

## Running a module

```
vita_host <module.elf> [--frames N] [--dump out.ppm[@FRAME]] [--press BUTTON@FROM[-TO]]...
```

The host is built against one module's emitted C: configure with
`-DVITARECOMP_RECOMP_DIR=<dir>` pointing at the output of `armrecomp emit`.
It loads the ELF's segments at their link addresses, carves an arena for
memory blocks and a heap above them, registers the dispatch table, and calls
`module_start`. Every firmware import the runtime implements is linked by
name; every one it does not traps by name on first use
(`VITARECOMP_TRACE=1` to log and continue). It is headless: input is scripted
and frames are written as PPM. A windowed host is the same three functions in
`include/vitarecomp/host.h` pointed at a window.

The core has no external dependencies: a fresh clone builds with CMake and a C
compiler only. zlib inflate is written in-tree.

## The tool

```
armrecomp info     <file>                       identify a module, report its structure
armrecomp extract  <self> <out.elf>             SELF in, plain ELF32 out
armrecomp cover    <elf>                        coverage, class histogram, instruction set
armrecomp funcs    <elf> [nid-db]               the HLE work list, from the import table
armrecomp relocs   <elf>                        the relocation stream, decoded and classified
armrecomp discover <elf>                        function discovery
armrecomp emit     <elf> <out.c> <n> [nid-db]   translate to C
```

`nid-db` is a directory from vita-headers, for example `external/vita-headers/db/360`.

## Inputs

| Source | Container | What is needed |
|---|---|---|
| Homebrew `.vpk` / `eboot.bin` (vita-make-fself) | SELF, plaintext zlib segments | nothing |
| QA / prototype builds | SELF, plaintext zlib segments | nothing |
| Retail, decrypted on a console with FAGDec (the Vita3K input format) | plain SCE ELF | a dump you made from a game you own |
| Retail NoNpDrm dumps | PFS + SELF | out of scope: refused by name |

No game data, keys, or decryption code are in this repo, now or later.

## Documentation

| Document | Covers |
|---|---|
| [docs/PLAN.md](docs/PLAN.md) | What is done, what is next, in order, with the first milestone named |
| [docs/PRIOR-ART.md](docs/PRIOR-ART.md) | Every related project found: Vita, ARM/Thumb recomps, Vita3K, loaders, databases |
| [docs/VITA3K-REFERENCE.md](docs/VITA3K-REFERENCE.md) | Where in Vita3K each fact lives: loader, relocations, HLE bridge, shader recompiler, NID table |
| [docs/DECISIONS.md](docs/DECISIONS.md) | Open decisions with recommendations: licence boundary, retail inputs, host graphics API |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Upstream: the pipeline stage by stage |
| [docs/TRANSLATION.md](docs/TRANSLATION.md) | Upstream: coverage figures and what still traps |
| [docs/CORPUS.md](docs/CORPUS.md) | Upstream: 15 measured modules, the static/relocatable era split |
| [docs/DECRYPT.md](docs/DECRYPT.md) | Upstream: the SELF container and why QA builds need no keys |
| [docs/HLE.md](docs/HLE.md) | Upstream: the firmware surface, measured |
| [docs/LESSONS.md](docs/LESSONS.md) | Upstream: bugs found by running rather than reasoning |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Upstream: the six phases as they stood on 2026-08-20 |

## Relationship to other projects

| Project | Licence | Use |
|---|---|---|
| [sp00nznet/vitarecomp](https://github.com/sp00nznet/vitarecomp) | MIT | The base of this repo; `git remote upstream` |
| [vitasdk/vita-headers](https://github.com/vitasdk/vita-headers) | MIT | NID database, loaded at run time |
| [vitasdk/vita-toolchain](https://github.com/vitasdk/vita-toolchain) | MIT | Reference for `sce_module_info`, import/export and process-param layouts |
| [Vita3K/Vita3K](https://github.com/Vita3K/Vita3K) | GPLv2 | Reference and oracle only; nothing copied, linked or vendored |

## Licence

MIT, inherited from upstream; see [LICENSE](LICENSE). Not affiliated with or
endorsed by Sony Interactive Entertainment. Bring your own game.
