# Lessons

Corrections, and bugs found by running the thing rather than reasoning about
it. Kept because the wrong answers here were all plausible, and the record of
*why* they were wrong is worth more than the fixed code.

## Running it is what found the next bug

The recompiled `module_start` was given a host — ELF segments loaded into guest
memory at their own vaddrs, a stack, and a call — and traced. The result was
immediate and unambiguous:

```
TRACE unimpl  0x8100B92A  raw 0xF2C81262  movt
TRACE unimpl  0x8100B930  raw 0xF2C81C4B  movt
TRACE indirect 0x8100B936 -> 0x00000000
TRACE unimpl  0x8100B93C  raw 0xF2C81056  movt
```

**`MOVT` dominated, and the indirect branches to `0x00000000` were the
consequence.** ARM builds a 32-bit address with a `MOVW`/`MOVT` pair — low half
then high half — so leaving `MOVT` untranslated costs every constructed pointer
its top 16 bits, and every computed branch lands near zero.

It had been left trapping on the reasoning that "emitting it as a move would
discard the low 16 bits". That was right about the hazard and wrong about the
conclusion: the correct translation is one line,
`rd = (rd & 0xFFFF) | (imm << 16)`. Refusing to approximate had turned into
refusing to implement, and only running the thing showed the cost.

The same trace surfaced `LDRD`/`STRD` falling through as unknown — bit 6
separates the dual forms from the multiple forms, and the mask kept it, so they
never matched. After both fixes the `movt` traps are gone from the trace
entirely.

**`VITARECOMP_TRACE=1`** switches traps from abort to log-and-continue. It is a
discovery aid and is labelled as one: past the first missing piece the machine
state is wrong and every later trap is reached down a path that would not have
happened on hardware. What it is good for is one run naming many missing pieces
instead of one.


## A generated-code bug only scale revealed

Scaling the emit from 100 to 1,500 functions also exposed a generated-code bug
that smaller runs could not. Labels were recorded the moment a branch target was
seen inside a function's extent, but the block behind one is only collected if
the walk reaches it — and the walk can stop short at three separate limits. The
result was `goto L_81521D2C` with no such label, which does not compile.

The fix is to prune labels against what was actually emitted rather than raise
the limits. A `goto` into a block we never translated would be a lie even if the
compiler accepted it; dropping the label routes the branch through the
call-or-trap path, which says what is true.


## Two wrong guesses before the right measurement

The 2,547 trapping branches looked like a discovery problem — targets that were
never registered as functions. Two plausible fixes came first, and both were
wrong:

1. **An inconsistency between `bound` and `fn->end`** in the emitter's collect
   pass, where the walk used one and the labelling used the other. That was a
   genuine bug and worth fixing, but it moved translation by 0.01%.
2. **Branch targets needing promotion to functions.** Reasonable, and it is
   still true for a residue — but it was not what the bulk of them were.

Making the emitter report *why* each branch trapped settled it in one run: only
177 were the not-a-function case. The other 2,359 were something else entirely
— the decoder set the class, the mnemonic, and a correctly computed target for
`b.w` and `b<cond>.w`, and never set `op`. The emitter's translation switch is
keyed on `op`, so every wide branch decoded perfectly and then fell through to a
trap. Two missing assignments, worth **1.8% of the entire module**.

The lesson is the one this project keeps relearning in new costumes: the
plausible explanation and the true one are different things, and instrumenting
the tool to say which case it hit is cheaper than reasoning about which case it
probably hit.

Reading what remains in value order rather than count order:

1. **Advanced SIMD — 1,647 (1.18%), of which ~1,300 is true NEON.** The only
   genuinely hard piece left.

   Splitting this bucket paid twice. The first split found 82% of it was scalar
   VFP. Splitting the *remainder* found 40% of that was scalar VFP too —
   `VPUSH`/`VPOP`/`VLDM`/`VSTM` (293) and multiply-accumulate (476), both of
   which had been declined earlier on the reasoning that approximating them
   would be worse than trapping. `VMLA` expresses directly as

## Three bugs the compile caught that tests had not

**`MOV pc, rN` was decoded as an ALU write** and emitted as `pc = r6`. That is a
*branch*, not an assignment. It surfaced only because `cpu.h` deliberately has
no `pc` variable, so the C refused to compile — had `pc` existed "for
completeness", it would have built cleanly and silently run past a jump it
should have taken.

**Function extent is not function size.** The emitter first swept each
function's extent linearly, but extent is the highest address the *flow* walk
reached, and tail-call branches drag it across other functions. Every function
re-emitted that whole span: **2.39 GB of C**. Flow-following instead of sweeping
gave 133 MB and raised the translation rate, because the duplicated
data-as-code was gone.

**Being called is what makes an address a function.** Discovery gated
registration on its "already decoded here" bitmap, so a call target another
function's walk had wandered through never became a function — while the
emitter still emitted a call to it. The C compiled and failed to link. Those are
now two separate questions with two separate records.

A related one, from the same link failure: the emitter now **never names a
symbol it does not define**. Two call targets in this module land *inside the
import table*, one of them 0x32 bytes into the first entry — data that happened
to decode as a `BL`. They are traps now, not calls.


## The unknown rate is not a validity measure

The first version of `cover` picked the dominant instruction set by whichever
sweep produced fewer unknowns, and confidently reported **ARM** for a module
whose returns are 10:1 Thumb.

The reason is that the unknown rate measures *how permissive the decoder is for
a given mode*, not whether the bytes are that mode. The ARM decoder assigns a
class to every `op` value, so it reports near-zero unknowns on any input at all
— including pure noise. Two tells gave it away: the ARM sweep claimed **22.62%
system instructions** and **13.52% SIMD**, and no real compiled code has that
shape.

The honest discriminator is **branch-target validity**, because it is arithmetic
performed on the decoded bits rather than a classification of them. Decode real
code in its real mode and its branches point at other code in the same segment;
decode it in the wrong mode and the offsets come from misaligned bits and
scatter. It is a check the wrong answer can fail — 99.89% versus 32.15% on the
same bytes.

**Its known limit:** the in-range test weakens as the segment grows, since a
larger segment is an easier target to hit by chance. *Volume* (53 MB) scores ARM
at 96.30% and *AC III: Liberation* at 77.92%, where normal-sized modules put ARM
at 16–45%. Thumb wins everywhere, but the metric is least discriminating exactly
where the module is largest.

**The unknown rate is an upper bound, not a decode failure rate.** ARM puts
PC-relative constants in literal pools *inside* `.text`, so part of any linear
sweep is decoding data. Those bytes are not instructions we failed to decode;
they are not instructions. Separating them needs the control flow that discovery
recovers.

## The SIMD figure was an undercount

Earlier revisions of the README put SIMD at 2.45% for this
title. That was an undercount caused by a decoder bug: the coprocessor mask
`(h1 & 0xEE00) == 0xEC00` matches `0xEC` and `0xED` but not `0xEE`/`0xEF`,
which is where VFP's CDP/MCR/MRC forms live. Roughly 2,800 VFP instructions
were being counted as *unknown* instead. The real figure is **5.19%**, and
the "unknown" figures were correspondingly inflated.

It surfaced from the trap breakdown: 2.16% of instructions decoding to
`"?"` is a number that demands an explanation, and there wasn't one.


## Two corrections on reading the container

Both are the same lesson from a different angle: a plausible reading of a binary
is not a verified one.

**The SELF header is not the PS3 SELF header.** Vita inserts `self_filesize` and
a padding qword at `0x20`, shifting every field after it by `0x10`. Built against
the widely-published PS3 layout, the parser read `elf_offset` from `0x30` and got
`4` — not garbage, not obviously wrong, just a small plausible offset. What
caught it was that the ELF header is independently locatable: scanning for
`7F 45 4C 46` puts it at `0xA0`, which is the field at `0x40`.

**The synthetic test agreed with the bug.** It was built from the same wrong
layout as the parser, so it passed. That is the general limit of synthetic tests
on a reverse-engineered format, and the reason a real module is in the loop from
phase 1 rather than phase 3.

A third, found the same way: **MSVC Release defines `NDEBUG`**, which compiles
every `assert()` in the suite to nothing. The tests printed "all passed" while
checking nothing. The build now strips `NDEBUG` for test targets, and
`test_container.c` `#error`s if it ever comes back — a silent green run is worse
than a red one.



## The verification you keep deferring is the one that finds the bug

The generated C was confirmed to compile, link and run at 100 functions. Seven
toolkit changes later — VFP, dispatch, wide branches, VFP list forms, bitfields
— it had not been re-confirmed, and the README carried "it still builds at
scale" as a stated assumption rather than a result.

It did not build. `OP_VLDR`/`OP_VSTR` formatted their own address expression
instead of going through the shared `address()` helper, so a PC-relative VFP
load emitted `vita_read32(pc + 0x18)` — and `cpu.h` deliberately has no `pc`.
The same design decision that caught `MOV pc, rN` caught this one, three months
later, the moment anything actually invoked a compiler.

It survived that long for a mundane reason: the 100-function sample contained
no VFP literal. The check was real, and it was run, and it covered a prefix of
the program that did not include the construct that would break.

Fixing it at the shared helper rather than at the caller cured a second bug
nothing had noticed. `address()` ignored the U bit entirely, so the T32 8-bit
`[rN, #-imm]` form — which `decode.c` correctly decodes, setting `mem_add = 0`
— was emitting `rN + imm` and reading from the wrong side of the base register.
That one affected ordinary integer loads and stores, silently, everywhere.

## A prefix is not a sample

The same emit run reports **98.33%** over the first 1,500 functions and
**98.86%** over all 19,120. Functions come out in address order, so "the first
N" is a contiguous region of `.text`, not a cross-section of it — and the low
end of this module is denser in the constructs that still trap.

Every headline figure in these docs was a prefix measurement until this was
noticed. They were not wrong about direction, and each one understated the
result.

## The first indirect transfer is a firmware call, not missing code

With the build fixed, the recompiled `module_start` runs and stops at
`0x8100B936` with `unresolved indirect transfer -> 0x814B9590`. The obvious
reading is a discovery gap: an address that should have been recognised as a
function and was not.

It is not. `0x814B9590` is `SceLibc::__cxa_set_dso_handle_main` — an import
stub. Imports bind at emit time on a direct `BL` to a stub address, but this
call is reached *through a pointer*, so it never went through that path;
`vita_dispatch` searched the function table, correctly failed to find it, and
trapped.

The fix is in the dispatcher, not in discovery: an indirect transfer to a known
stub address is a firmware call and should be routed as one. Worth recording
because the trap message named the symptom accurately and still pointed at the
wrong subsystem — and because the diagnostic only became readable at all once
`vita_dispatch` was given the address of the transfer as well as its
destination. It had been reporting `at 0x00000000`.


## The same bug five times, and what finally stopped it

`cpu.h` deliberately has no `pc` variable. A recompiled program has no program
counter, so any instruction that genuinely reads the PC has its value resolved
at translation time — and the absence of the variable means an emitter path
that gets this wrong produces C that will not compile.

That design caught the bug. It did not stop it recurring:

1. `MOV pc, rN` decoded as an ALU write and emitted `pc = r6`.
2. `VLDR` from a literal pool built its own address expression.
3. `STRD` with `rn == 15` did the same.
4. `MOVT`/`MOVW`/`ADDW` with `rd == 15`.
5. `bx pc`, a register list naming the PC, and a register-offset address.

Five rounds, each fixed at the site, each followed by another site. The reason
is visible in hindsight: the rule was never written anywhere the code could
enforce it, so every new translation had to remember it independently, and
eventually one did not.

The rule is two sentences. **A destination of r15 is a branch** — the decoder
classifies the real ones as branches, so anything still carrying r15 as a
destination is an encoding the emitter does not model, and it must trap. **A
source of r15 is a constant** — the instruction's own address plus four, which
is how the architecture defines reading the PC, and it must resolve.

Both halves now live in one place each: a single guard on the path every
translation takes, and a single `src_name()` that every source operand goes
through. `reg_name()` is left naming only destinations, which the guard covers.

Two things about the fix are worth keeping:

**Scoping the guard needed measurement, not reasoning.** The first version
trapped on `rt == 15` unconditionally, which is right for a load or a store and
wrong for `VMRS APSR_nzcv, FPSCR`, where the same field means "the condition
flags". That cost 3,827 working instructions and showed up immediately as
translation dropping 98.87% to 98.63% — a number moving the wrong way is a
better reviewer than re-reading the diff.

**One site is neither a source nor a destination.** The `VLDM`/`VSTM` base
register is written back, so it is an lvalue; resolving r15 there would emit an
assignment to a constant. It traps. A rule with two cases will meet a third.

## A prefix hides the bugs that matter

Every one of rounds 2 through 5 was reachable only by building the *whole*
module. The 100-function check was real and passed, and it covered a contiguous
region of `.text` that happened to contain no VFP literal, no `STRD` from a
literal pool, and no PC destination.

The same is true of what made rounds 4 and 5 findable at all: they are in
address ranges nothing had ever compiled, because the functions containing them
were not discovered until data-segment scanning found the constructor table.
Better discovery did not just add coverage — it added *reach*, and reach is
what exposes emitter bugs.

## Four bugs the first running homebrew found (2026-10-10)

A translation percentage says how many instructions produced C. It says
nothing about whether the C does what the instruction did. The first module
to run end to end — `samples/fbdemo`, 179 instructions, eight imports —
translated at 95% on the first pass and was wrong four times, and every one
of the four was invisible in the percentage and obvious in a frame.

1. **The entry had its Thumb bit set.** vita-elf-create writes `module_start`
   as an odd offset (`0x25`; VitaShell's is `0x8231`). Discovery seeded the
   odd address and decoded the entry from one byte in: 163 instructions of
   confident nonsense, every `BLX` to a stub gone, and `import calls 0`
   on a program that makes eight. The SNC-built QA corpus had never set the
   bit, so nothing upstream had ever seen it. Found by `armrecomp funcs`
   printing an odd `module_start` next to a disassembly that started at the
   even one.

2. **Post-indexed write-back was a plain offset.** `strb.w r4, [r3], #1` was
   emitted as a store to `r3 + 1` with `r3` untouched. The T32 8-bit
   immediate form carries P, U and W bits; only U had been read. The program
   zeroed one byte forever: the host sat at 130% CPU and printed nothing,
   because the first trap was on the other side of the loop. Found by reading
   the generated C for the first loop in the disassembly.

3. **IT blocks were a comment.** The emitter wrote `/* it ge */` and then
   emitted the predicated `movge` unconditionally, on the assumption that the
   decoder had attached the condition — but the decoder is stateless and
   never could. Every clamp in the program ran both branches: a 64-pixel
   square became a 512-by-288 block. Found by looking at the frame, then at
   the C beside the `it` in the disassembly. IT state now lives where
   instructions are seen in order — the emitter's block walk and discovery's
   — and a predicated return or branch no longer ends the flow.

4. **`RSB` took no second operand.** `rsb r7, r5, r5, lsl #4` (that is,
   `r7 = r5 * 15`) became `0 - r5`, because the 16-bit negate was the only
   form the emitter knew. The row offset of the square went negative. Found in
   the same frame as bug 3, after bug 3 was fixed and the block was the right
   size in the wrong place.

What the four have in common: each is an instruction form the corpus used
rarely or never on the paths that had been compiled, and each was a
*silently plausible* translation — valid C, no trap, wrong value. The
percentage went from 95.53% to 95.53%. The frame went from wrong to right.
Which is why `tools/e2e.sh` hashes a frame rather than counting traps.

## What the retail title found in its first hours of running (2026-10-10)

The emit was 99.98 % translated and the first run reached the fifth
firmware call before reading through a null pointer. Sixteen bugs between
that and the title's first frames, each found by the same tools: the
trace of HLE calls with their arguments, the function trace, the register
trace in a window of instructions around a bad address, and the title's own
log, which it writes through `printf` and the runtime echoes into the trace.

1. **A function returned from its middle.** Discovery's walk stops at code
   another walk has seen, so the extent it records can end mid-function;
   the emitter used that extent as its bound and cut the body there, frame
   still pushed. The caller then read through registers the callee never
   restored. Bodies are bounded by the segment now, and a walk that does run
   out ends in a trap that says so.

2. **`sceFiosFileExistsSync` returns its answer.** Read as the three-argument
   asynchronous form, it wrote the answer through whatever r2 held: 0x6E,
   the last byte of the path. The title calls it with two arguments, which
   the register trace showed at once and the API's own naming had not.

3. **A `stat` that cannot say 3 GB says "no such file".** MSVC's `stat`
   fails on files past 2 GB, and the title's archive is 2.8 GB; it therefore
   did not exist, and the title went on without its resources. `_stat64`.

4. **The data was still encrypted.** A NoNpDrm dump keeps its PFS layer; what
   Vita3K's decryption produced was the executables. The copy Vita3K
   installs into its own `ux0:` is the decrypted data, and its archive
   begins with the magic the title checks for. The toolkit's note on retail
   input now says so.

5. **The decoder took bit 6 for VCVT's signedness.** The integer-to-float
   form keeps it in bit 7 and always has bit 6 set, so every conversion was
   signed. The C library's unsigned divide goes through a double for large
   quotients: 0x81959988 / 3 came out 0, the remainder came out as the
   dividend, and an object pointer was hashed into 0xA248A038. Found by
   following the first bad address back through the function trace to a
   66-instruction helper and reading its last branch.

6. **The 32-bit extends read a three-bit field as two.** `sxtb.w`, `uxtb.w`
   and the halfword forms share one encoding with the op in bits 6:4; the
   decoder indexed a four-entry table with bits 5:4, so every `sxtb.w` was
   a halfword extend, and it dropped the rotate, so `uxtb.w r0, r1, ror #8`
   kept the wrong byte. The extend-and-add forms (`uxtab`, `sxtah`) and
   the byte-pair forms were not decoded at all. No sample had caught it:
   GCC reaches for the 16-bit forms, and the rotated byte extracts are
   what SNC emits for a hash loop. 1,016 instructions in the title.

7. **An unimplemented import is a wrong answer, not a missing one.** The
   title's archive index is 38,717 entries of (CRC32, size, offset),
   sorted, and every resource is found by `bsearch` with the title's own
   comparator. The import's default trapped by name and returned 0, so
   nothing was ever found and the first font failed with the title's own
   error code. The C library's searching and sorting now call the guest
   comparator through the dispatch table, and `ldiv` writes its two-word
   result where the caller pointed. The hash, for the record, is CRC32
   with the standard table, seeded with all ones and never inverted.

8. **A jump table's guard is further away than four instructions, and
   not always a `cmp` of the index.** The count of a `tbb`/`tbh` table came
   from a `cmp rm, #n` among the four instructions before it. SNC schedules
   loads and stores between the two, subtracts the range's base from the
   index after the compare (`cmp r0, #58; bgt; cmp r0, #39; ble; subs r0,
   #40; tbh`), moves the index between registers (`adds r5, r1, #0; cmp
   r1, #4; bhi; tbh [pc, r5]`), compares against a MOVW constant, and
   guards with a branch *to* the table (`cmp r0, #37; bls table`), which
   the walk that reaches the table never sees because it starts at the
   label. The finder now reads the guard's direction from the branch after
   the compare, follows the index through adds, subtracts and moves, and
   the walk from a branch target is seeded with the compare that guarded
   it. The title's tables went from 416 to 483, unresolved computed
   transfers in the emit from 50 to 7, and the functions reached from 77,455
   to 78,158.

9. **A module imports variables, not only functions.** `_Tolotab`, the C
   library's lowercase table, is a `const short *` the library exports; the
   program builds its address with a MOVW/MOVT pair the loader was to patch,
   loads the pointer through it and indexes the table with each character.
   The toolkit read the function imports and never the variable ones, so
   the pair kept its placeholder, the placeholder pointed back at the
   import's own stub block, and every lowercased name came out as sixteen-bit
   garbage: the resource index, 38,717 CRCs of lowercased names, matched
   nothing, and the resource map the title loads first (the file that turns
   the hash of a name as written into the hash of the name as stored) was
   itself not found. The module parser now reads the variable tables and
   the relocation blocks they name, the emitter reads a host global at every
   MOVW, MOVT and literal site instead of folding the placeholder, the loader
   binds each variable to what the runtime builds (the case tables, the
   `_Ctype` classification table, the standard streams) or to a zeroed page
   with a trace line, and writes the address over every placeholder word.
   Eighteen variables in the title: six from the C library, twelve RTTI
   vtables and type_info objects from the C++ runtime. The `type_info name`
   trap with `this` of 0x2F01 had been a vtable read through one of those
   placeholders: 0x2F01 is the first word of a relocation entry.

10. **The single-structure NEON loads were halfword stores.** The decoder
    claimed VLD1/VST1 with whole registers and handed everything else in
    the structure load/store space back, and the core load/store decoder,
    which shares the top bits, took it: `vld1.32 {d8[]}, [sl]`, one word to
    every lane, became `strh r8, [sl, #0xC8F]`. The title's gesture file
    reader loads two fields that way per record and the store wrote the
    loop counter over the records, so the loop never found its end: a
    vector grew by halves to 28 MB and the title's own allocator asserted
    out of its 'Global' pool. The one-lane, all-lanes and de-interleaving
    forms are decoded now, and the whole space is claimed, so a form still
    missing is a trap that names itself rather than a store that corrupts
    the program's data. Found by reading the loop whose allocations grew by
    exactly 1.5 each time, then the disassembly of its head.

11. **A PC-relative load's offset has a sign.** The 32-bit `ldr rt, [pc,
    #imm]` is its own encoding when Rn is 15: a twelve-bit offset whose sign
    is bit 7 of the first halfword, the bit the register-based forms use to
    choose the twelve-bit immediate. The decoder read `ldr.w r2, [pc, #-32]`
    as a register offset of zero, and the emitter folded the word at PC, the
    next instruction, as the constant. The title keeps 273 of its constants
    before the code that uses them and 97 after; among the former is the
    magic multiplier for dividing by a record's size, so a vector of records
    believed it had room for 69 in a buffer of 30, wrote past it, and the
    heap's free lists filled with the records' text. Found by watching the
    vector's end-of-storage word (`VITARECOMP_WATCH`), which was always
    written correctly, then reading the capacity arithmetic in the register
    trace until a multiplier that was not the one in the disassembly.

12. **One printable halfword is an instruction as often as a string.** A
    MOVW/MOVT pair that builds an odd address into the code is a function
    pointer unless the bytes there are text, and the test for text was the
    first halfword's two bytes being printable. `ldr r0, [r1, #4]` is "Hh".
    A ten-instruction callback that begins with it was never emitted, and
    the dispatcher missed it 845 times. The test is now the eight bytes
    around the address, all but one printable: strings run longer than
    instructions do.

13. **A MOVW's half can move before its MOVT.** Discovery pairs a MOVW with
    the MOVT of the same register. The scheduler is free to copy in
    between: `movw r7, #0x7605; adds r5, r7, #0; movt r5, #0x8160` is how
    the title stores a thunk into a dispatch table in `.bss`, and the pair
    never met, so the thunk was never emitted and a `blx` through the table
    missed. The MOVW's value now follows plain register copies (`mov`,
    `adds rd, rn, #0`) until a MOVT closes it or another write clears it.
    Eight functions, all reached only this way. Found from the dispatcher's
    one miss in a run, the disassembly of the call site (a load from a
    table that the file does not hold), and a search of the image for the
    address as a word and as a MOVW: only the MOVW was there.

14. **A thread that spins is waiting for another core.** After the load
    screen's thread started, the trace went silent and stayed silent for a
    fifteen-minute run; shorter runs before it had been read as a slow
    start-up. A sampling profile, written for the question, put every
    sample in one function and two thirds of them on one instruction: `ldr
    r0, [sp]; cmp r0, #0; bne`, the wait of a queued spinlock, whose holder
    clears a word on the waiter's stack. The holder was the thread that had
    started the waiter, and a scheduler that switched only at kernel waits
    never ran it again. Everything the title does before that point takes
    two seconds. Fixed twice over: backward branches count a time slice,
    and a loop that can be shown to only wait gives way at once
    (docs/ARCHITECTURE.md, Threads). `samples/threads` is the regression:
    three threads that wait for each other and never call the kernel.

15. **A stub that hands out records has to hand the same one out twice.**
    The audio synthesiser was stubbed as "accept everything": each voice
    handle and each parameter lock got a fresh 64-byte record. A sound
    engine locks a voice's parameters, writes them, and locks them again
    expecting to read them back; this one makes those calls by the million
    while it starts. The records ate the 57 MiB the arena had left, 7.2
    million more requests were refused, and the title's own assert said
    the rest: `sceKernelAllocMemBlock failed`. The allocator had been
    asked for 3 MiB and said only "exhausted", so the first step was to
    make it say how full it was and that its own runtime had filled it.
    SceNgs is now a model with no signal (`src/hle_ngs.c`):
    racks, voices, patches and parameter blocks all live in the memory the
    title supplies for a rack, a handle is an address inside it, and a
    voice plays in silence for as long as its sample lasts and then calls
    back.

16. **"Already decoded" is not "already a root".** Discovery scans the
    image for stored words that look like pointers to code, and skipped a
    word whose target had been decoded before, as nothing new. But a
    function first met as the target of a branch in code that nothing
    reaches is registered and is no root, and the word in a vtable is then
    the only evidence that anything can call it. A three-instruction
    virtual method was left out of the output that way and the title
    called it. The word now names its target either way: 5,452 stored
    words in this title point at code that was already decoded, and for 23
    functions such a word was the only root. Found with
    `VITARECOMP_WHY=<address>`,
    written for the purpose: discovery prints what it made of that one
    address each time it meets it, which is the question a dispatch miss
    always raises.

Also learned: the title's own crash handler, once reached, walks the stack
for ever and prints a line per frame; a run captures stdout with a bound.
Silence in the trace is not slowness: where the samples fall says which,
and it took twenty seconds to ask.
And a resource missing from the index is not a toolkit bug: the title asks
for 248 shaders and textures that the base archive does not hold, which the
v1.03 patch's own archive presumably does.

## The size was duplication, and the guess was data (2026-10-10)

After the NEON pass the retail emit was 1.12 GB of C for a 9.43 MB code
segment, and the plan said why: the function walker runs into data tables
and emits them as code. That was a guess written down as a finding. The
measurement that would have tested it took one `awk` over the output: lines
and trap lines per function. Not one of the 70,967 functions was more than
half traps; three were a quarter. The data was a sprinkling. What the file
actually held was 13.07 M emitted instructions for 3.33 M distinct ones,
four copies of the module, because discovery registers every branch target
as a function (so a `b` out of its function has somewhere to land) and each
one registered is a second copy of everything after it.

The fix is not to register fewer: an unregistered target is a trap. It is to
emit only what can run. The emitter now keeps the closure of discovery's
roots (the entry point, the exports, every stored pointer) under the calls
and non-local branches of their bodies, and writes nothing else. A function
reached only by a branch its caller inlines is a copy; one reached only from
data the linear sweep read as a `bl` is nothing at all. The hello sample
dropped from 94,005 emitted instructions to 15,705 and runs the same; the
retail title from 1.12 GB to 194 MB.

Two things went wrong on the way, both found by running rather than reading.

The first version made every stored pointer a root, and VitaShell's trap
list was then led by one undefined word, 15,739 times. The relocation stream
named ~250 odd addresses in a table of `{pointer, length}` records, each
pointer at an OID string (`2A 86 48 86 F7 0D` is 1.2.840.113549), each
walked as Thumb for 4 KB. Those pointers are real; the targets are not code.
So a pointer target with no prologue and no table of pointers around it
became *weak*: walked, so its callees are known, but not a root.

The second version dropped `register_fini` from the hello sample, and the
first function-pointer call of the program missed. It is exactly the thing
the weak rule describes: a leaf with no push, named only by one word in
`.init_array`, a table too short to count as one. The rule that holds is in
the emitter, where the body is known: a weak target is promoted to a root
when its whole body translates cleanly, every instruction claimed and every
branch resolved. Data read as code never manages that for long; a leaf
always does. Thirteen promotions in the hello sample, 337 in VitaShell.

What a miss costs now is stated: a computed transfer to a function that was
discovered but not emitted is a dispatch miss that names the address, which
is the announced failure, where a copy that runs would have been the silent
one.

## Let the title order the work (2026-10-10)

Milestone B was planned as "classify the SIMD spaces, then translate integer
and single-precision lanes". What was done instead: emit the retail module,
tally the raw encoding behind every trap, turn the top 160 into mnemonics
with the vitasdk `objdump` (`tools/trap-census.py` does both steps now:
it writes the encodings to a binary and joins the disassembly back to the
counts), and implement families in count order. The first list was
unambiguous: `vdup.32` by lane 35,000 times, `vmul.f32` 30,000, `vmov.f32
#imm` 45,000 across its constants, `ldrex/strex` 17,000, then the
reciprocal estimates and their refinement steps. Nothing in the original
plan's order would have put `VMOV` immediate or `LDREX` near the top, and
they were a fifth of the traps. Two passes, 96.23 % to 99.7 %.

The census also found eight decoder bugs that no sample had reached:

1. **`VLDM`/`VSTM` without write-back were never claimed.** The list forms
   are P=0 U=1 whatever W is, and P=1 U=0 W=1; the decoder took W alone,
   so `vldmia r0, {d0-d1}` trapped 4,000 times while `vpush`/`vpop`
   worked. The comment above the test recorded an earlier wrong guess at
   the same condition.

2. **`LDMIA.W` and `LDMDB.W` were told apart by the wrong sense of the
   bit.** Bit 7 of the first halfword set is increment-after. The first
   version of the general `LDM/STM` emitter had it backwards; the unit test
   written alongside it caught that before any module did.

3. **The two-register miscellaneous group's opcode column is five bits in
   the architecture's table, `opc2` then `Q`.** Reading the column as the
   four-bit `opc2` and halving it again put `VMVN` where `VCNT` should be.

4. **`VNMLA` decoded as `VNMUL`, and `VNMUL` as `VMUL`.** The VFP group
   with opc1 = 01 is the pair of negated accumulates, and the op bit in the
   multiply group is the negated product. Found by reading the group while
   adding to it, not by a test: a wrong sign on a multiply-accumulate is the
   kind of error that survives a frame hash on a program that never uses one.

5. **Scalar `VCVT` between f64 and an integer numbered the integer side as a
   D register.** The integer side of a conversion is always an S register,
   whatever the float side is.

6. **The 16-bit `REVSH` was read as `REV`, and `REV16` was not read.** The
   op field has four values; the decoder excluded one and named the rest.

7. **Float to integer was a C cast.** Undefined out of range, and on x86
   0x80000000 for every such input, signed or not, where the architecture
   saturates and sends NaN to zero. Four saturating helpers now, used by
   the scalar and the lane conversions alike.

8. **Two test anchors would not match because the test file held
   double-encoded em dashes** from an earlier heredoc. The file looked right
   in an editor and had the wrong bytes. Fixed in four files; the patch
   scripts now read and write bytes with the encoding stated.

Two design choices worth stating, since both are deliberate departures from
the hardware. `LDREX`/`STREX` translate as a plain load and a store that
always succeeds, because one guest thread runs at a time on this host;
when threads arrive they will be scheduled cooperatively on that same
thread, and no switch point falls between an exclusive pair. And
`VRECPE`/`VRSQRTE` compute `1/x` and `1/sqrt(x)` exactly where the console
returns eight-bit estimates; compiled code always follows the estimate with
the Newton-Raphson step, which converges from either, and code that depends
on the exact estimate bits would be the first thing a differential run
against Vita3K shows.

The TLS pointer is the one piece of this pass that needed the emulator as
reference rather than the architecture manual. `mrc p15, 0, rt, c13, c0, 3`
reads TPIDRURO, 600 sites in the retail title; what it points at is the
kernel's decision. Vita3K's `thread.cpp` has it: 0x800 bytes of kernel
slots, which is where `sceKernelGetTLSAddr(key)` hands out its 0x101 words,
then a copy of the module's thread-local image, and TPIDRURO points at the
image. The runtime builds one block the same way, from three constants the
emitter now writes (`vita_module_tls_addr/filesz/memsz`), because a program
that keeps its reent pointer in a slot and its errno in the image is
reading one block through two doors.

## Nine more from the first newlib program (2026-10-10)

`samples/hello` is 179 instructions of its own and some 13,000 of newlib's,
and newlib is a different compiler's output from a different decade of
idioms. It translated at 99.7% before anything ran and was wrong nine times.
Every one of the nine was found by *tracing*, not by reading: a function
trace with names, a per-instruction register trace in a window, a watchpoint
on a stack slot, and bad-access reports with the PC. Those four tools were
built during this bring-up and are in the runtime now (`host.h`).

1. **The conditional-branch decoder tested the wrong bits.** `b<cond>.w`
   with S = 1 and cond = 110x — every backward `bgt.w` and `ble.w` — was
   classed as a system instruction, because the test for the 111x
   condition field read bits 10:8 instead of 9:6.

2. **`pop {..., pc}` in its 32-bit form returned nothing.** `ldmia.w sp!,
   {r4-r10, pc}` was classified as a return before its register list was
   recorded, and emitted as a bare `return`. Callee-saved registers came
   back holding the callee's locals and the stack pointer stayed where the
   callee left it. The 16-bit `pop` never had the bug, so every leaf
   function in the corpus had worked. Found with a watchpoint on the saved
   r4 slot: nothing wrote it after the push, so the restore was what was
   missing.

3. **`bx lr` was a dispatch.** The register-indirect case came before the
   return check, so every leaf return looked up the link register in the
   dispatch table — a value nothing maintains.

4. **The ITE mask bit was off by one.** `ite ls; movls r1, #0; movhi r1, #1`
   ran both moves under `ls`, so r1 was always 1, so malloc's size check
   always failed with ENOMEM. The else-bit for the second instruction is
   mask bit 3, not bit 2. And a second rule the same block taught: 16-bit
   instructions inside an IT block do not set flags.

5. **`ldr.w r4, [r3, r4, lsl #2]` dropped the shift.** The register-offset
   address builder ignored the scale, so newlib's descriptor table was read
   one byte in and the pointer it produced looked like any other.

6. **`asr.w r3, r9, ip` shifted r3.** The 32-bit shift-by-register names a
   separate source; the emitter read rd for both forms. The font's bit test
   became noise and every glyph came out scrambled.

7. **`pld [r0]` was `ldr pc`.** The cache hints share the byte-load
   encoding with Rt = 15. strlen opens with one; it was emitted as a return,
   strlen returned its argument, and the write length became the string's
   address. Found by a register trace that showed the function executing
   exactly one instruction.

8. **Jump tables were computed transfers.** `tbb [pc, r2]` is how every
   switch compiles, including the one in `_fstat_r` and the one in printf.
   The count comes from the `cmp rm, #n` that guards the table; with it the
   table is a `switch` in the C, and discovery follows the cases.

9. **Veneers and tail calls.** vita-elf-create links with PIC veneers, so
   every Thumb tail call to a firmware stub goes through `bx pc; nop;
   ldr ip, [pc]; add pc, ip, pc; .word off`. The veneer is now resolved to
   its destination by the decoder, and unconditional branch targets — and
   conditional ones that leave the function backwards — are registered as
   functions, which is what made `b.w __aeabi_idivmod`'s neighbours
   reachable. VitaShell went from 9,195 functions to 21,593.

Also learned, about the platform rather than the toolkit: on vitasdk,
`printf` output never appears. The C library's internals use a per-thread
reent reached through `__getreent()`, while the `stdout` user code names
goes through `_impure_ptr`, which nothing ever repoints — two different
FILE objects. So `setvbuf(stdout, ...)` and `fflush(stdout)` from a program
configure and flush a stream printf never writes to, and exit() cleans up
a third reent. The disassembly shows it: `_impure_ptr` is read in `main`
and in `_reclaim_reent` and written nowhere. The hour spent proving that
the write never happened ended at the toolchain, not the translation; real
homebrew logs through `sceClibPrintf`, and so does the sample.

## What the first pictures found (2026-10-11)

Writing a renderer under a title that already ran found faults in what had
been written while nothing was drawn, and a few in the renderer's own first
day.

1. **Two header offsets nobody had read.** The null driver answered
   "vertex or fragment?" from the word at 0x10 of a program and the size of
   its default uniform buffer from 0x6C. They are at 0x14 and 0x64. The
   title never acted on either answer while nothing was drawn. Parsing 200
   real programs found both at once, because nothing else fitted.

2. **Texture fields one bit out.** The mip count, the level bias and the
   gamma mode were each stored a bit below their place in the control word.
   Written and read back by the same code they agreed with themselves; the
   first reader that was not that code was the sampler.

3. **A flag that was not a bank bit.** The reference reads bit 50 of a
   conditional move as part of the tested operand's register bank. Over the
   title's programs that bit is set exactly where a phase of a program ends,
   as the same bit is in every other instruction that has it; read the
   reference's way, two programs test a register that holds a texture's
   control word. The corpus was the evidence, not either reading.

4. **A discard names the survivor.** `KILL p1` discards the pixel when p1
   is false. Every use in the title follows a comparison whose truth means
   "keep".

5. **One is not 16,777,216.** The depth surface stores 24 bits. Depth 1.0
   times 16,777,215, plus a half, is not a float: it rounds up, the store
   wrote zero into the depth and one into the stencil above it, and the
   whole background came back as the nearest thing there is.

6. **An integer depth buffer compares integers.** The title lays depth
   down in one scene, stores it, loads it in the next and draws the same
   geometry with the test "equal". Kept as floats and rounded only on the
   way to the surface, half the pixels no longer equalled themselves. The
   depth is now rounded to the surface's precision before it is compared
   or kept, the same way on every path.

7. **A trap that returned its argument.** With traps set to continue, an
   unimplemented import returns whatever was in r0. For the movie open
   that is the path's address, and the title took it for a handle: the
   "movie" opened, and what followed was the title waiting on a stream that
   did not exist. The trace mode says of itself that what it reports past
   the first trap is not to be believed; this was that, at length.

What found the fifth and sixth was not the picture, which was only
scrambled, but the per-draw pixel counts beside it: 3,787 pixels in the
pass that wrote the depth, 1,984 in the pass that tested against it.

## Between the first pictures and the menus (2026-10-11)

1. **A failure the title cannot hear is a hang.** The title's first logo
   movie stopped it. The open was made to fail, with no handle, and the
   title waited all the same. The reason was in the title's own player,
   read as disassembly: an open that fails is logged and reported to
   nobody. The player does tell its caller when it cannot load the movie
   library as a system module, and the runtime had been answering
   "loaded" for every module there is. Saying "not loaded" for the one it
   does not have took the title straight to its title screen. An absent
   library is to be reported where the title asks whether it is there.

2. **The time was not where it was assumed to be.** A scene of 45,000
   triangles took a second, and the vertices were shared out among
   threads first, because 45,000 triangles sounds like vertex work. A
   cycle count by stage, on one thread, said otherwise: nearly all of it
   was pixels, in texture samples and fragment programs. Wall-clock times
   on a machine doing other work had said nothing either way; a cycle
   counter on one thread is what could be read.

3. **A sample of a compressed texture was a block's decoding.** With
   eight texels to a filtered pixel and a small cache of blocks, a PVRTC
   texture was being decoded over and over, nine words to a block. Now a
   compressed texture is decoded whole when a draw first binds it and
   kept; once a frame a hash of some of its bytes says whether the title
   has changed it. What was left of a frame after that: fragment programs
   54 %, their inputs 32 % (texture samples 22 %), the store 7 %,
   vertices 3 %.

4. **A second interpreter is only as good as its agreement with the
   first.** Running a program for a row of pixels at once is a second
   implementation of every instruction. Before it shaded a pixel it was
   run against the first on instruction words made at random, every
   register of every lane compared. Two differences came out that no
   picture had shown: a texture sample's level operand, which is read in
   the coordinate's type and not its own; and a signalling NaN in a
   half-float, which the processor's conversion makes quiet and the C
   conversion had passed through as it was. The first is now refused
   by the row form, the second fixed in the C, which the CPU side shares.
   Then the frames: twenty of a run, the same to the byte with the rows
   and without.

5. **A path nobody had run had a trap on it.** Skipping the movie sent
   the title down its failure path, which formats a message with its own
   printf, and that function's switch on the conversion character was one
   of seven jump tables left as computed transfers. Its compiler had
   split the range check across a tree of compares: `cmp r0, #103; bgt
   elsewhere; cmp r0, #62; bgt table`, and at the table only the lower
   end, `cmp r0, #97; ble out; subs r0, #98`. The branch that reaches the
   table says the index is above 62 and nothing about how far; the
   compare before it had already bounded it. A guard that branches to
   its table on "greater" now takes its upper bound from the earlier
   compare of the same register, if nothing wrote the register between,
   and the walk from the table trusts that bound whatever has set the
   flags since. Four of the seven tables were of this shape. With traps
   set to continue the run had gone on regardless, which is why it was
   seen at all only as a count of one at the end.

6. **The commonest vtable has two entries.** Leaving the main menu, the
   title destroyed ten objects and each call missed: a destructor, called
   through the first slot of its class's table, that nothing had emitted.
   The table was two words long, the two destructors every class with a
   virtual destructor has, and the first of them stores the table's
   address and returns: four instructions, the first a MOVW. Discovery
   wanted either a run of four pointers or a prologue at the target, and
   this had neither. What it does have is the two words in front that
   every vtable has, the offset to the top and the pointer to the type
   information. Reading those found 193 such functions in the title;
   treating the remaining lone pointers to prologue-less code as weak
   evidence, emitted if their bodies translate cleanly, found most of 420
   more. 613 stored pointers had led nowhere; 81 still do, to things that
   do not read as functions.

7. **A dialog that answers "cancelled" has not answered.** Starting a
   campaign dimmed the screen and stopped there. The title had put up the
   system's save data dialog, which a title neither draws nor sees: it
   starts it, polls it, and is told what the player chose. The stub for
   it said "finished" to every poll and "cancelled" to every result, and
   wiped the result structure, where a title leaves a pointer for part
   of the answer to be written through. The dialog is a machine
   of two levels, a dialog running until the title ends it and inside it
   one screen at a time, each running until answered or, where it has no
   buttons, until the title takes it down; a title walks it screen by
   screen, here a list of slots, "save?", "saving", "saved". It is now
   that machine, with the answer a player pressing confirm would give,
   and the title creates its slot, writes its file and goes on to load
   its first level.

8. **The smallest functions spell words.** Loading that level missed
   three functions: `bx lr`, `ldr r0, [r0, #4]; bx lr`, and `movs r1, #70;
   str r1, [r0]; bx lr`. Their addresses are built by MOVW/MOVT pairs, 51
   of them for the first alone, and discovery had seen every pair and
   refused each: the bytes at the target were printable, and a pointer
   into printable bytes had been taken for a pointer into a string.
   `bx lr` is "pG". An accessor between two accessors is eight printable
   bytes and no prologue. What tells it from a string is that it
   returns: text decoded as Thumb is loads and stores without end. 130
   more functions in the title, and 32 in VitaShell that the same test
   had refused through its relocations.

9. **The census said which draws, and the tool said which instruction.**
   In the level 25 draws of every frame were counted as "without a
   program". Eighteen vertex programs had four instructions each that
   the decoder did not know: the 32-bit integer multiply-add, two
   sixteen-bit halves multiplied and a word added. They are the
   skinning programs, and the instruction is how a bone's number becomes
   the address of its matrix in a buffer of 96 of them: `IMAD2 pa4.x
   pa4.x 48 sa37.x`, then a load of 48 bytes through the result. The
   programs the title registers at boot never skin anything, so the
   count of "every instruction decoded" had been true of 193 programs
   and said nothing of the 61 more a level brings.

10. **The other instruction set was there all along, as 0.3 % of the
    module.** The level was black, and each of its frames made thousands
    of calls that trapped: `blx` from Thumb into 36 hand-written ARM
    routines, which discovery refused on principle (an ARM target that is
    not an import stub had only ever been a misdecode) and the decoder
    read no further than their class. Reading them cost less than it
    looked. Their SIMD is the Thumb SIMD with the leading bits moved, so
    it goes through the decoders already written; their integer
    instructions fit the fields the Thumb decoder fills; and of the whole
    Advanced SIMD repertoire a title's compiled code had left only three
    instructions for hand-written code to be first to use: VTRN, VTBL and
    VPADDL. A fourth turned up in compiled code that had never run
    before the level did: four singles converted to four halves at once,
    where the title packs the vertices it builds.

11. **A threshold picked by eye was wrong in both directions.** Which
    `blx` targets are ARM code was first decided on 24 words, three in
    four unconditional. Two real routines open with a run of conditional
    moves and have seventeen in twenty-four. Asking for half let in data:
    the output trebled, 62 % of it traps. Then the question was put to
    the module instead: every one of the 2,129 targets, how far it reads
    cleanly and how much of that is unconditional. The real ones are all
    above nine in ten over 96 words; the false ones that read cleanly at
    all sit at one half. The rule now stands in the gap between, and the
    table that found it took less time than either guess.

12. **Sixteen bytes too many, since the first day.** With everything
    translated the level was drawn and the screen stayed black: the title
    kept a fade down over a scene of one character standing still.
    Nothing trapped, nothing missed, no thread spun. What found it was a
    list of the threads at the end of the run, one line each, and one of
    them read "not started": the thread that streams the title's sound,
    created when the sound library came up and never run. The library
    keeps, in one record, a condition variable, the thread's id and a
    mutex, in that order; it creates the thread, stores the id, creates
    the mutex and the condition, and starts the id. The runtime's
    condition variable cleared its work area on creation, 32 bytes of
    it, and the structure is 16. The id was in the sixteen after. So the
    title started thread 0, was told no, and went on without sound from
    its first minute, through every menu, until it reached something
    that waits for the sound to begin. A structure in the title's memory
    is the title's to pack as it likes; a call may write the bytes the
    SDK gives the structure and no others, and there is now a test that
    looks at the bytes either side.

13. **The first playable level began at a jump table.** Forty-three
    seconds of cinematic later the title loaded the level a player moves
    in, and its first frame took a computed transfer to nowhere: one of
    the three tables still left unsized. All three were of one shape, a
    state machine's. The table sits at the top of a loop; the next state
    is range-checked at the bottom, `cmp r0, #31; bhi out`, and the
    table is reached by an unconditional jump back up. The rule for
    guards that branch to their table only looked at conditional
    branches. An unconditional one now carries what the flow before it
    had settled, and seven more tables are followed.

14. **The card that would not go away was a book.** The level opened on
    what looked like a tutorial card over a still of the game, and nothing
    took it away: no tap, no button. START paused, and resuming brought it
    back. Three things were tried to find what the title was waiting for,
    and each said something true. The functions entered between two
    vertical blanks, listed from a run with a finger on the screen and
    from one without (`VITARECOMP_COVERAGE`), differed by 51 for a tap in
    the middle and by 324 for a tap on an icon: the touch arrived, and
    was acted on. The trace of that second tap showed three files read
    and nine more draws for six frames, and not one bit of the picture
    changed. So the log of a frame's draws was made to say where each
    draw's vertices came out, and one draw's were no numbers at all, from
    a matrix of 0xFFFFFFFF words: a cause, at last, and it was not one.
    That draw is the title's own unused slot, and draws nothing on the
    console either. What the positions did show was what the screen is:
    two pages side by side and stencil shapes turned about the bottom
    corners, a book. Not a card: the player's journal, which the game
    opens by itself at its first tutorial page, and the nine draws were
    a page lifted for as long as the finger stayed on it, six frames,
    moving nowhere because the finger did not move. The journal closes at
    the icon in its corner. Nothing had been wrong since the level loaded.
    A screen that does not answer is first a screen to be read as the
    title's designer meant it, and a picture of a console with arrows on
    it was the manual; the half day bought two tools that stay.

15. **Most of the threads were waiting.** A sampler of the host's own
    threads (`VITARECOMP_HOST_PROFILE`) put the first number on what the
    stage profile could not show: of the samples that found a thread at
    work, a third found it turning in the pool's short wait for the next
    job. A draw was drawn as it arrived, its rows shared out, and a scene
    of this title is five hundred draws of which most cover little; the
    vertices of every draw were one thread's, 92 ms of a frame. The
    console does none of that as it goes: it keeps a scene's draws and
    draws them when the scene ends, tile by tile. Doing the same, a draw
    to a thread for the vertices and then a tile to a thread for the
    pixels, took a frame from 350 ms to 110, and the summed work of the
    threads did not change at all. The work had never been the problem.

16. **Six frames in seventy-five were not the same, and no two runs
    agreed which.** The new order was held against the old on frames
    spread over two minutes of the title. Sixty-nine were identical. The
    six differed in soft round patches, and differently on every run,
    which is a race; and a run on one thread differed from drawing at
    once, which is not. Two causes were found and one was real. Programs
    read registers they never wrote, seventeen of them, and one lets what
    it reads through to its result: a row of pixels run twice on two
    fills says which (`VITARECOMP_GXM_UNWRITTEN`). Registers now start
    at zero, as they should have from the first, and the six frames
    still differed. The real cause was a scene that reads what it has
    just drawn: a chain of ever smaller copies of the picture kept side
    by side in one surface, each made from the one before by a draw
    that samples the surface it draws to. Drawn a draw at a time that
    is in order by construction; drawn a tile at a time, a tile can
    read a part of the surface that another thread has not filled yet.
    The console finishes its tiles in order and the title leans on it.
    A draw that samples its own target now waits for everything before
    it and is waited for, and one thread, thirty, a pixel at a time and
    a draw at a time give the same seventy-five frames as before.

17. **The other timeline was a save file.** In the middle of that, some
    runs came out at a different shot of the cinematic altogether, the
    title's own code having run a different number of instructions, and
    it tracked whichever experiment was being tried: filling registers
    with zero did it, filling them with a mark did not. An afternoon's
    theory of pixels feeding back into the title's timing was built on
    that. The runs that differed were the ones whose names had been
    used before: each run kept its save data in a directory named for
    it, and a title that finds a save takes another path through its
    menus and arrives at the cinematic some frames later. Every run now
    gets a directory nothing has written to. Before two runs are
    compared they have to be known to start in the same place, and "the
    same command" is not that.

18. **Asking first cost more than it saved.** Nearly half the pixels a
    scene's triangles cover are turned away by the depth or stencil
    test, after the walk has worked out every varying they would have
    had. So the walk was given a second callback, asked before the
    varyings: the tests moved into it, and the pixels that failed were
    spared the arithmetic. The walk got slower by a twelfth. A call
    through a pointer for every covered pixel costs more than four
    varyings four at a time, and the pixels that pass now paid for two
    calls. What worked was no callback: the walk reads the stencil byte
    and the depth itself, for the draws whose state lets a failed pixel
    go without a trace, and calls nobody for it. The measurement that
    said so took three minutes, and without a process priority above
    the machine's other work it would have said nothing: two runs of the
    same build differed by more than the change.

19. **The plan was to split the big draws; the measurement said which
    draws.** With a draw to a thread, the vertices of play took 15 ms of
    a frame for 107 ms of work, and the reason seemed plain: a list
    cannot finish before its longest draw. Before building vertex
    shading in pieces, one line was printed for the largest list: its
    time, the sum of its draws, the longest draw. The longest was 5 ms,
    of a list that took 7, and it was not long because it was large: it
    was a character, a thousand vertices through a program that fetches
    four bone matrices from memory, and one load from memory was enough
    to send a whole program a vertex at a time. Running that one kind
    of instruction for each lane inside the row took the longest draw
    to a third and the step to 7 ms. No draw is split.

20. **A test of random instructions had been lucky in its numbers.**
    Adding three instruction formats to the list the two interpreters
    are compared on changed which random words came up, and an old
    instruction failed: a multiply of two of the instruction set's
    constants. The last eight of those constants are not numbers, in two
    bit patterns, and a product of two operands that are both not
    numbers is one of the two, whichever the processor's rule about
    operand order picks, which is the compiler's choice of order and is
    made twice, once for each interpreter. Nothing is wrong with either.
    The test now leaves out that one case and says why; and what a load
    from memory finds, in the test, is made finite like everything else
    it starts from, because a value that is not a number, once in a
    register, meets another sooner or later.

## What a second renderer found in the first (2026-10-11)

The title's draws were handed to a graphics card and every list was
drawn both ways and compared. Six things the card did were not what
the interpreter does, and the comparison itself was wrong in four ways
before it could say so.

1. **The comparison has to be fair before it is strict.** The first
   reports were of thousands of pixels in every list. A surface of
   signed bytes was being compared byte by byte without knowing that 255
   is one step from 0; a surface of three ten-bit floats against the
   card's full floats, which differ by rounding at a third of the
   pixels; a surface of one channel on four. Each was the checker's own
   mistake. A difference is now counted in what rounding can leave in
   the surface's own fields, and nothing was learned about the card
   until it was.
2. **"Equal" has to mean the card's own.** A pass drawn where the depth
   equals an earlier pass's drew a ninth of its pixels. The check was
   starting each list from the processor's depth, and two rasterisers
   never agree to the last bit. Carrying the card's own depth from list
   to list fixed it, at the second attempt: the first kept it by the
   address of the planes, and every scene's depth is worked on in the
   same planes. Before that was found, a theory about compilers joining
   a multiply to an add was built, tested and dropped. Drawing
   the same draw twice on the card, the second time under "equal", is
   what cleared the card: all its pixels passed.
3. **A conversion's rounding is part of the arithmetic.** A pass that
   shrinks a picture by four samples came out blurred differently. The
   card makes a half of a float by cutting bits off; the interpreter
   rounds. One step in a texture coordinate at one corner of the screen
   is a third of a texel in the middle.
4. **The reference had sixteen steps to a pixel and the card 256.**
   With the comparison fair, a fifth of a textured draw's pixels were
   still a few steps apart, in patches. Giving the processor's
   rasteriser the card's precision for one run made them vanish, which
   said what it was; the card was then given the processor's, because
   that is the console's.
5. **A sample on the line between two texels.** A pass at half size
   reads a full-size surface with no filter, at points that fall exactly
   between texels. The processor took whichever side the last bit of a
   float fell on, the card always the same one, and a pass that compares
   depths with its neighbours' differed at every edge in the picture.
   Neither was a rule. The rule now is 256 steps to a texel, in both,
   and the card's shader chooses the texel itself.
6. **A draw whose triangles overlap, in a program that blends for
   itself.** A shaft of light is two sheets crossed, drawn by a program
   that reads the pixel and adds to it. The processor gives the second
   sheet what the first left. The card was given a copy of the target
   made before the draw. Found by stopping the program after each of its
   four instructions in both renderers: every input agreed, and the sum
   did not.
7. **A cube is six textures.** The card blends across the edge of a
   cube's face; the console and the interpreter do not, and an ambient
   pass was four times too bright along the seams.
8. **A hash of some of a texture is a hash the change can miss.** A
   pass read the depth an earlier scene had stored; the texture was
   known by 256 places in it, and a small object turning in the middle
   of the screen changed none of them. The difference grew a little
   every frame, which is what a stale copy looks like.

The tool that did most of this is one switch: stop a named program after
n instructions and move a register into its result. It changes the
program both renderers run, so neither has to be taught to show its
insides, and the check that already compares pictures then compares
registers.

## Keeping surfaces on the card (2026-10-11)

The card drew every list right and a frame took twice as long as on the
processor, all of it in carrying surfaces to the card and back. Leaving
them there went wrong five ways before it went right, and each time the
picture was nearly right.

1. **A texture was freed under the list that had been handed it.** A
   list is made ready before it is drawn: its textures first. One
   texture was made from a surface on the card and handed to an early
   draw; a later draw's texture could not be, sent the surface back to
   memory, and the first texture, asked for again, was made anew from
   memory and the old one freed. The early draw still held it. What a
   list replaces is now freed when the next list begins.
2. **A texture's size is not where its bytes end.** Layouts round up: a
   tiled texture of 720 by 408 is said to occupy four megabytes, and the
   surface it reads ends after two and a third, where the next surface
   begins. Every such texture "overlapped" its neighbour, and the
   neighbour was sent back to memory to be read. A texture that begins
   in a surface is a reading of that surface.
3. **A half-size scene tested against the corner of the full-size
   depth.** The same depth memory is described twice, at 720 by 408 and
   at 360 by 204 with the same rows. The card held the first; the second
   was read from memory, which was a frame old, and a sheet of mist that
   should have been behind a wall was in front of it.
4. **The title's own code reads a surface.** With everything else right
   the whole picture was one or two parts in a hundred too bright. The
   exposure of a frame is set from the brightness of the last, and the
   title reads that off a surface 32 pixels square with its processor,
   not with a shader. Memory was never told. Found by sending back only
   the surfaces under some size and watching the difference from the
   long way round vanish.
5. **A texture larger than the surface it was made from.** With one way
   of keeping two surfaces in one memory switched off, to find another
   fault, the picture got worse: a full-size texture was being made
   from the half-size surface in its corner, and what lay outside the
   corner came out as nothing. A reading is made on the card only when
   every texel of it is a pixel of the surface.

`VITARECOMP_GXM_GPU_KEEP=0` is what made each of these findable: the
same card, the same shaders, every surface through memory. Whatever
differs between the two is the keeping.

## Showing the picture (2026-10-12)

The frame was on the card and the window was a metre away, by way of the
title's memory. Putting the one in the other was not much code. What
was learnt was on either side of it.

1. **A picture shown twice is waited for twice.** The first measurement
   said showing cost more than fetching had: three to twelve milliseconds
   a frame. Handing a picture to the screen waits for the card to have
   drawn what is in it, half a millisecond when the card has finished
   and several when it has not, and the host showed at every vertical
   blank what the title changes at every other. Shown only when it is
   another picture, 0.3 ms a frame. Timed apart, the call and the wait
   for the card, before anything was changed: the first guess had been
   the screen's own timing, and three other ways of asking the screen
   not to be waited for had made no difference.
2. **The system stretched the finished picture.** A window 960 across
   came out 1200 across on a screen the system enlarges things for, and
   soft: a program that does not say it knows about such screens is
   drawn at the size it asked for and enlarged afterwards. Nothing in
   the renderer could have been sharp through that. The host says it
   knows, and a size asked for is a size in the screen's pixels.
3. **Look at the window, not the screen.** The first picture taken of the
   window to check it was a picture of another program's text: the
   screen was photographed where the window was, and something lay over
   it. The system will hand over a window's own contents whatever
   covers it, and those were compared with the frames the host wrote to
   files: at twice the size, every pixel of the window the colour of its
   block and the frame matched to the byte.

## A sleep with the baton in hand (2026-10-12)

Every measurement of the first title had been made on the virtual clock,
where a wait takes no time. Run by the real one, in a window, it took a
hundred seconds to load its first level, and its first hundred frames
took a tenth of a second each.

1. **The host slept, and the program slept with it.** One thread of the
   program runs at a time. The one that waits for the vertical blank
   calls the host, and the host slept there until the next sixtieth of a
   second, baton in hand; the threads that load were given one slice of
   twenty thousand branches after each sleep. On the console the kernel
   sleeps that thread and the others have the processors. The host now
   waits through the scheduler. A hundred seconds became fifty-five,
   and the first frames 33 ms.
2. **A sleep was as long as the system's tick.** A program that has not
   asked otherwise has its sleeps rounded up to a tick of fifteen
   milliseconds, most of a sixtieth of a second. The frame rate had
   held all the same, because the host counts sixtieths from where it
   started and a late one is followed by a short one; what did not hold
   was the time of any one frame. The host asks for a millisecond.
3. **What was left was not the title's either.** A sample of the host's
   threads through the fifty-five seconds found four fifths of the time
   that was not waiting inside the system's shader compiler: each
   program of the title is translated and compiled when a draw first
   uses it, and a level's first frames use a great many for the first
   time: twenty-nine seconds in all. The virtual clock had hidden that
   as well, a frame there being a frame however long it took.
4. **Nine programs were most of it.** With each compile timed, eighty
   programs took a few tenths of a second each and nine vertex programs
   four to six seconds. Those nine load words from the title's memory,
   fifty times each, through a helper that looked in eight places with a
   loop and returned from the middle of it when it found the one. The
   compiler writes a helper out wherever it is called and unrolls its
   loop, and a way out of the middle is the expensive thing to write
   out. The same search from the last place to the first, keeping what
   it finds and loading once at the end, is the same answer: the nine
   take a quarter of a second each. The helper that rounds a half the
   way the interpreter does had two ways out of its middle and is called
   a hundred times by a long fragment program; as choices made at the
   end it is a third faster to compile. Fifty-five seconds of compiling
   became eleven, and the pictures are the same bytes.
5. **Asking the compiler to try less made it slower.** Its lowest level
   took twenty-seven seconds over what the next level up takes eleven.
6. **And then it is kept.** What the compiler makes depends on the text
   and nothing else, so it is kept in a file by a hash of the text, and
   a run that finds it there compiles nothing. The level that took a
   hundred seconds to arrive takes eleven the first time and four after
   that. The file was then cut short, spoilt in the middle and given a
   false start, and a run with it compiled the two programs that had
   been lost and drew the same frame.

## Two lists in three thousand (2026-10-12)

The check of card against processor, run over the front end and a minute
of play together for the first time, found two lists that differ, and
found them again with the translation changed and unchanged.

1. **Look at what both hold before what they differ by.** The report
   said seventy-six pixels where the processor has not-a-number and the
   card has nothing. Both results, read as numbers, had not-a-number at
   more than four thousand pixels each: the difference was the edge of
   something the two agree on, and the something is the finding. The
   draw's three textures hold not-a-number at ninety, three hundred and
   eighty and fourteen hundred texels, a region that widens from one to
   the next as a blur widens it.
2. **A neighbour of no weight.** At the edge the sample falls exactly on
   a texel's middle. Its neighbour, not a number, has a weight of
   nothing; the interpreter multiplies and gets not-a-number, the
   card's filter leaves it out. Neither is wrong by any rule the card's
   makers give, which say only that such texels are not to be relied
   on. The check is right to report it and the renderers are left as
   they are.

## Twice the size (2026-10-12)

Making every surface on the card twice as wide and as high was a
wrapper round the three places a surface's size is said and the four
where rows cross between memory and the card. The first frame had the
right shapes and the wrong colours, in stripes one pixel wide.

1. **A texture that is not a picture.** The stripes were the geometry
   buffer. The title keeps two words a pixel and reads them as a texture
   of twice the width, a word a texel, choosing the word by a quarter of
   a pixel in the coordinate. At twice the size the fragments of one of
   the title's pixels sit a quarter of a pixel either side of its
   middle, and a quarter of a pixel was the whole of what told the two
   words apart. No arrangement of the larger texture can answer from the
   coordinate alone: the same coordinate is one word for one fragment
   and the other for its neighbour. The fragment has to say which part
   of the pixel it is, and the coordinate has to be the pixel's, not
   the fragment's. Saying each way a surface is read as a texture, once,
   in the log, is what showed a 720 wide surface read 1440 wide.
2. **Only that one.** Taking every unfiltered texture's coordinate back
   to the pixel's middle would have been simpler and wrong: a texture
   half the target's size, read without a filter, would have had each
   of its texels' four parts repeated over four pixels. The rule is for
   a texture whose texels are parts of a surface's pixels and for no
   other.
3. **A percent sign in a format.** The first try compiled no program at
   all: the text of a program is written through a function that takes
   a format, and a remainder was written with one percent sign. Every
   list that read a surface was left to the processor, and the picture
   at the title's own size differed from the reference, which is what
   gave it away before the larger one was looked at.

## The count that cost more than the counting it saved (2026-10-12)

"The rest of a frame", sixteen milliseconds that were neither the card
nor its path, had been put down to the title and the graphics library.
A sample of the host's threads over five hundred frames of play put a
fifth of the time that was not waiting in one call of the system's: the
one that says which pages have been written.

1. **A cost outside every timer is somebody else's.** The call was made
   once at the start of each list, before the first of the path's
   timers started, so the path's own account came to three milliseconds
   and the remainder was the title's by subtraction. Timed, it was nine
   and a half milliseconds of a frame on a busy machine: more than the
   three milliseconds of hashing it had been brought in to save.
2. **What it costs is what it finds, not what it looks through.** The
   call was asked about all of the program's memory and found forty
   pages written each time, the stack and the heap, which nobody asks
   about; each page it names it makes ready to be caught again. Asked
   only about the pages a texture or a draw's vertices lie on, when one
   of those is about to be looked at, it is asked five hundred times a
   frame, finds six pages, and takes 1.2 ms in all; asked about sixteen
   pages together, since what lies beside one draw's vertices is the
   next draw's, two hundred and sixty times, seventeen pages and 0.6
   ms. (A megabyte together was a hundred askings, ninety pages and 0.4
   ms: more of the program's own writing taken in, for little.) The
   card's path went from fifteen milliseconds of a frame to six, and
   the frames of a run through the front end and into play are the
   same bytes, all hundred and seven of them.
3. **A page found written is found later than it was written.** With
   the asking put off until somebody asks, a reader who read a page
   after it was written and before anybody asked is told, the next
   time, that it changed. It reads it once more and finds what it had.
   The other order would be the fault: told nothing of a change that
   happened. A page is given the moment it was found at, and a moment
   is never before the writing.

## Figures from a busy machine (2026-10-12)

Three and four times the title's size were written down as costing seven
to ten milliseconds of a frame, and play by the real clock as missing
one frame in five. Both were measured while another program compiled on
twenty cores. With the machine quiet, three and four times the size
lose no frame at all and the card is waited for two and four
milliseconds; play is 33.3 ms from one frame to the next.

1. **Say what else was running, or look.** The figures were given with
   "other programs at work on the machine" beside them, which was true
   and not enough: nobody reading it could tell a cost from a
   coincidence. Looking at which programs were using the processor took
   one command.
2. **The virtual clock does not wait for the card.** The same sizes had
   been measured on the virtual clock first, where a frame's time is
   the processor's and the card's work is only seen when something
   waits for it. A level that had settled took 8 ms a frame at every
   size, which said nothing about the sizes. What a size costs is to
   be measured where the frame is shown.
