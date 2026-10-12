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

### ARM, for what is written by hand

Compilers for this console emit Thumb. The ARM a module holds is its
import stubs, which are bound and never translated, and whatever somebody
wrote in assembly: in *Golden Abyss* 36 routines, 8,500 instructions,
Advanced SIMD from end to end with a few dozen integer instructions around
each to fetch pointers, count a loop and return. Its first level calls
them 300,000 times in its first fourteen seconds.

Two things kept this from being a second decoder and a second emitter.
An ARM Advanced SIMD or VFP instruction is its Thumb encoding with the
leading bits moved (`1111 001U` for `111U 1111`, `1111 0100` for
`1111 1001`, and the coprocessor forms with a condition where Thumb has
`1110`), so it is re-encoded and handed to the decoders that already read
those: nothing new. And the integer instructions are decoded into the
same fields the Thumb decoder fills, so the emitter translates them as it
does their Thumb counterparts; the two things it had to learn are that the
PC reads as the instruction's address plus 8, not 4, and that a return
under a condition does not end the function. Every ARM instruction
carries a condition where Thumb needs an IT block; the condition is filled
in as an IT block's would be and the guard that wraps an IT block's
instructions wraps these. What the integer decoder does not read (a
register shifted by a register as an operand, the two load-multiple
directions Thumb lacks, a subtracted index) it leaves without an
operation, to trap by name.

Which calls into ARM to believe is the harder half (below, in discovery).

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
whose own branch had already sent everything larger elsewhere. And a
sixth, from a state machine with its table at the top of a loop: the next
state is checked at the bottom and the table reached by a plain jump
(`cmp r0, #31; bhi out; b table`), so an unconditional branch carries to
its target the bound the flow before it had settled. Without a
count the table is a computed transfer like any other, and traps by
address; with one that is too small, the cases beyond it trap by address
too, so a wrong reading is loud.

### ARM targets are believed when they read as ARM

A `blx` to an address is the one way into ARM code, and data decoded as
Thumb is full of things that look like `blx`: of the 2,129 distinct
targets in *Golden Abyss*'s text, 36 are routines, some are import
stubs, and most are nothing. The ARM encoding is dense, so
"does it decode" refuses almost none of them, and a walk begun in data
does not stop on its own. What separates written code from data is the
condition field: real ARM runs unconditionally nine words in ten, and
data, or Thumb code read four bytes at a time, spreads its top nibble over
all sixteen values. The rule is measured, not guessed: no word in the
first 96 (or up to a return) that written code does not have, four in five
of them unconditional, and either a return or 48 words of it. Every one of
the 36 passes with room, and what else passes is a handful of one-word
returns that nothing calls. `VITARECOMP_WHY=<address>` says which part of
the rule refused a target.

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

A host that keeps the display's time has to wait the same way.
`vita_host_vblank` is called by the thread that waits for the vertical
blank, and that thread has the baton: a host that sleeps there until the
next sixtieth of a second has put every thread of the program to sleep
with it. `host/host.c` waits through the scheduler (`vita_sched_delay`),
so the others run meanwhile, and sleeps out only the last two
milliseconds itself, to be on time; with the clock virtual it sleeps as
before, since a wait there takes no time and would move the clock. The
first title loads on other threads while its main thread turns a coin
thirty times a second, and it had been loading in the moments between
two sleeps: a level that took a hundred seconds to arrive takes
fifty-five, and what is left of that is not the title's (LESSONS.md).

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
That something is small on purpose, and `host/host.c` is it: headless
unless told otherwise.

It is not a program by itself. `vita_host_main(argc, argv)`
(`include/vitarecomp/host_main.h`) is its whole face, and `vita_host` is
`host/main.c`, five lines that pass the command line on. A game's program
is the same host behind a `main()` of its own: one that knows where the
title's files are, reads the player's settings, sets the title up the first
time it is run, and then makes up the arguments a person would have typed,
the module, the mounts, `--window`, `--title`. In CMake that is

```cmake
add_subdirectory(vitarecomp)
vitarecomp_add_game(my-game RECOMP_DIR <where armrecomp emit wrote> SOURCES src/main.c ...)
```

which is how `vita_host` itself is made (`host/CMakeLists.txt`). Whatever
the game's `main()` does not know of its own command line it hands on, so
the scripted input, the frame dumps and every trace below work on the
game's executable as they do on `vita_host`. The host is linked as objects
and not as an archive, because the runtime calls the host and the host the
runtime, and two archives that need each other link in no single order.

What a game's setup needs of the recompiler is a library too:
`vitacontainer` (`tools/armrecomp/container.c`, `extract.c`, `inflate.c`)
is the SELF parser and `vc_extract()`, the plain ELF out of a decrypted
SELF, so the player's module can be taken from where an emulator left it
without the recompiler being shipped beside the game.

### Mounts

A guest path names a mount and a path under it: `app0:gamedata.bin`. The
host declares where each mount lives (`--mount app0=DIR`), the file HLEs
resolve through the table, and a path under no mount is reported missing.
The same prefix mounted twice is an overlay in the order given, which is how
a title's patch sits over its data on the console.

### The contract

`include/vitarecomp/host.h` is the whole interface. The HLE calls five
functions a host supplies:

```c
void     vita_host_vblank(void);    /* sceDisplayWaitVblankStart: present, poll, pace */
uint32_t vita_host_buttons(void);   /* the VITA_CTRL_* mask held right now            */
void     vita_host_sticks(uint8_t xy[4]);   /* the two sticks, 128 at rest            */
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
touches and stick positions the same way (`--touch 480,340@900-910`: a
finger at that pixel of the display for ten vertical blanks;
`--stick L,0,-100@950-1100`: the left stick pushed forward); a frame is
written at the vblank asked for, or at exit. That makes a run reproducible
to the byte, which is what lets `tools/e2e.sh` compare a SHA-256 of the frame
and lets CI prove the chain on both platforms without a display. The sample's
frame — a gradient whose blue channel is the frame counter, and a square
where sixty frames of RIGHT and thirty of DOWN put it — is the fixture.

`--window` is the same five functions with a window behind them
(`host/window_win32.c`, GDI and nothing else): the frame shown at each
vertical blank, stretched from the program's memory, or, when a graphics
card drew it, shown by the card that holds it (`vita_display_show`: the
host hands over the window and paints nothing in it afterwards), the
keyboard added to the scripted buttons and sticks
(arrows for the pad, W A S D and I J K L for the sticks, X C Z V for
cross, circle, square and triangle, Q and E for the shoulders, Enter for
START, Shift for SELECT), and the left mouse button as a finger, wherever
on the picture it is held. The picture keeps the frame's shape in a window
of any other, with black beside it; `--window-size WxH` says how large it
starts, in the screen's own pixels (the program tells the system it knows
how dense the screen is, or the system would stretch the finished picture
and blur it), `--fullscreen`, F11 or Alt and Enter put the window over the whole
screen, `--title` names it, and its icon is the program's own when it has
one. A controller is read with the keyboard: the first pad XInput reports,
its buttons added to the keys held and its sticks the sticks wherever no
key holds one, a trigger counted as the shoulder button above it because
the console has one a side. The library is found by name when it is first
wanted, so the host links nothing for it; and like the keys, a pad is
listened to only while the window has the focus. The core keeps its
no-dependency promise.

### Bring-up: seeing what the program did

A recompiled program that is wrong is wrong quietly: valid C, no trap, a
pixel in the wrong place or a pointer with a plausible value. The runtime
carries eight ways of looking, all off by default and free when off:

| Tool | Switch | What it shows |
|---|---|---|
| Function-entry trace | `vita_host --trace-fns`, `--syms module.sym` | every function entered, by name when an `nm -n` listing is given |
| Instruction trace | C emitted with `VITARECOMP_EMIT_PC=1`; `--trace-insns LO-HI` | registers and flags at each instruction in an address window |
| Bad-access reports | `VITARECOMP_TRACE=1` | the first bad guest accesses, with the PC when the C records it |
| Write watchpoint | `VITARECOMP_WATCH=<hex>` | every store that covers an address, with value and PC |
| Sampling profile | `VITARECOMP_PROFILE=N`, C emitted with `VITARECOMP_EMIT_PC=1` | every N seconds: the share of samples per guest thread and per function, and the instruction each function was most often at |
| Threads at the end | `VITARECOMP_TRACE=1` | when the run ends, every thread on a line: running, ready, never started, or waiting, on what and for how much longer, and the address it was last at |
| Which functions ran | `VITARECOMP_COVERAGE=FROM-TO@FILE` | every function entered between two vertical blanks, with how often, one a line in address order: made to be compared with the same list from a run that differs in one thing |
| Discovery, one address | `VITARECOMP_WHY=<hex>` on `armrecomp discover` or `emit` | what discovery made of that address each time it met it: walked, decoded inside another function, named by a stored word, refused and why |

The order they are reached for is the order above: the function trace says
which function went wrong, the instruction trace says where in it, the
watchpoint says who touched the memory it read. The nine newlib bugs in
LESSONS.md were found that way, none of them by reading the disassembly
first. The profile is for the other failure, a program that says nothing:
the trace is as silent while it computes as while it spins, and the samples
tell the two apart in one period. A program that runs and yet does not
get on is a third: nothing is wrong where it is looked at, and something
it is waiting for never comes. The list of threads is for that one. A
title that drew its level and kept the screen black had ten threads, and
one line read "not started". A fourth is a program that is given something
and does nothing with it, a finger on a button that stays as it was. The
call log of such a run is millions of lines and so is the log of the run
without the finger; the functions entered in one and not the other are a
few dozen, and they are where the finger went: `comm -13` on two lists of
the row above. And a dispatch miss names an address the output has no
function for; the last row is how to ask why.

### Seeing what was drawn

A renderer that is wrong is wrong in a picture, and a picture does not say
which of two hundred draws spoiled it. Six more ways of looking, for
frames:

| Tool | Switch | What it shows |
|---|---|---|
| The frame census | `VITARECOMP_TRACE=1` | at each flip whose shape differs from the last: scenes, draws, triangles, pixels, and what was not drawn and why (lines or points, a program that could not be run, a program run that stopped, a sample of a unit with nothing bound); each kind of scene once, with its target, formats and what its depth surface loads and stores |
| One frame's draws | `VITARECOMP_GXM_FRAME=<n>` | every draw of frame n: its programs by name, its geometry, how many pixels came of it, the depth, stencil, cull, blend and viewport state; where its vertices, indices and uniforms are in the title's memory, and where its first vertices came out of the vertex program; for a draw whose vertices came out as no number, the vertex that went in and the matrix it met |
| Textures | `VITARECOMP_GXM_TEXTURES=<dir>` | each texture a draw samples, once, as an image, with its layout on stderr; a format not decoded yet as its bytes |
| One frame's scenes | `VITARECOMP_GXM_SCENES=<dir>`, with the frame switch above | what each scene of that frame left in its colour surface, as an image: the geometry buffer, the lit scene, each pass of the chain that ends on the display |
| Programs | `VITARECOMP_GXP_DUMP=<dir>`, then `armrecomp gxp <file>` | the shader programs a title registers, and each one's tables and code as text |
| Registers read unwritten | `VITARECOMP_GXM_UNWRITTEN=1` | each fragment program that reads a register nothing wrote, with the instruction and the register, and whether any of it reaches the program's result or its verdict on a pixel: every row of pixels is run twice on two fills and the two compared. The frames drawn in this mode are not pictures |

Three switches are for time, not for looking. `VITARECOMP_GXM_PROFILE=<n>`
prints, every n frames, what a frame cost by stage: what it took from one
frame to the next, all told, which has the title's own work in it; the
drawing thread's own time in making the draws and in the three steps of
drawing a scene's list of them (vertices and triangles, sorting into
tiles, the fill); and, summed over the threads that take part, the
vertices and triangles again and, in the fill, the fragment programs'
inputs, their texture samples, the programs, the tests and the store,
with how many pixels a triangle covered, how many went through a program
in rows and how many one at a time; and on a second line what drawing cost
by the size of the surface drawn to, with how many lists of draws and how
many draws went to each, which says whether a frame's time is in its
picture or in the small passes around it; and after that the fragment
programs the fill's time went to, the dearest first, each by the name a
dump of it has (`VITARECOMP_GXP_DUMP`, `armrecomp gxp`), with the pixels
it shaded, the pixels its triangles covered, and what a shaded pixel
cost. A program that covers a third of a million pixels to shade one
shows there, and so does the one whose four samples are a seventh of the
frame. `VITARECOMP_GXM_SKIP=<first>-<last>`
counts the draws of those frames and draws none of them, which takes a
title past a screen it holds for a fixed time, a health warning over a
costly scene, in seconds. What those frames would have left in any
surface is missing afterwards. And `VITARECOMP_HOST_PROFILE=<ms>[@FROM-TO]`
on `vita_host` is the stage profile's finer half: a thread stops every
other thread each few milliseconds, between two vertical blanks if they are
given, notes where it was, and at the end prints the functions of the host
the samples fell in and the lines, waiting threads under the name of the
system call they wait in. The lines want the runtime built with its debug
information, `-DVITARECOMP_SYMBOLS=ON`, which changes nothing in the code
and leaves the generated C without. It is the Windows host's; elsewhere
the system's own profiler reads the same build.

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
on several threads (`VITARECOMP_THREADS` says how many; left alone it is
three quarters of the host's processors): twenty frames of a run, the same
before and after. The same twenty stood behind keeping compressed textures
decoded and behind running fragment programs for a row of pixels at a
time; for the last there is a switch that stays, `VITARECOMP_GXM_SCALAR=1`,
which shades every pixel by itself, so the two can be compared on any
frame of any title. Drawing a scene's draws together at its end, a tile of
the target to a thread, has its switch too: `VITARECOMP_GXM_AT_ONCE=1`
draws each draw as it is made. One thread, thirty, a pixel at a time, a
draw at a time: the same frame, and seventy-five frames spread over two
minutes of a title were the same as before the change. A run compared
that way has to start where the other started: a title that finds a save
from the last run goes another way through its menus, and its frames are
then some other frames.

The first scrambled picture went through all four in that order. The
census said every program ran. The textures were right. The geometry
buffer, dumped as the texture the light pass reads, was right. The draw
list had the answer in one column: a pass drawn with the depth test
"equal" kept half the pixels of the pass that had laid that depth down
(LESSONS.md).

### The same draws on a graphics card

The renderer above is the definition, and a frame of play takes it a
twelfth of a second. `VITARECOMP_GXM_GPU=1` hands each scene's list of
draws to the host's graphics card instead; `VITARECOMP_GXM_GPU=check`
draws every list both ways and says where the two results differ. Unset,
none of it runs. The card is reached through Direct3D 11 on Windows and
through OpenGL on the Nintendo Switch ("The same on OpenGL", below);
elsewhere the switch finds no card and every list stays the processor's.

Three files, each knowing less of the console than the one before.

- `src/gxm_gpu.c` takes a list as `gxm_draw.c` keeps it
  (`src/gxm_draw_list.h`) and decides whether the card can draw it as
  the processor would. If it can, the card draws it, to surfaces it
  keeps (below). A list it cannot do is left to the processor whole,
  and the reason is said once: a draw of several instances, a draw
  whose two faces are tested differently, a region drawn outside of, a
  mask that turns pixels away, a stencil test beside sixteen bits of
  depth, a program that does not translate.
- `src/gxp_hlsl.c` writes a program of the title's as HLSL: the
  interpreter's steps as text, a block to an instruction, the registers
  still banks of 32-bit words, so that what a program does to its bits
  it does there too. `armrecomp hlsl <file.gxp>` prints the translation
  of a dumped program. The same file writes the program as GLSL
  (`armrecomp glsl`): the statements of an instruction are spelt once,
  in HLSL's words, and the GLSL begins by saying what those words are in
  its own.
- `src/gpu.h` is a card as far as the rest needs one: shaders as text,
  textures as rows of texels, surfaces it draws to and keeps, a draw as
  buffers and state; and which of the two languages its shaders are
  written in.
  `src/gpu_d3d11.c` and `src/gpu_gl.c` are the two implementations,
  `src/gpu_none.c` what is linked where there is none.

A card left to itself does several things another way than the
interpreter, and wherever the check found one the translation now does
the interpreter's thing in so many words:

- **Halves.** A float is made a half by rounding to the nearest. The
  card's own conversion cuts the bits off, and a texture coordinate a
  step short samples somewhere else: `h16()` in the shader's prelude
  takes the card's answer and puts it right from its neighbour.
- **Corners.** A vertex lands on a sixteenth of a pixel, where
  `src/raster.c` puts it. A card's steps are sixteen times finer, and
  everything interpolated across a triangle follows its corners.
- **The texel a point falls in.** A title that draws a target half a
  texture's size samples exactly on the line between two texels, and
  cards differ on which side of the line that is. A texture read with no
  filter has its texel chosen by the program's own arithmetic
  (`texel_at`), on 256 steps to a texel, and `src/gxm_tex.c` chooses on
  the same steps; a gathering sample likewise (`gather_at`).
- **Cubes.** A cube is six flat faces: the face a direction points at is
  worked out by the program (`cube_at`, which is `gxm_tex.c`'s
  `cube_face`) and a sample near the edge of a face is of that face.
  A card blends across the edge.
- **A program that blends for itself** is handed the target itself, to
  read and write a pixel at a time in the order its triangles come
  (`gpu_ordered`): two triangles of one draw that lie over each other
  each find what the last one left. A card without the means, or a
  program the fixed blend follows or that can discard, gets the target
  as it stood before the draw.
- **Depth.** A surface of twenty-four bits is one on the card too, the
  same whole numbers, so a draw that passes only where the depth equals
  what an earlier draw left finds it equal.
- **Textures** are known to be unchanged by a hash of all their bytes.
  A title writes into its textures where it pleases, and a scene's depth
  is a later scene's texture.

What the check calls a difference is measured in what rounding alone can
leave: a step and a half of the least a pixel's field can tell apart,
ten bits of a float in a surface of three small floats, a byte's step
elsewhere. A list differs when more than a two-hundredth of the pixels
it drew are over four times that apart, or a third are apart at all;
depth and stencil are held against the surface. While lists are checked
each begins on the card from what the processor has, except its depth,
which is the card's own from the list before: the processor's arithmetic
never equals the card's to the last bit, and "equal" has to mean the
card's.

One difference is left alone. Which level of a texture a sample falls on
comes from how fast the coordinate changes from pixel to pixel, which
the processor has exactly and a card measures from neighbouring pixels;
where a surface is seen edge on they choose differently and both are
right. `VITARECOMP_GXM_ONE_LEVEL=1` reads every texture from its first
level, in both renderers, and with it the two agree list for list
through the first title's front end and its first minute of play.

| Tool | Switch | What it shows |
|---|---|---|
| Both ways, compared | `VITARECOMP_GXM_GPU=check` | each list that differs: how many pixels, how far, where the worst is and what both hold there; each draw's programs, the pixels it drew on the card and on the processor, its tests; and at the end how many lists were checked, differed, were not the card's |
| The lists that differ | `VITARECOMP_GXM_GPU_DUMP=<dir>`, with the check | the first twenty-four as pictures: both colours, both depths, and where the colours are apart, grey within rounding, yellow to four times that, red beyond |
| Their numbers | `VITARECOMP_GXM_GPU_DUMP_RAW=1`, with the dump | both results as the card holds them, the first draw's textures as floats, its secondary attributes: for working a pixel out by hand |
| A program part-way | `VITARECOMP_GXM_PROBE=<name>:<count>:<from>:<to>` | the program of that name runs only its first `count` instructions and then moves the register `from` (a half of it with `.x` to `.w`) into `to`: with `to` its result register, the check compares what `from` held there, and so finds the first instruction two renderers part at |
| One level | `VITARECOMP_GXM_ONE_LEVEL=1` | every texture read from its first level, in both renderers |
| The shaders | `VITARECOMP_GXM_GPU_SHADERS=<dir>` | each program's text as it was compiled, HLSL or GLSL as the card reads |
| What travels | `VITARECOMP_GXM_GPU_LOG=1` | each texture made from memory: size, levels, format, filters; each surface filled from memory or sent back to it |
| The long way round | `VITARECOMP_GXM_GPU_KEEP=0` | every surface back in memory after every list |
| Another card | `VITARECOMP_GPU_WARP=1`, `VITARECOMP_GPU_DEBUG=1` | the system's own rasteriser in place of the card; Direct3D's checking layer. Under OpenGL the second is the library's own complaints as they are made and, at the end, how often the card's context changed threads and how long that took |
| Programs kept | `VITARECOMP_GPU_CACHE=<dir>` | compiled programs kept in that directory from one run to the next; with the log, each program compiled and how long it took |
| A larger picture | `VITARECOMP_GXM_GPU_SCALE=<n>` | every surface the card keeps `n` times as wide and as high as the title made it, `n` from 2 to 8 |

`VITARECOMP_GXM_AT_ONCE=1` makes a list of every draw, which is how a
difference in a scene of two hundred draws is brought down to one.

#### The same on OpenGL

`src/gpu_gl.c` is `gpu.h` again, for a host whose card is reached no
other way: the Nintendo Switch, through Mesa and EGL. It builds on
Windows too (`-DVITARECOMP_GPU_OPENGL=ON`, in place of Direct3D 11), which
is where it is worked on and where the check above is run against it: the
same lists, the same count of them differing, the same frames byte for
byte as the other card's.

- **Which OpenGL.** The desktop's, 4.3 core, though the console's library
  offers the one for small devices as well. That one has no count of the
  pixels a draw passed, no plane of a program's own to cut by, no depth
  held to the near plane, and a language that converts nothing by itself,
  which the translator's text leans on.
- **Rows.** OpenGL counts a picture's rows from the bottom; everything
  above `gpu.h` counts from the top. A texture's first row is the top row
  here, the card is told to read clip space as Direct3D does
  (`glClipControl`), and only the window is turned over as it is shown.
- **The language.** `gxp_hlsl.c` writes either. What an instruction
  becomes is spelt once; what differs is what goes round it, where the
  languages have different things and not different names: how a texture
  is declared and sampled, how a program is given its inputs, and that
  GLSL's variables begin holding nothing in particular. Each of a draw's
  two programs is linked alone and a pipeline names the pair.
- **No divide.** The console's card has no instruction that divides whole
  numbers; its compiler calls a routine for `/` and `%`, and an emulator
  of the console does not follow the call. Where the HLSL divides, the
  GLSL asks a function that does it by floating point and puts the answer
  right (`over`, `left_of`).
- **Threads.** A context belongs to one thread, and the title's threads
  take turns (`sched.h`). The context goes with the baton: the thread
  that holds it lets it go when it gives way (`vita_sched_on_handover`)
  and the next call takes it up. On the console a program is compiled on
  a thread of its own with a large stack, the context lent to it: the
  compiler is deep and the title's threads have 128 KiB.
- **The screen.** The console's window is the card's or the host's own
  frame buffer's and never both (`host/window_switch.c`): the frame
  buffer is made when the first frame is drawn into it, and the card
  takes the screen when it first shows one.
- **What it has not.** A target read and written in the order the
  triangles come (`gpu_ordered` says no, and a program that blends for
  itself reads the target as it stood before its draw). And on the
  console's Mesa a compiled program cannot be handed back to be kept, so
  `VITARECOMP_GPU_CACHE` keeps nothing there and each run compiles again.

#### Surfaces stay on the card

A frame of play is twenty lists to a dozen surfaces, and most of what a
list samples is what an earlier list drew. Carried through the title's
memory between them, converted both ways on the processor, that was 177
ms a frame, twice the processor's own renderer. So a surface a list has
drawn stays where it was drawn, and for each of the title's surfaces
`gxm_gpu.c` knows which holds what the surface is: the card, or memory,
and if memory, whether the card's copy still agrees, by a hash of the
bytes.

- **A list's target** is found on the card if the card holds it, and
  filled from memory only if memory is the newer.
- **A texture whose bytes lie in a surface the card holds** is made
  from that surface on the card, by a program written for the reading
  (`view_text`): for a texel of the texture, which bytes of the
  surface's memory it would be, which pixel of the card's surface
  holds them, the words those were, and the texel's fields out of the
  words as `pixfmt.c` reads them. A geometry buffer of two words a
  pixel read as a texture twice as wide of four bytes a texel, and
  again from four bytes further on; depth read as a number; a surface
  the card holds as floats read in the surface's own form, as that form
  would have kept it. A texture is a reading of the surface it begins
  in and of no other, whatever its layout rounds its size up to.
- **A scene's depth** is where the scene's lists left it. A scene that
  ends on the card with a depth surface to keep leaves it there as that
  surface's; one that begins by reading a surface the card holds starts
  from it there, from a copy if the scene is not to change it, from its
  corner if the scene's surface is the corner of a larger one (which
  the first title does: a half-size pass tested against the top left of
  the full-size depth).
- **A surface that is the corner of another's memory**, a half-size
  picture drawn into a full-size surface's rows, is drawn through the
  larger: fetched from its corner before the list and put back after,
  on the card.
- **Bytes go back to memory** when something off the card wants them:
  a list the processor draws, the display (`vita_display_current`), a
  scene that reads a surface the card cannot hand over, a texture no
  program can be written for (each reason said once), the same memory
  about to be drawn as a surface of another form. And at once for a
  surface of a few pixels: the first title reads the brightness of its
  frame off a surface 32 pixels square, in its own code, and nothing
  says when.

- **Two surfaces in one piece of memory** are both the card's, each
  with the number of the list that drew it last. The first title draws
  its geometry buffer, eight bytes a pixel, and later a picture of four
  bytes a pixel into the first half of the same bytes. Neither is sent
  to memory to make way for the other: if memory is ever asked for they
  go in the order they were drawn, and a texture that lies wholly in
  the one drawn last is a reading of that one.
- **A list whose first draw covers its target** fetches nothing of what
  the target held, and a surface that lay wholly within the target's
  memory is gone unseen. `gxm_draw_covers` says so of a draw: one or two
  triangles over the whole target, no test or mask that could spare a
  pixel, a program that neither reads the pixel nor can discard it,
  every channel written. The corners are worked out by the interpreter;
  a title clears a surface this way, having no other.

Two more things are kept, for the same reason: they were made last
frame from the same bytes.

- **A draw's geometry.** What a draw's vertices come to, each attribute
  read from its stream and packed as the program's registers want it,
  is kept on the card and found again by where the draw's indices and
  streams lie and how they are read. A draw whose bytes change three
  times running, particles written anew each frame, is made each time
  and not kept.
- **Bytes not written are not read again.** A texture from memory and a
  draw's geometry are known by a hash of their bytes, and hashing thirty
  megabytes a frame to learn that nothing changed was three
  milliseconds. The program's memory keeps count of the pages written
  (`vita_mem_moment`, `vita_mem_written` in `mem.h`; Windows does the
  counting, for memory allocated for it), and bytes on pages not
  written since they were last read are not looked at. Where nothing
  keeps count the answer is "perhaps", and the bytes are hashed as
  before. The system is asked about the pages somebody asks about,
  when they ask, sixty-four kilobytes at a time, and about a page once
  a list: asked about all of the program's memory at the start of
  every list it took half a millisecond each time, because every page
  it reports is made ready to be caught again and a program's stack
  and heap are written all the time. Two hundred and sixty askings a
  frame find seventeen pages written and take 0.6 ms between them.

And the frame itself does not come back to be looked at. A host with a
window asks for the display's frame to be shown in it
(`vita_display_show`, `gxm_gpu_show`, `gpu_show`): when the card holds
the surface those bytes are, newer than they are, it draws that surface
into the window and nothing goes to memory; when it does not, a film the
processor decoded or a list the processor drew, the bytes are sent to the
card and shown the same way, once for each time they are written. The
window is the card's from the first frame it shows, because the system
lets nothing else draw in such a window. A frame is shown when it is
another frame than the last or the window another size, and not
otherwise: showing waits for the card to have drawn, and a title that
makes thirty frames a second was shown sixty times, half of them for
nothing and at several milliseconds a frame.

The picture is as large as fits at the frame's shape, in the middle. A
pixel of the window that lies wholly within one pixel of the frame is
that pixel; one that lies across two is as much of each as it covers. So
at twice or three times the frame's size every pixel of the frame is a
whole block of the window's, to the byte, and at one and a half times
its edges are blended and its middle is not.

One thing is kept from one run to the next: a compiled program. A
program is translated and compiled when a draw first uses it, and the
system's compiler takes a tenth of a second to a second over each, so
the first frames of a level waited ten seconds and more with the title's
loading screen standing still. What the compiler makes depends on the
text it is given, the kind of program and the way it is asked, and on
nothing else: `gpu_d3d11.c` keeps it by a hash of those in one file,
`programs.d3d11` in the directory `VITARECOMP_GPU_CACHE` names, and a
text met again is not compiled. Nothing is kept when no directory is
named; a game's program names one of its own. The file is records added
at the end in one write each and never changed, so two runs may share
it, and reading it passes over whatever is not a whole record whose sum
is right: a file cut short, or spoilt in the middle, costs the records
that were. A change to the translation changes the text, so nothing kept
is ever stale. The profile's line for the card says how many programs a
stretch made ready and how many of those it had to compile.

**Larger than the title drew.** With `VITARECOMP_GXM_GPU_SCALE=n` every
surface the card keeps is `n` times as wide and as high as the title
made it, and a list is drawn into it at that size. Almost nothing has to
know. A title's viewports, its textures' coordinates and its programs
are in fractions of a surface; a program that reads the pixel it writes
reads the one it is at; a surface read as a texture is made the larger
size too, each texel of the title's the `n` by `n` the card has for it
(`view_text`). What is in the title's own units is put right where it
meets the card: a clip rectangle, the count of pixels a draw passed,
and a surface's rows on their way from memory, each pixel repeated, or
back to it, each pixel the mean of what it has become (or the middle
one, for a form that is not numbers to be averaged). The window shows
the surface at whatever size the window is: where it is the smaller, a
pixel of it is the mean of the surface's it covers.

One thing does have to know. The first title keeps two words a pixel in
its geometry buffer and reads them back as a texture twice as wide, a
word a texel. On a larger target the point a fragment samples at moves
by parts of a pixel, and there a part of a pixel is a whole texel and
the other word: at twice the size every other column of the picture was
lit from the wrong half of its pixel. A texture whose texels are parts
of a surface's pixels (`packed_in_surface`, the binding's `tex_locked`)
is read another way, by `texel_locked`: the coordinate is taken back to
the middle of the title's own pixel, by how fast it changes across the
screen, the texel found as the title's size would have found it, and of
the `n` by `n` the card keeps for that texel the one this fragment is.

It is not done while the card is checked against the processor, which
draws at the title's size. What it costs is the card's time, which a
run on the virtual clock does not wait for: in a window by the real
clock the card is waited for 2.0 ms of a frame at three times the size
and 3.7 at four (GeForce RTX 4090), and the frame rate is the title's
own at all four.

What is not yet known is when a title's own code reads a larger surface.
The plan for that is a watch on the pages (PLAN.md).

`VITARECOMP_GXM_GPU_KEEP=0` sends every surface back at the end of every
list: the same arithmetic by the same card, the long way round, for
telling a fault in the keeping from a fault in the drawing.
`VITARECOMP_GXM_GPU_LOG=1` says each surface filled from memory and each
sent back, and the stage profile (`VITARECOMP_GXM_PROFILE`) has a line
for the card: what a frame spent looking over textures' bytes, making
textures, on targets, on vertices, in draws, waiting, showing, making
programs ready, and how often the system was asked which pages were
written and how many it named.

A frame of play is 19 to 22 ms of work so in the first hundred frames
of a level and 8 ms once the level has settled (2026-10-12, Direct3D 11,
GeForce RTX 4090, nothing else at work on the machine), against 85 for
the processor's renderer on twenty-four threads. Of the 19, four are the
card's path by its own line of the profile, half a millisecond of that
asking which pages were written, and one is waiting for the card. In a
window, by the real clock, the title holds its thirty frames a second
with the card drawing at one to four times its size: 33.3 ms from one
frame to the next through the front end, the opening scene and play.
