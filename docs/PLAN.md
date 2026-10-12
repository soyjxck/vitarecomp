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
- **A GPU.** First a null one, `sceGxm` accepted in full and drawn
  nowhere, which is what let the frame loop be reached; now one that
  draws (milestone D, below).
- **Variable imports.** The C library's case tables, `_Ctype` and the
  standard streams, bound by the loader at every site that names them; the
  C++ runtime's vtables get zeroed pages until the runtime has them.
- **Half precision.** `vcvtb`/`vcvtt` between a half and a single, with
  the subnormals and round-to-nearest-even, for the title's packed vertex
  data.
- **A time slice.** The title's locks and job queues wait by spinning, on
  a console with cores to spare. Backward branches count a slice, and a
  loop that only waits gives way at once (docs/ARCHITECTURE.md, Threads).
- **Audio without sound.** SceNgs as a graph that remembers what the sound
  engine wrote and plays each voice in silence for its length; the output
  port paces the audio thread (docs/HLE.md).
- **Four decoder faults and four in discovery** that the title's own code
  found: the 32-bit extends, the structure loads, the sign of a PC-relative
  load's offset, the table-branch guards, the text test on a pointer's
  target, a MOVW copied to another register before its MOVT, a stored
  pointer to a function already walked (docs/LESSONS.md).

Where it stands: the title starts, loads its front end
(`LOAD LEVEL( /frontend.bsp )`, `LEVEL START` in its own log) and draws
it, a flip every second vertical blank. The graphics layer counts what
each frame asks for and what became of it, and reports the count when
its shape changes; the counts are the front end's sequence:

| Stage | A frame is |
|---|---|
| Loading screen, while the front end loads | 7 scenes, 19 draws, 1,200 triangles, 2 million pixels shaded |
| The health warning | 8 scenes, 282 draws (255 of them precomputed), 45,000 triangles, 5 million pixels |
| The title screen | 1 scene, 7 draws, 380 triangles, 780,000 pixels |
| The main menu, after a touch | 1 scene, 9 draws, 80 triangles, 900,000 pixels |

Between the last two the title means to play a logo movie,
`game/movies/bend_logo3.mp4` out of its archive. Its player is its own
code; what it imports is SceMp4, to take the file apart, and the decoders.
None of that is here, and the runtime says so in the one place the player
asks first, the loading of the system module (docs/HLE.md, "Movies"); the
player fails to start, the front end's script is told the movie is over,
and the title screen follows. A touch on it (`--touch 480,335@1500-1512`,
or the mouse on the window) brings the main menu, and three more touches
start a campaign: New Game, the chapter's picture, Yes. The title then
saves through the system's dialog (docs/HLE.md), and loads and starts its
first level:

| Stage | A frame is |
|---|---|
| New Game: chapter and difficulty | 1 scene, 17 draws |
| "Start New Campaign?" | 1 scene, 28 draws |
| Loading `igc_snake_temple_room_start` | the loading screen again, 7 scenes |
| The level: the opening cinematic | 13 scenes, 255 draws (227 precomputed), 38,000 triangles, 5.3 million pixels, 81 ms |
| The first playable level, its first frame | 12 scenes, 544 draws (516 precomputed), 50,000 triangles, 6 million pixels |
| The journal, opened on that level's first frame at a tutorial page | 1 scene, 16 draws, 2.3 million pixels |
| Play: the level, the character run through it | 13 scenes, 546 draws (516 precomputed), 50,000 triangles, 5.4 million pixels, 85 ms |

The level's picture was black, and the first reason was the ARM
instruction set: the title carries some 8,500 instructions of hand-written
ARM-mode NEON, vertex and matrix work by the look of it, called from Thumb
code through `blx`, and every call was a trap (300,000 of them in the
level's first fourteen seconds). They are translated now
(docs/ARCHITECTURE.md, "ARM, for what is written by hand"), and the scene
was drawn, under a black rectangle the title laid over it last of all: a
fade it would not lift. It was waiting for its sound. The thread that
streams it had been created and never started, because the runtime
cleared a condition variable's work area at twice its size and with it
the thread's id (docs/LESSONS.md). With that thread running the level is
what it is meant to be, the cinematic that opens the game.

The cinematic runs for some forty seconds of the title's time, with the
next level loading behind it, and then the title saves a checkpoint and
switches: `LOAD LEVEL( /snake_temple_intro.bsp, /mov_min_game_intro.bsp )`,
the first level a player moves in. Its first frame took the last jump
tables discovery had not sized (LESSONS.md); with those followed it comes
up and opens the journal, the book the game keeps its tutorials in, at the
page for the journal itself. That screen took half a day to be understood
as one that was working (LESSONS.md again). A touch on the icon in its
corner closes it, the chapter's title fades in over the level, and the
left stick runs the character forward until the game opens the journal
again, at jumping and climbing.

What comes next: the levels at more than twelve frames a second (milestone
E), which is now the whole of the distance between this and playing; sound
that can be
heard, since the title streams it and the synthesiser plays it in silence;
and the movies, which want SceMp4's demultiplexer and the H.264 and AAC
decoders behind `sceAvcdec` and `sceAudiodec`.

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

## Milestone D: a `sceGxm` that draws — first pictures 2026-10-11

Upstream planned a host graphics API with a fixed substitute shader per
program, and a shader compiler after it. What was built instead is a
software renderer that runs the title's shader programs as they are, by
interpretation, so that the first picture is already the title's own:

| Piece | File | What it is |
|---|---|---|
| State | `src/hle_gxm.c` | the library's functions: nearly all of them only record something, in structures laid out so the title can copy them and read them back |
| Programs | `src/gxp.c` | the GXP container: where every input of a program arrives, where its outputs leave |
| Bytecode | `src/usse.c`, `src/usse_fields.h` | the GPU's instruction set, decoded, printed and run |
| Triangles | `src/raster.c` | clipping, the fill rule, perspective, derivatives |
| Textures | `src/gxm_tex.c`, `src/texdec.c`, `src/pvrtc.c` | the control words, the layouts, the block formats and both generations of PVRTC, filtering |
| Pixels | `src/pixfmt.c` | every texel and surface format, between bytes and colours |
| A draw | `src/gxm_draw.c` | vertex fetch to stored pixel: the tests, the fragment program, the blend |
| A scene | `src/gxm_draw.c` | its draws kept until it ends, then drawn together: each draw's vertices and triangles, the triangles sorted into tiles of the target, the tiles filled |
| Threads | `src/pool.c` | a draw to a thread for the vertices, a tile to a thread for the pixels, on three quarters of the host's processors |
| Rows | `src/usse.c`, `src/half_rows.c` | the same bytecode run for sixty-four pixels at a time, each instruction taken apart once for all of them |

How it was kept honest: the decoder's listing is compared line for line
with the disassembly Vita3K logs for the same programs; each piece has a
test built from values made in the test, with no title's data; and the
three switches in ARCHITECTURE.md ("Seeing what was drawn") show a
frame's draws, the textures they sampled and the programs they ran.

Not there yet, in the order the title will want them:

1. Speed. One pixel at a time through an interpreter was a second a
   frame of the loading screen on one thread. Three things since, each
   leaving the same frames to the byte: bands of rows to a thread (80 ms
   on sixteen); compressed textures decoded once and kept, not a block
   at a time as they are sampled; and the fragment program run for
   sixty-four pixels at a time (30 ms). The scene behind the health
   warning, 45,000 triangles and five million pixels, went from a second
   to about 350 ms. Then the texture samples, which were over half of
   what a frame of the opening cinematic costs, nine million of them: a
   field's value looked up where it has few values (a half, a small
   float, a byte) in place of a division or a scaling each time, and a
   level read as rows of words where it is rows of words, which took a
   quarter off them. Then the threads: a draw at a time with its rows
   shared out left most of them waiting through most draws, since a
   scene is hundreds of draws and most are small, and the vertices were
   one thread's. A scene's draws are now kept and drawn together when it
   ends, as the console draws them: a draw to a thread for vertices and
   triangles, then a tile of the target to a thread, the fullest tiles
   first. A frame of the cinematic went from 350 ms and more to about
   110 on twenty-four threads, and two minutes of the title that took
   twenty-seven minutes to draw took six. Then the walk over the
   triangles, which covers 8.6 million pixels a frame to shade 4.8: a
   pixel's varyings are worked out four at a time, their rates of change
   only where a texture with smaller copies is placed by them, and the
   stencil and depth tests are made in the walk itself, before either,
   for the draws where a pixel that fails changes nothing. A fifth off
   the walk. Then the samples again, from the other end. A census of
   what play samples said nearly half of it is the scene's own buffers
   read back as textures, depth beside a byte, small floats, halves in
   tiles, signed bytes, and every texel of those went through the general
   reader and the general decoder; a texture now works out once, from its
   control words, how its texel is read as a word and each channel had
   from it. And a texture input is sampled for the whole row of pixels
   waiting in one call, the texture's levels looked at once for the row
   and the results put into the registers a row at a time. Samples went
   from 860 ms of the threads' time a frame to 440, and a frame from 163
   ms to 123 on a machine doing other work. `VITARECOMP_GXM_PROFILE`
   says where a frame's time goes now: samples and programs level, then
   the walk and the store, and a quarter of a frame is the title's own
   code. Four more things, of a morning: a result of four halves goes
   into its pixel by lookup, since what each field of the pixel gets
   depends on one half and there are 65,536 of those (the store, 210 ms
   of the threads' time to 55); vertices go through their program
   sixty-four at a time as pixels do, the loads of bone matrices taken
   a lane at a time inside the row (the vertices of play, 15 ms of a
   frame to 7); tiles are 32 pixels a side and not 64, which keeps
   twenty-four threads busy to the end of a list (the fill, 78 ms to
   59); and the pool's threads take their items by counting, with no
   lock. A frame of the cinematic is 81 ms and of play 85 where the
   morning began at 123, and the two minutes of the title that first
   took twenty-seven minutes to draw take four. One thing after those:
   the title keeps its depth from scene to scene, so a third of a
   million depths were read from its surface and written back to it
   several times a frame, by one thread; the rows are now shared out
   sixteen at a time, and a frame of play measured that afternoon at
   99 ms was 90 after it. The title's own reads and writes of memory
   are done in line, a comparison and the bytes, which is the usual
   thing to do and has measured nothing here yet: its code is a sixth
   of a frame. What is left is in milestone E.
2. Lines and points, depth bias, the mask-update program, multisampled
   targets, the YUV textures a movie arrives in, the transfer calls.
   Each is counted or named where it is skipped, none is silent.

## Milestone E: the renderer at speed

The interpreter is the definition; what runs every frame should be
generated from it. A first step is taken: a program whose instructions
allow it is run for a row of pixels at once, which takes the decoding out
of the cost of a pixel and leaves the arithmetic, the texture samples and
the store. Beyond that, two routes, not exclusive: translate each
program's bytecode to C once, the way the title's own code is translated;
or translate it to a host GPU's shading language and draw there
(DECISIONS.md, 3). The first keeps the pipeline exactly as it is and needs
no graphics API; the second is where real time at full resolution is.
Either is checked against the interpreter, frame for frame, as the rows
were (`VITARECOMP_GXM_SCALAR=1` is the interpreter one pixel at a time).

What the first route has left, measured on a frame of play (2026-10-11,
74 ms, of which the fill is 52 on twenty-four threads): the title draws
fifteen screens' worth of pixels a frame, 4.3 million, at 270 ns of a
thread each. No part of that stands out. By function the bilinear read
of a texel is 14 % of the threads' work, the triangle walk 10 %, and
everything else is pieces of 2 to 5 %: the conversions to and from
halves, the loads and stores of rows, the tests. By program, two
materials of four samples each and the ambient pass take 37 % between
them, at 380 to 470 ns a pixel, and they are what they look like, long
programs over every pixel. Two guesses were measured and dropped: the
sampler's floors and blends done without a call and four channels at a
time changed nothing, and the logarithm that chooses a level of detail
is 1.2 %. Translating the programs to C would take the decoding and the
row traffic out of a third of the work, a sampler rewritten for several
samples at a time some of another third: together perhaps a frame and a
half where there is one. Thirty frames a second is not on that route.
It is on the second.

The second is begun (2026-10-11; ARCHITECTURE.md, "The same draws on a
graphics card"). Every fragment and vertex program the first title
registers translates to HLSL and compiles, a scene's list of draws is
drawn by Direct3D 11, and drawn both ways the two results agree list for
list through the front end and the first minute of play, once the choice
of a texture's level is taken out of the comparison. Surfaces stay on the
card from one list to the next, a draw's geometry from one frame to the
next, and bytes that were not written are not read again: a frame of play
takes 23 ms against the processor's 85, which is past the console's
thirty a second. The frame is shown in the host's window by the card
that drew it, and does not pass through the title's memory on the way;
a program compiled in one run is not compiled in the next
(`VITARECOMP_GPU_CACHE`); and the card draws at a whole number of times
the title's size when asked (`VITARECOMP_GXM_GPU_SCALE`), twice at no
cost. It is switched on by `VITARECOMP_GXM_GPU=1` and is not yet the
default. What is left:

1. **Where the title's own not-a-numbers come from.** The check over the
   front end and the first minute of play finds two lists in three
   thousand that differ, the same draw each time, and what it found is
   older than the difference: the textures that draw reads, the chain a
   bright part of the picture is spread through, hold what is not a
   number at a hundred to fourteen hundred texels, in both renderers.
   The two differ only at the edge of those, in seventy-six pixels,
   where a sample falls on the middle of a texel and its neighbour has
   no weight: the card's filter gives a neighbour of no weight no say,
   and the interpreter's arithmetic makes nothing times not-a-number
   not a number. Nothing here says which the console does, and the
   frames that come of it agree. What makes the first of them, and
   whether the console's own arithmetic would, is the question.
2. **A watch on the pages of a surface the card holds**, so that the
   title's own code reading it is noticed. Today that is known only for
   surfaces of a few pixels, by sending those back always.
3. **The rest of a frame.** Sixteen of the 23 ms were neither the card
   nor its path. Some of that was the path's after all, asking the
   system which pages had been written, and is gone (LESSONS.md); what
   is left is the title's code and the graphics library's handling of
   each draw as it is made, and has still to be measured on a quiet
   machine.
4. **Textures in their own form.** A compressed texture goes to the card
   as four floats a texel; the card has the compressed forms. It costs
   memory and the first frame of a scene, not the frames after.
5. **A frame's time by the real clock.** In a window, in play, a frame
   is 33 to 37 ms where the virtual clock says 23 to 28 of work: some
   frames miss their sixtieth. What the other threads take while the
   main one waits, and what the wait itself costs, has not been
   measured.
6. A second implementation of `src/gpu.h` for hosts without Direct3D.
7. Making it the default where there is a card, with the processor's
   renderer for what a card's path leaves and for checking it.

## Milestone F: first title repo

A per-game repo consuming this one as a submodule, as upstream intended:
host executable, game-specific HLE overrides, patches, packaging. The toolkit
stays game-agnostic.

The first exists, for the reference title: this repository as a submodule
and a program of its own in front of it. Its first form was a CMake file
of twenty lines and three scripts, one of which started the generic host
with the module and the mounts on its command line; that is not a program
anybody runs, and the host was made linkable for it (`vita_host_main`,
`vitarecomp_add_game`, docs/ARCHITECTURE.md, stage 7). It is now an
executable with a `main()` of its own, a settings file, and a setup that
finds the player's copy where the emulator keeps it, checks the module by
its hash and installs it; arguments it does not know go on to the host, so
it is also its own test harness. It has needed no override and no patch:
what is the title's in it is its name, its title ID, the hash of its
module and three frame hashes.

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
- VitaShell's output has grown again: measured 2026-10-11 it is 9,780
  functions and 13.6 M emitted instructions for 802,000 reached, where
  *Golden Abyss* emits 2.43 M of which 2.37 M are distinct. Code that
  several functions run into is written into each; in GCC's output, where
  a call in tail position is a plain branch, that is most of the output.
  To find: which of the discovery changes since made it worse, and whether
  a branch to another function's start can always be a call.

- CI on Windows and Linux: build, `ctest`, and the smoke run on the VitaShell
  fixture.
- Keep upstream's rule: every untranslated instruction and every unimplemented
  import traps by name. No silent zeros.
- Measure whole modules, not prefixes (LESSONS.md, "A prefix is not a sample").
- Track upstream: `git fetch upstream` and merge if it moves again.
