# Architecture

How a Vita dump becomes a native executable, and what each stage is allowed to
assume about the one before it.

## The pipeline

```
  dump / .vpk ──► armrecomp ──► eboot.bin / *.suprx      peel the container
                  (zip · dir)    (a SCE\0-wrapped ELF)    stack down to a module
                      │
                      ▼
              ┌────────────────────┐   segment table is PLAINTEXT; segments are
              │  inflate           │   zlib. QA builds need no keys at all.
              └────────────────────┘   SELF ──► a plain ELF32 ARM module
                      │
                      ▼
              ┌────────────────────┐   decode ARMv7 + Thumb-2, discover functions
              │  armrecomp         │   (following the module's export table and
              │  (tools/)          │   SCE_RELA relocations), translate to C:
              └────────────────────┘   vita_func_<addr>
                      │  generated/recomp_funcs.c
                      ▼
              ┌────────────────────┐   the runtime the generated C links against:
              │  vitarecomp (lib)  │   CPU state · memory · semantic helpers ·
              │  (src/, include/)  │   sceKernel/sceGxm HLE · dispatch
              └────────────────────┘
                      │
                      ▼   (+ a per-game host: load module, register, run, present)
              native executable — the recompiled game runs
```


## Stage 1 — the container stack

`container.c` parses SCE/SELF header, appinfo, ELF32 and program headers, and
the segment table, bounds-checking every offset it reads out of the file rather
than walking off the end of it. A retail NoNpDrm eboot is ciphertext from byte
zero; it is reported by name, not parsed into plausible garbage.

The encryption flag is cross-checked against the actual segment bytes (zlib
CMF/FLG, including the header checksum), so the report says "plain *and the
bytes agree*" — and prints a loud disagreement line if they ever do not. See
[`DECRYPT.md`](DECRYPT.md) and [`CORPUS.md`](CORPUS.md).

```
$ armrecomp info eboot.bin
size:     1680560 bytes
format:   SELF (SCE\0-wrapped ELF)

SCE header:
  version         3
  header len      0x1000
  elf filesize    3522160 bytes (decrypted)
  auth id         0x2F00000000000001
  self type       8
  sys version     0x1010000000000

ELF:
  type            ET_SCE_RELEXEC (0xFE04)
  machine         EM_ARM
  entry           0x002B36E8
  segments        5

segments:
  #   type         flags  vaddr      filesz     memsz
  0   LOAD         r-x    0x81000000 3053164    3053164
  1   LOAD         rw-    0x812EA000 7044       1204744
  2   SCE_RELA     ---    0x00000000 446924     0
  3   SCE_RELA     ---    0x00000000 4480       0
  4   SCE_VERSION  ---    0x00000000 10298      0

segment encryption:
  0   1501685    bytes  zlib      plain      (zlib header verified)
  ...
no encrypted segments, verified against zlib headers — this
module can go straight to the decoder once inflated.
```

## Stage 2 — inflate and reassemble

DEFLATE and zlib are written, not vendored (`inflate.c`, RFC 1951/1950):
canonical Huffman decoding, all three block types, overlapping back-references
handled byte-at-a-time, and the Adler-32 trailer verified. It is validated
against a fixed-Huffman *encoder* built from the spec in the test file, so the
decoder is checked against an independent implementation rather than a pasted
blob.

`armrecomp extract` takes a SELF in and writes a plain ELF32 out. Verification
is three independent numbers the file itself declares, all of which must agree;
see [`CORPUS.md`](CORPUS.md#reassembly-is-checkable-not-merely-plausible).

## Stage 3 — decode

`decode.c` handles both instruction sets with correct width determination,
control-flow extraction (targets, conditionality, interworking), IT blocks
distinguished from the NOP hints that share their encoding, and NEON/VFP
identified as a class. Coverage, class histogram, and instruction-set
determination come out of `armrecomp cover`. See
[`TRANSLATION.md`](TRANSLATION.md).

## Stage 4 — discovery

### `e_entry` does not point at code

The single most consequential thing discovery turned up. On Vita, `e_entry` names
`.sce_module_info`, not an instruction. Following it as a code address on
*Uncharted: Fight for Fortune* lands in the middle of a string table, on the
module name `cardgame` — and every function discovered from that seed would be
fiction.

The real entry is `module_start` inside that structure, and the structure also
names the export and import tables. This is psprecomp's "module_start is not
the program" in a sharper form: here the ELF entry point is not even in `.text`.

**Two address conventions, not one.** The `.sce_module_info` header fields
(`export_top`, `import_top`, `module_start`) are **segment-relative offsets**.
The pointers stored *inside* an import or export entry (`library_name`, the NID
tables, the entry tables) are **absolute virtual addresses**. Resolving one as
the other does not fault — it fails a bounds check and yields nothing, so the
symptom is a table of `(unnamed)` libraries rather than an error. The two
resolvers are kept as separate functions so the choice is explicit at every
call site.


### What discovery honestly does not know

```
$ armrecomp discover uncharted.elf
module:   cardgame   (ET_SCE_EXEC, no relocations)
seeds:
  call targets        52763
  pointer shape       22288  (heuristic; 4993 candidates rejected)
functions:            18978
  from seeds           8764
  from shape          10214  (heuristic)
bytes covered:        3424148 / 5656372  (60.5%)
  indirect sites      17970  (unresolved computed transfers)
```

Three things this does not claim:

- **54% of functions come from a heuristic.** Shape recovery requires a word
  with bit 0 set (the Thumb bit, the one genuinely reliable signal), pointing
  into `.text`, that decodes as a function prologue. An earlier version required
  only that it "decode as something", which rejected 45 of 27,281 candidates —
  0.16%, meaning it was barely filtering at all. Requiring a prologue rejects
  18%. It will still admit false positives and will miss leaf functions that
  push nothing, so the two tiers are reported separately rather than summed.
- **60.5% coverage is not 39.5% missed code.** The denominator is the whole
  executable segment, which holds literal pools and read-only data. 100% would
  indicate over-reach, not success.
- **17,970 indirect call sites are unresolved.** These are register-form `BLX`
  instructions whose destination is computed, and they are exactly what
  relocation seeding would enumerate on a 2015+ module. `ET_SCE_EXEC` has no
  relocations, so shape is all there is — which is the cost of the launch-window
  catalogue being where the exclusives are.

### Jump tables are followed, with their guards read as the compiler wrote them

A `switch` compiles to a bounds check and a `tbb`/`tbh` indexed off the PC,
with the table of offsets in the bytes after the instruction. The walk reads
the table when it knows its length, and the length comes from the guard: a
compare of the index against the last valid case. `arm_table_count` walks
back through the instructions decoded in flow before the table branch and
reads that compare, with the things a retail compiler does to it: the
branch after the compare says whether the compare bounds the index from
above (`bgt out`) or below (`ble out`, the lower end of a range whose upper
compare sits further back); the index may be adjusted after the compare
(`subs r0, #40`) or moved between registers (`adds r5, r1, #0`), so the
finder follows it through adds, subtracts and moves; the compare may be
against a register a MOVW just set; and the guard may branch *to* the table
(`cmp r0, #37; bls table`), which the walk starting at that label would
never see, so a walk started at a conditional branch's target is seeded
with the compare that guarded it. A fifth, from a tree of compares over a
dense range: the branch to the table is taken on "greater" and bounds
nothing from above (`cmp r0, #103; bgt elsewhere; cmp r0, #62; bgt
table`), and the seed is then the earlier compare of the same register,
whose own branch had already sent everything larger elsewhere. Without a
count the table is a computed transfer like any other, and traps by
address; with one that is too small, the cases beyond it trap by address
too, so a wrong reading is loud.

### Relocatable modules: the stream names the pointers

On an `ET_SCE_RELEXEC` module the `PT_SCE_RELA` segment lists every word the
linker filled with an address. `reloc.c` decodes it: ten entry formats
selected by the low four bits of the first word, of which format 0 is the
plain three-word form (the only one vita-toolchain writes) and 1 to 9 are
Sony's compact forms that carry a running offset and reuse the previous
entry's type pair. The layouts are the loader's own, cross-checked against
the toolchain's writer.

The stream is applied at the **identity base** — every segment at its own
`p_vaddr`, the address space the decoder and the literal folder already use —
and the number of bytes that changes is reported. On a module linked at those
addresses it is zero, and that is the check that the bytes discovery reads
are the bytes the program runs with. Loading elsewhere is the loader's
business at run time, not translation's.

For discovery the question is only what each relocated word points at. Into
`.text` with the Thumb bit set there are two candidates: a Thumb function,
or an odd byte of the read-only data that shares the segment — a string,
mostly, since half of all byte-packed strings start on an odd address. So a
target without a prologue whose first two bytes are both printable is
refused as text, except inside a run of four or more adjacent relocated
Thumb-tagged words, which is a pointer table and settles the matter the way
the data-segment scan does.

The text test has a blind side, here and wherever else it is applied (the
MOVW/MOVT pairs, the lone stored words): the commonest instructions of the
smallest functions are printable. `bx lr` is "pG", `ldr r0, [r0, #4]` is
"@h", `movs r1, #70` is "F!", and an accessor that is two of those, set
between two more accessors, is eight printable bytes. Text read as Thumb
is loads and stores for as long as it lasts and does not come to a return,
so bytes that return within six instructions, each one claimed and none a
branch, are a function whatever they spell.

```
$ armrecomp relocs vitashell.elf
entries:  63334  ->  63334 relocations
  format 0     63334  100.0%
   10  R_ARM_THM_CALL            27050   42.7%
    2  R_ARM_ABS32               13301   21.0%
   47  R_ARM_THM_MOVW_ABS_NC     11315   17.9%
   48  R_ARM_THM_MOVT_ABS        11338   17.9%
  into .text, Thumb bit       6735
  into .text, even           14532  (ARM code or data)

$ armrecomp discover vitashell.elf
  relocations         6646  (of 6735 Thumb-tagged pointers into .text; 89 refused as text)
    in pointer tables 4566
functions:            9195
  from seeds          4523
  from relocations    2975  (enumerated)
  from shape          1697  (heuristic)
bytes covered:        1830444 / 2561820  (71.5%)
```

Before the stream was read the same module gave 8,074 functions, 3,551 of
them from shape, and 65.3% coverage. Relocation seeds are queued after the
call harvest, so "from relocations" counts what the stream *added*: a
function reachable by `BL` is credited to the call. The 14,532 even-aligned
pointers are not seeded — nothing in an entry says whether an even `.text`
address is ARM code or a literal — and are reported so the gap is visible.


## Stage 5 — emit

### The pipeline runs end to end

```
$ armrecomp emit uncharted.elf recomp_funcs.c 100 path/to/vita-headers/db/360
  functions     100
  instructions  8827
  translated    7607  (86.18%)
  trapped       1220  (13.82%)
  literals      24  folded to constants
  import calls  49  bound to firmware

$ cmake --build build --config Release && ./gencheck
runtime up: sp=0x81800000  bad_access=0
lsl(1,32)=0 (ARM says 0, naive C gives 1)
asr(0x80000000,32)=0xFFFFFFFF (ARM says all ones)
5-3: c=1 (ARM sets carry when there is NO borrow)
3-5: c=0
linked ok
```

That is **SELF → ELF → decode → discover → emit → compile → link → run**, with
no key material anywhere in it.

### The output is meant to be read

`emit.c` translates one function at a time to readable C, with the disassembly
as comments:

```c
/* ---------------------------------------------------------------
 * vita_func_8100B910  --  4 instructions, 8 bytes
 * ------------------------------------------------------------- */
void vita_func_8100B910(void) {
    /* 8100B910  bl               */
    vita_func_8100B918();
    /* 8100B914  nop              */
    /* nop */
}
```

Anything untranslated emits a **named run-time trap**, never silence.
`VITARECOMP_TRACE=1` switches traps from abort to log-and-continue. It is a
discovery aid and is labelled as one: past the first missing piece the machine
state is wrong and every later trap is reached down a path that would not have
happened on hardware. What it is good for is one run naming many missing pieces
instead of one.

### Indirect transfers need a run-time table

Once the trace stopped reporting untranslated instructions
([`LESSONS.md`](LESSONS.md#running-it-is-what-found-the-next-bug)) it started
reporting what the program was actually trying to do: **transfer control
through a register.** `module_start` does not call the real entry point,
it passes it as a *pointer* — the same shape psprecomp documented on PSP — and
C++ virtual dispatch does the rest. The module has ~18,000 such sites.

A recompiled program has no program counter, so `BX r3` cannot be translated
statically; the destination is a value known only at run time. The answer is a
sorted address→function table and a binary search
([`dispatch.c`](../src/dispatch.c)). A hit is a real transfer; a miss is a named
trap, never a silent return — jumping somewhere untranslated has to stop,
because continuing runs the caller with the callee's work undone.

Masking bit 0 in one place matters here: every pointer to Thumb code carries it
as an instruction-set marker, and a lookup that does not mask misses *every*
entry by one.

**Import stubs live in the same table.** A module reaches firmware through
pointers as well as through `BL` — `module_start` makes its very first firmware
call that way — so a dispatcher that only knows recompiled functions reports the
C++ runtime init as a missing address. The two lists have the same shape already
(guest address → `void(void)`) and both arrive sorted, so merging them keeps the
lookup a single binary search instead of adding a second one. Where both claim
an address the stub wins: being in the import table is what that address
*means*, and the bytes discovery found there are the placeholder body the loader
was supposed to overwrite.

The search assumes a sorted table, and an unsorted one does not crash — it
silently fails to find entries that are present, which looks exactly like
missing coverage. `vita_dispatch_init` checks the order once and refuses to
search a table that fails.

### Imports are bound, so firmware calls say what they are

A module never calls firmware directly. It calls a **stub** inside its own
`.text` that the loader patches at load time, so the recompiler sees an ordinary
`BL` to an ordinary address. Left unbound, every firmware call looks like an
internal call to a function whose body is placeholder filler — and recompiling
that filler would translate it and then "return" into whatever it happened to
be.

Pairing the import NID table with the entry table gives the stub address for
each function, so calls bind at emit time:

```
$ armrecomp emit uncharted.elf recomp_funcs.c 100 path/to/vita-headers/db/360
  translated    6161  (69.80%)
  import calls    49  bound to firmware

wrote recomp_funcs_imports.c
  524 import stubs, each trapping by name
```

```c
void vita_hle_sceGxmMapFragmentUsseMemory(void);
```

and in the companion file, one default per import:

```c
/* SceRtcUser::sceRtcGetCurrentTick  stub 0x814B83D0 */
void vita_hle_sceRtcGetCurrentTick(void) { vita_trap_import(0x814B83D0, 0x23F79274); }
```

That is what makes the HLE **incrementally implementable**. The generated C
links from the first build, and an unimplemented firmware call names exactly
which function the game wanted — rather than failing to link, or silently
returning zero. Implementing one means removing its stub and providing a real
body in the link.

### Only what can run is emitted

Discovery registers every address anything branches to, which is what keeps a
`b` that leaves its function from trapping; but most such targets are blocks
their own function already carries, and each one registered is a second copy
of everything after it. On the first retail title that was four copies of the
module: 13.07 M emitted instructions for 3.33 M distinct ones, 1.12 GB of C.

So the emitter keeps only what can be entered. Discovery records why each
address was seeded (`vf_func.reasons`); the *roots* are the entry point, the
exports, and the stored pointers, whether a relocation named them, a word in
`.text` or a data segment has their shape, or a MOVW/MOVT pair in walked code
builds them. The emitter collects every function's body once, takes the
functions it calls or tail-calls (a branch that is not a label of its own,
and not an import), and emits the closure of the roots under those edges. The
dispatch table lists what is emitted, so a computed transfer to anything else
is a miss that names the address.

A stored pointer whose target has no prologue and no table of pointers around
it is as likely a record at an odd offset as a function, and it is *weak*:
walked, so its callees are known, but a root only if its whole body
translates cleanly, every instruction claimed and every branch resolved. Data
read as code never manages that for long; a prologue-less leaf that only a
pointer names always does, and `.init_array` is full of those.

The scan of stored words reads two shapes besides a run of four pointers.
One is the head of a virtual table: the C++ ABI puts the offset to the top
of the object, then a pointer to the class's type information, then the
functions, so a run of any length that follows a zero (or a small negative
number) and an aligned address inside the module is a table. It has to be
read, because the commonest table of all has two entries, a class's two
destructors, and the first of them, with nothing to do but store the
table's address, has no prologue. The other is the lone word: odd, into
`.text`, no prologue, no table, no vtable in front. That is weak evidence
too, and gets the weak treatment above, where it used to be refused
outright. Both are read only in a module without relocations. Where there
are relocations they name every stored pointer, and a module linked at
address zero is full of small odd numbers that land in its own `.text`:
on VitaShell the lone-word reading alone named 30,000 of them.

`armrecomp emit` reports the three counts: discovered, not emitted, and weak
targets promoted.

### Building the output at full scale

**The functions arrive in parts.** `armrecomp emit` writes the bodies into
`recomp_funcs_000.c`, `_001.c`, ... of at most 2,000 functions and at most
120,000 instructions each (`VITARECOMP_EMIT_SPLIT` and
`VITARECOMP_EMIT_SPLIT_INSNS` in the environment change the sizes; a split
of 0 keeps one file), every part including a generated `recomp_funcs.h`
that declares every import and every function, and the main
`recomp_funcs.c` keeps the dispatch table and the module constants.
`host/CMakeLists.txt` globs the parts. One translation unit per module was
fine at a homebrew's size and is not at a retail title's: a compiler works
on one file for as long as it takes, and a build waits for its slowest
file. Functions are not of a size, which is why the second limit: of
*Golden Abyss*'s thirteen parts of 2,000 functions one was 1.65 million
lines and took MSVC twelve minutes while everything else waited, fourteen
minutes for the host in all; in 22 parts of at most 450,000 lines it is
four, eight compilers at a time. (Eight, not one a core: each of these
takes half a gigabyte and more, and twenty-two at once ran a 32 GB machine
with other work on it out of memory.)

Two things about compiling a whole recompiled module are worth knowing before
meeting them as errors.

**It is a section-count limit, not a size limit.** Each function becomes its own
COMDAT section, and a COFF object file can index 65,535 of them. At 20,989
functions in one file this module went straight past that and MSVC reported
`C1128: number of sections exceeded object file format limit`, which names a
symptom nobody would connect to the cause. `/bigobj` fixes it; other toolchains
do not have the limit; the parts keep well under it, and `armrecomp emit` says
so in its own output when a part does not.

**The import defaults must be a static library, listed after the runtime.** One
file per import, one archive member per function. See the note in `emit.h`: a
linker pulls an archive member only for a symbol still undefined, which is what
makes implementing an import a matter of defining it rather than editing
generated code.

## Stage 6 — the runtime

### Where the runtime earns its keep

Every helper exists because the obvious C is **wrong**, not merely verbose:

- **Shifts by ≥ 32.** C leaves them undefined; ARM defines them as zero; x86
  masks the count to 5 bits, so the naive `v << 32` returns `v` *unchanged* —
  the exact opposite of the right answer. Invisible until a shift amount is
  computed rather than constant.
- **Carry on subtraction is NOT a borrow.** ARM sets it when there is *no*
  borrow. Getting it backwards inverts every unsigned comparison in the program.
- **`ADC`'s carry cannot be tested with `res < a`.** With a carry in, adding
  `0xFFFFFFFF` leaves the value unchanged while genuinely carrying, so the naive
  test silently breaks every multi-word addition.
- **Flag operands are captured before the destination is written**, because `rd`
  and `rn` are frequently the same register and the flag computation needs the
  original values.

All of it is pinned by `tests/test_runtime.c`, with the expected values derived
from the architecture's definition rather than from what the code returns.

### Variables are bound like functions

A module imports variables as well as functions: the C library's case
tables and standard streams, the C++ runtime's RTTI vtables. Where a
function import is a stub the loader redirects, a variable import is an
address the loader writes into the image, and the import table names a
block per variable listing where: MOVW/MOVT pairs that build the address,
and words that hold it. The module parser reads those blocks, the emitter
emits a host global in place of each site's placeholder (a MOVW at one
becomes `rd = vita_var__Tolotab & 0xFFFF`, a literal load becomes the
global), and the generated C publishes `vita_var_imports`: the variable's
name, the global, and the words to patch. At load the runtime builds each
variable it knows in guest memory (`src/hle_vars.c`), gives the rest a zeroed
page and a trace line, writes the address into the global and over every
listed word. An unbound variable is not a missing feature but a wrong
answer: the placeholder points at the import block, and a table read
through it returns the block's bytes.

### Threads

The recompiled code keeps its registers in globals and has no idea which
thread it is on; threads are the runtime's. `src/sched.c` gives each guest
thread a host thread and lets exactly one of them run guest code at a time.
At a switch the running thread saves the register globals into its record,
loads the next runnable thread's, hands it the baton and sleeps on its own
condition variable until the baton comes back. Nothing is ever concurrent,
so what held for one thread still holds: memory needs no ordering.

A switch happens in two kinds of place. The first is a wait the kernel
cannot satisfy: a semaphore short, a mutex held, a condition variable, a
thread's end, a delay, the vertical blank. The second is the end of a time
slice. The console has several cores, and a title's own locks and job queues
wait for another thread by reading a word until it changes, with no kernel
call for a scheduler to see; a thread that does that while the other never
runs waits for ever. So every backward branch in the generated C counts the
running thread's slice down (`vita_back_edge`, 20,000 to a slice), and when
it runs out each other runnable thread gets a slice of its own. The slice is
a count rather than a clock, so a run schedules the same way every time, and
it is counted at backward branches because every loop has one and an
exclusive pair has none between its load and its store: `LDREX`/`STREX`
still cannot be interrupted. A cycle counter checked where blocks end is how
emulators of single-core machines have always scheduled; this is that, with
the block boundaries a recompiler has.

A loop that only waits should not burn its slice first, and the emitter can
tell one exactly: loads, compares, and a conditional branch back to the top,
with every register the loop reads either untouched by it or loaded earlier
in the same turn. Nothing in such a loop writes memory and nothing carries
from one turn to the next, so only another thread can end it. There the
generated C calls `vita_spin_wait`, and another runnable thread goes at
once. `ldr r1, [r1]; cmp r1, #0; bne` reads what its last turn wrote, is a
list walk, and is left alone. When no other thread is runnable and none
sleeps on the clock, nothing can change the word: that is a deadlock, and
the runtime says which thread spins where and what the others wait on, as
it does when every thread is waiting in the kernel.

A started thread runs first, up to its first wait, which is what a worker
needs to set itself up before its maker goes on.

### Calling back into the program

Some of the library runs the program's code: `bsearch` and `qsort` take a
comparator, the display queue takes a callback. The HLE sets the argument
registers and calls `vita_dispatch` with the guest address; the generated
function runs on the same host thread and returns the way every generated
function does, so the HLE reads r0 and goes on. `qsort` sorts indices on the
host, comparing the elements where they lie, and moves the bytes once at the
end: stable, and the comparator sees the array as it was given.

## Stage 7 — the host

The generated C, the runtime and the HLE are a library. Something has to load
the module, hand the HLE a window and a gamepad, and call `module_start`.
That something is small on purpose, and `host/main.c` is the headless version
of it.

### Mounts

A guest path names a mount and a path under it: `app0:gamedata.bin`. The
host declares where each mount lives (`--mount app0=DIR`), the file HLEs
resolve through the table, and a path under no mount is reported missing.
The same prefix mounted twice is an overlay in the order given, which is how
a title's patch sits over its data on the console.

### The contract

`include/vitarecomp/host.h` is the whole interface. The HLE calls four
functions a host supplies:

```c
void     vita_host_vblank(void);    /* sceDisplayWaitVblankStart: present, poll, pace */
uint32_t vita_host_buttons(void);   /* the VITA_CTRL_* mask held right now            */
int      vita_host_touch(int *x, int *y);   /* a finger on the front panel, and where */
void     vita_host_exit(int);       /* sceKernelExitProcess: never returns             */
```

and the host asks the HLE two things: `vita_display_current()` for the
framebuffer the program last set (guest address, pitch, format, size) and
`vita_memblock_init()` to say where memory blocks come from. Nothing in
`src/hle_*.c` knows what a window is; nothing in `host/` knows what a
`SceDisplayFrameBuf` is.

### The address space

One flat region, 64 MiB at `0x81000000`, which is where vita-elf-create links
and where the console loads a user module. The module's segments go at their
link addresses; above them an arena for `sceKernelAllocMemBlock`, a heap for
`SceLibc`, and a stack at the top. A module that does not fit below the arena
is refused with its range named rather than overlapped.

### Headless, scripted, hashed

```
vita_host fbdemo.elf --frames 240 --press RIGHT@10-70 --press DOWN@30-60 \
                     --press START@200 --dump frame.ppm@120
```

Input is a list of buttons held between two vertical blanks, and of
touches the same way (`--touch 480,340@900-910`: a finger at that pixel of
the display for ten vertical blanks); a frame is
written at the vblank asked for, or at exit. That makes a run reproducible
to the byte, which is what lets `tools/e2e.sh` compare a SHA-256 of the frame
and lets CI prove the chain on both platforms without a display. The sample's
frame — a gradient whose blue channel is the frame counter, and a square
where sixty frames of RIGHT and thirty of DOWN put it — is the fixture.

`--window` is the same four functions with a window behind them
(`host/window_win32.c`, GDI and nothing else): the frame shown at each
vertical blank, the keyboard added to the scripted buttons, and the left
mouse button as a finger, wherever on the picture it is held. The core
keeps its no-dependency promise.

### Bring-up: seeing what the program did

A recompiled program that is wrong is wrong quietly: valid C, no trap, a
pixel in the wrong place or a pointer with a plausible value. The runtime
carries six ways of looking, all off by default and free when off:

| Tool | Switch | What it shows |
|---|---|---|
| Function-entry trace | `vita_host --trace-fns`, `--syms module.sym` | every function entered, by name when an `nm -n` listing is given |
| Instruction trace | C emitted with `VITARECOMP_EMIT_PC=1`; `--trace-insns LO-HI` | registers and flags at each instruction in an address window |
| Bad-access reports | `VITARECOMP_TRACE=1` | the first bad guest accesses, with the PC when the C records it |
| Write watchpoint | `VITARECOMP_WATCH=<hex>` | every store that covers an address, with value and PC |
| Sampling profile | `VITARECOMP_PROFILE=N`, C emitted with `VITARECOMP_EMIT_PC=1` | every N seconds: the share of samples per guest thread and per function, and the instruction each function was most often at |
| Discovery, one address | `VITARECOMP_WHY=<hex>` on `armrecomp discover` or `emit` | what discovery made of that address each time it met it: walked, decoded inside another function, named by a stored word, refused and why |

The order they are reached for is the order above: the function trace says
which function went wrong, the instruction trace says where in it, the
watchpoint says who touched the memory it read. The nine newlib bugs in
LESSONS.md were found that way, none of them by reading the disassembly
first. The profile is for the other failure, a program that says nothing:
the trace is as silent while it computes as while it spins, and the samples
tell the two apart in one period. And a dispatch miss names an address the
output has no function for; the last row is how to ask why.

### Seeing what was drawn

A renderer that is wrong is wrong in a picture, and a picture does not say
which of two hundred draws spoiled it. Five more ways of looking, for
frames:

| Tool | Switch | What it shows |
|---|---|---|
| The frame census | `VITARECOMP_TRACE=1` | at each flip whose shape differs from the last: scenes, draws, triangles, pixels, and what was not drawn and why (lines or points, a program that could not be run, a program run that stopped, a sample of a unit with nothing bound); each kind of scene once, with its target, formats and what its depth surface loads and stores |
| One frame's draws | `VITARECOMP_GXM_FRAME=<n>` | every draw of frame n: its programs by name, its geometry, how many pixels came of it, the depth, stencil, cull, blend and viewport state |
| Textures | `VITARECOMP_GXM_TEXTURES=<dir>` | each texture a draw samples, once, as an image, with its layout on stderr; a format not decoded yet as its bytes |
| One frame's scenes | `VITARECOMP_GXM_SCENES=<dir>`, with the frame switch above | what each scene of that frame left in its colour surface, as an image: the geometry buffer, the lit scene, each pass of the chain that ends on the display |
| Programs | `VITARECOMP_GXP_DUMP=<dir>`, then `armrecomp gxp <file>` | the shader programs a title registers, and each one's tables and code as text |

Two switches are for time, not for looking. `VITARECOMP_GXM_PROFILE=<n>`
prints, every n frames, what a frame cost by stage: the drawing thread's
own time in setting a draw up, shading its vertices, setting up its
triangles and filling them, and, summed over the threads that fill, the
fragment programs' inputs, their texture samples, the programs, the tests
and the store, with how many pixels went through a program in rows and how
many one at a time. `VITARECOMP_GXM_SKIP=<first>-<last>`
counts the draws of those frames and draws none of them, which takes a
title past a screen it holds for a fixed time, a health warning over a
costly scene, in seconds. What those frames would have left in any
surface is missing afterwards.

And the frames themselves: `vita_host --window` shows them as they are
flipped, `--dump out.ppm@N` writes one, `--dump-dir DIR@N` one every N
vertical blanks.

None of that compares from one run to the next while the title reads the
host's clock: its animations are at a different place in every run, and it
moves between screens after so many seconds however few frames it drew in
them. `vita_host --virtual-clock` makes time a count of what the program
did (`src/clock.c`): a sixtieth of a second at each vertical blank, the
length of a wait when every thread is waiting, a microsecond for each
reading. Two runs then write the same frames, byte for byte, and a change
to the renderer can be judged by a hash. That is how the renderer was put
on several threads (`VITARECOMP_THREADS` says how many; one is the
renderer as it was): twenty frames of a run, the same before and after.
The same twenty stood behind keeping compressed textures decoded and
behind running fragment programs for a row of pixels at a time; for the
last there is a switch that stays, `VITARECOMP_GXM_SCALAR=1`, which shades
every pixel by itself, so the two can be compared on any frame of any
title.

The first scrambled picture went through all four in that order. The
census said every program ran. The textures were right. The geometry
buffer, dumped as the texture the light pass reads, was right. The draw
list had the answer in one column: a pass drawn with the depth test
"equal" kept half the pixels of the pass that had laid that depth down
(LESSONS.md).

