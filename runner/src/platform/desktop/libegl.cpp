#include <pvz_tv/diagnostics.h>
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/surface.h>

#include <SDL.h>
#include <glad/gl.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>

extern SDL_Window *g_sdl_window;

namespace pvz_tv {

namespace {

constexpr uint32_t kEglDisplay = 0x4e800001u;
constexpr uint32_t kEglConfig  = 0x4e800002u;
constexpr uint32_t kEglSurface = 0x4e800003u;
constexpr uint32_t kEglContext = 0x4e800004u;

constexpr uint32_t kEglSuccess = 0x3000;
constexpr uint32_t kEglTrue    = 1;
constexpr uint32_t kEglFalse   = 0;

constexpr uint32_t kEglBufferSize  = 0x3020;
constexpr uint32_t kEglAlphaSize   = 0x3021;
constexpr uint32_t kEglBlueSize    = 0x3022;
constexpr uint32_t kEglGreenSize   = 0x3023;
constexpr uint32_t kEglRedSize     = 0x3024;
constexpr uint32_t kEglDepthSize   = 0x3025;
constexpr uint32_t kEglStencilSize = 0x3026;
constexpr uint32_t kEglConfigId    = 0x3028;
constexpr uint32_t kEglSurfaceType = 0x3033;
constexpr uint32_t kEglRenderable  = 0x303d;

constexpr uint32_t kEglWidth  = 0x3057;
constexpr uint32_t kEglHeight = 0x3056;

void egl_get_display(GuestCall &c) {
    c.set_result(kEglDisplay);
}

void egl_initialize(GuestCall &c) {
    uint32_t major = c.arg(1);
    uint32_t minor = c.arg(2);
    if (major && c.in_bounds(major, 4)) c.write32(major, 1);
    if (minor && c.in_bounds(minor, 4)) c.write32(minor, 4);
    c.set_result(kEglTrue);
}

void egl_get_error(GuestCall &c) {
    c.set_result(kEglSuccess);
}

void egl_choose_config(GuestCall &c) {
    uint32_t configs = c.arg(2);
    uint32_t num_config = c.arg(4);
    if (configs && c.in_bounds(configs, 4)) {
        c.write32(configs, kEglConfig);
    }
    if (num_config && c.in_bounds(num_config, 4)) {
        c.write32(num_config, 1);
    }
    c.set_result(kEglTrue);
}

void egl_get_config_attrib(GuestCall &c) {
    uint32_t attr = c.arg(2);
    uint32_t out  = c.arg(3);
    int32_t v = 0;
    switch (attr) {
        case kEglBufferSize: v = 32; break;
        case kEglRedSize: v = 8; break;
        case kEglGreenSize: v = 8; break;
        case kEglBlueSize: v = 8; break;
        case kEglAlphaSize: v = 8; break;
        case kEglDepthSize: v = 24; break;
        case kEglStencilSize: v = 8; break;
        case kEglConfigId: v = 1; break;
        case kEglSurfaceType: v = 0x0004; break; // EGL_WINDOW_BIT
        case kEglRenderable: v = 0x0001; break;  // EGL_OPENGL_ES_BIT
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

void egl_destroy_surface(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_destroy_context(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_make_current(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_get_current_display(GuestCall &c) {
    c.set_result(kEglDisplay);
}

void egl_get_current_context(GuestCall &c) {
    c.set_result(kEglContext);
}

void egl_query_context(GuestCall &c) {
    uint32_t out = c.arg(3);
    if (out && c.in_bounds(out, 4)) c.write32(out, 1);
    c.set_result(kEglTrue);
}

void egl_query_surface(GuestCall &c) {
    uint32_t attr = c.arg(2);
    uint32_t out = c.arg(3);
    uint32_t v = 0;
    if (attr == kEglWidth) v = pvz2_surface_width();
    else if (attr == kEglHeight) v = pvz2_surface_height();
    if (out && c.in_bounds(out, 4)) c.write32(out, v);
    c.set_result(kEglTrue);
}

static void take_screenshot(const char *filename) {
    int w = pvz2_surface_width();
    int h = pvz2_surface_height();
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, surf->pixels);
    int pitch = surf->pitch;
    std::vector<uint8_t> temp(pitch);
    uint8_t *pixels = (uint8_t *)surf->pixels;
    for (int y = 0; y < h / 2; ++y) {
        uint8_t *top = pixels + y * pitch;
        uint8_t *bot = pixels + (h - 1 - y) * pitch;
        std::memcpy(temp.data(), top, pitch);
        std::memcpy(top, bot, pitch);
        std::memcpy(bot, temp.data(), pitch);
    }
    SDL_SaveBMP(surf, filename);
    SDL_FreeSurface(surf);
    printf("[*] Screenshot saved: %s\n", filename);
}

static void dispatch_pointer_event(GuestCall &c, uint32_t handleEvents, uint32_t appDriver, uint32_t s_event_buf,
                                   uint32_t type, float x, float y, float pressure) {
    uint32_t uX, uY, uP;
    std::memcpy(&uX, &x, 4);
    std::memcpy(&uY, &y, 4);
    std::memcpy(&uP, &pressure, 4);

    c.write32(s_event_buf + 0x00, type); // 2 = POINTER_DOWN, 3 = POINTER_MOVE, 4 = POINTER_UP, 5 = POINTER_CANCEL
    c.write32(s_event_buf + 0x04, 0);    // pointer_id = 0
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

static void dispatch_key_event(GuestCall &c, uint32_t handleEvents, uint32_t appDriver, uint32_t s_event_buf,
                               uint32_t action, int keyCode) {
    c.write32(s_event_buf + 0x00, action); // 0 = KEY_DOWN, 1 = KEY_UP
    c.write32(s_event_buf + 0x04, 0);
    c.write32(s_event_buf + 0x08, keyCode); // AKEYCODE_BACK = 4
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
    static bool s_logged_app = false;
    ++s_swap_count;
    if (s_swap_count <= 5 || (s_swap_count % 300 == 0)) {
        PVZTV_TRACE("[*] eglSwapBuffers #%d", s_swap_count);
    }

    if (!s_event_buf) {
        s_event_buf = c.rt->heap.alloc(128);
    }

    uint32_t gameMainBase = 0;
    for (uint32_t i = 0; i < c.img->module_count; ++i) {
        if (strstr(c.img->modules[i].name, "libGameMain.so")) {
            gameMainBase = c.img->modules[i].base;
            break;
        }
    }

    uint32_t lawnApp = gameMainBase ? c.read32(gameMainBase + 0x00715d50) : 0;
    uint32_t appDriver = lawnApp ? c.read32(lawnApp + 0x2ec) : 0;
    uint32_t handleEvents = gameMainBase + 0x003f9589;

    if (appDriver && !s_logged_app) {
        s_logged_app = true;
        PVZTV_TRACE("[+] PopCap LawnApp detected at 0x%08X (appDriver=0x%08X), input dispatcher ACTIVE!",
                    lawnApp, appDriver);
    }

    // Touch & Click reliable state tracking
    static bool s_touch_down = false;
    static float s_down_x = 0.0f;
    static float s_down_y = 0.0f;
    static float s_last_x = 0.0f;
    static float s_last_y = 0.0f;
    static bool s_is_dragging = false;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            printf("[*] SDL_QUIT requested by user\n");
            c.halt("user quit");
        } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F12) {
            char name[64];
            snprintf(name, sizeof(name), "screenshot_manual_%d.bmp", s_swap_count);
            take_screenshot(name);
        } else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_F11 || (ev.key.keysym.sym == SDLK_RETURN && (ev.key.keysym.mod & KMOD_ALT)))) {
            static bool s_is_fullscreen = false;
            s_is_fullscreen = !s_is_fullscreen;
            if (g_sdl_window) {
                SDL_SetWindowFullscreen(g_sdl_window, s_is_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
        } else if (ev.type == SDL_WINDOWEVENT && (ev.window.event == SDL_WINDOWEVENT_RESIZED || ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
            pvz2_surface_set((std::uint32_t)ev.window.data1, (std::uint32_t)ev.window.data2);
        } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            if (s_touch_down && appDriver != 0 && s_event_buf != 0) {
                dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf, 4, s_last_x, s_last_y, 0.0f);
                s_touch_down = false;
                s_is_dragging = false;
            }
        } else if (appDriver != 0 && s_event_buf != 0) {
            int winW = 1280, winH = 720;
            if (g_sdl_window) {
                SDL_GetWindowSize(g_sdl_window, &winW, &winH);
            }
            if (winW <= 0) winW = 1280;
            if (winH <= 0) winH = 720;

            uint32_t board = lawnApp ? c.read32(lawnApp + 0x8a0) : 0;
            uint32_t awardScreen = lawnApp ? c.read32(lawnApp + 0x8cc) : 0;
            uint32_t seedChooser = lawnApp ? c.read32(lawnApp + 0x8c4) : 0;
            int gameScene = lawnApp ? (int)c.read32(lawnApp + 0x900) : 0;
            int daveMsg = lawnApp ? (int)c.read32(lawnApp + 0x954) : -1;
            bool boardPaused = (board != 0) ? (c.read8(board + 0x259) != 0) : false;
            int tutorialState = (board != 0) ? (int)c.read32(board + 0x56a0) : 0;
            bool inShovelTutorial = (tutorialState >= 15 && tutorialState <= 17);
            bool is_gameplay = (board != 0) && !boardPaused && (awardScreen == 0) &&
                               ((gameScene == 3 /* SCENE_PLAYING */) || inShovelTutorial) &&
                               (daveMsg == -1);

            if (ev.type == SDL_MOUSEBUTTONDOWN) {
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    float mouseX = (float)ev.button.x * 1280.0f / (float)winW;
                    float mouseY = (float)ev.button.y * 720.0f / (float)winH;

                    s_touch_down = true;
                    s_down_x = mouseX;
                    s_down_y = mouseY;
                    s_last_x = mouseX;
                    s_last_y = mouseY;
                    s_is_dragging = false;

                    // Type 2 = POINTER_DOWN (pressure = 1.0f)
                    dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf, 2, mouseX, mouseY, 1.0f);
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    // Right click -> Android BACK key (AKEYCODE_BACK = 4)
                    dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 0, 4);
                    dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 1, 4);
                }
            } else if (ev.type == SDL_MOUSEMOTION) {
                float mouseX = (float)ev.motion.x * 1280.0f / (float)winW;
                float mouseY = (float)ev.motion.y * 720.0f / (float)winH;

                if (is_gameplay) {
                    // In active gameplay only: continuously update cursor position so plant preview follows mouse!
                    dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf, 3, mouseX, mouseY, s_touch_down ? 1.0f : 0.0f);
                    if (s_touch_down) {
                        s_last_x = mouseX;
                        s_last_y = mouseY;
                        s_is_dragging = true;
                    }
                } else if (s_touch_down && (ev.motion.state & SDL_BUTTON_LMASK)) {
                    float dx = mouseX - s_down_x;
                    float dy = mouseY - s_down_y;
                    float dist = std::sqrt(dx * dx + dy * dy);

                    if (!s_is_dragging) {
                        // Deadzone: a mouse trembles where a finger does not, so
                        // small movements (< 8px) stay a tap rather than turning
                        // into a drag the moment the button goes down.
                        if (dist >= 8.0f) {
                            s_is_dragging = true;
                            s_last_x = mouseX;
                            s_last_y = mouseY;
                            // Type 3 = POINTER_MOVE
                            dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf, 3, mouseX, mouseY, 1.0f);
                        }
                    } else {
                        // Genuine drag / scroll gesture
                        s_last_x = mouseX;
                        s_last_y = mouseY;
                        // Type 3 = POINTER_MOVE
                        dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf, 3, mouseX, mouseY, 1.0f);
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP) {
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (s_touch_down) {
                        float upX = s_is_dragging ? s_last_x : s_down_x;
                        float upY = s_is_dragging ? s_last_y : s_down_y;

                        // Type 4 = POINTER_UP (pressure = 0.0f)
                        dispatch_pointer_event(c, handleEvents, appDriver, s_event_buf, 4, upX, upY, 0.0f);
                        s_touch_down = false;
                        s_is_dragging = false;
                    }
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 1, 4);
                }
            } else if (ev.type == SDL_KEYDOWN && (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER || ev.key.keysym.sym == SDLK_SPACE)) {
                dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 0, 66 /* AKEYCODE_ENTER */);
            } else if (ev.type == SDL_KEYUP && (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER || ev.key.keysym.sym == SDLK_SPACE)) {
                dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 1, 66 /* AKEYCODE_ENTER */);
            } else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 0, 4);
            } else if (ev.type == SDL_KEYUP && ev.key.keysym.sym == SDLK_ESCAPE) {
                dispatch_key_event(c, handleEvents, appDriver, s_event_buf, 1, 4);
            }
        }
    }
    if (g_sdl_window) {
        SDL_GL_SwapWindow(g_sdl_window);
    }
    c.set_result(kEglTrue);
}

void egl_terminate(GuestCall &c) {
    c.set_result(kEglTrue);
}

void egl_get_proc_address(GuestCall &c) {
    c.set_result(0);
}

} // namespace

void register_libegl(ImportTable &t) {
    t.add("eglGetDisplay", egl_get_display);
    t.add("eglInitialize", egl_initialize);
    t.add("eglGetError", egl_get_error);
    t.add("eglChooseConfig", egl_choose_config);
    t.add("eglGetConfigAttrib", egl_get_config_attrib);
    t.add("eglCreateWindowSurface", egl_create_window_surface);
    t.add("eglCreateContext", egl_create_context);
    t.add("eglDestroySurface", egl_destroy_surface);
    t.add("eglDestroyContext", egl_destroy_context);
    t.add("eglMakeCurrent", egl_make_current);
    t.add("eglGetCurrentDisplay", egl_get_current_display);
    t.add("eglGetCurrentContext", egl_get_current_context);
    t.add("eglQueryContext", egl_query_context);
    t.add("eglQuerySurface", egl_query_surface);
    t.add("eglSwapBuffers", egl_swap_buffers);
    t.add("eglTerminate", egl_terminate);
    t.add("eglGetProcAddress", egl_get_proc_address);
}

} // namespace pvz_tv
