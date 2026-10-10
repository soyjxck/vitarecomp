# Vita3K as a reference

Vita3K is GPLv2. This repo is MIT. Nothing below is copied; this is a map of
where each *fact* lives so that the clean-room implementation can be checked
against the emulator's behaviour. Paths are relative to the Vita3K repository
root, `vita3k/`.

## Tree

```
cpu/        dynarmic_cpu.cpp (JIT), disasm.cpp (capstone)
kernel/     load_self.cpp, relocation.cpp, thread.cpp, sync_primitives.cpp, kernel.cpp
mem/        mem.cpp, allocator.cpp (guest address space)
module/     module.h (EXPORT macro), bridge.h (make_bridge), lay_out_args.h (AAPCS layout)
modules/    ~160 directories, one per firmware library, e.g. SceCtrl/SceCtrl.cpp
nids/       nids.inc: NID(name, 0xVALUE) lines grouped by module and library
gxm/        gxp.cpp (program container), textures.cpp, color.cpp, attributes.cpp
shader/     gxp_parser.cpp, usse_*.cpp, spirv_recompiler.cpp (GXP to SPIR-V/GLSL)
renderer/   gl/, vulkan/, scene.cpp, batch.cpp, state_set.cpp, sync.cpp
external/   dynarmic, SPIRV-Cross, glslang, psvpfstools, vita-toolchain, capstone, ...
```

## Loader: `kernel/src/load_self.cpp`

- SELF detection: first bytes `SCE\0`; requires header `version == 3` and
  `header_type == 1`. ELF at `elf_offset`, program headers at `phdr_offset`,
  per-segment `segment_info` at `section_info_offset`. Raw ELF is accepted
  too, which is what FAGDec output is.
- Accepted `e_type`: `ET_SCE_EXEC` (fixed address) and `ET_SCE_RELEXEC`
  (relocatable). `ET_SCE_PSP2RELEXEC` is rejected.
- Segment flags: `compression == 2` means zlib; `encryption == 2` means
  plain. Both read backwards from what a first guess assumes.
- `PT_LOAD` tries the requested `p_vaddr`, falls back to any address for
  relocatable modules. `PT_SCE_RELA` is fed to `relocate()`.
  `PT_SCE_COMMENT`, `PT_SCE_VERSION`, `PT_ARM_EXIDX` are skipped.
- `e_entry` is **not code**: segment index in the top two bits, byte offset
  in the low 30, pointing at `sce_module_info`.
- Import records: `size == 0x24` short form, `size == 0x34` full form.
- Function import stubs: resolved ones are patched to
  `MOVW/MOVT r12, target; BX r12`; unresolved ones become `SVC #0; MOV pc, lr`
  with the NID in the third word, and the SVC handler dispatches on that NID.
  This is the emulator's equivalent of upstream's trap-by-name defaults.
- Special export NIDs: `module_start 0x935CD196`, `module_stop 0x79F8E492`,
  `module_exit 0x913482A9`, `SceProcessParam 0x70FBA1E7`,
  `SceModuleInfo 0x6C2224BA`, `SceLib syslyb 0x936C8A78`.
- Variable imports with no export get a shared 4-byte `0xDEADBEEF`
  placeholder per NID.

## Relocations: `kernel/src/relocation.cpp`

Every entry starts with a 4-bit `format` in the low bits of its first word.
Formats 0 to 3 are multi-word; 4 to 9 fit in one word.

| Format | Size | Meaning |
|---|---|---|
| 0 | 12 B | Full: symbol segment (4), code (8), patch segment (4), code2 (8), dist2 (4); addend (32); offset (32). Second relocation at `p + dist2 * 2` |
| 1 | 8 B | Full with 22-bit offset: `offset_lo` (12) in word 0, `offset_hi` (10) + addend (22) in word 1 |
| 2 | 8 B | Incremental: symbol segment, code, offset (16) advancing the running offset; addend (32) |
| 3 | 8 B | MOVW/MOVT pair: `mode` bit (0 ARM, 1 Thumb), offset (18), dist2 (5), addend (22) |
| 4 | 4 B | Repeat the last pair: offset (23), dist2 (5) |
| 5 | 4 B | Two repeats: dist1 (9), dist2 (5), dist3 (9), dist4 (5) |
| 6 | 4 B | Rebase an absolute pointer already in memory: offset (28) |
| 7 / 8 / 9 | 4 B | Packed offset lists: four 7-bit, seven 4-bit, fourteen 2-bit word counts |

Symbol segment `0xF` means absolute (base 0). Var-import streams accept only
formats 1 and 2, with the symbol supplied by the caller, and carry a 32-bit
header: 4-bit zero, 24-bit `reloc_data_size` including the header, 4-bit zero.

Relocation codes are the standard ARM ELF numbers: `R_ARM_ABS32` 2,
`REL32` 3, `ABS8` 8, `THM_CALL` 10, `CALL` 28, `JUMP24` 29, `TARGET1` 38,
`V4BX` 40, `TARGET2` 41, `PREL31` 42, `MOVW_ABS_NC` 43, `MOVT_ABS` 44,
`THM_MOVW_ABS_NC` 47, `THM_MOVT_ABS` 48, and `RBase` 255 as a no-op.

Instruction patching: ARM `BL` writes `(S + A - P) >> 2` into bits 0..23.
Thumb `BL` splits into imm10/imm11 with `S`, `J1 = !(v[23] ^ S)`,
`J2 = !(v[22] ^ S)`, preserving bit 12 so BLX stays BLX. Thumb-2 MOVW/MOVT
(T3) place `value[15:12]` in bits 0..3 and `value[11]` in bit 10 of the upper
halfword, `value[10:8]` in bits 12..14 and `value[7:0]` in bits 0..7 of the
lower halfword.

This is what upstream's phase 4 item "`SCE_RELA` relocation seeding" needs,
and every 2015+ title (and every homebrew module) is `ET_SCE_RELEXEC`.

## HLE bridge: `module/include/module/module.h`, `bridge.h`

- `EXPORT(ret, name, args...)` declares `export_<name>` with three leading
  parameters, `EmuEnvState &`, `SceUID thread_id`, `const char *export_name`,
  then the Vita-visible arguments as plain C types. `make_bridge<&export_x>`
  builds an `ImportFn` (`void(EmuEnvState &, CPUState &, SceUID)`).
- Arguments are laid out at compile time per AAPCS (`lay_out<...>()`) and read
  from r0..r3 and the stack; `Ptr<T>` arguments are guest addresses
  translated through the memory state; the return value goes through
  `write_return_value`.
- `UNIMPLEMENTED()` and `STUBBED("...")` log once and return 0. Grep for them
  to learn which firmware functions the emulator itself never needed, which is
  a cheap upper bound on what a recomp needs.

Upstream's convention is simpler and already decided: every `vita_hle_<name>`
is `void(void)`, reads `r0..r3` from the CPU globals, translates guest
pointers with `vita_mem_ptr`, and writes `r0`. The emulator's per-function
signatures, which come from vita-headers, are the specification for what
those bodies must do.

## Modules: `modules/`

The directory list is the full firmware surface the emulator knows. The ones
a single-player bring-up touches first:

`SceLibKernel`, `SceKernelThreadMgr`, `SceSysmem`, `SceKernelModulemgr`,
`SceProcessmgr`, `SceLibc`, `SceIofilemgr`, `SceCtrl`, `SceTouch`,
`SceDisplay`, `SceGxm`, `SceAudio`, `SceRtc`, `SceAppMgr`, `SceAppUtil`,
`SceSysmodule`, `SceCommonDialog`, `SceFios2`, `SceNgs`.

Example of the emulator's depth: `SceCtrl/SceCtrl.cpp` has 37 exports, 20
implemented and 17 `UNIMPLEMENTED()`; the real ones read SDL gamepads into
`SceCtrlData`, gate PS TV behaviour on a config flag, and map actuator values
0..255 onto SDL's 0..65535 rumble range.

## NID table: `nids/include/nids/nids.inc`

321 KB of `NID(name, 0xVALUE)` lines under `// Module "X"` and
`// Library "Y"` comments. The emulator's own flat database, derived from the
same community data as vita-headers; useful to cross-check a NID that the
YAML database does not name. `// Some nids are fake` marks IPMI C++ mangled
entries.

## Shader recompiler: `shader/`

```
GeneratedShader convert_gxp(const SceGxmProgram &program, const std::string &shader_hash,
    const FeatureState &features, const Target target, const Hints &hints,
    bool maskupdate = false, bool force_shader_debug = false,
    const std::function<bool(const std::string &ext, const std::string &dump)> &dumper = nullptr);
```

`Target` is `GLSLOpenGL`, `SpirVOpenGL` or `SpirVVulkan`; `Hints` carries the
vertex attribute list, colour surface format and the sampled texture formats
that the GXP alone does not tell you. Files: `gxp_parser.cpp` (the container),
`usse_disasm.cpp` and `usse_decode_helpers.cpp` (the SGX543 USSE instruction
set), `usse_program_analyzer.cpp`, `usse_translator_entry.cpp`,
`usse_utilities.cpp`, `spirv_recompiler.cpp` (the back end, ~100 KB).
Shader version constant: 13.

This is the single largest piece of the project and the one where the licence
decision bites hardest; see [DECISIONS.md](DECISIONS.md).

## Using the emulator as an oracle

- Run the same module in Vita3K with its logging enabled and compare the
  sequence of firmware calls (NID, arguments) against the recompiled
  program's trap-by-name log. Divergence points at the first wrong HLE body.
- For instruction semantics, drive a single recompiled function and the
  emulator (or touchHLE's interpreter core) from identical register and
  memory state and compare the result, the way the UMK3 iOS project does.
- Vita3K's `dump_elfs` option writes the reconstructed ELF it loaded, which is
  a second implementation to byte-compare `armrecomp extract` against.
