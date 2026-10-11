# vitarecomp

> A library of static-recompilation tools for PlayStation Vita games:
> ARMv7 / Thumb-2 to native C, with the firmware surface implemented as HLE
> rather than emulated.

This repository is a fork of [sp00nznet/vitarecomp](https://github.com/sp00nznet/vitarecomp)
(MIT, 38 commits, July to August 2026), the only Vita-to-PC static
recompiler found anywhere, and carries it forward. Its history is preserved
here unchanged and the `upstream` remote still points at it. Everything
that toolkit established is kept; this fork exists to finish what it
stopped on and to widen it from one QA build to retail titles.

**Milestone A reached 2026-10-10: recompiled Vita homebrew runs natively.**
`samples/fbdemo` (freestanding, eight firmware imports) and `samples/hello`
(a newlib-linked C program: snprintf, malloc, text on screen, a console log) go
through `armrecomp emit`, link against the runtime's HLE, and run in the
headless host with zero traps, pixel-exact, exiting through firmware on a
scripted START. `tools/e2e.sh` repeats both in CI on Windows and Linux and
hash-checks the frames, with a third sample, `samples/threads`, whose threads
wait for each other by spinning. The newlib sample needed a kernel, file and C-runtime
HLE surface, the NEON a retail title's own code asked for, jump tables, and fixes for nine emitter
bugs that only running could find (docs/LESSONS.md).

**A retail title plays (2026-10-11).**
*Uncharted: Golden Abyss*, recompiled from its decrypted module, starts in
the host, loads its front end and renders it: the loading screen's turning
medallion, lit through the title's own deferred pipeline (a geometry
buffer, light passes, bloom, tone mapping, a scale to the panel), the
health warning over the temple, the waterfall and the shafts of light, the
title screen with its logo, legal lines and "Touch to Start", and, touched,
the main menu and the screens behind New Game: the chapter, the
difficulty, "Start New Campaign?". Answered yes, it creates its save,
writes it, loads its first level, and plays the cinematic that opens the
game: a line of Coronado's over the snake temple in the rain, then two men
talking in front of it, faces, cloth and foliage lit through the same
pipeline, thirteen scenes, 255 draws and 38,000 triangles a frame. Forty
seconds on it saves a checkpoint, loads the first level a player moves in,
and opens the player's journal at its first tutorial page. The journal is
closed with a touch on its corner, the chapter's title comes up over the
jungle, and the left stick runs the character through it to the next
thing the game has to teach.
Every pixel comes out of the
title's own shader programs, run by an interpreter of the GPU's bytecode
that is written here; nothing of the title's is replaced. It is a reference
renderer, right before fast: with a scene's draws drawn together at its
end, a tile of the target to a thread, and each fragment program run for
sixty-four pixels at a time, a frame of the title screen or the menu takes
about 20 ms and a frame of the opening cinematic or of play, 50,000
triangles and five million pixels, 110 to 120 on twenty-four threads:
eight frames a second, where the console shows thirty.
The logo movie that comes before the
title screen is not played: the runtime has no movie library, says so when
the title asks for it, and the title carries on as it does when a movie
ends. The synthesiser still plays in silence. `vita_host --window` shows it
all, with the left mouse button for a finger on the screen and the keys for
the pad and the sticks; docs/PLAN.md says what is missing.

**Status, measured on this machine (2026-10-11):**

| Check | Result |
|---|---|
| `tools/e2e.sh`: `samples/fbdemo` SELF to pixels | extract, emit (one function, 163 instructions, all translated), build `vita_host`, run 200 frames: 0 traps, 0 bad accesses, frame hash matches |
| `tools/e2e.sh`: `samples/hello`, a newlib-linked program | 52 KB SELF, 54 imports across 11 libraries, all implemented on its path; 132 of 764 discovered functions emitted (the rest nothing can reach), 99.96 % translated; runs 60 frames with 0 traps and 0 bad accesses: `sceClibPrintf` reaches the console, `snprintf`/`malloc` output is drawn on screen with its own font, frame hash matches |
| `tools/e2e.sh`: `samples/threads`, threads that never call the kernel to wait | two workers and the main thread wait for each other by spinning on memory, and share a test-and-set spinlock for 4,000 increments; the frame shows every check green and the counter exact, however the threads were interleaved |
| `armrecomp emit` on VitaShell, whole module | 23,326 functions discovered (253 jump tables, 23,337 tail-call targets, 1,098 MOVW/MOVT pointers), 802,000 instructions reached; 9,780 functions emitted: what an entry point, an export or a stored pointer can reach; 13.6 M instructions of C, 99.93 % translated, 1.2 GB: seventeen times what is reached, code that several functions run into being written into each of them, which has grown since it was last measured (docs/PLAN.md, housekeeping); what still traps is NEON forms not yet claimed (0.06 %) and 953 branches to addresses discovery never registered |
| *Uncharted: Golden Abyss* (retail, PCSA00029), decrypted by Vita3K from a NoNpDrm dump with its own licence | `ET_SCE_EXEC`, 9.43 MB of code, 3.33 M Thumb instructions, 5.34 % SIMD; 79,348 functions discovered (20,331 of them from MOVW/MOVT pairs, 36 of them hand-written ARM), 522 jump tables, 84.0 % covered; 618 imports across 42 libraries, 585 named (SceGxm 122, SceLibc 79, SceFios2 42, SceLibm 39, SceLibKernel 39, SceNgs 34); 24,300 functions emitted, 1,204 of them leaves that only a stored pointer names, 2.45 M instructions at 99.99 % translated, 300 MB of C in 22 parts; 293 trapped instructions, 278 of them the breakpoints of its own assertions, 12 the 64-bit exclusive loads and stores, and no Advanced SIMD or VFP instruction or jump table of a real function left; 68,217 backward branches carry the time slice |
| *Golden Abyss* running in `vita_host` with Vita3K's decrypted data mounted as `app0:` | `module_start` allocates its memory, initialises FIOS2 and SceUlt, reads the 3 GB archive's index, starts its worker threads on the runtime's scheduler, finds its resources by their CRC, parses its input tables, builds its sound engine's eight racks on the silent synthesiser, loads its front end (`LOAD LEVEL( /frontend.bsp )`, `LEVEL START` in its own log) and draws it: 7 scenes and 2 million shaded pixels a frame for the loading screen, 8 scenes, 282 draws and 45,000 triangles for the health warning; 700 vertical blanks with no trap, no dispatch miss, no bad access, no shader instruction the interpreter lacks and no texture unit left unread; left to run, the title screen after them (1 scene, 7 draws) and, with scripted touches, the main menu, the New Game screens, the save through the system's dialog and the first level's load (`LOAD LEVEL( /igc_snake_temple_room_start.bsp )`): the level is the game's opening cinematic, 13 scenes, 255 draws (227 precomputed) and 38,000 triangles a frame, every program of them run and the title's 36 hand-written ARM-mode routines with them; after it the first playable level (`snake_temple_intro`, 546 draws and 50,000 triangles a frame): the journal opened and closed, the chapter's title, the character run forward with the left stick to the second tutorial: 7,200 vertical blanks, no trap, with the trace or without it; with `--virtual-clock` the same frames, byte for byte, on every run, and the same again with every pixel shaded by itself (`VITARECOMP_GXM_SCALAR=1`) |
| `armrecomp gxp` on the shader programs the title registers | the 193 of its boot and the 254 it has registered by its first level: every instruction decoded and executable (4,014 in the first set); on the 52 programs Vita3K logged, the listing is identical to its disassembly, 1,208 lines, apart from the two branches it does not print |
| Build (MSVC 14.50, CMake 4.3, Ninja) | `armrecomp.exe` + `vitarecomp.lib` build clean |
| `ctest` | 13 / 13 suites pass (container, inflate, decode, discover, reloc, runtime, raster, texdec, usse, usse_wide, pixfmt, gxm_tex, pool); the interpreter that runs a program for sixty-four pixels at once leaves every register of every one as the one-at-a-time interpreter does, on 60,000 random instructions and 4,000 random programs; the PVRTC decoder gives, on words from a fixed generator, the images Imagination's codec gives for them |
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
tools/*.sh, *.ps1     helpers: MSVC env import, dependency fetch, smoke, end-to-end, vitasdk in WSL,
                      Vita3K-driven decryption and import of a retail title
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
#   wsl -d <distro> -- bash /mnt/<drive>/vitarecomp/tools/wsl-vitasdk.sh
```

## Running a module

```
vita_host <module.elf> [--frames N] [--dump out.ppm[@FRAME]] [--press BUTTON@FROM[-TO]]...
                       [--touch X,Y@FROM[-TO]]... [--stick L|R,X,Y@FROM[-TO]]...
                       [--mount NAME=DIR]... [--window]
```

The host is built against one module's emitted C: configure with
`-DVITARECOMP_RECOMP_DIR=<dir>` pointing at the output of `armrecomp emit`.
It loads the ELF's segments at their link addresses, carves an arena for
memory blocks and a heap above them, registers the dispatch table, and calls
`module_start`. Every firmware import the runtime implements is linked by
name; every one it does not traps by name on first use
(`VITARECOMP_TRACE=1` to log and continue). It is headless by default: input
is scripted, buttons, sticks and touches alike, and frames are written as
PPM. With `--window` the five functions of `include/vitarecomp/host.h` have
a window behind them: the frames as they are shown, the keyboard as the
buttons and the sticks, the left mouse button as a finger on the screen. A
title's files are found under the directories given as mounts: `app0` its
data, `ux0` somewhere it may write, `savedata0` where its saves are kept.

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
| Retail NoNpDrm dumps | PFS + SELF | refused by name; Vita3K decrypts them with the dump's own licence, and `tools/vita3k-decrypt.ps1` + `tools/import-vita3k.sh` bring the result in (docs/PLAN.md, milestone C) |

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
