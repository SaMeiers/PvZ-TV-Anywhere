#ifndef PVZ2NATIVE_ELF32_LOADER_H
#define PVZ2NATIVE_ELF32_LOADER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes allocated past mem_size so a wide, unaligned access in the last guest
 * page cannot run off the end of the buffer -- see the calloc in
 * pvz2_elf_load() and build_page_table() in pvz2_run_test.cpp. */
#define PVZ2_MEM_GUARD_SLACK 4096u

/* Where libPVZ2.so's segments get placed inside the emulated address space, and
 * where the synthetic "SVC #index" import trampolines go. Chosen so everything
 * fits comfortably below a modest space_size (see pvz2_elf_load).
 *
 * Declared here rather than kept private to the loader because the gap between
 * the end of the trampolines and PVZ2_SO_BASE is where the harness puts all its
 * fabricated guest structures -- see <pvz2native/runtime/guest_memmap.h>, which
 * static_asserts that they fit and do not overlap each other. */
#define PVZ2_SO_BASE         0x00100000u
#define PVZ2_TRAMPOLINE_BASE 0x00001000u
#define PVZ2_TRAMPOLINE_MAX  2048u
/* Each import's trampoline is two ARM instructions, `SVC #idx; BX LR`, so the
 * handler never has to move the PC: the JIT predicted the SVC falls through to
 * the BX LR, and the BX LR returns through its return stack buffer. With a
 * one-instruction trampoline the handler set PC = LR itself, which no
 * prediction can follow -- every libc call the guest made went back out to the
 * dispatcher for a hash lookup. */
#define PVZ2_TRAMPOLINE_STRIDE 8u

/* How many shared objects may share one emulated address space: libPVZ2.so plus
 * the DT_NEEDED libraries actually shipped beside it -- for Reflourished that is
 * libc++_shared.so and libNimble.so.
 *
 * Well past what an APK ships, on purpose, because a mod is free to bring more
 * and the cost is nothing: about 8 KB of struct (each module carries a needed[]
 * of this size, so the array grows as n^2) against tens of megabytes of guest
 * image. The limit that actually binds is guest address space, and map_module
 * checks that per module and says so.
 *
 * Running out of table entries is a HARD ERROR, not a warning. A DT_NEEDED
 * library that ships with the game but does not get mapped still "resolves":
 * every symbol it exports falls through to a host trampoline that nothing
 * implements, so the boot carries on and dies much later somewhere unrelated --
 * or worse, calls a null pointer that NOP-slides to the $halt sentinel and
 * reports success. The load refuses instead. */
#define PVZ2_MAX_MODULES 32

/* One shared object mapped into the address space.
 *
 * Older builds needed only a single module: 1.6 and 4.5.2 link the STL
 * statically and import nothing but system libraries, all of which are answered
 * by host shims. Reflourished (PvZ2 9.6.1) does not -- it has libc++_shared.so
 * and libNimble.so as real DT_NEEDED dependencies, and libPVZ2.so imports 119
 * symbols from them, 89 of those being libc++ internals (locale, regex,
 * iostreams, the time_get/time_put facets). Re-implementing those as host shims
 * would mean reproducing libc++'s object layouts across the host/guest boundary;
 * mapping the real ARM32 library and letting the JIT run it is correct by
 * construction and costs 0.6 MB of guest address space. */
typedef struct {
    char name[64];             /* DT_SONAME, or the filename if absent */
    uint32_t base;             /* load bias: where this module's segments were placed */
    uint32_t span;             /* highest (vaddr+memsz) among its PT_LOAD segments */

    /* dynamic symbol table, for cross-module resolution and find_symbol */
    const uint8_t *dynsym;     /* points into mem */
    uint32_t dynsym_count;
    const char *dynstr;        /* points into mem */

    uint32_t dynamic_vaddr;    /* PT_DYNAMIC, unbiased -- re-walked during relocation */

    /* PT_ARM_EXIDX for this module (unbiased). __gnu_Unwind_Find_exidx must
     * answer with the table covering the THROWING pc, so with more than one
     * module this is a per-module lookup rather than a single range. */
    uint32_t exidx_vaddr;
    uint32_t exidx_size;

    /* This module's R+X range (unbiased) -- see text_vaddr on the image. */
    uint32_t text_vaddr;
    uint32_t text_size;

    uint32_t init_vaddr;        /* DT_INIT, unbiased; 0 if absent */
    uint32_t init_array_vaddr;  /* DT_INIT_ARRAY, unbiased; 0 if absent */
    uint32_t init_array_count;

    /* DT_NEEDED entries that resolved to another mapped module, by index. Used
     * only to order constructors: a dependency's must run before its
     * dependent's. */
    uint32_t needed_count;
    uint32_t needed[PVZ2_MAX_MODULES];
} pvz2_elf_module_t;

/* A flat emulated 32-bit ARM address space, plus enough bookkeeping to
 * resolve exported symbols by name (to find JNI entry points) and to
 * dispatch imported symbols to host trampolines (see trampoline_names). */
typedef struct {
    uint8_t *mem;              /* flat buffer covering the whole emulated address space */
    uint32_t mem_size;
    /* Non-zero when `mem` starts a full 4 GiB reservation, of which only the
     * first mem_size (+ slack) bytes are accessible. Every 32-bit guest address
     * then has a host address at mem + addr, which is what lets the JIT use
     * fastmem: one host load or store per guest one, with addresses past
     * mem_size faulting into Dynarmic's handler instead of the page table
     * being consulted on every access. Zero means a plain heap allocation. */
    uint8_t mem_reserved_4g;

    /* Every shared object mapped here. modules[0] is ALWAYS libPVZ2.so, and the
     * legacy single-image fields below (so_base, so_span, dynsym, dynstr, the
     * exidx, text and init_array pairs) mirror it -- so all the code that
     * addresses the game as so_base + offset (symbols.cpp, patches.cpp, the
     * diagnostics) keeps working untouched. Anything that must be right for ALL
     * modules -- the unwinder, constant folding, constructor order, the heap
     * base -- goes through this array instead. */
    uint32_t module_count;
    pvz2_elf_module_t modules[PVZ2_MAX_MODULES];

    /* Module indices in the order their constructors must run: a dependency
     * before every module that needs it (post-order over DT_NEEDED). */
    uint32_t init_order[PVZ2_MAX_MODULES];

    /* Highest base+span across every module -- where the guest heap may start.
     * NOT so_base + so_span once dependencies are mapped after the main image. */
    uint32_t images_end;

    uint32_t so_base;          /* load bias: where libPVZ2.so's segments were placed in mem */
    uint32_t so_span;          /* highest (vaddr+memsz) among PT_LOAD segments, unbiased */
    uint32_t so_entry;         /* e_entry + so_base (mostly irrelevant for a .so) */

    /* dynamic symbol table, kept around for pvz2_elf_find_symbol() */
    const uint8_t *dynsym;     /* points into mem */
    uint32_t dynsym_count;
    const char *dynstr;        /* points into mem */

    /* One synthetic "SVC #index" stub per unique unresolved imported symbol.
     * trampoline_names[index] is the imported symbol's name (owned, malloc'd). */
    uint32_t trampoline_base;
    uint32_t trampoline_count;
    uint32_t trampoline_capacity;
    char **trampoline_names;

    /* PT_ARM_EXIDX: the ARM exception index table (unbiased vaddr + byte
     * size), needed by __gnu_Unwind_Find_exidx so C++ throws can unwind. */
    uint32_t exidx_vaddr;
    uint32_t exidx_size;

    /* The R+X PT_LOAD segment (unbiased vaddr + byte size): .text, .rodata and
     * the ELF/dynsym metadata, i.e. everything the guest may execute or read
     * but never writes. Nothing in the loader needs this -- the JIT does, to
     * constant-fold loads from it at translation time (Pvz2Env::
     * IsReadOnlyMemory). ARM32 reaches every 32-bit constant, string address
     * and function pointer through a PC-relative literal pool that lands in
     * exactly this range, so folding it turns a memory access into an
     * immediate. Deliberately EXCLUDES the R+W segment, where .data.rel.ro and
     * the GOT live: those are written during relocation and must not be folded.
     * 0 when no such segment exists, which simply turns the folding off. */
    uint32_t text_vaddr;
    uint32_t text_size;

    /* Imported symbols of type STT_OBJECT: data the guest READS, so they get
     * a block of real guest memory each rather than an SVC trampoline (which
     * the guest would read as an instruction word). data_import_addrs[i] is
     * where data_import_names[i] lives; dependency modules populate the ones
     * whose contents matter -- notably bionic's ctype tables. */
    uint32_t data_import_count;
    char *data_import_names[64];
    uint32_t data_import_addrs[64];

    /* DT_INIT_ARRAY: table of C++ static/global constructor function
     * pointers the ELF loader normally calls automatically at load time
     * (before any other code runs). Entries are already so_base-relocated
     * by the time pvz2_elf_load() returns (covered by DT_REL like any other
     * data word). Caller is responsible for actually invoking them (see
     * run_init_array() in pvz2_run_test.cpp) -- pvz2_elf_load() only loads
     * and relocates, it doesn't execute guest code itself. */
    uint32_t init_array_vaddr;   /* 0 if absent */
    uint32_t init_array_count;   /* number of 4-byte function pointers */

    /* Set when the trampoline or data-import table ran out mid-relocation, which
     * leaves that symbol bound to address 0. It cannot be reported by a return
     * value -- it is discovered deep inside relocation, one symbol at a time --
     * so it is recorded here and pvz2_elf_load fails on it at the end.
     *
     * Worth failing on rather than warning about: a guest call through a null
     * pointer does not fault here. It runs into the zero page, NOP-slides down to
     * the $halt sentinel at trampoline index 0, and returns as though the
     * function had succeeded. */
    uint32_t import_overflow;
} pvz2_elf_image_t;

/* Loads and fully relocates libPVZ2.so (or any armeabi-v7a ET_DYN .so) into
 * a freshly allocated flat buffer. Returns 0 on success.
 * space_size is the total size of the emulated address space to allocate
 * (must be large enough for the .so image + trampoline table + stack).
 *
 * DT_NEEDED dependencies are mapped automatically, exactly as Android's linker
 * does it: for each name, a file of that name NEXT TO the main .so is mapped
 * into the same address space (transitively), and the whole set is relocated
 * together so cross-module references bind to real code. A DT_NEEDED entry with
 * no file beside the .so is simply skipped -- that is the normal case for
 * libc.so, libm.so, libGLESv2.so and friends, whose symbols fall through to the
 * host shim layer as they always have. So a directory holding only libPVZ2.so
 * (1.6, 4.5.2) behaves exactly as it did before this existed.
 *
 * That skip is the ONLY thing this tolerates. A dependency that IS shipped but
 * cannot be mapped -- table full, out of address space, not an ARM32 ELF --
 * fails the whole load, because the alternative is a boot that runs for a while
 * on symbols bound to nothing. */
int pvz2_elf_load(const char *path, uint32_t space_size, pvz2_elf_image_t *out);

/* Looks up a defined (exported) symbol's address in the emulated address
 * space by name, e.g. "Java_com_ea_EAThread_EAThread_Init". Searches every
 * mapped module, main image first. Returns 0 if not found or not defined
 * (SHN_UNDEF) anywhere. */
uint32_t pvz2_elf_find_symbol(const pvz2_elf_image_t *img, const char *name);

/* The same lookup restricted to ONE module.
 *
 * Needed because the search above returns the first match across the whole set,
 * which is right for "where does this function live" and wrong for the symbols
 * every library defines SEPARATELY -- JNI_OnLoad above all. libNimble.so has its
 * own, holding its own JavaVM* in its own global, and the whole-image search can
 * only ever return libPVZ2.so's. Skipping it left EA::Nimble::getEnv() calling
 * through a NULL vm. */
uint32_t pvz2_elf_find_symbol_in(const pvz2_elf_module_t *m, const char *name);

/* The module whose mapped range contains `pc`, or NULL. The unwinder needs this
 * to hand back the exception index table belonging to the throwing code rather
 * than the main image's. */
const pvz2_elf_module_t *pvz2_elf_module_for_pc(const pvz2_elf_image_t *img, uint32_t pc);

/* Mints an additional "SVC #index" stub after loading, for guest-callable host
 * code that corresponds to no imported symbol -- specifically the OpenSL ES
 * interface vtables, whose entries the guest calls through
 * `(*obj)->Method(obj, ...)` and which therefore must be real guest addresses.
 * Returns the stub's address, or 0 if the table is full. `name` is only used
 * for tracing. The caller must bind a handler to the matching index; see
 * make_guest_callback() in dependency.h, which does both. */
uint32_t pvz2_elf_add_trampoline(pvz2_elf_image_t *img, const char *name);

void pvz2_elf_free(pvz2_elf_image_t *img);

#ifdef __cplusplus
}
#endif

#endif
