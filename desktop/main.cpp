#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <memory>
#include <array>
#include <vector>
#include <string>
#include <fstream>
#include <exception>
#include <filesystem>

#include <SDL.h>
#include <glad/gl.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#else
#include <unistd.h>
#endif

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <pvz_tv/elf32/elf32_loader.h>
#include <pvz_tv/runtime/guest_runtime.h>
#include <pvz_tv/runtime/guest_memmap.h>
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/surface.h>
#include <pvz_tv/gfx/gl_requirements.h>
#include <pvz_tv/config.h>
#include <pvz_tv/diagnostics.h>

// Global window handle for EGL layer
SDL_Window *g_sdl_window = nullptr;
SDL_GLContext g_gl_context = nullptr;

namespace {

constexpr uint32_t kStackTop = 0x1FE00000; // 510 MB
constexpr uint32_t kThreadStacksTop = 0x1FC00000; // 508 MB
constexpr uint32_t kHeapBase = 0x01000000; // 16 MB (above SOs)
constexpr uint32_t kHeapSize = 0x1C000000; // 448 MB heap (ends at 0x1D000000)

class PvzTvGuestEnv;
struct ActiveJitInfo {
    uint32_t tid;
    const char *name;
    Dynarmic::A32::Jit *jit;
    PvzTvGuestEnv *env;
};
static std::mutex g_jits_lock;
static std::vector<ActiveJitInfo> g_active_jits;

static void register_active_jit(uint32_t tid, const char *name, Dynarmic::A32::Jit *jit, PvzTvGuestEnv *env) {
    std::lock_guard<std::mutex> lk(g_jits_lock);
    g_active_jits.push_back({tid, name, jit, env});
}

static void unregister_active_jit(Dynarmic::A32::Jit *jit) {
    std::lock_guard<std::mutex> lk(g_jits_lock);
    for (auto it = g_active_jits.begin(); it != g_active_jits.end(); ++it) {
        if (it->jit == jit) {
            g_active_jits.erase(it);
            break;
        }
    }
}

class PvzTvGuestEnv final : public Dynarmic::A32::UserCallbacks {
public:
    pvz2_elf_image_t *img = nullptr;
    pvz_tv::GuestRuntime *rt = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    Dynarmic::ExclusiveMonitor *monitor = nullptr;
    std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> *page_table = nullptr;
    const std::vector<pvz_tv::ImportHandler> *handlers = nullptr;
    bool should_halt = false;
    uint64_t ticks_used = 0;
    uint32_t svc_calls = 0;
    std::atomic<uint32_t> current_swi{0xFFFFFFFF};
    std::atomic<uint32_t> current_lr{0};
    std::atomic<uint32_t> current_pc{0};
    std::atomic<const char*> current_state{"idle"};

    void AddTicks(uint64_t ticks) override {
        ticks_used += ticks;
    }
    uint64_t GetTicksRemaining() override {
        return 10000000;
    }

    bool in_bounds(uint32_t addr, uint32_t sz) const {
        return addr < img->mem_size && (uint64_t)addr + sz <= img->mem_size;
    }

    uint8_t MemoryRead8(uint32_t vaddr) override {
        return in_bounds(vaddr, 1) ? img->mem[vaddr] : 0;
    }
    uint16_t MemoryRead16(uint32_t vaddr) override {
        uint16_t v = 0;
        if (in_bounds(vaddr, 2)) memcpy(&v, &img->mem[vaddr], 2);
        return v;
    }
    uint32_t MemoryRead32(uint32_t vaddr) override {
        uint32_t v = 0;
        if (in_bounds(vaddr, 4)) memcpy(&v, &img->mem[vaddr], 4);
        return v;
    }
    uint64_t MemoryRead64(uint32_t vaddr) override {
        uint64_t v = 0;
        if (in_bounds(vaddr, 8)) memcpy(&v, &img->mem[vaddr], 8);
        return v;
    }

    void MemoryWrite8(uint32_t vaddr, uint8_t value) override {
        if (in_bounds(vaddr, 1)) img->mem[vaddr] = value;
    }
    void MemoryWrite16(uint32_t vaddr, uint16_t value) override {
        if (in_bounds(vaddr, 2)) memcpy(&img->mem[vaddr], &value, 2);
    }
    void MemoryWrite32(uint32_t vaddr, uint32_t value) override {
        if (in_bounds(vaddr, 4)) memcpy(&img->mem[vaddr], &value, 4);
    }
    void MemoryWrite64(uint32_t vaddr, uint64_t value) override {
        if (in_bounds(vaddr, 8)) memcpy(&img->mem[vaddr], &value, 8);
    }

    bool MemoryWriteExclusive8(uint32_t vaddr, uint8_t value, uint8_t) override {
        MemoryWrite8(vaddr, value);
        return true;
    }
    bool MemoryWriteExclusive16(uint32_t vaddr, uint16_t value, uint16_t) override {
        MemoryWrite16(vaddr, value);
        return true;
    }
    bool MemoryWriteExclusive32(uint32_t vaddr, uint32_t value, uint32_t) override {
        MemoryWrite32(vaddr, value);
        return true;
    }
    bool MemoryWriteExclusive64(uint32_t vaddr, uint64_t value, uint64_t) override {
        MemoryWrite64(vaddr, value);
        return true;
    }

    std::optional<uint32_t> MemoryReadCode(uint32_t vaddr) override {
        if ((vaddr & 0xFFFF0000) == 0xFFFF0000) {
            // Linux kernel kuser_helper page (ARM native instructions)
            switch (vaddr) {
            // __kuser_cmpxchg (r0=oldval, r1=newval, r2=ptr)
            case 0xFFFF0FC0: return 0xE1923F9F; // ldrex   r3, [r2]
            case 0xFFFF0FC4: return 0xE0533000; // subs    r3, r3, r0
            case 0xFFFF0FC8: return 0x01823F91; // strexeq r3, r1, [r2]
            case 0xFFFF0FCC: return 0x03330000; // teqeq   r3, #0
            case 0xFFFF0FD0: return 0xE2730000; // rsbs    r0, r3, #0
            case 0xFFFF0FD4: return 0xE12FFF1E; // bx      lr

            // __kuser_memory_barrier
            case 0xFFFF0FA0: return 0xF57FF05B; // dmb     ish
            case 0xFFFF0FA4: return 0xE12FFF1E; // bx      lr

            // __kuser_helper_version
            case 0xFFFF0FFC: return 5;

            default:
                return 0xE12FFF1E; // bx lr
            }
        }
        if (!in_bounds(vaddr, 4)) return std::nullopt;
        uint32_t v;
        memcpy(&v, &img->mem[vaddr], 4);
        return v;
    }

    void InterpreterFallback(uint32_t pc, size_t num_instructions) override {
        printf("[-] Interpreter fallback at 0x%08X (%zu instrs)\n", pc, num_instructions);
    }

    bool trace_svc = false;

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (exception == Dynarmic::A32::Exception::Yield ||
            exception == Dynarmic::A32::Exception::WaitForInterrupt ||
            exception == Dynarmic::A32::Exception::WaitForEvent ||
            exception == Dynarmic::A32::Exception::SendEvent ||
            exception == Dynarmic::A32::Exception::SendEventLocal) {
            std::this_thread::yield();
            return;
        }
        printf("[-] Exception %d raised at PC=0x%08X (LR=0x%08X)\n", (int)exception, pc, jit->Regs()[14]);
        for (int i = 0; i < 16; ++i) {
            printf("    r%-2d = 0x%08X%s", i, jit->Regs()[i], (i % 4 == 3) ? "\n" : "  ");
        }
        printf("    cpsr = 0x%08X\n", jit->Cpsr());
        uint32_t sp = jit->Regs()[13];
        printf("    Stack dump around SP=0x%08X:\n", sp);
        for (int i = 0; i < 16; ++i) {
            uint32_t val = MemoryRead32(sp + i * 4);
            printf("      [SP+0x%02X] = 0x%08X\n", i * 4, val);
        }
        should_halt = true;
        jit->HaltExecution();
    }

    void return_to_caller() {
        uint32_t lr = jit->Regs()[14];
        jit->Regs()[15] = lr & ~1u;
        if ((lr & 1u) != 0) {
            jit->SetCpsr(jit->Cpsr() | 0x20u); // Resume in Thumb
        } else {
            jit->SetCpsr(jit->Cpsr() & ~0x20u); // Resume in ARM
        }
    }

    struct CallbackJitSlot {
        std::unique_ptr<PvzTvGuestEnv> env;
        std::unique_ptr<Dynarmic::A32::Jit> jit;
        bool in_use = false;
        uint32_t id = 0;
    };
    static inline std::mutex s_cb_mutex;
    static inline std::vector<CallbackJitSlot> s_cb_slots;

    uint32_t run_guest_callback(uint32_t fn, const uint32_t *args, int nargs) {
        if (!fn || !monitor || !page_table) return 0;

        CallbackJitSlot *slot = nullptr;
        size_t slot_idx = 0;
        {
            std::lock_guard<std::mutex> lk(s_cb_mutex);
            for (size_t i = 0; i < s_cb_slots.size(); ++i) {
                if (!s_cb_slots[i].in_use) {
                    slot = &s_cb_slots[i];
                    slot_idx = i;
                    slot->in_use = true;
                    break;
                }
            }
            if (!slot) {
                s_cb_slots.emplace_back();
                slot = &s_cb_slots.back();
                slot_idx = s_cb_slots.size() - 1;
                slot->in_use = true;
                slot->id = 50 + (uint32_t)slot_idx;

                slot->env = std::make_unique<PvzTvGuestEnv>();
                slot->env->img = img;
                slot->env->rt = rt;
                slot->env->monitor = monitor;
                slot->env->page_table = page_table;
                slot->env->handlers = handlers;
                slot->env->trace_svc = false;

                Dynarmic::A32::UserConfig nested_config;
                nested_config.callbacks = slot->env.get();
                nested_config.global_monitor = monitor;
                nested_config.processor_id = slot->id;
                nested_config.page_table = page_table;
                nested_config.absolute_offset_page_table = true;
                nested_config.optimizations = Dynarmic::all_safe_optimizations;

                slot->jit = std::make_unique<Dynarmic::A32::Jit>(nested_config);
                slot->env->jit = slot->jit.get();
            }
        }

        auto *nested_jit = slot->jit.get();
        auto *nested_env = slot->env.get();
        nested_env->should_halt = false;

        uint32_t sp = (kStackTop - 0x40000) - ((uint32_t)slot_idx * 0x20000);

        for (int i = 0; i < nargs && i < 4; ++i) {
            nested_jit->Regs()[i] = args[i];
        }
        for (int i = nargs; i < 4; ++i) {
            nested_jit->Regs()[i] = 0;
        }
        if (nargs > 4) {
            int extra = nargs - 4;
            sp -= (extra * 4 + 7) & ~7;
            for (int i = 0; i < extra; ++i) {
                *(uint32_t*)&img->mem[sp + i * 4] = args[4 + i];
            }
        }
        nested_jit->Regs()[13] = sp;
        nested_jit->Regs()[14] = img->trampoline_base; // LR -> $halt

        uint32_t pc = fn;
        uint32_t cpsr = nested_jit->Cpsr();
        if (pc & 1) { pc &= ~1; cpsr |= 0x20; } else { cpsr &= ~0x20; }
        nested_jit->Regs()[15] = pc;
        nested_jit->SetCpsr(cpsr);

        while (!nested_env->should_halt && !rt->shutdown_requested.load(std::memory_order_acquire)) {
            nested_jit->ClearHalt();
            nested_jit->Run();
        }

        uint32_t result = nested_jit->Regs()[0];

        {
            std::lock_guard<std::mutex> lk(s_cb_mutex);
            slot->in_use = false;
        }

        return result;
    }

    uint32_t spawn_thread(uint32_t start_routine, uint32_t arg) {
        uint32_t stack_top;
        uint32_t id;
        {
            std::lock_guard<std::mutex> lock(rt->threads_lock);
            if (rt->next_stack_slot >= pvz_tv::kThreadStackMax) {
                printf("[-] Out of guest thread stack slots (max %u)\n", pvz_tv::kThreadStackMax);
                return 0;
            }
            stack_top = kThreadStacksTop - rt->next_stack_slot * 0x00100000;
            rt->next_stack_slot++;
            id = rt->next_thread_id++;
        }

        std::thread th([this, start_routine, arg, stack_top, id]() {
            pvz_tv::guest_tls::self_id = id;
            PvzTvGuestEnv thread_env;
            thread_env.img = img;
            thread_env.rt = rt;
            thread_env.monitor = monitor;
            thread_env.page_table = page_table;
            thread_env.handlers = handlers;
            thread_env.trace_svc = this->trace_svc;

            Dynarmic::A32::UserConfig thread_config;
            thread_config.callbacks = &thread_env;
            thread_config.global_monitor = monitor;
            thread_config.processor_id = id;
            thread_config.page_table = page_table;
            thread_config.absolute_offset_page_table = true;
            thread_config.optimizations = Dynarmic::all_safe_optimizations;

            Dynarmic::A32::Jit thread_jit(thread_config);
            thread_env.jit = &thread_jit;

            thread_jit.Regs()[0] = arg;
            thread_jit.Regs()[13] = stack_top;
            thread_jit.Regs()[14] = img->trampoline_base; // LR -> $halt

            uint32_t pc = start_routine;
            uint32_t cpsr = thread_jit.Cpsr();
            if (pc & 1) { pc &= ~1; cpsr |= 0x20; } else { cpsr &= ~0x20; }
            thread_jit.Regs()[15] = pc;
            thread_jit.SetCpsr(cpsr);

            PVZTV_TRACE("[+] Guest thread %u started at 0x%08X (arg=0x%08X, stack_top=0x%08X)",
                   id, start_routine, arg, stack_top);

            register_active_jit(id, "guest_thread", &thread_jit, &thread_env);
            while (!thread_env.should_halt && !rt->shutdown_requested.load(std::memory_order_acquire)) {
                thread_jit.ClearHalt();
                thread_jit.Run();
            }
            unregister_active_jit(&thread_jit);

            uint32_t retval = thread_jit.Regs()[0];
            PVZTV_TRACE("[+] Guest thread %u finished with retval=0x%08X", id, retval);

            {
                std::lock_guard<std::mutex> lock(rt->threads_lock);
                rt->thread_retvals[id] = retval;
            }

            {
                std::lock_guard<std::mutex> mlk(rt->mutexes_lock);
                for (auto &kv : rt->guest_mutexes) {
                    if (kv.second->owner == id) {
                        printf("[!] Thread %u exited while holding mutex 0x%08X! Unlocking automatically.\n", id, kv.first);
                        kv.second->unlock(id);
                    }
                }
            }
        });

        std::lock_guard<std::mutex> lock(rt->threads_lock);
        rt->threads[id] = std::move(th);
        return id;
    }

    uint32_t join_thread(uint32_t id) {
        std::thread th;
        {
            std::lock_guard<std::mutex> lock(rt->threads_lock);
            auto found = rt->threads.find(id);
            if (found != rt->threads.end()) {
                th = std::move(found->second);
                rt->threads.erase(found);
            }
        }
        if (th.joinable()) {
            th.join();
        }
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        auto it = rt->thread_retvals.find(id);
        return (it != rt->thread_retvals.end()) ? it->second : 0;
    }

    void CallSVC(uint32_t swi) override {
        ++svc_calls;
        struct SvcScope {
            PvzTvGuestEnv *e;
            SvcScope(PvzTvGuestEnv *env, uint32_t s) : e(env) {
                e->current_swi.store(s, std::memory_order_relaxed);
                if (e->jit) {
                    e->current_lr.store(e->jit->Regs()[14], std::memory_order_relaxed);
                    e->current_pc.store(e->jit->Regs()[15], std::memory_order_relaxed);
                }
                e->current_state.store("in_svc", std::memory_order_relaxed);
            }
            ~SvcScope() {
                e->current_state.store("running", std::memory_order_relaxed);
            }
        } scope(this, swi);

        if (swi == 0) {
            uint32_t syscall_num = jit ? jit->Regs()[7] : 0;
            if (syscall_num == 0x000f0002) {
                // Linux ARM __ARM_NR_cacheflush syscall from __clear_cache
                jit->Regs()[0] = 0;
                return;
            }
            uint32_t pc = jit ? jit->Regs()[15] : 0;
            bool is_trampoline_halt = (pc >= img->trampoline_base && pc < img->trampoline_base + 16);
            if (!is_trampoline_halt) {
                printf("[!] Guest inline syscall SVC #0 at PC=0x%08X (r7=0x%X), returning 0\n", pc, syscall_num);
                jit->Regs()[0] = 0;
                return;
            }
            // $halt sentinel
            should_halt = true;
            jit->HaltExecution();
            return;
        }

        if (trace_svc && swi < img->trampoline_count) {
            const char *name = img->trampoline_names[swi];
            if (strcmp(name, "memcmp") != 0 && strcmp(name, "memcpy") != 0 &&
                strcmp(name, "memmove") != 0 && strcmp(name, "memset") != 0 &&
                strcmp(name, "strlen") != 0 && strcmp(name, "strcmp") != 0 &&
                strcmp(name, "malloc") != 0 && strcmp(name, "free") != 0 &&
                strcmp(name, "calloc") != 0 && strcmp(name, "realloc") != 0 &&
                strcmp(name, "pthread_mutex_lock") != 0 && strcmp(name, "pthread_mutex_unlock") != 0 &&
                strcmp(name, "clock_gettime") != 0 && strcmp(name, "gettimeofday") != 0 &&
                strcmp(name, "select") != 0 && strcmp(name, "stat") != 0 &&
                strcmp(name, "readdir") != 0 && strcmp(name, "fnmatch") != 0 &&
                strcmp(name, "sprintf") != 0 && strcmp(name, "strdup") != 0 &&
                strcmp(name, "pow") != 0 && strcmp(name, "sin") != 0 &&
                strcmp(name, "cos") != 0 && strcmp(name, "atan2") != 0) {
                // For the calls that take a path, show it: a crash inside one of
                // these is nearly always about the path itself.
                const bool takes_path =
                    strcmp(name, "access") == 0 || strcmp(name, "fopen") == 0 ||
                    strcmp(name, "open") == 0 || strcmp(name, "mkdir") == 0 ||
                    strcmp(name, "opendir") == 0 || strcmp(name, "unlink") == 0;
                char path[256] = {0};
                if (takes_path) {
                    uint32_t p = jit->Regs()[0];
                    for (size_t i = 0; i + 1 < sizeof(path) && in_bounds(p + (uint32_t)i, 1); ++i) {
                        path[i] = (char)img->mem[p + i];
                        if (path[i] == '\0') break;
                    }
                }
                printf("[SVC tid=%u] #%u %s (lr=0x%08X, r0=0x%08X, r1=0x%08X)%s%s\n",
                       pvz_tv::guest_tls::self_id, swi, name, jit->Regs()[14], jit->Regs()[0], jit->Regs()[1],
                       takes_path ? " path=" : "", takes_path ? path : "");
            }
        }

        if (handlers && swi < handlers->size() && (*handlers)[swi]) {
            pvz_tv::GuestCall c;
            c.img = img;
            c.rt = rt;
            c.regs = jit->Regs().data();
            c.env = this;
            c.halt_fn = [](void *env, const char *why) {
                auto *e = static_cast<PvzTvGuestEnv*>(env);
                PVZTV_TRACE("[*] Guest requested halt: %s", why ? why : "unspecified");
                if (e->rt) e->rt->shutdown_requested.store(true, std::memory_order_release);
                e->should_halt = true;
                e->jit->HaltExecution();
            };
            c.call_guest_fn = [](void *env, uint32_t fn, const uint32_t *args, int nargs) -> uint32_t {
                auto *e = static_cast<PvzTvGuestEnv*>(env);
                return e->run_guest_callback(fn, args, nargs);
            };
            c.spawn_thread_fn = [](void *env, uint32_t start, uint32_t arg) -> uint32_t {
                auto *e = static_cast<PvzTvGuestEnv*>(env);
                return e->spawn_thread(start, arg);
            };
            c.join_thread_fn = [](void *env, uint32_t id) -> uint32_t {
                auto *e = static_cast<PvzTvGuestEnv*>(env);
                return e->join_thread(id);
            };

            (*handlers)[swi](c);

            if (c.returns) {
                return_to_caller();
            }
            return;
        }

        const char *name = (swi < img->trampoline_count) ? img->trampoline_names[swi] : "unknown";
        static std::vector<std::string> unhandled;
        bool seen = false;
        for (const auto &s : unhandled) {
            if (s == name) { seen = true; break; }
        }
        if (!seen) {
            unhandled.push_back(name);
            printf("    [!] Unhandled SVC #%u: %s (lr=0x%08x)\n", swi, name, jit->Regs()[14]);
        }
        jit->Regs()[0] = 0;
        return_to_caller();
    }
};

} // namespace

static uint32_t make_guest_string(pvz2_elf_image_t *img, pvz_tv::GuestRuntime *rt, const char *str) {
    size_t len = strlen(str);
    uint32_t rep = rt->heap.alloc((uint32_t)(len + 16));
    *(uint32_t*)&img->mem[rep + 0] = (uint32_t)len;
    *(uint32_t*)&img->mem[rep + 4] = (uint32_t)len;
    *(uint32_t*)&img->mem[rep + 8] = 1;
    memcpy(&img->mem[rep + 12], str, len + 1);
    return rep + 12;
}

static uint32_t make_guest_raw_string(pvz2_elf_image_t *img, pvz_tv::GuestRuntime *rt, const char *str) {
    size_t len = strlen(str);
    uint32_t p = rt->heap.alloc((uint32_t)(len + 1));
    memcpy(&img->mem[p], str, len + 1);
    return p;
}

uint32_t g_native_app_addr = 0;
uint32_t g_process_works_fn = 0;
uint32_t g_pipe_write_token = 0;

void setup_transmension_bridge(pvz2_elf_image_t *img, pvz_tv::GuestRuntime *rt) {
    uint32_t nativeBase = 0;
    for (uint32_t i = 0; i < img->module_count; ++i) {
        if (strstr(img->modules[i].name, "native_code")) {
            nativeBase = img->modules[i].base;
            break;
        }
    }
    if (!nativeBase) {
        printf("[-] libnative_code.so not found!\n");
        return;
    }

    printf("[*] Setting up Transmension NativeApp / BridgeApp bridge (nativeBase=0x%08X)...\n", nativeBase);

    // Allocate NativeApp (size 0x200) and BridgeApp (size 0x200)
    uint32_t nativeApp = rt->heap.alloc(0x200);
    uint32_t bridgeApp = rt->heap.alloc(0x200);

    memset(&img->mem[nativeApp], 0, 0x200);
    memset(&img->mem[bridgeApp], 0, 0x200);

    // NativeApp inner pointer: [nativeApp] points to self
    *(uint32_t*)&img->mem[nativeApp + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[nativeApp + 0x04] = make_guest_string(img, rt, "android_main");
    *(uint32_t*)&img->mem[nativeApp + 0x08] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x0c] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x10] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x14] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x18] = make_guest_string(img, rt, "com.popcap.pvz");
    *(uint32_t*)&img->mem[nativeApp + 0x1c] = make_guest_string(img, rt, "assets");
    *(uint32_t*)&img->mem[nativeApp + 0x20] = make_guest_string(img, rt, "1.1.5");
    *(uint32_t*)&img->mem[nativeApp + 0x24] = make_guest_string(img, rt, "115");
    *(uint32_t*)&img->mem[nativeApp + 0x28] = make_guest_string(img, rt, "fugu");
    *(uint32_t*)&img->mem[nativeApp + 0x2c] = make_guest_string(img, rt, "NexusPlayer");
    *(uint32_t*)&img->mem[nativeApp + 0x30] = make_guest_string(img, rt, "device123");
    *(uint32_t*)&img->mem[nativeApp + 0x34] = make_guest_string(img, rt, "android123");
    *(uint32_t*)&img->mem[nativeApp + 0x38] = make_guest_string(img, rt, "en_US");
    *(uint32_t*)&img->mem[nativeApp + 0x3c] = make_guest_string(img, rt, "en_US");

    *(uint32_t*)&img->mem[nativeApp + 0x84] = 115;
    *(uint32_t*)&img->mem[nativeApp + 0x88] = 21;  // SDK version (Android 5.0)
    *(uint32_t*)&img->mem[nativeApp + 0x8c] = 115; // Version code

    // Subsystems inside NativeApp
    uint32_t audioOut = rt->heap.alloc(0x80);
    uint32_t audioRec = rt->heap.alloc(0x80);
    uint32_t gameCenter = rt->heap.alloc(0x80);
    uint32_t shareMgr = rt->heap.alloc(0x80);
    uint32_t notifMgr = rt->heap.alloc(0x80);
    uint32_t inputMgr = rt->heap.alloc(0x80);
    memset(&img->mem[audioOut], 0, 0x80);
    memset(&img->mem[audioRec], 0, 0x80);
    memset(&img->mem[gameCenter], 0, 0x80);
    memset(&img->mem[shareMgr], 0, 0x80);
    memset(&img->mem[notifMgr], 0, 0x80);
    memset(&img->mem[inputMgr], 0, 0x80);

    *(uint32_t*)&img->mem[audioOut + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[audioRec + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[gameCenter + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[shareMgr + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[notifMgr + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[inputMgr + 0x00] = nativeApp;

    *(uint32_t*)&img->mem[nativeApp + 0xbc] = audioOut;
    *(uint32_t*)&img->mem[nativeApp + 0xc0] = audioRec;
    *(uint32_t*)&img->mem[nativeApp + 0xc4] = gameCenter;
    *(uint32_t*)&img->mem[nativeApp + 0xc8] = shareMgr;
    *(uint32_t*)&img->mem[nativeApp + 0xcc] = notifMgr;
    *(uint32_t*)&img->mem[nativeApp + 0xd0] = inputMgr;

    // Mark subsystems as already created/initialized so their create() returns immediately
    *(uint8_t*)&img->mem[inputMgr + 0x10] = 1;
    *(uint8_t*)&img->mem[gameCenter + 0x28] = 1;
    *(uint8_t*)&img->mem[shareMgr + 0x18] = 1;
    *(uint8_t*)&img->mem[notifMgr + 0x0c] = 1;

    // Self-pipe for NativeApp work loop
    int pipe_fds[2] = {-1, -1};
#if defined(_WIN32)
    _pipe(pipe_fds, 4096, _O_BINARY);
#else
    pipe(pipe_fds);
#endif
    uint32_t read_token = 0;
    uint32_t write_token = 0;
    {
        std::lock_guard<std::mutex> lk(rt->files_lock);
        read_token = rt->alloc_fd_token();
        rt->host_fds[read_token] = pipe_fds[0];
        write_token = rt->alloc_fd_token();
        rt->host_fds[write_token] = pipe_fds[1];
    }
    *(uint32_t*)&img->mem[nativeApp + 0x100] = read_token;
    *(uint32_t*)&img->mem[nativeApp + 0x104] = write_token;

    // Circular lists inside NativeApp: sentinel.next = sentinel.prev = &sentinel
    *(uint32_t*)&img->mem[nativeApp + 0x4c] = nativeApp + 0x44;
    *(uint32_t*)&img->mem[nativeApp + 0x50] = nativeApp + 0x44;
    *(uint32_t*)&img->mem[nativeApp + 0x64] = nativeApp + 0x5c;
    *(uint32_t*)&img->mem[nativeApp + 0x68] = nativeApp + 0x5c;
    *(uint32_t*)&img->mem[nativeApp + 0xf8] = nativeApp + 0xf8;
    *(uint32_t*)&img->mem[nativeApp + 0xfc] = nativeApp + 0xf8;
    *(uint32_t*)&img->mem[nativeApp + 0x108] = nativeApp + 0x108;
    *(uint32_t*)&img->mem[nativeApp + 0x10c] = nativeApp + 0x108;
    *(uint32_t*)&img->mem[nativeApp + 0x13c] = nativeApp + 0x13c;
    *(uint32_t*)&img->mem[nativeApp + 0x140] = nativeApp + 0x13c;
    *(uint32_t*)&img->mem[nativeApp + 0x144] = nativeApp + 0x144;
    *(uint32_t*)&img->mem[nativeApp + 0x148] = nativeApp + 0x144;
    *(uint32_t*)&img->mem[nativeApp + 0x110] = 1; // main thread ID
    *(uint32_t*)&img->mem[nativeApp + 0x120] = 1; // app thread ID
    *(uint32_t*)&img->mem[nativeApp + 0x158] = 0;
    *(uint32_t*)&img->mem[nativeApp + 0x15c] = 0;
    *(uint32_t*)&img->mem[nativeApp + 0x160] = nativeApp + 0x158;
    *(uint32_t*)&img->mem[nativeApp + 0x164] = nativeApp + 0x158;

    *(uint32_t*)&img->mem[nativeApp + 0xa8] = bridgeApp; // AppDelegate pointer

    // BridgeApp
    uint32_t vtable = nativeBase + 0x4e8f8 + 8;
    *(uint32_t*)&img->mem[bridgeApp + 0x00] = vtable;
    *(uint32_t*)&img->mem[bridgeApp + 0x04] = nativeApp;
    *(uint32_t*)&img->mem[bridgeApp + 0x08] = bridgeApp + 0x08;
    *(uint32_t*)&img->mem[bridgeApp + 0x0c] = bridgeApp + 0x08;
    // Native::EventDispatcher lists at bridgeApp + 0x28, 0x30, 0x38
    *(uint32_t*)&img->mem[bridgeApp + 0x28] = bridgeApp + 0x28;
    *(uint32_t*)&img->mem[bridgeApp + 0x2c] = bridgeApp + 0x28;
    *(uint32_t*)&img->mem[bridgeApp + 0x30] = bridgeApp + 0x30;
    *(uint32_t*)&img->mem[bridgeApp + 0x34] = bridgeApp + 0x30;
    *(uint32_t*)&img->mem[bridgeApp + 0x38] = bridgeApp + 0x38;
    *(uint32_t*)&img->mem[bridgeApp + 0x3c] = bridgeApp + 0x38;
    *(uint32_t*)&img->mem[bridgeApp + 0x70] = make_guest_raw_string(img, rt, "en_US");
    *(uint32_t*)&img->mem[bridgeApp + 0x74] = 3; // touchscreen = 3 (ACONFIGURATION_TOUCHSCREEN_FINGER)
    *(uint32_t*)&img->mem[bridgeApp + 0x78] = 1; // keyboard
    *(uint32_t*)&img->mem[bridgeApp + 0x88] = 1280; // width
    *(uint32_t*)&img->mem[bridgeApp + 0x8c] = 720;  // height
    *(uint32_t*)&img->mem[bridgeApp + 0x90] = 1280; // surface width
    *(uint32_t*)&img->mem[bridgeApp + 0x94] = 720;  // surface height
    *(uint32_t*)&img->mem[bridgeApp + 0x9c] = 0x4e800001u; // kEglDisplay
    *(uint32_t*)&img->mem[bridgeApp + 0xa0] = 0x4e800003u; // kEglSurface
    *(uint32_t*)&img->mem[bridgeApp + 0xa4] = 0x4e800004u; // kEglContext

    // Write BridgeApp singleton pointer at nativeBase + 0x4f014
    *(uint32_t*)&img->mem[nativeBase + 0x4f014] = bridgeApp;

    g_native_app_addr = nativeApp;
    g_pipe_write_token = write_token;
    g_process_works_fn = nativeBase + 0x16b55; // Thumb mode

    printf("[+] BridgeApp singleton configured at 0x%08X (BridgeApp=0x%08X, NativeApp=0x%08X, processWorks=0x%08X)\n",
           nativeBase + 0x4f014, bridgeApp, nativeApp, g_process_works_fn);
}

// Where this executable lives, so the player can find the game next to itself
// instead of depending on the directory it happens to be started from.
static std::filesystem::path executable_directory(const char *argv0) {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
#else
    std::error_code ec;
    auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return self.parent_path();
#endif
    std::error_code ec2;
    auto from_argv = std::filesystem::absolute(argv0 ? argv0 : ".", ec2);
    return ec2 ? std::filesystem::current_path() : from_argv.parent_path();
}

// An exception that escapes to std::terminate otherwise aborts the process
// with nothing printed, which says nothing about what went wrong.
static void report_unhandled_exception() {
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception &ex) {
            fprintf(stderr, "[-] fatal: unhandled exception: %s\n", ex.what());
        } catch (...) {
            fprintf(stderr, "[-] fatal: unhandled exception of unknown type\n");
        }
    } else {
        fprintf(stderr, "[-] fatal: terminate called\n");
    }
    fflush(stderr);
    std::abort();
}

int main(int argc, char *argv[]) {
    std::set_terminate(report_unhandled_exception);
#if defined(_WIN32)
    // Guest paths are UTF-8 (the asset packs ship with Chinese file names), so
    // the narrow CRT and std::filesystem have to speak UTF-8 as well; otherwise
    // converting such a path throws "no mapping for the Unicode character".
    // Only LC_CTYPE is switched, to keep the C conventions for number parsing.
    std::setlocale(LC_CTYPE, ".UTF8");
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("=========================================\n");
    printf("   PvZ-TV-Native: Android TV 64-bit Port \n");
    printf("   %s build\n", pvz_tv::diag::build_name());
    printf("=========================================\n\n");

    // An explicit path is resolved against the caller's directory; everything
    // else (assets, saves, the guest libraries) lives next to the executable.
    std::filesystem::path explicitSo;
    if (argc > 1) {
        std::error_code ec;
        explicitSo = std::filesystem::absolute(argv[1], ec);
    }
    const std::filesystem::path exeDir = executable_directory(argv[0]);
    std::error_code chdirEc;
    std::filesystem::current_path(exeDir, chdirEc);
    if (chdirEc) {
        fprintf(stderr, "[-] Could not enter %s: %s\n",
                exeDir.string().c_str(), chdirEc.message().c_str());
    }

    pvz2_config_load(nullptr, "./");
#if defined(_WIN32)
    timeBeginPeriod(1);
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // Initialize SDL2 Video & Audio
    printf("[*] Initializing SDL2...\n");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[-] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Configure OpenGL 2.0 Compatibility profile
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    int winWidth = 1280;
    int winHeight = 720;
    g_sdl_window = SDL_CreateWindow(
        "Plants vs. Zombies (Android TV) - Native PC Loader",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        winWidth, winHeight,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!g_sdl_window) {
        fprintf(stderr, "[-] Failed to create SDL window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowBordered(g_sdl_window, SDL_TRUE);
    SDL_SetWindowResizable(g_sdl_window, SDL_TRUE);

    g_gl_context = SDL_GL_CreateContext(g_sdl_window);
    if (!g_gl_context) {
        fprintf(stderr, "[-] Failed to create OpenGL context: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_MakeCurrent(g_sdl_window, g_gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Load OpenGL entry points via GLAD
    int gladVersion = gladLoaderLoadGL();
    if (!gladVersion) {
        fprintf(stderr, "[-] Failed to initialize GLAD!\n");
        return 1;
    }
    printf("[+] OpenGL initialized: version %d.%d\n", GLAD_VERSION_MAJOR(gladVersion), GLAD_VERSION_MINOR(gladVersion));
    pvz2_gl_check_requirements(g_sdl_window);

    pvz2_surface_set(winWidth, winHeight);

    // Load ARM32 SOs
    // The guest libraries are not shipped with the player. They are looked up
    // next to the executable, either loose or in libs/; the loader then picks
    // up libHomura.so and the others from that same folder.
    std::string soPathStorage;
    if (!explicitSo.empty()) {
        soPathStorage = explicitSo.string();
    } else {
        for (const char *dir : { ".", "libs" }) {
            auto candidate = std::filesystem::path(dir) / "libGameMain.so";
            if (std::filesystem::exists(candidate)) {
                soPathStorage = candidate.string();
                break;
            }
        }
    }

    if (soPathStorage.empty() || !std::filesystem::exists(soPathStorage)) {
        char message[512];
        snprintf(message, sizeof(message),
                 "libGameMain.so was not found.\n\n"
                 "Put the game's libraries (libGameMain.so, libHomura.so,\n"
                 "libGameRegister.so, libnative_code.so, libfmodex.so) and its\n"
                 "assets folder next to this program, in:\n\n%s",
                 exeDir.string().c_str());
        fprintf(stderr, "[-] %s\n", message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "PvZ TV", message, g_sdl_window);
        return 1;
    }
    const char *soPath = soPathStorage.c_str();

    printf("[*] Target SO: %s\n", soPath);

    uint32_t spaceSize = 512 * 1024 * 1024;
    pvz2_elf_image_t image;
    memset(&image, 0, sizeof(image));

    printf("[*] Allocating guest address space (%u MB)...\n", spaceSize / (1024 * 1024));
    int err = pvz2_elf_load(soPath, spaceSize, &image);
    if (err != 0) {
        fprintf(stderr, "[-] Failed to load SO: error code %d (import_overflow=%u)\n", err, image.import_overflow);
        return 1;
    }

    printf("[+] Loaded %u modules! Setting up Runtime & Heap...\n", image.module_count);

    bool has_homura = false;
    for (uint32_t i = 0; i < image.module_count; ++i) {
        if (std::strstr(image.modules[i].name, "libHomura.so")) {
            has_homura = true;
            break;
        }
    }

    std::filesystem::create_directories("pseudo_fs/proc/self");
    std::ofstream maps_f("pseudo_fs/proc/self/maps");
    if (maps_f.is_open()) {
        for (uint32_t i = 0; i < image.module_count; ++i) {
            const auto &m = image.modules[i];
            char line[256];
            snprintf(line, sizeof(line), "%08x-%08x r-xp 00000000 00:00 0 /data/app/%s\n",
                     m.base, m.base + m.span, m.name);
            maps_f << line;
            snprintf(line, sizeof(line), "%08x-%08x rw-p %08x 00:00 0 /data/app/%s\n",
                     m.base + m.span, m.base + m.span + 0x10000, m.span, m.name);
            maps_f << line;
        }
        maps_f.close();
        printf("[+] Generated pseudo_fs/proc/self/maps for %u modules\n", image.module_count);
    }

    pvz_tv::GuestRuntime rt;
    rt.img = &image;
    rt.heap.init(kHeapBase, kHeapSize);

    // Populate data imports (__sF, _ctype_, etc.)
    pvz_tv::initialize_data_imports(&image, &rt);

    // Setup Transmension NativeApp / BridgeApp singletons
    setup_transmension_bridge(&image, &rt);

    // Build O(1) import handler cache
    const auto &table = pvz_tv::import_table();
    std::vector<pvz_tv::ImportHandler> handlers(image.trampoline_count, nullptr);
    uint32_t resolvedCount = 0;
    for (uint32_t i = 0; i < image.trampoline_count; ++i) {
        const char *name = image.trampoline_names[i];
        auto fn = table.find(name);
        if (fn) {
            handlers[i] = fn;
            ++resolvedCount;
        } else {
            printf("[-] Unresolved import #%u: '%s'\n", i, name);
        }
    }

    printf("[+] Resolved %u / %u import trampolines against HLE table (%zu available in table)\n",
           resolvedCount, image.trampoline_count, table.size());

    // Setup page table
    using PageTable = std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
    auto pageTable = std::make_unique<PageTable>();
    for (size_t i = 0; i < pageTable->size(); ++i) {
        size_t addr = i << Dynarmic::A32::UserConfig::PAGE_BITS;
        if (addr < image.mem_size) {
            (*pageTable)[i] = image.mem;
        } else {
            (*pageTable)[i] = nullptr;
        }
    }

    Dynarmic::ExclusiveMonitor monitor(pvz_tv::kMonitorProcessorCount);
    PvzTvGuestEnv env;
    env.img = &image;
    env.rt = &rt;
    env.monitor = &monitor;
    env.page_table = pageTable.get();
    env.handlers = &handlers;

    Dynarmic::A32::UserConfig config;
    config.callbacks = &env;
    config.global_monitor = &monitor;
    config.processor_id = 0;
    config.page_table = pageTable.get();
    config.absolute_offset_page_table = true;
    config.optimizations = Dynarmic::all_safe_optimizations;

    Dynarmic::A32::Jit jit(config);
    env.jit = &jit;

    printf("[+] Dynarmic A32 JIT initialized!\n\n");

    // Execute static constructors
    printf("[*] Running C++ static constructors (.init_array)...\n");
    uint32_t executedCtors = 0;
    // PVZTV_TRACE=2 logs every call the guest makes into the host, which is
    // how you find the last one before a crash. Diagnostic builds only.
    env.trace_svc = pvz_tv::diag::tracing_svc();

    for (uint32_t k = 0; k < image.module_count; ++k) {
        const pvz2_elf_module_t &m = image.modules[image.init_order[k]];
        PVZTV_TRACE("--- Executing %u constructors for [%s] ---", m.init_array_count, m.name);

        for (uint32_t i = 0; i < m.init_array_count; ++i) {
            uint32_t entryAddr = 0;
            memcpy(&entryAddr, &image.mem[m.base + m.init_array_vaddr + i * 4], 4);
            if (entryAddr == 0 || entryAddr == 0xFFFFFFFFu) continue;
            PVZTV_TRACE("  [%u/%u] ctor at 0x%08X", i + 1, m.init_array_count, entryAddr);

            env.should_halt = false;
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            jit.Regs()[2] = 0;
            jit.Regs()[3] = 0;
            jit.Regs()[13] = kStackTop;
            jit.Regs()[14] = image.trampoline_base; // LR -> $halt

            uint32_t pc = entryAddr;
            uint32_t cpsr = jit.Cpsr();
            if ((pc & 1u) != 0) {
                pc &= ~1u;
                cpsr |= 0x20u;
            } else {
                cpsr &= ~0x20u;
            }
            jit.Regs()[15] = pc;
            jit.SetCpsr(cpsr);

            while (!env.should_halt) {
                jit.Run();
            }
            ++executedCtors;
        }
    }

    printf("\n[+] Static constructors completed! Intercepted %u SVC calls.\n\n", env.svc_calls);

    // Nothing in the loaded image is patched: libHomura hooks the UI functions
    // (AwardScreen, SeedChooserScreen, the shovel, the gamepad cursor) and fixes
    // them itself, and patching over its hooks corrupts them.
    if (!has_homura) {
        printf("[-] libHomura.so is not loaded: the game's touch UI will not work.\n");
    }

    // Call libGameMain.so entry point: main(0, NULL) at 0x00131e15 (Thumb)
    uint32_t mainAddr = image.modules[0].base + 0x00131e15;
    printf("[*] Entering libGameMain.so main() at 0x%08X...\n", mainAddr);

    env.trace_svc = false;
    env.should_halt = false;
    jit.Regs()[0] = 0; // argc
    jit.Regs()[1] = 0; // argv
    jit.Regs()[2] = 0;
    jit.Regs()[3] = 0;
    jit.Regs()[13] = kStackTop;
    jit.Regs()[14] = image.trampoline_base; // LR -> $halt

    uint32_t pc = mainAddr;
    uint32_t cpsr = jit.Cpsr();
    if ((pc & 1u) != 0) {
        pc &= ~1u;
        cpsr |= 0x20u;
    } else {
        cpsr &= ~0x20u;
    }
    jit.Regs()[15] = pc;
    jit.SetCpsr(cpsr);

    pvz_tv::guest_tls::self_id = 1;
    register_active_jit(1, "main", &jit, &env);

    // The watchdog prints what every guest thread is doing every 5 seconds:
    // indispensable when the game hangs, pure noise when it does not, so it
    // exists only in a diagnostic build.
    std::atomic<bool> watchdog_running{pvz_tv::diag::kBuiltIn};
    std::thread watchdog;
    if (pvz_tv::diag::kBuiltIn) watchdog = std::thread([&]() {
        int tick = 0;
        while (watchdog_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!watchdog_running.load(std::memory_order_relaxed)) break;
            ++tick;
            std::lock_guard<std::mutex> lk(g_jits_lock);
            printf("[Watchdog #%d] %zu active guest thread(s):\n", tick, g_active_jits.size());
            for (auto &entry : g_active_jits) {
                uint32_t swi = entry.env->current_swi.load(std::memory_order_relaxed);
                const char *swi_name = (entry.env->img && swi < entry.env->img->trampoline_count) 
                                           ? entry.env->img->trampoline_names[swi] : "none";
                uint32_t lr = entry.env->current_lr.load(std::memory_order_relaxed);
                uint32_t pc = entry.env->current_pc.load(std::memory_order_relaxed);
                printf("  -> tid=%u (%s): state=%s, last_swi=#%u (%s), pc=0x%08X, lr=0x%08X, SVCs=%u\n",
                       entry.tid, entry.name,
                       entry.env->current_state.load(std::memory_order_relaxed),
                       swi, swi_name,
                       pc, lr,
                       entry.env->svc_calls);
            }
            if (image.mem) {
                uint32_t ptrAddr = image.modules[0].base + 0x00715d50;
                if (ptrAddr + 4 <= image.mem_size) {
                    uint32_t lawnApp = *(uint32_t*)&image.mem[ptrAddr];
                    if (lawnApp != 0 && lawnApp + 0x600 <= image.mem_size) {
                        bool started = image.mem[lawnApp + 0x56D];
                        bool completed = image.mem[lawnApp + 0x56E];
                        bool loaded = image.mem[lawnApp + 0x56F];
                        int tasks = *(int*)&image.mem[lawnApp + 0x58C];
                        int totalTasks = *(int*)&image.mem[lawnApp + 0x588];
                        int pct = totalTasks > 0 ? (tasks * 100 / totalTasks) : 0;
                        printf("  [LawnApp Status] started=%d, completed=%d, loaded=%d, tasks=%d / %d (%d%%)\n",
                               started, completed, loaded, tasks, totalTasks, pct);
                    }
                }
            }
        }
    });

    // Run main()
    while (!env.should_halt && !rt.shutdown_requested.load(std::memory_order_acquire)) {
        jit.ClearHalt();
        jit.Run();
    }

    watchdog_running.store(false, std::memory_order_relaxed);
    if (watchdog.joinable()) watchdog.join();
    unregister_active_jit(&jit);

    printf("[+] main() completed with return code: %d\n", (int)jit.Regs()[0]);

    // A guest thread the game never joined is still joinable here, and
    // destroying the runtime while one of those is around aborts the process.
    // Threads that already returned are joined; the rest are parked in a
    // blocking call (the network listeners always are), so they are cut loose.
    bool guest_threads_parked = false;
    {
        std::lock_guard<std::mutex> lock(rt.threads_lock);
        for (auto it = rt.threads.begin(); it != rt.threads.end(); it = rt.threads.erase(it)) {
            if (!it->second.joinable()) continue;
            if (rt.thread_retvals.count(it->first) != 0) {
                it->second.join();
            } else {
                guest_threads_parked = true;
                it->second.detach();
            }
        }
    }

    if (g_gl_context) SDL_GL_DeleteContext(g_gl_context);
    if (g_sdl_window) SDL_DestroyWindow(g_sdl_window);
    SDL_Quit();

#if defined(_WIN32)
    timeEndPeriod(1);
    WSACleanup();
#endif

    if (guest_threads_parked) {
        // Guest code is still sitting in a blocking call, so end the process
        // here rather than unmapping the image from under it.
        std::fflush(nullptr);
        std::_Exit(0);
    }

    pvz2_elf_free(&image);
    return 0;
}
