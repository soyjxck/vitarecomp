/* module.c — parsing .sce_module_info and the import/export tables.
 *
 * Every offset here comes out of the file and is resolved through vm_ptr(),
 * which bounds-checks against the segment rather than the whole image. A table
 * pointer that lands outside its own segment is a sign the structure is being
 * misread, and reading it anyway would produce a confident list of imports that
 * do not exist.
 */

#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t r16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int stub_cmp(const void *a, const void *b) {
    uint32_t x = ((const vm_stub *)a)->addr, y = ((const vm_stub *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int fail(char *err, size_t errsz, const char *msg) {
    if (err && errsz) snprintf(err, errsz, "%s", msg);
    return 1;
}

const uint8_t *vm_ptr(const vm_image *img, uint32_t off, uint32_t need) {
    if (off > img->seg_len) return NULL;
    if ((uint64_t)off + need > img->seg_len) return NULL;
    uint64_t file = (uint64_t)img->seg_file + off;
    if (file + need > img->size) return NULL;
    return img->data + file;
}

const uint8_t *vm_va(const vm_image *img, uint32_t vaddr, uint32_t need) {
    if (vaddr < img->seg_vaddr) return NULL;
    return vm_ptr(img, vaddr - img->seg_vaddr, need);
}

/* Copy a NUL-terminated string named by an ABSOLUTE virtual address. Library
 * name strings sit just past the end of the import table, in the same
 * segment. */
static void read_name_va(const vm_image *img, uint32_t vaddr,
                         char *dst, size_t cap) {
    dst[0] = '\0';
    if (!vaddr) return;
    const uint8_t *p = vm_va(img, vaddr, 1);
    if (!p) return;

    uint32_t off  = vaddr - img->seg_vaddr;
    uint32_t room = img->seg_len - off;
    size_t n = 0;
    while (n < cap - 1 && n < room && p[n]) { dst[n] = (char)p[n]; n++; }
    dst[n] = '\0';
}

int vm_parse(const vm_image *img, uint32_t entry, vm_module *out,
             char *err, size_t errsz) {
    memset(out, 0, sizeof(*out));
    if (err && errsz) err[0] = '\0';

    /* e_entry carries a segment index in its top two bits. Only segment 0 is
     * handled here because the module info always lives in the executable
     * segment; a different index means the layout is not what we think. */
    uint32_t seg = entry >> 30;
    uint32_t off = entry & 0x3FFFFFFF;
    if (seg != 0)
        return fail(err, errsz, "module info is not in segment 0");

    const uint8_t *mi = vm_ptr(img, off, 0x5C);
    if (!mi) return fail(err, errsz, "module info offset lies outside the segment");

    vm_module_info *m = &out->info;
    m->attributes = r16(mi + 0x00);
    m->version    = r16(mi + 0x02);
    memcpy(m->name, mi + 0x04, 27);
    m->name[27]   = '\0';
    m->type       = mi[0x1F];

    m->gp_value     = r32(mi + 0x20);
    m->export_top   = r32(mi + 0x24);
    m->export_end   = r32(mi + 0x28);
    m->import_top   = r32(mi + 0x2C);
    m->import_end   = r32(mi + 0x30);
    m->module_nid   = r32(mi + 0x34);
    m->tls_start    = r32(mi + 0x38);
    m->tls_filesz   = r32(mi + 0x3C);
    m->tls_memsz    = r32(mi + 0x40);
    m->module_start = r32(mi + 0x44);
    m->module_stop  = r32(mi + 0x48);
    m->exidx_top    = r32(mi + 0x4C);
    m->exidx_end    = r32(mi + 0x50);
    m->extab_top    = r32(mi + 0x54);
    m->extab_end    = r32(mi + 0x58);

    /* Structural checks. These are cheap and they are the difference between
     * "the bytes parsed" and "the bytes mean what we think". A misread layout
     * fails at least one of them. */
    if ((m->version >> 8) != 1)
        return fail(err, errsz, "unexpected module info version");
    if (m->export_top > m->export_end || m->import_top > m->import_end)
        return fail(err, errsz, "export/import table bounds are inverted");
    if (m->import_end > img->seg_len)
        return fail(err, errsz, "import table extends past the segment");

    out->have_info = 1;

    /* --- imports ---------------------------------------------------------- */
    /*
     * Entries are self-describing: each begins with its own size, so the walk
     * advances by the value in the file rather than a constant. Vita uses both
     * a 0x34 and a 0x24 form, and assuming either one uniformly desynchronises
     * the walk on a module that mixes them.
     */
    uint32_t p = m->import_top;
    while (p + 4 <= m->import_end) {
        const uint8_t *e = vm_ptr(img, p, 4);
        if (!e) break;

        uint16_t size = r16(e + 0x00);
        if (size < 0x24 || p + size > m->import_end) break;

        e = vm_ptr(img, p, size);
        if (!e) break;

        if (out->import_count >= VM_MAX_IMPORTS) { out->import_truncated = 1; break; }
        vm_import *im = &out->imports[out->import_count];
        memset(im, 0, sizeof(*im));

        im->num_funcs = r16(e + 0x06);
        im->num_vars  = r16(e + 0x08);

        /* Note vm_va, not vm_ptr: the pointers inside an import entry are
         * absolute virtual addresses. */
        if (size >= 0x34) {
            im->library_nid      = r32(e + 0x10);
            read_name_va(img, r32(e + 0x14), im->name, sizeof(im->name));
            im->func_nid_table   = r32(e + 0x1C);
            im->func_entry_table = r32(e + 0x20);
            im->var_nid_table    = r32(e + 0x24);
            im->var_entry_table  = r32(e + 0x28);
        } else {
            im->library_nid      = r32(e + 0x0C);
            read_name_va(img, r32(e + 0x10), im->name, sizeof(im->name));
            im->func_nid_table   = r32(e + 0x14);
            im->func_entry_table = r32(e + 0x18);
            im->var_nid_table    = r32(e + 0x1C);
            im->var_entry_table  = r32(e + 0x20);
        }

        out->total_func_imports += im->num_funcs;
        out->total_var_imports  += im->num_vars;
        out->import_count++;
        p += size;
    }

    /* --- the stub table --------------------------------------------------- */
    /*
     * The NID table and the entry table are parallel arrays: entry k of one
     * names the function, entry k of the other gives the address of the stub
     * that calls it. Pairing them is what lets the emitter turn a BL into a
     * named firmware call.
     *
     * Stub addresses carry the Thumb bit, since they are branch destinations.
     * It is cleared here so lookups compare against the same value the decoder
     * produces for a branch target.
     */
    if (out->total_func_imports) {
        out->stubs = (vm_stub *)calloc(out->total_func_imports, sizeof(vm_stub));
        if (!out->stubs) return fail(err, errsz, "out of memory");

        for (int i = 0; i < out->import_count; i++) {
            const vm_import *im = &out->imports[i];
            const uint8_t *nt = vm_va(img, im->func_nid_table,
                                      (uint32_t)im->num_funcs * 4);
            const uint8_t *et = vm_va(img, im->func_entry_table,
                                      (uint32_t)im->num_funcs * 4);
            if (!nt || !et) continue;

            for (uint32_t k = 0; k < im->num_funcs; k++) {
                if (out->stub_count >= out->total_func_imports) break;
                vm_stub *s = &out->stubs[out->stub_count];
                s->lib_nid  = im->library_nid;
                s->func_nid = r32(nt + k * 4);
                s->addr     = r32(et + k * 4) & ~1u;
                out->stub_count++;
            }
        }

        qsort(out->stubs, out->stub_count, sizeof(vm_stub), stub_cmp);
    }

    /* --- the variables ---------------------------------------------------- */
    /*
     * The same parallel arrays as the functions, but an entry names a block
     * rather than a stub: a header word whose bits 4..27 give the block's
     * size in bytes, header included, then two-word relocation entries, the
     * ARM relocation code in bits 8..15 of the first and the offset into the
     * executable segment in the second, and the addend in bits 16..27 of
     * the first. The retail module uses three codes: ABS32 with an addend
     * of 8 for the vtable pointers in its RTTI objects (past the vtable's
     * two-word header), and the THM_MOVW_ABS_NC / THM_MOVT_ABS pair, addend
     * 0, for the instructions that build a variable's address.
     */
    if (out->total_var_imports) {
        out->vars = (vm_var *)calloc(out->total_var_imports, sizeof(vm_var));
        if (!out->vars) return fail(err, errsz, "out of memory");

        for (int i = 0; i < out->import_count; i++) {
            const vm_import *im = &out->imports[i];
            if (!im->num_vars) continue;
            const uint8_t *nt = vm_va(img, im->var_nid_table,   (uint32_t)im->num_vars * 4);
            const uint8_t *et = vm_va(img, im->var_entry_table, (uint32_t)im->num_vars * 4);
            if (!nt || !et) continue;

            for (uint32_t k = 0; k < im->num_vars; k++) {
                if (out->var_count >= out->total_var_imports) break;
                vm_var *v = &out->vars[out->var_count];
                v->lib_nid = im->library_nid;
                v->var_nid = r32(nt + k * 4);
                v->stub    = r32(et + k * 4);

                const uint8_t *hdr = vm_va(img, v->stub, 4);
                uint32_t bytes = hdr ? (r32(hdr) >> 4) & 0x00FFFFFFu : 0;
                uint32_t n = bytes >= 4 ? (bytes - 4) / 8 : 0;
                const uint8_t *body = n ? vm_va(img, v->stub + 4, n * 8) : NULL;
                if (body) {
                    v->sites = (vm_var_site *)calloc(n, sizeof(vm_var_site));
                    for (uint32_t s = 0; v->sites && s < n; s++) {
                        uint32_t a = r32(body + s * 8), off = r32(body + s * 8 + 4);
                        if (off >= img->seg_len) continue;      /* not in the code: ignored */
                        v->sites[v->site_count].addr   = img->seg_vaddr + off;
                        v->sites[v->site_count].addend = (a >> 16) & 0xFFFu;
                        v->sites[v->site_count].code   = (uint8_t)((a >> 8) & 0xFF);
                        v->site_count++;
                    }
                }
                out->var_count++;
            }
        }
    }

    return 0;
}

void vm_free(vm_module *m) {
    if (!m) return;
    free(m->stubs);
    m->stubs = NULL;
    m->stub_count = 0;
    for (uint32_t i = 0; i < m->var_count; i++) free(m->vars[i].sites);
    free(m->vars);
    m->vars = NULL;
    m->var_count = 0;
}

const vm_stub *vm_find_stub(const vm_module *m, uint32_t addr) {
    if (!m || !m->stubs) return NULL;
    uint32_t lo = 0, hi = m->stub_count;
    addr &= ~1u;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (m->stubs[mid].addr == addr) return &m->stubs[mid];
        if (m->stubs[mid].addr <  addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}
