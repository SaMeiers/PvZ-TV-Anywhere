/* The output back end shared by the runner -- see diagnostics.h for what the
 * two builds are and what belongs in each. */

#include <pvz_tv/diagnostics.h>

#include <pvz_tv/elf32/elf32_loader.h>

#include <cstring>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace pvz_tv {
namespace diag {

/* One lock for everything the runner prints: guest threads run concurrently,
 * and half-interleaved lines are worse than no lines at all. */
static std::mutex s_output_lock;
static std::atomic<int> s_level{-1};

static int read_initial_level() {
    if (!kBuiltIn) return 0;

    const char *text = nullptr;
#if defined(__ANDROID__)
    /* `adb shell setprop debug.pvztv.trace 2` before launching the app. */
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.pvztv.trace", prop) > 0) text = prop;
#else
    text = std::getenv("PVZTV_TRACE");
#endif
    /* A diagnostic build traces host calls unless told otherwise; that is the
     * whole reason someone installed it. */
    if (text == nullptr || *text == '\0') return 1;
    return std::atoi(text);
}

int level() {
    int current = s_level.load(std::memory_order_relaxed);
    if (current < 0) {
        current = read_initial_level();
        s_level.store(current, std::memory_order_relaxed);
    }
    return current;
}

void set_level(int level) {
    s_level.store(level < 0 ? 0 : level, std::memory_order_relaxed);
}

static void emit(bool is_trace, const char *fmt, std::va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);

#if defined(__ANDROID__)
    /* stdout goes nowhere in an Android app process; logcat is the console.
     * Traces get their own tag so `logcat RunnerTrace:I *:S` isolates them. */
    __android_log_write(ANDROID_LOG_INFO, is_trace ? "RunnerTrace" : "RunnerGuest", buf);
#else
    (void)is_trace;
    std::lock_guard<std::mutex> lg(s_output_lock);
    std::printf("pvz2: %s\n", buf);
    std::fflush(stdout);
#endif
}

void vreport(const char *fmt, std::va_list ap) { emit(false, fmt, ap); }

void report(const char *fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    emit(false, fmt, ap);
    va_end(ap);
}

void vtrace(const char *fmt, std::va_list ap) {
    if (!tracing()) return;
    emit(true, fmt, ap);
}

void trace(const char *fmt, ...) {
    if (!tracing()) return;
    std::va_list ap;
    va_start(ap, fmt);
    emit(true, fmt, ap);
    va_end(ap);
}

static bool name_in(const char *name, const char *const *list, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (std::strcmp(name, list[i]) == 0) return true;
    }
    return false;
}

/* The handlers whose first argument is a path worth printing: a call that goes
 * wrong in one of these is nearly always about the path itself. */
static bool takes_path(const char *name) {
    static const char *const kNames[] = {
        "access", "fopen", "freopen", "open", "open64", "mkdir", "opendir",
        "unlink", "remove", "rename", "gzopen",
    };
    return name_in(name, kNames, sizeof(kNames) / sizeof(*kNames));
}

/* Calls the guest makes constantly and that say nothing about what it is
 * doing; printing them buries everything else. */
static bool is_noise(const char *name) {
    static const char *const kNames[] = {
        "memcmp", "memcpy", "memmove", "memset", "strlen", "strcmp", "malloc",
        "free", "calloc", "realloc", "pthread_mutex_lock", "pthread_mutex_unlock",
        "clock_gettime", "gettimeofday", "select", "stat", "readdir", "fnmatch",
        "sprintf", "strdup", "pow", "sin", "cos", "atan2",
    };
    return name_in(name, kNames, sizeof(kNames) / sizeof(*kNames));
}

void trace_svc(const pvz2_elf_image_t *img, unsigned swi, const std::uint32_t *regs, unsigned tid) {
    if (!tracing_svc() || img == nullptr || regs == nullptr) return;

    const char *name = (swi < img->trampoline_count && img->trampoline_names[swi])
                           ? img->trampoline_names[swi]
                           : "?";
    if (is_noise(name)) return;

    char path[256] = {0};
    const bool with_path = takes_path(name);
    if (with_path) {
        const std::uint32_t at = regs[0];
        for (std::size_t i = 0; i + 1 < sizeof(path); ++i) {
            const std::uint32_t addr = at + (std::uint32_t)i;
            if (addr >= img->mem_size) break;
            path[i] = (char)img->mem[addr];
            if (path[i] == '\0') break;
        }
    }

    trace("[SVC tid=%u] #%u %s (lr=0x%08X, r0=0x%08X, r1=0x%08X)%s%s", tid, swi, name,
          regs[14], regs[0], regs[1], with_path ? " path=" : "", with_path ? path : "");
}

const char *build_name() { return kBuiltIn ? "diagnostic" : "normal"; }

}  // namespace diag
}  // namespace pvz_tv
