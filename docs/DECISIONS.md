# Decisions

Open decisions, each with the options, the consequence, and a recommendation.
Decided items move to the bottom with a date.

## 1. Licence boundary with Vita3K (decided 2026-10-11: A)

Vita3K is GPLv2. Upstream chose MIT and treats the emulator as reference and
oracle only. That holds as long as every line is written here; it breaks the
moment the GXP shader translator is wanted, because that component is the one
piece nobody wants to write twice.

| Option | Consequence |
|---|---|
| **A. Stay MIT throughout.** Write the USSE-to-SPIR-V translator clean-room from Vita3K's behaviour and public notes. | Largest single task in the project. Toolkit stays forkable by anyone, including the per-game repos sp00nznet's model depends on. |
| **B. Whole repo GPLv2.** Vendor Vita3K's `shader/` (and possibly `gxm/`, `renderer/`) directly. | Shortest path to pixels. Every per-game repo and every recompiled game becomes GPLv2, which is fine for non-commercial preservation and rules out nothing the project wants. Loses upstream's "fork and keep your licence" property. |
| **C. Split.** Toolkit and runtime core stay MIT; a separate GPLv2 component (own directory with its own `LICENSE`, or its own repo) holds Vita3K-derived graphics code behind a small C interface. A game host that links it is GPLv2; one that links a future MIT replacement is not. | Keeps the recompiler reusable, gets pixels early, makes the boundary explicit. Needs discipline: the GPL component may include nothing from the MIT side's generated code, only call it. |

The recommendation had been **C**. What was done is **A**, and by a
shorter road than the table assumed: not a translator to SPIR-V but an
interpreter of the bytecode (`src/usse.c`), written from what the
instructions do and checked against the text of Vita3K's disassembly for
the same programs. An interpreter needs no host shading language, no
control-flow reconstruction and no graphics API, which is most of what
made the translator the largest task in the project. The repository stays
MIT throughout.

## 2. Retail inputs (decided 2026-10-10)

Upstream limited itself to QA builds (plaintext SELF) and refused retail
dumps outright. Decided: retail modules enter through Vita3K's own
decryption, driven by the licence the dump carries, and this repository
consumes what Vita3K writes. The refusal of PFS/NoNpDrm dumps stays, and
all key handling stays out of the repo, exactly as upstream wrote in
DECRYPT.md. The procedure (`tools/vita3k-decrypt.ps1`,
`tools/import-vita3k.sh`) is in PLAN.md under milestone C, and the first
title through it is *Uncharted: Golden Abyss*.

## 3. Host graphics API (open; not needed for a first picture)

The first renderer is software and writes into the title's own surfaces,
so it needs none. The question returns with speed (PLAN.md, milestone E).

| Option | Notes |
|---|---|
| Vulkan | Vita3K's primary back end; its shader output is SPIR-V, so a GPLv2 graphics component would drop straight in. Verbose; needs a window system layer (SDL). |
| OpenGL 4.x | Simplest for the first triangles; also SPIR-V capable via `GL_ARB_gl_spirv`. Deprecated on macOS. |
| D3D12 | Known from dod3-recomp; Windows only; needs SPIR-V to DXIL (SPIRV-Cross can emit HLSL). |

Recommendation: write milestone D's pipeline against an internal interface
and implement it first on **Vulkan**, because that is where the shader
translator's output lands with no further conversion.

## 4. NID database handling (open)

Upstream loads vita-headers from a directory at run time and does not
vendor it. `tools/fetch-deps.sh` keeps that. Alternative: a git submodule at
`external/vita-headers` pinned to a commit, for reproducible CI.

Recommendation: submodule, pinned, once CI needs the database for an
`armrecomp funcs` check. Until then the fetch script is enough.

## 5. Fixture policy (open)

VitaShell is GPLv3 and is used only as an *input* to the tools in CI and
local smoke runs, never committed and never linked. A vitasdk sample built
from source (MIT) would be a cleaner permanent fixture and small enough to
reach 100 % translation.

Recommendation: add a vitasdk sample as the second fixture in milestone A;
keep VitaShell as the large-module regression input.

## Decided

- **2026-10-10: base the repo on sp00nznet/vitarecomp with history intact**,
  `upstream` remote kept, MIT retained. Reason: it is the only Vita
  recompiler in existence, its measured results are sound, and its design
  (trap-by-name, sorted dispatch, written-not-vendored core) is what this
  repo would have built anyway.
- **2026-10-10: keep upstream's "no game data, no keys" rule unchanged.**
