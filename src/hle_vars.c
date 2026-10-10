/* hle_vars.c — the variables a module imports from firmware libraries.
 *
 * A function import is a call the loader redirects; a variable import is an
 * ADDRESS the loader writes into the image, at every MOVW/MOVT pair and
 * literal word that names it. The C library exports its case tables and its
 * standard streams this way, the C++ runtime its RTTI vtables and type_info
 * objects. Left unbound, the placeholders point at the import's own stub
 * block, and a program that lowercases a name through `_Tolotab` reads
 * sixteen-bit garbage for every character: a retail title hashed its
 * resource names through exactly that and found none of them.
 *
 * Each variable the runtime knows is built here in guest memory; one it does
 * not know gets a zeroed page and a trace line, so a read through it is a
 * null pointer rather than a stub's bytes. */

#include "vitarecomp/hle.h"
#include "vitarecomp/host.h"
#include "vitarecomp/recomp_rt.h"
#include "hle_sysmem.h"

#include <string.h>

/* Dinkumware's _Ctype classification bits, as the library's headers define
 * them and its is*() macros test them. */
#define CT_XD 0x001   /* hexadecimal digit                 */
#define CT_UP 0x002   /* upper case                        */
#define CT_SP 0x004   /* space                             */
#define CT_PU 0x008   /* punctuation                       */
#define CT_LO 0x010   /* lower case                        */
#define CT_DI 0x020   /* decimal digit                     */
#define CT_CN 0x040   /* control that is also white space  */
#define CT_BB 0x080   /* other control                     */
#define CT_XB 0x400   /* blank                             */

/* A 258-entry table of shorts indexed from -1 (EOF) to 255, with the
 * variable itself, a pointer to entry 0, in front of it. The variable's
 * address is what the import binds to; the program loads the pointer through
 * it and indexes the table with a signed char. */
static uint32_t short_table(int16_t (*entry)(int c)) {
    uint32_t base = vita_sysmem_reserve(16 + 258 * 2);
    if (!base) return 0;
    uint32_t tab = base + 16;
    for (int c = -1; c < 256; c++)
        vita_write16(tab + (uint32_t)(2 * (c + 1)), (uint16_t)entry(c));
    vita_write32(base, tab + 2);
    return base;
}

static int16_t to_lower(int c) {
    if (c < 0) return (int16_t)c;
    return (int16_t)(c >= 'A' && c <= 'Z' ? c + 32 : c);
}
static int16_t to_upper(int c) {
    if (c < 0) return (int16_t)c;
    return (int16_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
}
static int16_t ctype_of(int c) {
    if (c < 0 || c > 127) return 0;
    if (c == ' ')                      return CT_SP | CT_XB;
    if (c == '\t')                     return CT_CN | CT_XB;
    if (c == '\n' || c == '\v' || c == '\f' || c == '\r') return CT_CN;
    if (c < 32 || c == 127)            return CT_BB;
    if (c >= '0' && c <= '9')          return CT_DI | CT_XD;
    if (c >= 'A' && c <= 'F')          return CT_UP | CT_XD;
    if (c >= 'a' && c <= 'f')          return CT_LO | CT_XD;
    if (c >= 'A' && c <= 'Z')          return CT_UP;
    if (c >= 'a' && c <= 'z')          return CT_LO;
    return CT_PU;
}

static uint32_t make_tolotab(void) { return short_table(to_lower); }
static uint32_t make_touptab(void) { return short_table(to_upper); }
static uint32_t make_ctype(void)   { return short_table(ctype_of); }

/* The standard streams are FILE objects, not pointers: the program takes
 * their addresses. Three slices of one page, zeroed; the stream HLE does not
 * read them. */
static uint32_t g_files;
static uint32_t make_file(void) {
    static uint32_t next;
    if (!g_files) { g_files = vita_sysmem_reserve(0x1000); next = g_files; }
    if (!g_files) return 0;
    uint32_t f = next;
    next += 0x100;
    return f;
}

/* --- the C++ runtime's RTTI ----------------------------------------------------
 *
 * A program's type_info objects live in its own image, but their vtable
 * pointers name the runtime's vtables, and the runtime's type_info objects
 * for its own classes are what the program compares against to learn what
 * kind of type_info it holds: `typeid(*ti) == typeid(__si_class_type_info)`
 * is how an inlined dynamic_cast asks whether a type has one base. So the
 * shapes have to be right, not the code behind them: a vtable is two words
 * of header (offset to top, the type_info of the class) and then slots, an
 * object's vtable pointer points past the header, and a type_info is
 * { vtable pointer, name, base } with the Itanium ABI's mangled names. The
 * slots are zero; a virtual call on one is a dispatch miss that says so. */
enum {
    VT_CLASS = 0x000, VT_SI = 0x020, VT_VMI = 0x040, VT_POINTER = 0x060,
    VT_LOGIC = 0x080, VT_LENGTH = 0x0A0, VT_RANGE = 0x0C0, VT_TYPEINFO = 0x0E0,
    VT_EXCEPTION = 0x100,
    TI_TYPEINFO = 0x140, TI_CLASS = 0x150, TI_SI = 0x160, TI_VMI = 0x170,
    TI_POINTER = 0x180, TI_PBASE = 0x190, TI_EXCEPTION = 0x1A0, TI_LOGIC = 0x1B0,
    TI_LENGTH = 0x1C0, TI_RANGE = 0x1D0, TI_BADTYPEID = 0x1E0, TI_BADCAST = 0x1F0,
    OBJ_NOTHROW = 0x200,
    STRINGS = 0x300
};

static uint32_t g_rtti;

static uint32_t rtti_string(uint32_t *cursor, const char *s) {
    uint32_t at = *cursor;
    size_t n = strlen(s) + 1;
    for (size_t i = 0; i < n; i++) vita_write8(at + (uint32_t)i, (uint8_t)s[i]);
    *cursor = (at + (uint32_t)n + 3) & ~3u;
    return at;
}

/* A vtable: offset-to-top, the class's type_info, then empty slots. */
static void rtti_vtable(uint32_t vt, uint32_t ti) {
    vita_write32(vt, 0);
    vita_write32(vt + 4, ti);
}

/* A type_info: the vtable of its own class (past the header), its name, and
 * for the single-inheritance kind its base. */
static void rtti_typeinfo(uint32_t ti, uint32_t vt, uint32_t name, uint32_t base) {
    vita_write32(ti, vt + 8);
    vita_write32(ti + 4, name);
    vita_write32(ti + 8, base);
}

static uint32_t rtti_page(void) {
    if (g_rtti) return g_rtti;
    uint32_t b = vita_sysmem_reserve(0x1000);
    if (!b) return 0;
    g_rtti = b;
    uint32_t s = b + STRINGS;

    rtti_vtable(b + VT_TYPEINFO,  b + TI_TYPEINFO);
    rtti_vtable(b + VT_CLASS,     b + TI_CLASS);
    rtti_vtable(b + VT_SI,        b + TI_SI);
    rtti_vtable(b + VT_VMI,       b + TI_VMI);
    rtti_vtable(b + VT_POINTER,   b + TI_POINTER);
    rtti_vtable(b + VT_EXCEPTION, b + TI_EXCEPTION);
    rtti_vtable(b + VT_LOGIC,     b + TI_LOGIC);
    rtti_vtable(b + VT_LENGTH,    b + TI_LENGTH);
    rtti_vtable(b + VT_RANGE,     b + TI_RANGE);

    /* std::type_info and std::exception are plain classes; everything else
     * here has exactly one base. */
    rtti_typeinfo(b + TI_TYPEINFO,  b + VT_CLASS, rtti_string(&s, "St9type_info"), 0);
    rtti_typeinfo(b + TI_CLASS,     b + VT_SI,    rtti_string(&s, "N10__cxxabiv117__class_type_infoE"), b + TI_TYPEINFO);
    rtti_typeinfo(b + TI_SI,        b + VT_SI,    rtti_string(&s, "N10__cxxabiv120__si_class_type_infoE"), b + TI_CLASS);
    rtti_typeinfo(b + TI_VMI,       b + VT_SI,    rtti_string(&s, "N10__cxxabiv121__vmi_class_type_infoE"), b + TI_CLASS);
    rtti_typeinfo(b + TI_PBASE,     b + VT_SI,    rtti_string(&s, "N10__cxxabiv117__pbase_type_infoE"), b + TI_TYPEINFO);
    rtti_typeinfo(b + TI_POINTER,   b + VT_SI,    rtti_string(&s, "N10__cxxabiv119__pointer_type_infoE"), b + TI_PBASE);
    rtti_typeinfo(b + TI_EXCEPTION, b + VT_CLASS, rtti_string(&s, "St9exception"), 0);
    rtti_typeinfo(b + TI_LOGIC,     b + VT_SI,    rtti_string(&s, "St11logic_error"), b + TI_EXCEPTION);
    rtti_typeinfo(b + TI_LENGTH,    b + VT_SI,    rtti_string(&s, "St12length_error"), b + TI_LOGIC);
    rtti_typeinfo(b + TI_RANGE,     b + VT_SI,    rtti_string(&s, "St12out_of_range"), b + TI_LOGIC);
    rtti_typeinfo(b + TI_BADTYPEID, b + VT_SI,    rtti_string(&s, "St10bad_typeid"), b + TI_EXCEPTION);
    rtti_typeinfo(b + TI_BADCAST,   b + VT_SI,    rtti_string(&s, "St8bad_cast"), b + TI_EXCEPTION);
    return b;
}

static uint32_t rtti_at(uint32_t off) { uint32_t b = rtti_page(); return b ? b + off : 0; }
static uint32_t make_vt_class(void)     { return rtti_at(VT_CLASS); }
static uint32_t make_vt_si(void)        { return rtti_at(VT_SI); }
static uint32_t make_vt_vmi(void)       { return rtti_at(VT_VMI); }
static uint32_t make_vt_pointer(void)   { return rtti_at(VT_POINTER); }
static uint32_t make_vt_logic(void)     { return rtti_at(VT_LOGIC); }
static uint32_t make_vt_length(void)    { return rtti_at(VT_LENGTH); }
static uint32_t make_vt_range(void)     { return rtti_at(VT_RANGE); }
static uint32_t make_ti_si(void)        { return rtti_at(TI_SI); }
static uint32_t make_ti_vmi(void)       { return rtti_at(TI_VMI); }
static uint32_t make_ti_badtypeid(void) { return rtti_at(TI_BADTYPEID); }
static uint32_t make_ti_badcast(void)   { return rtti_at(TI_BADCAST); }
static uint32_t make_nothrow(void)      { return rtti_at(OBJ_NOTHROW); }

typedef struct { const char *name; uint32_t (*make)(void); } var_provider;
static const var_provider providers[] = {
    { "SceLibc::_Ctype",   make_ctype   },
    { "SceLibc::_Stderr",  make_file    },
    { "SceLibc::_Stdin",   make_file    },
    { "SceLibc::_Stdout",  make_file    },
    { "SceLibc::_Tolotab", make_tolotab },
    { "SceLibc::_Touptab", make_touptab },
    { "SceLibstdcxx::_ZSt7nothrow",                              make_nothrow      },
    { "SceLibstdcxx::_ZTIN10__cxxabiv120__si_class_type_infoE",  make_ti_si        },
    { "SceLibstdcxx::_ZTIN10__cxxabiv121__vmi_class_type_infoE", make_ti_vmi       },
    { "SceLibstdcxx::_ZTISt10bad_typeid",                        make_ti_badtypeid },
    { "SceLibstdcxx::_ZTISt8bad_cast",                           make_ti_badcast   },
    { "SceLibstdcxx::_ZTVN10__cxxabiv117__class_type_infoE",     make_vt_class     },
    { "SceLibstdcxx::_ZTVN10__cxxabiv119__pointer_type_infoE",   make_vt_pointer   },
    { "SceLibstdcxx::_ZTVN10__cxxabiv120__si_class_type_infoE",  make_vt_si        },
    { "SceLibstdcxx::_ZTVN10__cxxabiv121__vmi_class_type_infoE", make_vt_vmi       },
    { "SceLibstdcxx::_ZTVSt11logic_error",                       make_vt_logic     },
    { "SceLibstdcxx::_ZTVSt12length_error",                      make_vt_length    },
    { "SceLibstdcxx::_ZTVSt12out_of_range",                      make_vt_range     },
};

void vita_vars_bind(void) {
    for (uint32_t i = 0; i < vita_var_import_count; i++) {
        const vita_var_import *v = &vita_var_imports[i];
        if (!v->value) continue;
        uint32_t addr = 0;
        const char *how = "no implementation: a zeroed page";
        if (v->name) {
            for (size_t k = 0; k < sizeof(providers) / sizeof(providers[0]); k++) {
                if (strcmp(providers[k].name, v->name) == 0) {
                    addr = providers[k].make();
                    how = "built";
                    break;
                }
            }
        }
        if (!addr) addr = vita_sysmem_reserve(0x1000);
        *v->value = addr;
        /* The words in the image: the address plus the entry's addend, which
         * is 8 for a vtable pointer (past the vtable's header). */
        for (uint32_t s = 0; s < v->abs_count; s++)
            if (vita_mem_ptr(v->abs_sites[s].addr, 4))
                vita_write32(v->abs_sites[s].addr, addr + v->abs_sites[s].addend);
        vita_hle_trace("var import %s (nid 0x%08X) -> 0x%08X, %s, %u words patched",
                       v->name ? v->name : "(unnamed)", v->var_nid, addr, how, v->abs_count);
    }
}
