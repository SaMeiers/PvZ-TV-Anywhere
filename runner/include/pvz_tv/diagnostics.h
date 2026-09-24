#pragma once

/* Two builds out of one source tree.
 *
 * The normal build is what players get: it reports what went wrong -- guest
 * asserts, refused allocations, stuck threads, the game's own log -- and stays
 * quiet about everything else.
 *
 * The diagnostic build adds the running commentary that is only worth having
 * when something has already gone wrong: every host call the guest makes, every
 * file it opens, every socket it creates. That commentary costs a line of
 * output per guest libc call, which is why it is compiled in only when
 * PVZTV_DIAGNOSTICS is defined (CMake: -DPVZTV_DIAGNOSTICS=ON, which the
 * desktop diagnostic build and the Android relWithDebInfo build set).
 *
 * Use it through PVZTV_TRACE() for free functions and GuestCall::trace() inside
 * dependency handlers. Both vanish without a trace in the normal build --
 * including their arguments, so anything with a side effect belongs outside.
 */

#include <cstdarg>
#include <mutex>

namespace pvz_tv {
namespace diag {

#if defined(PVZTV_DIAGNOSTICS)
inline constexpr bool kBuiltIn = true;
#else
inline constexpr bool kBuiltIn = false;
#endif

/* How much of that commentary to print. The default is level 1 in a diagnostic
 * build and 0 otherwise; it is read once, from PVZTV_TRACE in the environment
 * (desktop) or the debug.pvztv.trace system property (Android).
 *
 *   0  off -- only reports of things going wrong
 *   1  host calls: files, sockets, threads, dynamic symbols
 *   2  every SVC the guest makes, with its arguments
 */
int level();
void set_level(int level);

inline bool tracing() { return kBuiltIn && level() >= 1; }
inline bool tracing_svc() { return kBuiltIn && level() >= 2; }

/* Prints one line, prefixed and serialised like the rest of the runner's
 * output. Does nothing unless tracing() holds. */
void trace(const char *fmt, ...);
void vtrace(const char *fmt, std::va_list ap);

/* Emits a line unconditionally: for things going wrong, in both builds. Prefer
 * this over printf in runner code -- an Android app process has no stdout, so a
 * printf there reports to nobody. */
void report(const char *fmt, ...);
void vreport(const char *fmt, std::va_list ap);

/* "diagnostic" or "normal", for the banner the player prints at startup. */
const char *build_name();

}  // namespace diag
}  // namespace pvz_tv

#define PVZTV_TRACE(...)                                     \
    do {                                                     \
        if (::pvz_tv::diag::kBuiltIn) {                      \
            ::pvz_tv::diag::trace(__VA_ARGS__);              \
        }                                                    \
    } while (0)
