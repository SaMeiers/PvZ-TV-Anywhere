#pragma once

#include <pvz_tv/elf32/elf32_loader.h>
#include <pvz_tv/runtime/guest_runtime.h>
#include <pvz_tv/dependencies/dependency.h>

#include <android/native_window.h>
#include <jni.h>
#include <string>
#include <memory>
#include <vector>

#define RUNNER_API __attribute__((visibility("default")))

namespace pvz_tv {

RUNNER_API void android_runner_set_window(ANativeWindow *window);
RUNNER_API void android_runner_destroy_window();
RUNNER_API bool android_runner_wait_for_window(int timeout_ms = 5000);
RUNNER_API bool android_runner_init_egl_on_render_thread();
RUNNER_API void android_runner_queue_touch(int action, float x, float y, int pointer_id);
RUNNER_API void android_runner_queue_key(int action, int key_code);

// Soft keyboard bridge. The guest's "show keyboard" works are spotted in the
// NativeApp work queue and turned into a Java text dialog (runner_jni.cpp);
// the dialog's result is queued here and handed to the guest on its own thread.
RUNNER_API void android_runner_queue_text(const std::string &text, bool cancelled);
void android_runner_inspect_pending_works(GuestCall &c, uint32_t native_app, uint32_t native_base);
void android_runner_show_text_dialog(int mode, const std::string &title, const std::string &hint,
                                     const std::string &initial);
void android_runner_hide_text_dialog();
// Activity lifecycle: park the guest frame loop and silence audio in the background.
RUNNER_API void android_runner_set_paused(bool paused);
void android_runner_set_audio_paused(bool paused);
// The guest's main() returned (the player quit): close the app.
void android_runner_finish_activity();

class RUNNER_API RunnerCore {
public:
    static RunnerCore &instance();

    bool init(const char *game_so_path, const char *data_dir);
    bool start();
    void pause();
    void resume();
    void stop();

    // Cheat / Mod Menu / Settings support
    void setFeature(int featNum, int value, bool boolean, const char *str);
    jobjectArray getFeatureList(JNIEnv *env);
    jobjectArray getSettingsList(JNIEnv *env);
    std::string getCurrentFormation();

    // Game controls and touch
    void sendSecondTouch(int x, int y, int action);
    void sendButtonEvent(bool isButtonDown, int buttonCode);
    void switchTwoPlayerMode(bool isOn);

    // State inspection
    bool isInGame();
    uint32_t findSymbol(const char *name);
    uint32_t callGuest(uint32_t fn, const uint32_t *args, int nargs);
    uint8_t *guestMemory() { return image_.mem; }
    uint32_t guestMemorySize() { return image_.mem_size; }

    pvz2_elf_image_t *getImage() { return &image_; }
    GuestRuntime *getRuntime() { return &runtime_; }

private:
    RunnerCore() = default;
    ~RunnerCore();

    pvz2_elf_image_t image_{};
    GuestRuntime runtime_{};
    bool initialized_ = false;
    bool running_ = false;
    std::string data_dir_;

    void cacheCheatSymbols();
};

} // namespace pvz_tv
