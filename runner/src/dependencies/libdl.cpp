/* libdl.so -- the dynamic loader interface.
 *
 * Implements dlopen/dlsym/dlerror/dladdr over the emulated ELF32 image.
 * This is crucial for libHomura.so, which dynamically opens libGameMain.so
 * and resolves game functions, fonts, and structures via dlsym.
 */

#include <pvz_tv/dependencies/dependency.h>

#include <cstring>
#include <string>

namespace pvz_tv {
namespace {

constexpr uint32_t kHandleGameMain = 0x20000001;
constexpr uint32_t kHandleHomura   = 0x20000002;
constexpr uint32_t kHandleSelf     = 0x20000000;

void dl_open(GuestCall &c) {
    uint32_t path_ptr = c.arg(0);
    uint32_t flags = c.arg(1);
    if (path_ptr == 0) {
        c.trace("[libdl] dlopen(NULL, 0x%x) -> handle 0x%08X", flags, kHandleSelf);
        c.set_result(kHandleSelf);
        return;
    }
    std::string name = c.cstr(path_ptr, 512);
    c.trace("[libdl] dlopen(\"%s\", 0x%x) [caller lr=0x%08X]", name.c_str(), flags, c.lr());

    if (c.img) {
        for (uint32_t i = 0; i < c.img->module_count; ++i) {
            if (name.find(c.img->modules[i].name) != std::string::npos ||
                std::string(c.img->modules[i].name).find(name) != std::string::npos) {
                c.trace("[libdl] dlopen(\"%s\") -> module[%u] '%s' handle 0x%08X",
                      name.c_str(), i, c.img->modules[i].name, 0x20000001 + i);
                c.set_result(0x20000001 + i);
                return;
            }
        }
    }
    c.set_result(kHandleSelf);
}

void dl_sym(GuestCall &c) {
    uint32_t handle = c.arg(0);
    uint32_t sym_ptr = c.arg(1);
    if (!sym_ptr) {
        c.set_result(0);
        return;
    }
    std::string sym_name = c.cstr(sym_ptr, 512);
    uint32_t addr = 0;

    if (c.img) {
        if (handle >= 0x20000001 && handle < 0x20000001 + c.img->module_count) {
            uint32_t idx = handle - 0x20000001;
            addr = pvz2_elf_find_symbol_in(&c.img->modules[idx], sym_name.c_str());
        }

        if (addr == 0) {
            addr = pvz2_elf_find_symbol(c.img, sym_name.c_str());
        }

        if (addr == 0) {
            for (uint32_t i = 0; i < c.img->trampoline_count; ++i) {
                if (std::strcmp(c.img->trampoline_names[i], sym_name.c_str()) == 0) {
                    addr = c.img->trampoline_base + i * 4;
                    break;
                }
            }
        }

        if (addr == 0) {
            const auto &table = import_table();
            if (table.find(sym_name.c_str()) != nullptr) {
                addr = pvz2_elf_add_trampoline(c.img, sym_name.c_str());
            }
        }
    }

    if (addr == 0) // a resolved symbol is the normal case and there are thousands of them
        c.trace("[libdl] dlsym(0x%08X, \"%s\") NOT FOUND [caller lr=0x%08X]", handle, sym_name.c_str(), c.lr());
    c.set_result(addr);
}

void dl_close(GuestCall &c) {
    c.set_result(0);
}

void dl_error(GuestCall &c) {
    // POSIX dlerror() returns NULL when no error occurred.
    c.set_result(0);
}

void dl_addr(GuestCall &c) {
    uint32_t addr = c.arg(0);
    uint32_t info_ptr = c.arg(1);
    if (!info_ptr || !c.img || !c.in_bounds(info_ptr, 16)) {
        c.set_result(0);
        return;
    }
    const pvz2_elf_module_t *m = pvz2_elf_module_for_pc(c.img, addr);
    if (!m) {
        c.set_result(0);
        return;
    }
    c.write32(info_ptr + 0, c.dup_cstr(m->name));
    c.write32(info_ptr + 4, m->base);
    c.write32(info_ptr + 8, 0);
    c.write32(info_ptr + 12, 0);
    c.set_result(1);
}

}  // namespace

void register_libdl(ImportTable &t) {
    t.add("dlopen", dl_open);
    t.add("dlsym", dl_sym);
    t.add("dlclose", dl_close);
    t.add("dlerror", dl_error);
    t.add("dladdr", dl_addr);
}

}  // namespace pvz_tv
