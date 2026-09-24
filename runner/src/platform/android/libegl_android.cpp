#include <pvz_tv/diagnostics.h>
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/surface.h>
#include <runner_core.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES/gl.h>
#include <GLES2/gl2.h>
#include <android/native_window.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#define LOG_TAG "RunnerEGL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace pvz_tv {

// Host EGL and window state
static std::mutex g_egl_lock;
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLConfig g_egl_config = nullptr;
static std::atomic<bool> g_surface_ready{false};

// Window synchronization between Android UI thread and runner thread
static std::mutex g_window_mutex;
static std::condition_variable g_window_cv;
static ANativeWindow *g_pending_window = nullptr;
static ANativeWindow *g_current_window = nullptr;
static std::atomic<bool> g_window_changed{false};

struct QueuedInputEvent {
    enum Type { TOUCH, KEY } type;
    int action; // Touch: 2=DOWN, 3=MOVE, 4=UP, 5=CANCEL; Key: 0=DOWN, 1=UP
    float x;
    float y;
    int pointer_id;
    int key_code;
};

static std::mutex g_input_queue_lock;
static std::vector<QueuedInputEvent> g_input_queue;

// Activity lifecycle: while paused the guest's frame loop parks inside
// eglSwapBuffers instead of spinning through frames nobody sees.
static std::mutex g_pause_lock;
static std::condition_variable g_pause_cv;
static std::atomic<bool> g_paused{false};

void android_runner_set_paused(bool paused) {
    {
        std::lock_guard<std::mutex> lk(g_pause_lock);
        g_paused.store(paused, std::memory_order_release);
    }
    g_pause_cv.notify_all();
}

// Called from JNI (UI thread)
void android_runner_set_window(ANativeWindow *window) {
    LOGI("android_runner_set_window(%p)", window);
    {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_pending_window != window) {
            if (g_pending_window) {
                ANativeWindow_release(g_pending_window);
            }
            if (window) {
                ANativeWindow_acquire(window);
            }
            g_pending_window = window;
            g_window_changed.store(true, std::memory_order_release);
        }
    }
    g_window_cv.notify_all();
}

// Called from JNI (UI thread)
void android_runner_destroy_window() {
    LOGI("android_runner_destroy_window()");
    {
        std::lock_guard<std::mutex> lk(g_window_mutex);
        if (g_pending_window) {
            ANativeWindow_release(g_pending_window);
            g_pending_window = nullptr;
        }
        g_window_changed.store(true, std::memory_order_release);
    }
    g_surface_ready.store(false, std::memory_order_release);
    g_window_cv.notify_all();
}

// Called from s_main_thread
bool android_runner_wait_for_window(int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_window_mutex);
    if (g_pending_window != nullptr) {
        LOGI("ANativeWindow already ready: %p", g_pending_window);
        return true;
    }
    LOGI("Waiting for ANativeWindow from SurfaceView...");
    bool ok = g_window_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] {
        return g_pending_window != nullptr;
    });
    if (!ok) {
        LOGE("Timed out waiting for ANativeWindow!");
        return false;
    }
    LOGI("ANativeWindow received: %p", g_pending_window);
    return true;
}

// Called from s_main_thread (render/runner thread)
bool android_runner_init_egl_on_render_thread() {
    std::lock_guard<std::mutex> lk(g_egl_lock);
    LOGI("Initializing EGL on render thread (tid=%d)...", (int)gettid());

    if (g_egl_display == EGL_NO_DISPLAY) {
        g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (g_egl_display == EGL_NO_DISPLAY) {
            LOGE("eglGetDisplay failed: 0x%04x", (unsigned)eglGetError());
            return false;
        }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(g_egl_display, &major, &minor)) {
            LOGE("eglInitialize failed: 0x%04x", (unsigned)eglGetError());
            return false;
        }
        LOGI("EGL initialized: version %d.%d", major, minor);
    }

    if (!g_egl_config) {
        eglBindAPI(EGL_OPENGL_ES_API);

        const EGLint config_attribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      16,
            EGL_NONE
        };

        EGLint num_configs = 0;
        if (!eglChooseConfig(g_egl_display, config_attribs, &g_egl_config, 1, &num_configs) || num_configs < 1) {
            LOGW("eglChooseConfig(depth=16) failed, trying fallback...");
            const EGLint fallback_attribs[] = {
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
                EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                EGL_NONE
            };
            if (!eglChooseConfig(g_egl_display, fallback_attribs, &g_egl_config, 1, &num_configs) || num_configs < 1) {
                LOGE("eglChooseConfig fallback failed: 0x%04x", (unsigned)eglGetError());
                return false;
            }
        }
        LOGI("eglChooseConfig succeeded, config=%p", g_egl_config);
    }

    if (g_egl_context == EGL_NO_CONTEXT) {
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 1,
            EGL_NONE
        };
        g_egl_context = eglCreateContext(g_egl_display, g_egl_config, EGL_NO_CONTEXT, ctx_attribs);
        if (g_egl_context == EGL_NO_CONTEXT) {
            LOGW("eglCreateContext(CLIENT_VERSION=1) failed, trying default attribs...");
            g_egl_context = eglCreateContext(g_egl_display, g_egl_config, EGL_NO_CONTEXT, nullptr);
        }
        if (g_egl_context == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext failed: 0x%04x", (unsigned)eglGetError());
            return false;
        }
        LOGI("eglCreateContext succeeded: %p", g_egl_context);
    }

    ANativeWindow *win = nullptr;
    {
        std::lock_guard<std::mutex> wlk(g_window_mutex);
        win = g_pending_window;
    }

    if (!win) {
        LOGE("No ANativeWindow available to create EGLSurface!");
        return false;
    }

    g_current_window = win;
    int32_t width = ANativeWindow_getWidth(win);
    int32_t height = ANativeWindow_getHeight(win);
    pvz2_surface_set(width > 0 ? width : 1280, height > 0 ? height : 720);

    EGLint format = 0;
    eglGetConfigAttrib(g_egl_display, g_egl_config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(win, 0, 0, format);

    if (g_egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(g_egl_display, g_egl_surface);
        g_egl_surface = EGL_NO_SURFACE;
    }

    g_egl_surface = eglCreateWindowSurface(g_egl_display, g_egl_config, win, nullptr);
    if (g_egl_surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed: 0x%04x", (unsigned)eglGetError());
        return false;
    }
    LOGI("eglCreateWindowSurface succeeded (%dx%d)", width, height);

    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        LOGE("eglMakeCurrent failed: 0x%04x", (unsigned)eglGetError());
        return false;
    }

    g_surface_ready.store(true, std::memory_order_release);
    g_window_changed.store(false, std::memory_order_release);
    LOGI("eglMakeCurrent SUCCESS on render thread! Host GLES 1.1 context is now active.");
    return true;
}

void android_runner_queue_touch(int action, float x, float y, int pointer_id) {
    std::lock_guard<std::mutex> lk(g_input_queue_lock);
    QueuedInputEvent ev;
    ev.type = QueuedInputEvent::TOUCH;
    ev.action = action;
    ev.x = x;
    ev.y = y;
    ev.pointer_id = pointer_id;
    ev.key_code = 0;
    g_input_queue.push_back(ev);
}

void android_runner_queue_key(int action, int key_code) {
    std::lock_guard<std::mutex> lk(g_input_queue_lock);
    QueuedInputEvent ev;
    ev.type = QueuedInputEvent::KEY;
    ev.action = action;
    ev.x = 0;
    ev.y = 0;
    ev.pointer_id = 0;
    ev.key_code = key_code;
    g_input_queue.push_back(ev);
}

// ---------------------------------------------------------------------------
// Soft keyboard bridge
// ---------------------------------------------------------------------------

// Runnable vtables in the guest libnative_code.so (v1.1.5), relative to its base.
constexpr uint32_t kSoftInputWorkVtbl = 0x4dc40;       // showSoftInput/hideSoftInput: +0xc show, +0x10 mode
constexpr uint32_t kTextDialogWorkVtbl = 0x4de68;      // showTextInputDialog: +0xc mode, +0x10/14/18 strings
constexpr uint32_t kHideTextDialogWorkVtbl = 0x4dd00;  // hideTextInputDialog
constexpr uint32_t kBridgeAppSingleton = 0x4f014;

struct PendingText {
    std::string text;
    bool cancelled;
};
static std::mutex g_text_lock;
static std::vector<PendingText> g_pending_texts;
static std::atomic<bool> g_text_dialog_open{false};

void android_runner_queue_text(const std::string &text, bool cancelled) {
    std::lock_guard<std::mutex> lk(g_text_lock);
    g_pending_texts.push_back({text, cancelled});
}

// Runs on the guest thread that just queued work for the "Java" side, before
// processWorks() executes it. The fake JNIEnv turns the work itself into a
// no-op, so this is where the request actually reaches Android.
void android_runner_inspect_pending_works(GuestCall &c, uint32_t native_app, uint32_t native_base) {
    uint32_t head = native_app + 0xf8; // std::list<Runnable*>
    uint32_t node = c.read32(head);
    for (int guard = 0; node && node != head && guard < 64; ++guard, node = c.read32(node)) {
        uint32_t work = c.read32(node + 8);
        if (!work) continue;
        uint32_t vtbl = c.read32(work) - native_base;
        if (vtbl == kSoftInputWorkVtbl) {
            bool show = c.read8(work + 0xc) != 0;
            LOGI("Guest %s soft input (mode=%u)", show ? "shows" : "hides", c.read32(work + 0x10));
            // A hide right after a show is just focus churn; the dialog closes itself.
            if (show && !g_text_dialog_open.exchange(true)) {
                android_runner_show_text_dialog(0, "", "", "");
            }
        } else if (vtbl == kTextDialogWorkVtbl) {
            int mode = (int)c.read32(work + 0xc);
            std::string title = c.cstr(c.read32(work + 0x10));
            std::string hint = c.cstr(c.read32(work + 0x14));
            std::string initial = c.cstr(c.read32(work + 0x18));
            LOGI("Guest shows text dialog (mode=%d, title=%s)", mode, title.c_str());
            g_text_dialog_open.store(true);
            android_runner_show_text_dialog(mode, title, hint, initial);
        } else if (vtbl == kHideTextDialogWorkVtbl) {
            if (g_text_dialog_open.exchange(false)) android_runner_hide_text_dialog();
        }
    }
}

// Game thread: hand queued dialog results to the game. This mirrors what
// Native::EventDispatcher::textInput() does -- store the text where
// AGViewGetTextInput() reads it, then raise AGEvent type 6 -- but calls the
// game's HandleEvents directly, since our hand-built BridgeApp has neither its
// init flag (+0x69) set nor the game's handlers registered in its dispatcher.
// The game then clears the EditWidget, types the text in and presses Enter
// (Escape when GetTextInput returns NULL, i.e. cancelled).
static void deliver_pending_texts(GuestCall &c, uint32_t handleEvents, uint32_t appDriver, uint32_t event_buf) {
    std::vector<PendingText> texts;
    {
        std::lock_guard<std::mutex> lk(g_text_lock);
        if (g_pending_texts.empty()) return;
        texts.swap(g_pending_texts);
    }
    uint32_t native_base = 0;
    for (uint32_t i = 0; i < c.img->module_count; ++i) {
        if (std::strstr(c.img->modules[i].name, "libnative_code.so")) {
            native_base = c.img->modules[i].base;
            break;
        }
    }
    uint32_t bridge = native_base ? c.read32(native_base + kBridgeAppSingleton) : 0;
    if (!bridge) return;
    uint32_t dispatcher = bridge + 0x28;

    for (const auto &t : texts) {
        g_text_dialog_open.store(false);
        // libstdc++ COW string rep: length, capacity, refcount, chars. The game
        // only reads c_str(), so a private rep that nobody releases is enough.
        uint32_t len = (uint32_t)t.text.size();
        uint32_t rep = c.rt->heap.alloc(len + 13);
        if (!rep) return;
        c.write32(rep + 0, len);
        c.write32(rep + 4, len);
        c.write32(rep + 8, 0);
        c.put_cstr(rep + 12, t.text);

        c.write8(dispatcher + 0x38, t.cancelled ? 0 : 1);
        c.write32(dispatcher + 0x3c, rep + 12);
        LOGI("Delivering text input to guest: \"%s\"%s", t.text.c_str(), t.cancelled ? " (cancelled)" : "");

        for (uint32_t off = 0; off < 0x30; off += 4) c.write32(event_buf + off, 0);
        c.write32(event_buf + 0x00, 6); // AGEvent type 6 -> AndroidAppDriver::HandleInputEvents
        uint32_t args[2] = { event_buf, appDriver };
        c.call(handleEvents, args, 2);
        c.write8(dispatcher + 0x38, 0);
    }
}

namespace {

constexpr uint32_t kEglTrue = 1;
constexpr uint32_t kEglFalse = 0;
constexpr uint32_t kEglDisplay = 0x4E800001u;
constexpr uint32_t kEglSurface = 0x4E800003u;
constexpr uint32_t kEglContext = 0x4E800004u;

void egl_get_display(GuestCall &c) {
    c.set_result(kEglDisplay);
}

void egl_initialize(GuestCall &c) {
    uint32_t major_ptr = c.arg(1);
    uint32_t minor_ptr = c.arg(2);
    if (major_ptr && c.in_bounds(major_ptr, 4)) c.write32(major_ptr, 1);
    if (minor_ptr && c.in_bounds(minor_ptr, 4)) c.write32(minor_ptr, 4);
    c.set_result(kEglTrue);
}

void egl_choose_config(GuestCall &c) {
    uint32_t configs_out = c.arg(2);
    uint32_t num_config_out = c.arg(4);
    if (configs_out && c.in_bounds(configs_out, 4)) c.write32(configs_out, 1);
    if (num_config_out && c.in_bounds(num_config_out, 4)) c.write32(num_config_out, 1);
    c.set_result(kEglTrue);
}

void egl_get_config_attrib(GuestCall &c) {
    uint32_t attr = c.arg(2);
    uint32_t out  = c.arg(3);
    int32_t v = 0;
    switch (attr) {
        case 0x3020 /* EGL_BUFFER_SIZE */: v = 32; break;
        case 0x3024 /* EGL_RED_SIZE */: v = 8; break;
        case 0x3023 /* EGL_GREEN_SIZE */: v = 8; break;
        case 0x3022 /* EGL_BLUE_SIZE */: v = 8; break;
        case 0x3021 /* EGL_ALPHA_SIZE */: v = 8; break;
        case 0x3025 /* EGL_DEPTH_SIZE */: v = 16; break;
        case 0x3026 /* EGL_STENCIL_SIZE */: v = 0; break;
        case 0x3028 /* EGL_CONFIG_ID */: v = 1; break;
        case 0x3033 /* EGL_SURFACE_TYPE */: v = 0x0004; break; // EGL_WINDOW_BIT
        case 0x303d /* EGL_RENDERABLE_TYPE */: v = 0x0001; break; // EGL_OPENGL_ES_BIT
        default: v = 0; break;
    }
    if (out && c.in_bounds(out, 4)) {
        c.write32(out, (uint32_t)v);
    }
    c.set_result(kEglTrue);
}

void egl_create_window_surface(GuestCall &c) {
    c.set_result(kEglSurface);
}

void egl_create_context(GuestCall &c) {
    c.set_result(kEglContext);
}

void egl_make_current(GuestCall &c) {
    c.set_result(kEglTrue);
}

static void dispatch_pointer_event(GuestCall &c, uint32_t handleEvents, uint32_t appDriver, uint32_t s_event_buf,
                                   uint32_t type, float x, float y, float pressure, int pointerId) {
    uint32_t uX, uY, uP;
    std::memcpy(&uX, &x, 4);
    std::memcpy(&uY, &y, 4);
    std::memcpy(&uP, &pressure, 4);

    c.write32(s_event_buf + 0x00, type); // 2 = POINTER_DOWN, 3 = POINTER_MOVE, 4 = POINTER_UP, 5 = POINTER_CANCEL
    c.write32(s_event_buf + 0x04, (uint32_t)pointerId);
    c.write32(s_event_buf + 0x08, 0x1002); // SOURCE_TOUCHSCREEN
    c.write32(s_event_buf + 0x0c, 0);
    c.write32(s_event_buf + 0x10, 0);
    c.write32(s_event_buf + 0x14, 0);
    c.write32(s_event_buf + 0x18, 0);
    c.write32(s_event_buf + 0x20, uX);
    c.write32(s_event_buf + 0x24, uY);
    c.write32(s_event_buf + 0x28, uP);

    uint32_t args[2] = { s_event_buf, appDriver };
    c.call(handleEvents, args, 2);
}

// AGEvent type 8 -> AndroidAppDriver::HandleFocusChangedEvent, which is how the
// game learns it lost focus and pauses itself.
static void dispatch_focus_event(GuestCall &c, uint32_t handleEvents, uint32_t appDriver,
                                 uint32_t s_event_buf, bool focused) {
    for (uint32_t off = 0; off < 0x30; off += 4) c.write32(s_event_buf + off, 0);
    c.write32(s_event_buf + 0x00, 8);
    c.write32(s_event_buf + 0x10, focused ? 1 : 0);
    uint32_t args[2] = { s_event_buf, appDriver };
    c.call(handleEvents, args, 2);
}

// Parks the guest frame loop while the activity is paused. Called from
// eglSwapBuffers, i.e. on the guest thread and between frames, so the game is
// stopped at a point where it expects to wait for the display anyway.
static void park_while_paused(GuestCall &c, uint32_t handleEvents, uint32_t appDriver,
                              uint32_t s_event_buf) {
    if (!g_paused.load(std::memory_order_acquire)) return;

    LOGI("Paused: parking the guest frame loop");
    dispatch_focus_event(c, handleEvents, appDriver, s_event_buf, false);
    android_runner_set_audio_paused(true);

    {
        std::unique_lock<std::mutex> lk(g_pause_lock);
        g_pause_cv.wait(lk, [&] {
            return !g_paused.load(std::memory_order_acquire) ||
                   c.rt->shutdown_requested.load(std::memory_order_acquire);
        });
    }

    android_runner_set_audio_paused(false);
    dispatch_focus_event(c, handleEvents, appDriver, s_event_buf, true);
    LOGI("Resumed");
}

static void dispatch_key_event(GuestCall &c, uint32_t handleEvents, uint32_t appDriver, uint32_t s_event_buf,
                               uint32_t action, int keyCode) {
    c.write32(s_event_buf + 0x00, action); // 0 = KEY_DOWN, 1 = KEY_UP
    c.write32(s_event_buf + 0x04, 0);
    c.write32(s_event_buf + 0x08, keyCode);
    c.write32(s_event_buf + 0x0c, 0);
    c.write32(s_event_buf + 0x10, 0);
    c.write32(s_event_buf + 0x14, 0);
    c.write32(s_event_buf + 0x18, keyCode);
    c.write32(s_event_buf + 0x1c, 0);
    c.write32(s_event_buf + 0x20, 0);

    uint32_t args[2] = { s_event_buf, appDriver };
    c.call(handleEvents, args, 2);
}

void egl_swap_buffers(GuestCall &c) {
    static int s_swap_count = 0;
    static uint32_t s_event_buf = 0;

    {
        std::lock_guard<std::mutex> lk(g_egl_lock);
        // If window changed, handle recreation on the render thread
        if (g_window_changed.load(std::memory_order_acquire)) {
            ANativeWindow *win = nullptr;
            {
                std::lock_guard<std::mutex> wlk(g_window_mutex);
                win = g_pending_window;
            }
            if (win && win != g_current_window) {
                LOGI("Window changed, recreating surface on render thread...");
                g_current_window = win;
                int32_t width = ANativeWindow_getWidth(win);
                int32_t height = ANativeWindow_getHeight(win);
                pvz2_surface_set(width > 0 ? width : 1280, height > 0 ? height : 720);

                EGLint format = 0;
                eglGetConfigAttrib(g_egl_display, g_egl_config, EGL_NATIVE_VISUAL_ID, &format);
                ANativeWindow_setBuffersGeometry(win, 0, 0, format);

                if (g_egl_surface != EGL_NO_SURFACE) {
                    eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                    eglDestroySurface(g_egl_display, g_egl_surface);
                    g_egl_surface = EGL_NO_SURFACE;
                }
                g_egl_surface = eglCreateWindowSurface(g_egl_display, g_egl_config, win, nullptr);
                if (g_egl_surface != EGL_NO_SURFACE && g_egl_context != EGL_NO_CONTEXT) {
                    eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
                    g_surface_ready.store(true, std::memory_order_release);
                }
            } else if (!win && g_egl_surface != EGL_NO_SURFACE) {
                LOGI("Window destroyed, releasing surface on render thread...");
                eglMakeCurrent(g_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroySurface(g_egl_display, g_egl_surface);
                g_egl_surface = EGL_NO_SURFACE;
                g_current_window = nullptr;
                g_surface_ready.store(false, std::memory_order_release);
            }
            g_window_changed.store(false, std::memory_order_release);
        }

        if (g_surface_ready.load(std::memory_order_acquire) && g_egl_display != EGL_NO_DISPLAY && g_egl_surface != EGL_NO_SURFACE) {
            eglSwapBuffers(g_egl_display, g_egl_surface);
        }
    }

    ++s_swap_count;
    if (s_swap_count <= 3 || (s_swap_count % 300 == 0)) {
        PVZTV_TRACE("eglSwapBuffers #%d", s_swap_count);
    }

    if (!s_event_buf) {
        s_event_buf = c.rt->heap.alloc(128);
    }

    uint32_t gameMainBase = 0;
    for (uint32_t i = 0; i < c.img->module_count; ++i) {
        if (std::strstr(c.img->modules[i].name, "libGameMain.so")) {
            gameMainBase = c.img->modules[i].base;
            break;
        }
    }

    uint32_t lawnApp = gameMainBase ? c.read32(gameMainBase + 0x00715d50) : 0;
    uint32_t appDriver = lawnApp ? c.read32(lawnApp + 0x2ec) : 0;
    uint32_t handleEvents = gameMainBase + 0x003f9589;

    static bool s_logged_app = false;
    if (appDriver && !s_logged_app) {
        s_logged_app = true;
        LOGI("PopCap LawnApp detected at 0x%08X (appDriver=0x%08X), input dispatcher ACTIVE!",
             lawnApp, appDriver);
    }

    if (appDriver && handleEvents) {
        park_while_paused(c, handleEvents, appDriver, s_event_buf);
        deliver_pending_texts(c, handleEvents, appDriver, s_event_buf);

        std::vector<QueuedInputEvent> events;
        {
            std::lock_guard<std::mutex> lk(g_input_queue_lock);
            events.swap(g_input_queue);
        }

        int winW = pvz2_surface_width();
        int winH = pvz2_surface_height();
        if (winW <= 0) winW = 1280;
        if (winH <= 0) winH = 720;

        float scaleX = 1280.0f / (float)winW;
        float scaleY = 720.0f / (float)winH;

        // The game (with libHomura's touch mod) handles touch itself, so the
        // events go through as they arrive, like the original Java side sends
        // them -- no drag deadzone and no poking at LawnApp internals.
        for (const auto &ev : events) {
            if (ev.type == QueuedInputEvent::TOUCH) {
                float mouseX = std::fmin(std::fmax(ev.x * scaleX, 0.0f), 1280.0f);
                float mouseY = std::fmin(std::fmax(ev.y * scaleY, 0.0f), 720.0f);
                float pressure = (ev.action == 4 || ev.action == 5) ? 0.0f : 1.0f;
                dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf,
                                       (uint32_t)ev.action, mouseX, mouseY, pressure, ev.pointer_id);
            } else if (ev.type == QueuedInputEvent::KEY) {
                dispatch_key_event(c, handleEvents, appDriver, s_event_buf,
                                   ev.action, ev.key_code);
            }
        }
    }

    c.set_result(kEglTrue);
}

void egl_query_surface(GuestCall &c) {
    uint32_t attr = c.arg(2);
    uint32_t out = c.arg(3);
    uint32_t v = 0;
    constexpr uint32_t kEglWidth = 0x3057;
    constexpr uint32_t kEglHeight = 0x3056;

    if (attr == kEglWidth) v = pvz2_surface_width();
    else if (attr == kEglHeight) v = pvz2_surface_height();

    if (out && c.in_bounds(out, 4)) c.write32(out, v);
    c.set_result(kEglTrue);
}

void egl_query_context(GuestCall &c) {
    uint32_t attr = c.arg(2);
    uint32_t out = c.arg(3);
    uint32_t v = 1; // EGL_CONTEXT_CLIENT_VERSION = 1 (GLES 1.1)
    if (out && c.in_bounds(out, 4)) {
        c.write32(out, v);
    }
    c.set_result(kEglTrue);
}

void egl_get_error(GuestCall &c) {
    c.set_result(EGL_SUCCESS);
}

void egl_terminate(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_destroy_surface(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_destroy_context(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_swap_interval(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_bind_api(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_query_string(GuestCall &c) {
    uint32_t name = c.arg(1);
    const char *str = "";
    if (name == EGL_VENDOR) str = "Android";
    else if (name == EGL_VERSION) str = "1.4 Android";
    else if (name == EGL_EXTENSIONS) str = "";

    uint32_t len = (uint32_t)std::strlen(str) + 1;
    uint32_t addr = c.rt->heap.alloc(len);
    if (addr) std::memcpy(&c.img->mem[addr], str, len);
    c.set_result(addr);
}

void egl_get_current_context(GuestCall &c) { c.set_result(kEglContext); }
void egl_get_current_display(GuestCall &c) { c.set_result(kEglDisplay); }
void egl_get_current_surface(GuestCall &c) { c.set_result(kEglSurface); }
void egl_get_proc_address(GuestCall &c) { c.set_result(0); }

} // namespace

void register_libegl(ImportTable &t) {
    t.add("eglGetDisplay", egl_get_display);
    t.add("eglInitialize", egl_initialize);
    t.add("eglChooseConfig", egl_choose_config);
    t.add("eglGetConfigAttrib", egl_get_config_attrib);
    t.add("eglCreateWindowSurface", egl_create_window_surface);
    t.add("eglCreateContext", egl_create_context);
    t.add("eglMakeCurrent", egl_make_current);
    t.add("eglSwapBuffers", egl_swap_buffers);
    t.add("eglQuerySurface", egl_query_surface);
    t.add("eglQueryContext", egl_query_context);
    t.add("eglGetError", egl_get_error);
    t.add("eglTerminate", egl_terminate);
    t.add("eglDestroySurface", egl_destroy_surface);
    t.add("eglDestroyContext", egl_destroy_context);
    t.add("eglSwapInterval", egl_swap_interval);
    t.add("eglBindAPI", egl_bind_api);
    t.add("eglQueryString", egl_query_string);
    t.add("eglGetCurrentContext", egl_get_current_context);
    t.add("eglGetCurrentDisplay", egl_get_current_display);
    t.add("eglGetCurrentSurface", egl_get_current_surface);
    t.add("eglGetProcAddress", egl_get_proc_address);
}

} // namespace pvz_tv
