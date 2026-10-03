#include <pvz_tv/dependencies/hle.h>

#include <pvz_tv/runtime/jit_tuning.h>

#include <cstring>

namespace pvz_tv {
namespace {

const GuestOverrides &overrides() {
    static const GuestOverrides o = [] {
        GuestOverrides list;
        register_hle_vorbis(list);
        register_hle_image(list);
        register_hle_reanim(list);
        return list;
    }();
    return o;
}

void put16(std::uint8_t *p, std::uint16_t v) { std::memcpy(p, &v, 2); }
void put32(std::uint8_t *p, std::uint32_t v) { std::memcpy(p, &v, 4); }

/* Overwrites the start of the function at `fn` (bit 0 set for Thumb) with a
 * jump to `target`, an ARM-state trampoline. LDR PC interworks, so the jump
 * also leaves Thumb state when it has to; LR is untouched, and the
 * trampoline's BX LR returns to whoever called the function. */
bool redirect(pvz2_elf_image_t *img, std::uint32_t fn, std::uint32_t target) {
    const std::uint32_t at = fn & ~1u;
    if (at + 12 > img->mem_size) return false;
    std::uint8_t *p = img->mem + at;
    if (fn & 1u) {
        /* Thumb: LDR.W PC, [PC, #0] reads the word at Align(insn + 4, 4). */
        if (at & 2u) {
            put16(p, 0xBF00); /* NOP, to put the load on a word boundary */
            p += 2;
        }
        put16(p, 0xF8DF);
        put16(p + 2, 0xF000);
        put32(p + 4, target);
    } else {
        put32(p, 0xE51FF004); /* LDR PC, [PC, #-4] */
        put32(p + 4, target);
    }
    return true;
}

}  // namespace

void register_hle(ImportTable &t) {
    for (const auto &[function, fn] : overrides().list()) {
        t.add(("hle$" + function).c_str(), fn);
    }
}

unsigned install_guest_overrides(pvz2_elf_image_t *img) {
    if (!(jit_tuning() & kJitHostDecoders) || img->module_count == 0) return 0;
    unsigned installed = 0;
    for (const auto &[function, fn] : overrides().list()) {
        const std::uint32_t addr = pvz2_elf_find_symbol_in(&img->modules[0], function.c_str());
        if (addr == 0) continue;
        const std::uint32_t trampoline = pvz2_elf_add_trampoline(img, ("hle$" + function).c_str());
        if (trampoline == 0) continue;
        if (redirect(img, addr, trampoline)) ++installed;
    }
    return installed;
}

}  // namespace pvz_tv
