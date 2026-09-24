#include <pvz_tv/diagnostics.h>
#include "runner_core.h"
#include <pvz_tv/surface.h>
#include <pvz_tv/config.h>

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <android/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

#define LOG_TAG "RunnerCore"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

uint32_t g_native_app_addr = 0;
uint32_t g_process_works_fn = 0;
uint32_t g_pipe_write_token = 0;

namespace pvz_tv {

constexpr uint32_t kStackTop = 0x1FE00000;      // 510 MB
constexpr uint32_t kThreadStacksTop = 0x1FC00000; // 508 MB
constexpr uint32_t kHeapBase = 0x01000000;      // 16 MB
constexpr uint32_t kHeapSize = 0x1C000000;      // 448 MB

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
    GuestRuntime *rt = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    Dynarmic::ExclusiveMonitor *monitor = nullptr;
    std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES> *page_table = nullptr;
    const std::vector<ImportHandler> *handlers = nullptr;
    bool should_halt = false;
    uint64_t ticks_used = 0;
    uint32_t svc_calls = 0;
    std::atomic<uint32_t> current_swi{0xFFFFFFFF};
    std::atomic<uint32_t> current_lr{0};
    std::atomic<uint32_t> current_pc{0};
    std::atomic<const char*> current_state{"idle"};

    void AddTicks(uint64_t ticks) override {
        ticks_used += ticks;
#if defined(PVZTV_DIAGNOSTICS)
        if (jit) { // watchdog bookkeeping only; two atomic stores per block
            current_pc.store(jit->Regs()[15], std::memory_order_relaxed);
            current_lr.store(jit->Regs()[14], std::memory_order_relaxed);
        }
#endif
    }
    uint64_t GetTicksRemaining() override { return 10000000; }

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
            // Linux kernel kuser_helper page
            switch (vaddr) {
            case 0xFFFF0FC0: return 0xE1923F9F; // ldrex   r3, [r2]
            case 0xFFFF0FC4: return 0xE0533000; // subs    r3, r3, r0
            case 0xFFFF0FC8: return 0x01823F91; // strexeq r3, r1, [r2]
            case 0xFFFF0FCC: return 0x03330000; // teqeq   r3, #0
            case 0xFFFF0FD0: return 0xE2730000; // rsbs    r0, r3, #0
            case 0xFFFF0FD4: return 0xE12FFF1E; // bx      lr
            case 0xFFFF0FA0: return 0xF57FF05B; // dmb     ish
            case 0xFFFF0FA4: return 0xE12FFF1E; // bx      lr
            case 0xFFFF0FFC: return 5;
            default: return 0xE12FFF1E; // bx lr
            }
        }
        if (!in_bounds(vaddr, 4)) return std::nullopt;
        uint32_t v;
        memcpy(&v, &img->mem[vaddr], 4);
        return v;
    }

    void InterpreterFallback(uint32_t pc, size_t num_instructions) override {
        LOGE("Interpreter fallback at 0x%08X (%zu instrs)", pc, num_instructions);
    }

    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (exception == Dynarmic::A32::Exception::Yield ||
            exception == Dynarmic::A32::Exception::WaitForInterrupt ||
            exception == Dynarmic::A32::Exception::WaitForEvent ||
            exception == Dynarmic::A32::Exception::SendEvent ||
            exception == Dynarmic::A32::Exception::SendEventLocal) {
            std::this_thread::yield();
            return;
        }
        LOGE("Exception %d raised at PC=0x%08X (LR=0x%08X)", (int)exception, pc, jit->Regs()[14]);
        dump_crash_state(pc);
        should_halt = true;
        jit->HaltExecution();
    }

    const char *describe(uint32_t addr, char *out, size_t n) const {
        for (uint32_t m = 0; m < img->module_count; ++m) {
            const auto &mod = img->modules[m];
            if (addr >= mod.base && addr < mod.base + mod.span) {
                snprintf(out, n, "%s+0x%X", mod.name, addr - mod.base);
                return out;
            }
        }
        snprintf(out, n, "0x%08X", addr);
        return out;
    }

    void dump_crash_state(uint32_t pc) const {
        const auto &r = jit->Regs();
        char a[96], b[96];
        LOGE("  pc=%s lr=%s cpsr=0x%08X (%s)", describe(pc, a, sizeof(a)), describe(r[14], b, sizeof(b)),
             jit->Cpsr(), (jit->Cpsr() & 0x20) ? "Thumb" : "ARM");
        for (int i = 0; i < 16; i += 4)
            LOGE("  r%-2d=%08X r%-2d=%08X r%-2d=%08X r%-2d=%08X", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3, r[i + 3]);
        // Also scan below sp: frames an unwinder already abandoned (e.g. the
        // path to a __cxa_throw) still hold their return addresses there.
        uint32_t sp = r[13];
        for (int32_t off = -0x1000; off < 0x400; off += 4) {
            uint32_t addr = sp + (uint32_t)off;
            if (!in_bounds(addr, 4)) continue;
            uint32_t v;
            memcpy(&v, &img->mem[addr], 4);
            if (!(v & 1)) continue; // Thumb return addresses only
            for (uint32_t m = 0; m < img->module_count; ++m) {
                const auto &mod = img->modules[m];
                if (v >= mod.base && v < mod.base + mod.span) {
                    LOGE("  [sp%c0x%03X] %08X %s", off < 0 ? '-' : '+', off < 0 ? -off : off, v,
                         describe(v, a, sizeof(a)));
                    break;
                }
            }
        }
    }

    void return_to_caller() {
        uint32_t lr = jit->Regs()[14];
        jit->Regs()[15] = lr & ~1u;
        if ((lr & 1u) != 0) {
            jit->SetCpsr(jit->Cpsr() | 0x20u); // Thumb
        } else {
            jit->SetCpsr(jit->Cpsr() & ~0x20u); // ARM
        }
    }

    uint32_t run_guest_callback(uint32_t fn, const uint32_t *args, int nargs);

    uint32_t spawn_thread(uint32_t start_routine, uint32_t arg) {
        uint32_t stack_top;
        uint32_t id;
        {
            std::lock_guard<std::mutex> lock(rt->threads_lock);
            if (rt->next_stack_slot >= kThreadStackMax) {
                LOGE("Out of guest thread stack slots (max %u)", kThreadStackMax);
                return 0;
            }
            stack_top = kThreadStacksTop - rt->next_stack_slot * 0x00100000;
            rt->next_stack_slot++;
            id = rt->next_thread_id++;
        }

        std::thread th([this, start_routine, arg, stack_top, id]() {
            guest_tls::self_id = id;
            PvzTvGuestEnv thread_env;
            thread_env.img = img;
            thread_env.rt = rt;
            thread_env.monitor = monitor;
            thread_env.page_table = page_table;
            thread_env.handlers = handlers;

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
            thread_jit.Regs()[14] = img->trampoline_base;

            uint32_t pc = start_routine;
            uint32_t cpsr = thread_jit.Cpsr();
            if (pc & 1) { pc &= ~1; cpsr |= 0x20; } else { cpsr &= ~0x20; }
            thread_jit.Regs()[15] = pc;
            thread_jit.SetCpsr(cpsr);

            register_active_jit(id, "guest_thread", &thread_jit, &thread_env);
            while (!thread_env.should_halt && !rt->shutdown_requested.load(std::memory_order_acquire)) {
                thread_jit.ClearHalt();
                thread_jit.Run();
            }
            unregister_active_jit(&thread_jit);

            uint32_t retval = thread_jit.Regs()[0];
            {
                std::lock_guard<std::mutex> lock(rt->threads_lock);
                rt->thread_retvals[id] = retval;
            }
            {
                std::lock_guard<std::mutex> mlk(rt->mutexes_lock);
                for (auto &kv : rt->guest_mutexes) {
                    if (kv.second->owner == id) {
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
        if (th.joinable()) th.join();
        std::lock_guard<std::mutex> lock(rt->threads_lock);
        auto it = rt->thread_retvals.find(id);
        return (it != rt->thread_retvals.end()) ? it->second : 0;
    }

    void CallSVC(uint32_t swi) override {
#if defined(PVZTV_DIAGNOSTICS)
        ++svc_calls;
        current_swi.store(swi, std::memory_order_relaxed);
        if (jit) {
            current_lr.store(jit->Regs()[14], std::memory_order_relaxed);
            current_pc.store(jit->Regs()[15], std::memory_order_relaxed);
        }
        current_state.store("in_svc", std::memory_order_relaxed);
#endif

        if (swi == 0) {
            uint32_t syscall_num = jit ? jit->Regs()[7] : 0;
            if (syscall_num == 0x000f0002) {
                jit->Regs()[0] = 0;
                    return;
            }
            uint32_t pc = jit ? jit->Regs()[15] : 0;
            bool is_trampoline_halt = (pc >= img->trampoline_base && pc < img->trampoline_base + 16);
            if (!is_trampoline_halt) {
                jit->Regs()[0] = 0;
                    return;
            }
            should_halt = true;
            jit->HaltExecution();
            return;
        }

        if (handlers && swi < handlers->size() && (*handlers)[swi]) {
            GuestCall c;
            c.img = img;
            c.rt = rt;
            c.regs = jit->Regs().data();
            c.env = this;
            c.halt_fn = [](void *env, const char *why) {
                auto *e = static_cast<PvzTvGuestEnv*>(env);
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

        const char *name = (img && swi < img->trampoline_count) ? img->trampoline_names[swi] : "unknown";
        LOGE("Unhandled SVC #%u: %s (lr=0x%08X)", swi, name, jit ? jit->Regs()[14] : 0);
        jit->Regs()[0] = 0;
        return_to_caller();
    }
};

struct CallbackJitSlot {
    std::unique_ptr<PvzTvGuestEnv> env;
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    bool in_use = false;
    uint32_t id = 0;
};
static std::mutex s_cb_mutex;
static std::vector<CallbackJitSlot> s_cb_slots;

uint32_t PvzTvGuestEnv::run_guest_callback(uint32_t fn, const uint32_t *args, int nargs) {
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

// Returns the value of a libstdc++ (COW) std::string whose text is str: the
// std::string object *is* its data pointer, so this is what goes in a
// std::string field. Rep header: length, capacity, refcount (0 = one owner).
static uint32_t make_guest_string(pvz2_elf_image_t *img, GuestRuntime *rt, const char *str) {
    size_t len = strlen(str);
    uint32_t rep = rt->heap.alloc((uint32_t)(len + 16));
    *(uint32_t*)&img->mem[rep + 0] = (uint32_t)len;
    *(uint32_t*)&img->mem[rep + 4] = (uint32_t)len;
    *(uint32_t*)&img->mem[rep + 8] = 0;
    memcpy(&img->mem[rep + 12], str, len + 1);
    return rep + 12;
}

static uint32_t make_guest_raw_string(pvz2_elf_image_t *img, GuestRuntime *rt, const char *str) {
    size_t len = strlen(str) + 1;
    uint32_t s = rt->heap.alloc((uint32_t)len);
    memcpy(&img->mem[s], str, len);
    return s;
}

static void setup_transmension_bridge(pvz2_elf_image_t *img, GuestRuntime *rt) {
    uint32_t nativeBase = 0;
    for (uint32_t i = 0; i < img->module_count; ++i) {
        if (strstr(img->modules[i].name, "native_code")) {
            nativeBase = img->modules[i].base;
            break;
        }
    }
    if (!nativeBase) {
        LOGE("libnative_code.so not found in guest modules!");
        return;
    }

    uint32_t nativeApp = rt->heap.alloc(0x200);
    uint32_t bridgeApp = rt->heap.alloc(0x200);
    memset(&img->mem[nativeApp], 0, 0x200);
    memset(&img->mem[bridgeApp], 0, 0x200);

    *(uint32_t*)&img->mem[nativeApp + 0x00] = nativeApp;
    *(uint32_t*)&img->mem[nativeApp + 0x04] = make_guest_string(img, rt, "android_main");
    *(uint32_t*)&img->mem[nativeApp + 0x08] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x0c] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x10] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x14] = make_guest_string(img, rt, "data");
    *(uint32_t*)&img->mem[nativeApp + 0x18] = make_guest_string(img, rt, "com.trans.pvztv");
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
    *(uint32_t*)&img->mem[nativeApp + 0x88] = 21;
    *(uint32_t*)&img->mem[nativeApp + 0x8c] = 0; // AAssetManager* = NULL

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

    *(uint8_t*)&img->mem[inputMgr + 0x10] = 1;
    *(uint8_t*)&img->mem[gameCenter + 0x28] = 1;
    *(uint8_t*)&img->mem[shareMgr + 0x18] = 1;
    *(uint8_t*)&img->mem[notifMgr + 0x0c] = 1;

    int pipe_fds[2] = {-1, -1};
    pipe(pipe_fds);
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
    g_pipe_write_token = write_token;
    g_native_app_addr = nativeApp;
    g_process_works_fn = nativeBase + 0x16b55; // Native::NativeApp::processWorks() (Thumb)

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
    *(uint32_t*)&img->mem[nativeApp + 0x110] = 1;
    *(uint32_t*)&img->mem[nativeApp + 0x120] = 1;
    *(uint32_t*)&img->mem[nativeApp + 0x158] = 0;
    *(uint32_t*)&img->mem[nativeApp + 0x15c] = 0;
    *(uint32_t*)&img->mem[nativeApp + 0x160] = nativeApp + 0x158;
    *(uint32_t*)&img->mem[nativeApp + 0x164] = nativeApp + 0x158;
    *(uint32_t*)&img->mem[nativeApp + 0xa8] = bridgeApp;

    // Fake JavaVM (+0x94) / JNIEnv (+0x98). Works posted to the "Java" side
    // (showSoftInput, setText, ...) call through these; with NULL the Runnable
    // crashes before notify() and waitWork() blocks the game thread forever.
    {
        constexpr uint32_t kJniFnCount = 240;
        uint32_t code = rt->heap.alloc(32);
        // ret0: movs r0,#0; movs r1,#0; bx lr
        const uint16_t ret0[3] = { 0x2000, 0x2100, 0x4770 };
        memcpy(&img->mem[code], ret0, sizeof(ret0));
        // store_env (code+8): ldr r2,[pc,#4]; str r2,[r1]; movs r0,#0; bx lr; .word env
        const uint16_t store_env[4] = { 0x4A01, 0x600A, 0x2000, 0x4770 };
        memcpy(&img->mem[code + 8], store_env, sizeof(store_env));

        uint32_t envFns = rt->heap.alloc(kJniFnCount * 4);
        for (uint32_t i = 0; i < kJniFnCount; ++i)
            *(uint32_t*)&img->mem[envFns + i * 4] = code | 1;
        uint32_t env = rt->heap.alloc(4);
        *(uint32_t*)&img->mem[env] = envFns;
        *(uint32_t*)&img->mem[code + 16] = env;

        // JNIInvokeInterface: 3 reserved, DestroyJavaVM, AttachCurrentThread,
        // DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon
        uint32_t vmFns = rt->heap.alloc(8 * 4);
        for (uint32_t i = 0; i < 8; ++i)
            *(uint32_t*)&img->mem[vmFns + i * 4] = code | 1;
        *(uint32_t*)&img->mem[vmFns + 4 * 4] = (code + 8) | 1;
        *(uint32_t*)&img->mem[vmFns + 6 * 4] = (code + 8) | 1;
        *(uint32_t*)&img->mem[vmFns + 7 * 4] = (code + 8) | 1;
        uint32_t vm = rt->heap.alloc(4);
        *(uint32_t*)&img->mem[vm] = vmFns;

        *(uint32_t*)&img->mem[nativeApp + 0x94] = vm;
        *(uint32_t*)&img->mem[nativeApp + 0x98] = env;
    }

    uint32_t vtable = nativeBase + 0x4e8f8 + 8;
    *(uint32_t*)&img->mem[bridgeApp + 0x00] = vtable;
    *(uint32_t*)&img->mem[bridgeApp + 0x04] = nativeApp;
    *(uint32_t*)&img->mem[bridgeApp + 0x08] = bridgeApp + 0x08;
    *(uint32_t*)&img->mem[bridgeApp + 0x0c] = bridgeApp + 0x08;
    *(uint32_t*)&img->mem[bridgeApp + 0x28] = bridgeApp + 0x28;
    *(uint32_t*)&img->mem[bridgeApp + 0x2c] = bridgeApp + 0x28;
    *(uint32_t*)&img->mem[bridgeApp + 0x30] = bridgeApp + 0x30;
    *(uint32_t*)&img->mem[bridgeApp + 0x34] = bridgeApp + 0x30;
    *(uint32_t*)&img->mem[bridgeApp + 0x38] = bridgeApp + 0x38;
    *(uint32_t*)&img->mem[bridgeApp + 0x3c] = bridgeApp + 0x38;
    *(uint32_t*)&img->mem[bridgeApp + 0x70] = make_guest_raw_string(img, rt, "en_US");
    *(uint32_t*)&img->mem[bridgeApp + 0x74] = 3;
    *(uint32_t*)&img->mem[bridgeApp + 0x78] = 1;
    *(uint32_t*)&img->mem[bridgeApp + 0x88] = 1280;
    *(uint32_t*)&img->mem[bridgeApp + 0x8c] = 720;
    *(uint32_t*)&img->mem[bridgeApp + 0x90] = 1280;
    *(uint32_t*)&img->mem[bridgeApp + 0x94] = 720;
    *(uint8_t*)&img->mem[bridgeApp + 0x98] = 1;
    *(uint32_t*)&img->mem[bridgeApp + 0x9c] = 0x4e800001u;
    *(uint32_t*)&img->mem[bridgeApp + 0xa0] = 0x4e800003u;
    *(uint32_t*)&img->mem[bridgeApp + 0xa4] = 0x4e800004u;

    *(uint32_t*)&img->mem[nativeBase + 0x4f014] = bridgeApp;
    LOGI("BridgeApp configured at 0x%08X (bridgeApp=0x%08X, nativeApp=0x%08X)",
         nativeBase + 0x4f014, bridgeApp, nativeApp);
}

// Global symbols cache for Homura and cheats
static uint32_t s_fn_changes = 0;
static uint32_t s_fn_get_feature_list = 0;
static uint32_t s_fn_settings_list = 0;
static uint32_t s_fn_get_current_formation = 0;
static uint32_t s_fn_send_second_touch = 0;
static uint32_t s_fn_send_button_event = 0;
static uint32_t s_fn_switch_two_player = 0;
static uint32_t s_fn_is_in_game = 0;

static std::unique_ptr<Dynarmic::ExclusiveMonitor> s_monitor;
static std::unique_ptr<std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>> s_pageTable;
static std::vector<ImportHandler> s_handlers;
static std::unique_ptr<PvzTvGuestEnv> s_main_env;
static std::unique_ptr<Dynarmic::A32::Jit> s_main_jit;
static std::thread s_main_thread;
static std::atomic<bool> s_watchdog_running{false};
static std::thread s_watchdog;

RunnerCore &RunnerCore::instance() {
    static RunnerCore s_instance;
    return s_instance;
}

RunnerCore::~RunnerCore() {
    stop();
}

bool RunnerCore::init(const char *game_so_path, const char *data_dir) {
    if (initialized_) return true;
    data_dir_ = data_dir ? data_dir : "";
    if (!data_dir_.empty()) {
        chdir(data_dir_.c_str());
    }

    LOGI("Initializing RunnerCore with %s (data: %s)", game_so_path, data_dir_.c_str());

    // Older builds stored NativeApp's std::string fields as pointers to string
    // objects, so the game's files dir came out as the raw pointer bytes (e.g.
    // "t\x04") and saves landed in a directory with an unprintable name. Move
    // that directory to "data", where the files dir now correctly points.
    {
        std::error_code ec;
        if (!std::filesystem::exists("data", ec)) {
            for (const auto &entry : std::filesystem::directory_iterator(".", ec)) {
                std::string name = entry.path().filename().string();
                bool unprintable = std::any_of(name.begin(), name.end(),
                                               [](unsigned char ch) { return ch < 0x20 || ch > 0x7e; });
                if (unprintable && entry.is_directory(ec) &&
                    std::filesystem::exists(entry.path() / "userdata", ec)) {
                    std::filesystem::rename(entry.path(), "data", ec);
                    LOGI("Migrated save data from a garbled directory name to data/ (%s)",
                         ec ? ec.message().c_str() : "ok");
                    break;
                }
            }
        }
    }

    uint32_t spaceSize = 512 * 1024 * 1024;
    int err = pvz2_elf_load(game_so_path, spaceSize, &image_);
    if (err != 0) {
        LOGE("Failed to load ELF image: error %d", err);
        return false;
    }

    LOGI("Loaded %u modules into guest space", image_.module_count);

    // Create pseudo_fs /proc/self/maps for libHomura GetLibBaseAddr
    std::filesystem::create_directories("pseudo_fs/proc/self");
    std::ofstream maps_f("pseudo_fs/proc/self/maps");
    if (maps_f.is_open()) {
        for (uint32_t i = 0; i < image_.module_count; ++i) {
            const auto &m = image_.modules[i];
            char line[256];
            snprintf(line, sizeof(line), "%08x-%08x r-xp 00000000 00:00 0 /data/app/%s\n",
                     m.base, m.base + m.span, m.name);
            maps_f << line;
            snprintf(line, sizeof(line), "%08x-%08x rw-p %08x 00:00 0 /data/app/%s\n",
                     m.base + m.span, m.base + m.span + 0x10000, m.span, m.name);
            maps_f << line;
        }
        maps_f.close();
    }

    std::ofstream cmd_f("pseudo_fs/proc/self/cmdline", std::ios::binary);
    if (cmd_f.is_open()) {
        cmd_f.write("com.popcap.pvz\0", 15);
        cmd_f.close();
    }

    runtime_.img = &image_;
    runtime_.heap.init(kHeapBase, kHeapSize);
    initialize_data_imports(&image_, &runtime_);
    setup_transmension_bridge(&image_, &runtime_);

    const auto &table = import_table();
    s_handlers.assign(image_.trampoline_count, nullptr);
    for (uint32_t i = 0; i < image_.trampoline_count; ++i) {
        const char *name = image_.trampoline_names[i];
        s_handlers[i] = table.find(name);
    }

    using PageTable = std::array<uint8_t*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
    s_pageTable = std::make_unique<PageTable>();
    for (size_t i = 0; i < s_pageTable->size(); ++i) {
        size_t addr = i << Dynarmic::A32::UserConfig::PAGE_BITS;
        (*s_pageTable)[i] = (addr < image_.mem_size) ? image_.mem : nullptr;
    }

    s_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMonitorProcessorCount);

    s_main_env = std::make_unique<PvzTvGuestEnv>();
    s_main_env->img = &image_;
    s_main_env->rt = &runtime_;
    s_main_env->monitor = s_monitor.get();
    s_main_env->page_table = s_pageTable.get();
    s_main_env->handlers = &s_handlers;

    Dynarmic::A32::UserConfig config;
    config.callbacks = s_main_env.get();
    config.global_monitor = s_monitor.get();
    config.processor_id = 0;
    config.page_table = s_pageTable.get();
    config.absolute_offset_page_table = true;
    config.optimizations = Dynarmic::all_safe_optimizations;

    s_main_jit = std::make_unique<Dynarmic::A32::Jit>(config);
    s_main_env->jit = s_main_jit.get();

    // Execute .init_array constructors
    LOGI("Running C++ static constructors (.init_array)...");
    for (uint32_t k = 0; k < image_.module_count; ++k) {
        const pvz2_elf_module_t &m = image_.modules[image_.init_order[k]];
        for (uint32_t i = 0; i < m.init_array_count; ++i) {
            uint32_t entryAddr = 0;
            memcpy(&entryAddr, &image_.mem[m.base + m.init_array_vaddr + i * 4], 4);
            if (entryAddr == 0 || entryAddr == 0xFFFFFFFFu) continue;

            s_main_env->should_halt = false;
            s_main_jit->Regs()[0] = 0;
            s_main_jit->Regs()[1] = 0;
            s_main_jit->Regs()[2] = 0;
            s_main_jit->Regs()[3] = 0;
            s_main_jit->Regs()[13] = kStackTop;
            s_main_jit->Regs()[14] = image_.trampoline_base;

            uint32_t pc = entryAddr;
            uint32_t cpsr = s_main_jit->Cpsr();
            if (pc & 1) { pc &= ~1; cpsr |= 0x20; } else { cpsr &= ~0x20; }
            s_main_jit->Regs()[15] = pc;
            s_main_jit->SetCpsr(cpsr);

            while (!s_main_env->should_halt) {
                s_main_jit->Run();
            }
        }
    }

    // Nothing in the loaded image is patched any more. The runner used to force
    // every ButtonWidget to fire on MouseUp, which also fired buttons calling
    // LawnApp::SdkExit(), and then disabled shutdown to hide that; the game now
    // quits only when asked to, and start() finishes the Activity afterwards.

    // The touch UI (AwardScreen, SeedChooserScreen, the shovel, the gamepad
    // cursor) is libHomura's job: it inline-hooks those functions and fixes them
    // there. The runner used to patch the same functions itself, which corrupted
    // the mod's hooks, so it only checks that the mod is actually loaded.
    bool has_homura = false;
    for (uint32_t i = 0; i < image_.module_count; ++i) {
        if (strstr(image_.modules[i].name, "libHomura")) has_homura = true;
    }
    if (!has_homura) {
        LOGE("libHomura.so is not loaded: the touch controls will not work");
    }

    cacheCheatSymbols();

    initialized_ = true;
    LOGI("RunnerCore initialized successfully!");
    return true;
}

void RunnerCore::cacheCheatSymbols() {
    s_fn_changes = pvz2_elf_find_symbol(&image_, "Java_com_android_support_Preferences_Changes");
    s_fn_get_feature_list = pvz2_elf_find_symbol(&image_, "Java_com_android_support_CkHomuraMenu_GetFeatureList");
    s_fn_settings_list = pvz2_elf_find_symbol(&image_, "Java_com_android_support_CkHomuraMenu_SettingsList");
    s_fn_get_current_formation = pvz2_elf_find_symbol(&image_, "Java_com_android_support_CkHomuraMenu_GetCurrentFormation");
    s_fn_send_second_touch = pvz2_elf_find_symbol(&image_, "Java_com_transmension_mobile_EnhanceActivity_nativeSendSecondTouch");
    s_fn_send_button_event = pvz2_elf_find_symbol(&image_, "Java_com_transmension_mobile_EnhanceActivity_nativeSendButtonEvent");
    s_fn_switch_two_player = pvz2_elf_find_symbol(&image_, "Java_com_transmension_mobile_EnhanceActivity_nativeSwitchTwoPlayerMode");
    s_fn_is_in_game = pvz2_elf_find_symbol(&image_, "Java_com_transmension_mobile_EnhanceActivity_nativeIsInGame");

    LOGI("Cached guest JNI symbols: Changes=0x%08X, GetFeatureList=0x%08X, isInGame=0x%08X",
         s_fn_changes, s_fn_get_feature_list, s_fn_is_in_game);
}

bool RunnerCore::start() {
    if (!initialized_ || running_) return true;
    running_ = true;

    s_main_thread = std::thread([this]() {
        guest_tls::self_id = 1;
        register_active_jit(1, "main", s_main_jit.get(), s_main_env.get());

        // Wait for NativeView ANativeWindow before starting game
        if (!android_runner_wait_for_window(5000)) {
            LOGE("Warning: Native window not ready after 5s timeout, proceeding anyway...");
        }

        if (!android_runner_init_egl_on_render_thread()) {
            LOGE("Failed to initialize EGL on render thread!");
        }

        uint32_t mainAddr = image_.modules[0].base + 0x00131e15; // Thumb
        LOGI("Entering libGameMain.so main() at 0x%08X", mainAddr);

        s_main_env->should_halt = false;
        s_main_jit->Regs()[0] = 0;
        s_main_jit->Regs()[1] = 0;
        s_main_jit->Regs()[2] = 0;
        s_main_jit->Regs()[3] = 0;
        s_main_jit->Regs()[13] = kStackTop;
        s_main_jit->Regs()[14] = image_.trampoline_base;

        uint32_t pc = mainAddr;
        uint32_t cpsr = s_main_jit->Cpsr();
        if (pc & 1) { pc &= ~1; cpsr |= 0x20; } else { cpsr &= ~0x20; }
        s_main_jit->Regs()[15] = pc;
        s_main_jit->SetCpsr(cpsr);

        while (!s_main_env->should_halt && !runtime_.shutdown_requested.load(std::memory_order_acquire)) {
            s_main_jit->ClearHalt();
            s_main_jit->Run();
        }

        unregister_active_jit(s_main_jit.get());
        LOGI("main() exited with code %d", (int)s_main_jit->Regs()[0]);
        // A clean return from main() is the game quitting (it already saved in
        // LawnApp::Shutdown). A crash halt or stop() must not close the app.
        if (!runtime_.shutdown_requested.load(std::memory_order_acquire) &&
            s_main_jit->Regs()[15] >= image_.trampoline_base &&
            s_main_jit->Regs()[15] < image_.trampoline_base + 16) {
            android_runner_finish_activity();
        }
    });

#if defined(PVZTV_DIAGNOSTICS)
    // Diagnostic builds only: one line per guest thread every 2 s, for
    // diagnosing hangs.
    s_watchdog_running.store(true, std::memory_order_release);
    s_watchdog = std::thread([this]() {
        int tick = 0;
        while (s_watchdog_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!s_watchdog_running.load(std::memory_order_relaxed)) break;
            ++tick;
            std::lock_guard<std::mutex> lk(g_jits_lock);
            for (auto &entry : g_active_jits) {
                uint32_t swi = entry.env->current_swi.load(std::memory_order_relaxed);
                const char *swi_name = (entry.env->img && swi < entry.env->img->trampoline_count)
                                           ? entry.env->img->trampoline_names[swi] : "none";
                uint32_t lr = entry.env->current_lr.load(std::memory_order_relaxed);
                uint32_t pc = entry.env->current_pc.load(std::memory_order_relaxed);
                LOGI("[Watchdog #%d] tid=%u (%s): state=%s, last_swi=#%u (%s), pc=0x%08X, lr=0x%08X, SVCs=%u",
                     tick, entry.tid, entry.name,
                     entry.env->current_state.load(std::memory_order_relaxed),
                     swi, swi_name,
                     pc, lr,
                     entry.env->svc_calls);
            }
        }
    });
#endif

    return true;
}

void RunnerCore::pause() { android_runner_set_paused(true); }
void RunnerCore::resume() { android_runner_set_paused(false); }

void RunnerCore::stop() {
    if (!running_) return;
    runtime_.shutdown_requested.store(true, std::memory_order_release);
    android_runner_set_paused(false); // wake the frame loop if it is parked
    s_watchdog_running.store(false, std::memory_order_release);
    if (s_main_env && s_main_jit) {
        s_main_env->should_halt = true;
        s_main_jit->HaltExecution();
    }
    if (s_main_thread.joinable()) {
        s_main_thread.join();
    }
    if (s_watchdog.joinable()) {
        s_watchdog.join();
    }
    running_ = false;
}

void RunnerCore::setFeature(int featNum, int value, bool boolean, const char *str) {
    if (!s_fn_changes || !s_main_env) return;

    uint32_t guest_str = 0;
    if (str && str[0]) {
        guest_str = make_guest_raw_string(&image_, &runtime_, str);
    }

    uint32_t args[8] = {
        0, // env
        0, // clazz
        0, // con
        (uint32_t)featNum,
        0, // featName
        (uint32_t)value,
        (uint32_t)(boolean ? 1 : 0),
        guest_str
    };

    s_main_env->run_guest_callback(s_fn_changes, args, 8);
}

void RunnerCore::sendSecondTouch(int x, int y, int action) {
    if (!s_fn_send_second_touch || !s_main_env) return;
    uint32_t args[5] = { 0, 0, (uint32_t)x, (uint32_t)y, (uint32_t)action };
    s_main_env->run_guest_callback(s_fn_send_second_touch, args, 5);
}

void RunnerCore::sendButtonEvent(bool isButtonDown, int buttonCode) {
    if (!s_fn_send_button_event || !s_main_env) return;
    uint32_t args[4] = { 0, 0, (uint32_t)(isButtonDown ? 1 : 0), (uint32_t)buttonCode };
    s_main_env->run_guest_callback(s_fn_send_button_event, args, 4);
}

void RunnerCore::switchTwoPlayerMode(bool isOn) {
    if (!s_fn_switch_two_player || !s_main_env) return;
    uint32_t args[3] = { 0, 0, (uint32_t)(isOn ? 1 : 0) };
    s_main_env->run_guest_callback(s_fn_switch_two_player, args, 3);
}

bool RunnerCore::isInGame() {
    if (!s_fn_is_in_game || !s_main_env) return false;
    uint32_t args[2] = { 0, 0 };
    return s_main_env->run_guest_callback(s_fn_is_in_game, args, 2) != 0;
}

uint32_t RunnerCore::callGuest(uint32_t fn, const uint32_t *args, int nargs) {
    if (!fn || !s_main_env) return 0;
    return s_main_env->run_guest_callback(fn, args, nargs);
}

uint32_t RunnerCore::findSymbol(const char *name) {
    return pvz2_elf_find_symbol(&image_, name);
}

} // namespace pvz_tv
