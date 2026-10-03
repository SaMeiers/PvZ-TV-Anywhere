#pragma once

/* How every guest JIT is configured -- the main one, one per guest thread, and
 * the callback JITs. Android and the desktop player both go through
 * apply_jit_tuning(), so the two cannot drift apart again.
 *
 * Each knob is a bit, so a build can be measured with any combination without
 * recompiling: the debug.pvztv.jit system property (Android) or PVZTV_JIT in
 * the environment (desktop) replaces kDefaultJitTuning when set, as a decimal
 * or 0x-prefixed number. */

#include <cstdint>
#include <cstdlib>

#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/optimization_flags.h>

#include <pvz_tv/elf32/elf32_loader.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace pvz_tv {

enum JitTuning : std::uint32_t {
    /* Stop charging cycles at the end of every block. Nothing here uses guest
     * time -- the game reads the host clock -- so it was pure overhead. The
     * diagnostic build keeps counting regardless: its watchdog learns where a
     * thread is from AddTicks. */
    kJitNoCycleCounting = 1u << 0,

    /* Floating point the way the host does it rather than bit-exact ARM11:
     * NaN payloads, FPSCR rounding and flush-to-zero modes, fused multiply-add.
     * A game drawing sprites never notices; every FP-heavy block gets shorter. */
    kJitLooseFloat = 1u << 1,

    /* Skip the global exclusive monitor: LDREX/STREX become plain loads and
     * stores. Faster, but two guest threads racing on the same atomic can both
     * win -- off by default, here to be measured. */
    kJitIgnoreGlobalMonitor = 1u << 2,

    /* Reach guest memory through fastmem -- one host load or store per guest
     * one -- instead of a page-table walk on every access. Needs the loader's
     * 4 GiB reservation (mem_reserved_4g); without it this bit does nothing. */
    kJitFastmem = 1u << 3,

    /* Replace the decoders the game carries (Tremor, libpng) with host ones,
     * see dependencies/hle.h. Not a JIT setting, but measured the same way. */
    kJitHostDecoders = 1u << 4,

    /* List "neon" in the guest's /proc/cpuinfo (vfs::write_pseudo_cpuinfo).
     * libpng and FMOD both read it to pick their NEON code. */
    kJitGuestNeon = 1u << 5,
};

inline constexpr std::uint32_t kDefaultJitTuning =
    kJitNoCycleCounting | kJitLooseFloat | kJitFastmem | kJitHostDecoders | kJitGuestNeon;

inline std::uint32_t jit_tuning() {
    static const std::uint32_t value = [] {
        const char *text = nullptr;
#if defined(__ANDROID__)
        static char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.pvztv.jit", prop) > 0) text = prop;
#else
        text = std::getenv("PVZTV_JIT");
#endif
        if (text == nullptr || *text == '\0') return kDefaultJitTuning;
        return static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0));
    }();
    return value;
}

inline void apply_jit_tuning(Dynarmic::A32::UserConfig &config, const pvz2_elf_image_t &img) {
    const std::uint32_t t = jit_tuning();
    config.optimizations = Dynarmic::all_safe_optimizations;
    if ((t & kJitFastmem) && img.mem_reserved_4g) {
        config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(img.mem);
        config.recompile_on_fastmem_failure = true;
    }
#if !defined(PVZTV_DIAGNOSTICS)
    config.enable_cycle_counting = (t & kJitNoCycleCounting) == 0;
#endif
    if (t & (kJitLooseFloat | kJitIgnoreGlobalMonitor)) {
        config.unsafe_optimizations = true;
    }
    if (t & kJitLooseFloat) {
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA |
                                Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                                Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN |
                                Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    }
    if (t & kJitIgnoreGlobalMonitor) {
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
    }
}

}  // namespace pvz_tv
