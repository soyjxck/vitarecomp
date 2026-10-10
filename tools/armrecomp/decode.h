/* decode.h — ARMv7-A instruction decoding, both instruction sets.
 *
 * The Vita runs a Cortex-A9. Its code is predominantly Thumb-2 with a real ARM
 * minority, so a decoder that handles only one of them silently misreads the
 * other — and because both are dense encodings, misreading produces plausible
 * instructions rather than obvious garbage.
 *
 * SCOPE. This decoder determines, for every instruction:
 *
 *   - its WIDTH   (2 or 4 bytes) — required to advance correctly at all
 *   - its CLASS   (ALU / load / store / branch / SIMD / ...)
 *   - its CONTROL FLOW (target, conditionality, call vs return vs jump)
 *
 * It names common instructions but does not yet decode every operand; that
 * lands in phase 5 where the emitter needs it. The distinction matters because
 * width and control flow are what phase 4 needs, and getting those right for
 * 100% of instructions is worth more than getting operands right for 60%.
 *
 * Anything not recognised decodes to A_UNKNOWN rather than being skipped or
 * guessed at, so `cover` can report honestly how much of a module is
 * understood.
 */

#ifndef ARMRECOMP_DECODE_H
#define ARMRECOMP_DECODE_H

#include <stdint.h>

typedef enum {
    ARM_A32 = 0,    /* 4-byte ARM instructions          */
    ARM_T32 = 1,    /* 2- or 4-byte Thumb-2             */
} arm_mode;

typedef enum {
    A_UNKNOWN = 0,  /* not recognised — counted, never guessed at   */
    A_UNDEF,        /* a genuinely UNDEFINED encoding                */
    A_ALU,
    A_LOAD,
    A_STORE,
    A_LOADM,        /* LDM / POP                                     */
    A_STOREM,       /* STM / PUSH                                    */
    A_BRANCH,       /* B, conditional or not                         */
    A_CALL,         /* BL / BLX                                      */
    A_RETURN,       /* BX LR, POP {..,pc}, MOV pc,lr                 */
    A_INDIRECT,     /* BX Rm, other computed transfers               */
    A_SIMD,         /* NEON / VFP — identified, not yet translated   */
    A_SYS,          /* SVC, MRS/MSR, barriers, coprocessor           */
    A_NOP,
} arm_class;

/* The specific operation, for instructions the emitter can translate. Anything
 * left as OP_NONE is decoded well enough to advance and classify but not well
 * enough to emit, and becomes a named run-time trap rather than silence. */
typedef enum {
    OP_NONE = 0,
    OP_MOV, OP_MOVT, OP_MVN, OP_ADD, OP_ADC, OP_SUB, OP_SBC, OP_RSB,
    OP_AND, OP_ORR, OP_EOR, OP_BIC,
    OP_CMP, OP_CMN, OP_TST,
    OP_LSL, OP_LSR, OP_ASR, OP_ROR,
    OP_MUL,
    OP_MLA, OP_MLS,                     /* rd = ra +/- rn * rm                  */
    OP_UMULL, OP_SMULL,                 /* rdhi:rd = rn * rm, 64-bit            */
    OP_UMLAL, OP_SMLAL,                 /* rdhi:rd += rn * rm, 64-bit           */
    OP_LDR, OP_LDRB, OP_LDRH, OP_LDRSB, OP_LDRSH, OP_LDRD,
    OP_STR, OP_STRB, OP_STRH, OP_STRD,
    OP_PUSH, OP_POP,
    OP_LDM, OP_STM,     /* against any base: mem_add = increment-after, else decrement-before */
    OP_B, OP_BL, OP_BX, OP_BLX,
    OP_TBB, OP_TBH,     /* jump through a byte/halfword table at PC          */
    OP_ADR,
    OP_SXTB, OP_SXTH, OP_UXTB, OP_UXTH, OP_REV,
    OP_REV16, OP_REVSH, OP_RBIT, OP_CLZ,
    OP_UADD8, OP_USUB8, OP_SEL,         /* byte-parallel, through the GE bits   */
    OP_IT, OP_NOP, OP_SVC, OP_CBZ, OP_CBNZ,
    OP_SBFX, OP_UBFX, OP_BFI, OP_BFC,
    OP_LDREX, OP_STREX, /* exclusive access, mem_size wide; rd gets STREX's status */
    OP_MRC_TLS,         /* rt = TPIDRURO, the running thread's TLS pointer     */

    /* Scalar VFP. Not vector work — 82% of this platform's coprocessor traffic
     * is ordinary floating point, and it maps onto C almost one to one. */
    OP_VLDR, OP_VSTR,
    OP_VADD, OP_VSUB, OP_VMUL, OP_VDIV,
    OP_VABS, OP_VNEG, OP_VSQRT,
    OP_VMOV,        /* register to register                        */
    OP_VMOV_TO_C,   /* VFP register -> core register               */
    OP_VMOV_TO_V,   /* core register -> VFP register               */
    OP_VCMP,
    OP_VCVT_F2I, OP_VCVT_I2F,
    OP_VMRS,
    OP_VLDM, OP_VSTM,   /* register-list float load/store, incl. VPUSH/VPOP */
    OP_VMLA, OP_VMLS,   /* multiply-accumulate: vd = vd +/- (vn * vm)       */
    OP_VNMUL,
    OP_VNMLA, OP_VNMLS, /* vd = -vd - vn*vm, vd = -vd + vn*vm                  */
    OP_VMOV_IMM,        /* a VFP constant: imm holds f32 bits, imm64 f64 bits  */
    OP_VMOV_D_TO_C,     /* rt2:rt = a D register, or the S pair vm, vm+1       */
    OP_VMOV_C_TO_D,     /* the other way                                       */
    OP_VCVT_FF,         /* between precisions; vfp_dp names the SOURCE kind     */

    /* Advanced SIMD on D registers (neon.h). vd/vn/vm are D numbers 0-31,
     * neon_q doubles them up, neon_size is the lane width in bits. */
    OP_NEON_MOV_IMM,    /* vd = imm64                                       */
    OP_NEON_ORR_IMM,    /* vd |= imm64                                      */
    OP_NEON_BIC_IMM,    /* vd &= ~imm64                                     */
    OP_NEON_AND, OP_NEON_BIC, OP_NEON_ORR, OP_NEON_ORN,
    OP_NEON_EOR, OP_NEON_BSL, OP_NEON_BIT, OP_NEON_BIF,
    OP_NEON_ADD, OP_NEON_SUB, OP_NEON_CEQ, OP_NEON_TST, OP_NEON_PADD,
    OP_NEON_DUP,        /* every lane from a core register rt               */
    OP_NEON_SET32,      /* vd[imm] = rt                                     */
    OP_NEON_GET32,      /* rt = vn[imm]                                     */
    OP_NEON_LD1, OP_NEON_ST1,  /* imm registers from/to [rn], rm as ARM has it */
    OP_NEON_DUP_LANE,   /* every lane from lane imm of vm                   */
    OP_NEON_EXT,        /* bytes imm.. of the pair vm:vn, vn the low half   */
    OP_NEON_SWP,
    OP_NEON_MVN, OP_NEON_CLZ, OP_NEON_CNT,
    OP_NEON_REV,        /* reverse the lanes within each imm-bit group      */
    OP_NEON_NEG, OP_NEON_ABS,
    OP_NEON_CMP0,       /* lane rel 0; imm: 0 gt, 1 ge, 2 eq, 3 le, 4 lt    */
    OP_NEON_MUL, OP_NEON_MLA, OP_NEON_MLS,
    OP_NEON_MAX, OP_NEON_MIN, OP_NEON_CGT, OP_NEON_CGE,  /* vfp_unsigned   */
    /* f32 lanes */
    OP_NEON_FADD, OP_NEON_FSUB, OP_NEON_FMUL, OP_NEON_FPADD, OP_NEON_FABD,
    OP_NEON_FMLA, OP_NEON_FMLS, OP_NEON_FMAX, OP_NEON_FMIN,
    OP_NEON_FCEQ, OP_NEON_FCGE, OP_NEON_FCGT,
    OP_NEON_FRECPS, OP_NEON_FRSQRTS, OP_NEON_FRECPE, OP_NEON_FRSQRTE,
    OP_NEON_FNEG, OP_NEON_FABS,
    OP_NEON_CVT,        /* imm: 0 s32->f32, 1 u32->f32, 2 f32->s32, 3 f32->u32 */
    /* shifts by an immediate; vfp_unsigned tells the right shifts their fill */
    OP_NEON_SHR, OP_NEON_SRA, OP_NEON_RSHR, OP_NEON_RSRA,  /* imm bits right  */
    OP_NEON_SHL, OP_NEON_SLI, OP_NEON_SRI,                  /* SLI/SRI insert */
    OP_NEON_MOVL, OP_NEON_SHLL, /* vd (Q) = vm (D) widened, SHLL then << imm  */
    OP_NEON_MOVN, OP_NEON_SHRN, OP_NEON_RSHRN,  /* vd (D) = vm (Q) >> imm, narrowed */
    /* one operand is lane imm of vm, broadcast; neon_f for the f32 forms     */
    OP_NEON_MUL_SCALAR, OP_NEON_MLA_SCALAR, OP_NEON_MLS_SCALAR,
} arm_op;

/* Shift types, in the architecture's encoding order. */
typedef enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3 } arm_shift;

#define ARM_COND_AL 0xE
#define ARM_NO_REG  (-1)

typedef struct {
    uint32_t   addr;
    uint32_t   raw;         /* 16-bit instructions occupy the low half */
    uint8_t    width;       /* 2 or 4                                  */
    arm_mode   mode;
    arm_class  cls;
    const char *mnemonic;   /* never NULL; "?" when unknown            */

    /* Control flow, filled in when the class is a branch form. */
    int        has_target;
    uint32_t   target;
    int        conditional;
    int        switches_mode;   /* BLX / BX to the other instruction set */

    /* --- operands (phase 5) ------------------------------------------------ */
    arm_op     op;
    int8_t     rd, rn, rm, rt;  /* ARM_NO_REG when unused                */
    int8_t     rt2;             /* second transfer register: LDRD/STRD    */
    int8_t     ra;              /* accumulator: MLA/MLS                   */
    int8_t     rdhi;            /* high result register: long multiplies  */

    /* VFP register numbers. Single-precision registers are numbered 0-31;
     * double-precision 0-15, aliased onto the same storage. */
    int8_t     vd, vn, vm;
    uint8_t    vfp_dp;          /* 1 = double precision                   */
    uint8_t    vfp_unsigned;    /* VCVT: unsigned integer form            */
    uint32_t   imm;
    uint8_t    has_imm;
    uint8_t    sets_flags;      /* writes NZCV                           */
    uint8_t    shift_type;      /* arm_shift                             */
    uint8_t    shift_amt;
    uint8_t    shift_is_reg;
    uint16_t   reglist;         /* PUSH/POP/LDM/STM                      */
    uint8_t    cond;            /* ARM_COND_AL when unconditional        */
    uint8_t    writeback;       /* W bit: the base register is updated        */
    uint8_t    mem_add;         /* U bit: offset added rather than subtracted */
    uint8_t    mem_post;        /* P == 0: the access uses the base as it was,
                                   the offset is applied to it afterwards    */
    uint8_t    it_mask;         /* IT: which of the next 4 are conditional */
    uint8_t    mem_size;        /* LDREX/STREX: the access width in bits  */

    /* Bitfield ops. Kept out of `imm`/`shift_amt` because a field named for
     * something else is how a reader ends up trusting the wrong one. */
    uint8_t    bf_lsb;
    uint8_t    bf_width;

    /* Advanced SIMD. */
    uint8_t    neon_q;          /* Q form: the operation covers vd and vd+1  */
    uint8_t    neon_size;       /* lane width in bits: 8, 16, 32 or 64       */
    uint64_t   imm64;           /* an expanded modified immediate            */
    uint8_t    neon_f;          /* the lanes are f32, where an op has both kinds */
} arm_insn;

const char *arm_cond_name(uint8_t cond);

const char *arm_class_name(arm_class c);

/* Decode one instruction at `addr` from `code` (which begins at `code_addr`).
 * Returns the width consumed, or 0 if there is not enough input left.
 *
 * `mode` is the current instruction set. The decoder never guesses it: mode is
 * caller state, changed only by an explicit interworking branch, because there
 * is no reliable way to infer it from a single instruction. */
int arm_decode(const uint8_t *code, uint32_t code_addr, uint32_t code_len,
               uint32_t addr, arm_mode mode, arm_insn *out);

/* Jump tables. A TBB/TBH indexes a table of byte or halfword offsets that
 * follows it at PC; the compiler guards the index with `cmp rm, #last`.
 * arm_table_count looks back through up to `nprev` instructions decoded in
 * flow before `tb` (most recent last) for that compare and returns the entry
 * count, or 0 when there is none to be found. arm_table_targets reads the
 * table and returns how many targets it wrote. */
uint32_t arm_table_count(const arm_insn *prev, uint32_t nprev, const arm_insn *tb);
uint32_t arm_table_targets(const uint8_t *code, uint32_t code_addr, uint32_t code_len,
                           const arm_insn *tb, uint32_t count, uint32_t *out, uint32_t max);

#endif /* ARMRECOMP_DECODE_H */
