# The HLE layer — measured, not estimated

The firmware surface a module needs is not a matter of opinion: the module's own
import table names every function it calls, and the MIT
[vita-headers](https://github.com/vitasdk/vita-headers) NID database turns those
hashes back into names.

```
$ armrecomp funcs uncharted.elf path/to/vita-headers/db/360
nid db:   154 files, 9274 functions
module:   cardgame
firmware libraries needed (524 functions across 39 libraries):
...
resolved 519 of 524 imported functions (99.0%)
```

**519 of 524 resolved.** That is the HLE work list, derived rather than guessed.

## Why the database is loaded, not vendored

`vita-headers` is MIT, so bundling it would be permitted. It is loaded from a
directory at run time anyway, because it is *data*: the toolkit stays free of
third-party source, a newer database needs no rebuild, and a module targeting a
different firmware needs a different directory rather than a different binary.

## Variables are imports too

The import table names variables as well as functions, and the database
names those too (`variables:` under a library). A retail title takes eighteen:
the C library's `_Tolotab`, `_Touptab` and `_Ctype` tables and its three
standard streams, and twelve RTTI vtables and type_info objects from the C++
runtime. They are not optional: the title lowercases every resource name
through `_Tolotab` before hashing it, and an inlined `dynamic_cast` compares
a type's type_info against the runtime's `__si_class_type_info` by address.
`src/hle_vars.c` builds each in guest memory with the shape the library gives
it, and the loader writes the address at every site the module's relocation
blocks name (docs/ARCHITECTURE.md, "Variables are bound like functions").

## Audio: the graph without the signal

SceNgs, the synthesiser, is a graph the title builds and then reads back:
racks of voices, each voice a fixed chain of modules (a sample player, an
envelope, filters), patched output to input up to a master buss, and mixed
one block per `sceNgsSystemUpdate`. A sound engine is a heavy reader of its
own writes. It locks a voice's parameter block, fills it, and later locks it
again and expects the same bytes; it polls how far a voice has played; it
frees a voice when the callback says the sample has finished. A stub that
accepts every call and remembers nothing breaks all three.

`src/hle_ngs.c` keeps the graph and leaves out the sound. A rack lives in
the block the title passes to `sceNgsRackInit`, sized by what
`sceNgsRackGetRequiredMemorySize` asked for, so nothing is allocated behind
the title's back: voice handles, patch handles and each module's parameter
block are addresses in that block, and the host keeps what they mean. Each
update moves every playing voice on by the block's samples at the voice's
own rate, converted to bytes for 16-bit PCM, ADPCM or ATRAC9 from the
stream's configuration word, through the buffer chain with its loop counts.
A voice that reaches the end of its data, or was keyed off, finishes at that
update and its callback runs on the thread that asked for the update. The
audio output port takes as long to accept a block as the block lasts, which
is what paces the thread. A decoder and a mixer can replace the silence
without the title seeing a difference in anything but the samples.

The voice definitions' module chains and the parameter structures' sizes
are the console's interface, taken from Vita3K's documentation of it
(`vita3k/ngs`).

## Movies: a library that is not there

A title that plays movies carries Sony's player in its own code and
imports the layer beneath it: SceMp4 to take a file apart into video and
audio units, `sceAvcdec` and `sceAudiodec` to decode them. The runtime has
none of the three yet. How it says so matters more than it looks.

The player loads SceMp4 as a system module and then opens the file. If the
open fails, the player writes a line to its log and tells its caller
nothing: no callback, no state change, and a title waits for ever on a
movie that never began. If the module cannot be loaded, the player's own
start fails in the call that asked, the title's script hears that the
movie has finished, and what follows the movie follows. So
`sceSysmoduleIsLoaded` and `sceSysmoduleLoadModule` no longer say yes to
everything: for the one module the runtime does not have (`0x2D`) they
answer "not loaded" and "cannot be loaded" (`src/hle_kernel.c`), and the
functions in `src/hle_movie.c` are what a title reaches only if it did not
ask.

The rule is general. Where a library is absent, the place to say so is
the place the title checks before it relies on it, and that is found by
reading the title's code, not by guessing an error code for the first
call that happens to trap.

## Services that are not there

A title is written for a console with a network, neighbours and a shop,
and asks after all of them. The near-field service (`src/hle_near.c`) is
the pattern for what to do: every function of it says there is no such
service, the title's own start of it fails, and the title carries on as it
does on a console where the service is switched off. Two things are worth
keeping from how that was found. The functions have no names here: the
title links a version of the library whose numbers no table carries, so
they are answered by number. And the second of them only appeared when
the title left its menus, and only stopped the run when the trace was
off: with the trace on, an import nobody implements returns and the run
goes on, which is the point of the trace and is also how a run that looks
healthy can be one that nobody could reproduce without it. The summary
line counts traps for that reason, and the count wanted is zero.

## Pad and panels

`src/hle_ctrl.c` and `src/hle_touch.c` are snapshots of what the host says
is held: a mask of buttons, and whether a finger is on the front panel and
where, in the panel's units of half a display pixel. A contact keeps its
number while it lasts and the next has the next one. A panel reports
nothing until the program has started its sampling, and nothing ever
touches the back. The host's side of it is two functions
(ARCHITECTURE.md, "The contract"), scripted on the command line or read
from a window.

## Save data, and the system's dialog about it

`src/hle_savedata.c`. A title's saves are slots, each a record of what the
system's screens show about it, and beside the records the title's own
files. The files go under whatever the host mounted as `savedata0`
(`--mount savedata0=DIR`), where the title reads them back as ordinary
files; a slot's record is one more file there. With no such mount a run
keeps its slots in memory and its files nowhere.

The dialog is the system's screen for saving and loading, and a title does
not draw it or see it: it starts it, polls it, and is told what the player
chose. It is a machine of two levels. The dialog runs from its start until
the title finishes it; inside that, one screen at a time is up, each
running until the player answers it or, where it has no buttons
("saving..."), until the title takes it down. The title reads the result
of each screen and puts up the next or ends the dialog. A stub that says
"finished, cancelled" to everything gets a title nowhere: it had asked a
question and is told nobody answered.

There is no screen here yet and nobody to ask, so a screen that wants an
answer gets the affirmative one, OK or Yes or the slot the list had
focused, after half a second of the title's time. That is a policy, and
the place a host that draws dialogs would replace.

## SceGxm: what it took

The section after this one is upstream's estimate, kept because it was
right about the shape: most of the library is bookkeeping and the work is
in a few places. What it took, now that it draws:

- **The bookkeeping has to be visible.** A title copies textures,
  surfaces and precomputed draws by value and reads them back field by
  field, so their state lives in the title's memory in layouts with the
  SDK's sizes: the texture's four hardware control words, 0x30 bytes of
  colour surface holding a texture over the same pixels, 0x14 bytes of
  depth surface, the precomputed structures with their arrays in the
  block the title supplies. Programs are read in place from the title's
  GXP data. Only what a title cannot see is kept on the host side.
- **A scene is drawn when it ends.** The console renders a scene after
  the call that ends it returns, and signals the title through
  notification words. Here a draw is put on a list as it is made, its
  state copied and its uniforms read, and the list is drawn inside the
  call that ends the scene, into the colour surface's own memory: every
  notification is true by the time the title can look, and the display
  queue's callback runs at once. A title leaves a draw's vertices,
  indices and textures alone until its scene is over, because the
  console reads them then, and so they are read then here. The list is
  also drawn early where waiting would be wrong: when the ring that
  default uniform buffers come from starts again, and before a draw that
  samples the surface its own scene draws to, which the next point is
  about.
- **A scene can read what it has just drawn.** One title keeps a chain
  of smaller and smaller copies of its picture side by side in one
  surface, and makes each from the last in a single scene: every draw
  samples a rectangle of the surface it is drawing to. On the console
  that works because tiles are finished in order. Here everything before
  such a draw is drawn first and the draw itself before anything after
  it, which is the same picture.
- **A program starts on registers that hold nothing.** Programs read
  registers they never wrote: a compiler that has three components to
  multiply multiplies four. Of seventeen programs of one title that do
  so, one lets the fourth through to its result. The console gives such
  a read whatever the last program left on that pipeline. Here every
  register a program can reach is zero when it starts, in both
  interpreters, or the picture would change with which thread drew what;
  `VITARECOMP_GXM_UNWRITTEN=1` names the programs and says whether what
  they read reaches what they write.
- **Depth lives beside the scene.** The console keeps depth, stencil and
  the mask bit in tile memory, and touches the title's depth surface
  only when asked to load or store. The title relies on that: it points
  the display's depth surface at memory that is a colour buffer it is
  still reading from.
- **The shader path is an interpreter**, not a compiler: `src/usse.c`
  runs a program's bytecode as it is, once per vertex and once per
  pixel. It is slow and it is the definition (PLAN.md, milestones D and
  E). The same file runs a fragment program for sixty-four pixels at
  once where the program allows it, each register a row of words; a
  test holds that form to the first, bit for bit, on instructions made
  at random.

`armrecomp gxp <file.gxp>` prints what the renderer makes of a program:
its parameters, where each input arrives, and its code.

## SceGxm is 107 functions, and that number is misleading

`SceGxm` is the largest single import — over a fifth of everything. But the
count flattens together work that differs by orders of magnitude:

| Group | Count | Depth |
|---|---:|---|
| State setters (`sceGxmSet*`) | 28 | Struct writes |
| Texture accessors (`sceGxmTexture*`) | 20 | Struct field get/set |
| Shader & program | 23 | **Mixed — see below** |
| Render target / colour surface / depth-stencil | 14 | Struct init and bind |
| Memory mapping | 7 | Map guest pages GPU-visible |
| Sync / display | 6 | Fences and flip |
| Scene, draw, context, lifecycle | 9 | **The pipeline itself** |

Roughly **85 of the 107 are shallow** — they write a field in a structure the
driver later reads. They need care about layout and nothing else.

The real work is in two places.

### 1. GXP shader translation

```
sceGxmShaderPatcherCreateVertexProgram
sceGxmShaderPatcherCreateFragmentProgram
sceGxmShaderPatcherRegisterProgram
sceGxmShaderPatcherGetProgramFromId
```

These consume **GXP**, Sony's compiled shader container, and must produce
something a host GPU will execute. That is a compiler, and it is the single
largest piece of work in the project — Vita3K's equivalent took years.

### 2. GXP reflection

```
sceGxmProgramFindParameterByName        sceGxmProgramParameterGetCategory
sceGxmProgramParameterGetType           sceGxmProgramParameterGetComponentCount
sceGxmProgramParameterGetArraySize      sceGxmProgramParameterGetResourceIndex
sceGxmProgramGetDefaultUniformBufferSize
```

The game asks the driver about its own shaders — where a uniform lives, what
type it is, how big the default buffer is. This needs the GXP container parsed
but not translated, so it is bounded and mechanical once the format is mapped.

### 3. The pipeline

`sceGxmInitialize`, `sceGxmCreateContext`, `sceGxmBeginScene`, `sceGxmDraw`,
`sceGxmDrawInstanced`, `sceGxmEndScene`, `sceGxmMidSceneFlush`. The Vita is a
tile-based deferred renderer, and scene semantics are where that shows: what is
resolved when, and what a mid-scene flush guarantees.

**So the honest shape is ~15 functions of real work, one of which is a compiler
project, and ~92 that are shallow.**

## Why this is written rather than borrowed

Vita3K is **GPLv2** — confirmed from its own README, which says the choice is
*"largely dictated by external dependencies, most notably Unicorn"*. Unicorn is
a CPU emulator: precisely the component a static recompiler exists to replace.
The licence is therefore downstream of a part this project would never touch,
but that gives no relicensing room, because contributors licensed their work
into the GPLv2 whole.

There is no LGPL escape here. The approach that works for
[`xboxrecomp`](https://github.com/sp00nznet/xboxrecomp) — extracting
LGPL-2.1 components from xemu, which the LGPL expressly permits linking into MIT
code — depends on **QEMU deliberately dual-tracking its hardware model under
LGPL** so it can be embedded. xemu inherits that. Vita3K has no equivalent, and
nothing in it is offered under LGPL.

What does carry over is the rest of that arrangement, unchanged:

| Tier | Applies here? |
|---|---|
| LGPL components, linked | **No** — none exist for Vita |
| Independently implemented algorithms, credited | **Yes** |
| Functional facts: NIDs, struct layouts, enum values, register semantics | **Yes** |

That last row is not a consolation prize. Structure layouts, GXP container
format, enum values and calling conventions are functional facts rather than
expression, and they are most of what the shallow 92 functions need.

Vita3K stays what it has been throughout: a **behavioural oracle**, run as a
separate process and compared against. No code copied, linked, or vendored.

## Order of work

1. **Bind imports.** Route recompiled calls into a NID-keyed dispatch table, so
   an unimplemented import is a named trap rather than an unresolved symbol.
2. **The shallow 92.** State setters, texture accessors, memory mapping — bulk
   but not depth.
3. **`SceLibc` / `SceLibm` (96 functions).** Largely forwardable to the host C
   library.
4. **GXP reflection.** Bounded once the container format is mapped.
5. **The pipeline.** `BeginScene`/`Draw`/`EndScene` against a host graphics API.
6. **GXP shader translation.** The compiler. Last, because everything above can
   be built and tested against a stub that returns a fixed shader.

(Steps 5 and 6 went another way: a software pipeline, and an interpreter
in place of the compiler. See "SceGxm: what it took" above.)

## What a single-player bring-up does not need

`SceNet`, `SceNpMatching2`, `SceHttp`, `SceNpBasic`, `SceNpScore`, `SceNetCtl`,
`SceNpUtility`, `SceNpMessage`, `SceNpManager`, `SceNpTrophy` — **112 functions
that exist only for online play and trophies.** Deferring them takes the target
from 524 to roughly 412, and is the same call `psprecomp` made about WTF's
ad-hoc networking path.
