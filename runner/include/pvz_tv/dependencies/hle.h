#pragma once

/* Guest functions replaced by host implementations.
 *
 * Everything else in src/dependencies/ implements what the game *imports*. A
 * few things it carries itself are worth taking over too, when running them
 * under the JIT costs far more than they are worth -- a statically linked
 * decoder above all. Each override names an exported function of the game's
 * main library; install_guest_overrides() gives it a trampoline like any
 * import's and rewrites the function's first instructions into a jump there,
 * so every caller, direct or not, lands in the host handler and returns
 * straight to its own caller.
 *
 * Install before any guest code runs and before the handler table is built;
 * the trampolines are named "hle$<function>" and resolve through
 * import_table() like everything else. */

#include <string>
#include <utility>
#include <vector>

#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/elf32/elf32_loader.h>

namespace pvz_tv {

class GuestOverrides {
public:
    void add(const char *function, ImportHandler fn) { list_.emplace_back(function, fn); }
    const std::vector<std::pair<std::string, ImportHandler>> &list() const { return list_; }

private:
    std::vector<std::pair<std::string, ImportHandler>> list_;
};

/* Adds every override's handler to the import table, as "hle$<function>". */
void register_hle(ImportTable &t);

/* Redirects the overridden functions of img's main module. Returns how many
 * were installed; none when the host decoders are turned off (see
 * kJitHostDecoders in jit_tuning.h). */
unsigned install_guest_overrides(pvz2_elf_image_t *img);

void register_hle_vorbis(GuestOverrides &o);
void register_hle_image(GuestOverrides &o);

}  // namespace pvz_tv
