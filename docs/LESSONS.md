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
