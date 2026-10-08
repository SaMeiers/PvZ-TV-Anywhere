#include "runner_core.h"
#include <pvz_tv/surface.h>

#include <android/native_window_jni.h>
#include <android/log.h>
#include <jni.h>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#define LOG_TAG "RunnerJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace pvz_tv;

static JavaVM *g_vm = nullptr;
static jobject g_view = nullptr; // global ref to com.transmension.mobile.NativeView

// Runs fn with a JNIEnv for the current thread, attaching (and detaching again)
// if it is a runner thread the VM does not know about.
template <typename Fn>
static void with_jni_env(Fn fn) {
    if (!g_vm || !g_view) return;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    fn(env);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

namespace pvz_tv {

void android_runner_show_text_dialog(int mode, const std::string &title, const std::string &hint,
                                     const std::string &initial) {
    with_jni_env([&](JNIEnv *env) {
        jclass cls = env->GetObjectClass(g_view);
        jmethodID mid = env->GetMethodID(cls, "showTextInputDialog2",
                                         "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        if (mid) {
            jstring jTitle = env->NewStringUTF(title.c_str());
            jstring jHint = env->NewStringUTF(hint.c_str());
            jstring jInitial = env->NewStringUTF(initial.c_str());
            env->CallVoidMethod(g_view, mid, (jint)mode, jTitle, jHint, jInitial);
            env->DeleteLocalRef(jTitle);
            env->DeleteLocalRef(jHint);
            env->DeleteLocalRef(jInitial);
        }
        env->DeleteLocalRef(cls);
    });
}

void android_runner_hide_text_dialog() {
    with_jni_env([&](JNIEnv *env) {
        jclass cls = env->GetObjectClass(g_view);
        jmethodID mid = env->GetMethodID(cls, "hideTextInputDialog2", "()V");
        if (mid) env->CallVoidMethod(g_view, mid);
        env->DeleteLocalRef(cls);
    });
}

bool android_runner_video(int op, const std::string &path) {
    // EnhanceActivity's video methods, in the order of the mod's HostVideoOp
    // (IntroVideo.cpp). show/hide are the one void method, videoShow(Z)V.
    static const char *const kMethods[] = {"videoOpen", "videoPlay", "videoStop", "videoClose",
                                           "videoIsPlaying", "videoShow", "videoShow"};
    if (op < 0 || op >= (int)(sizeof(kMethods) / sizeof(kMethods[0]))) return false;
    bool result = false;
    with_jni_env([&](JNIEnv *env) {
        jclass viewCls = env->GetObjectClass(g_view);
        jmethodID getContext = env->GetMethodID(viewCls, "getContext", "()Landroid/content/Context;");
        jobject activity = getContext ? env->CallObjectMethod(g_view, getContext) : nullptr;
        env->DeleteLocalRef(viewCls);
        if (!activity) return;
        jclass cls = env->GetObjectClass(activity);
        const bool isShow = op >= 5;
        const char *sig = op == 0 ? "(Ljava/lang/String;)Z" : isShow ? "(Z)V" : "()Z";
        jmethodID mid = env->GetMethodID(cls, kMethods[op], sig);
        if (!mid) {
            LOGE("Video bridge: %s%s not found on the activity", kMethods[op], sig);
        } else if (op == 0) {
            jstring jPath = env->NewStringUTF(path.c_str());
            result = env->CallBooleanMethod(activity, mid, jPath) == JNI_TRUE;
            env->DeleteLocalRef(jPath);
        } else if (isShow) {
            env->CallVoidMethod(activity, mid, (jboolean)(op == 5));
            result = true;
        } else {
            result = env->CallBooleanMethod(activity, mid) == JNI_TRUE;
        }
        env->DeleteLocalRef(cls);
        env->DeleteLocalRef(activity);
    });
    return result;
}

void android_runner_finish_activity() {
    LOGI("Guest main() returned: finishing the activity");
    with_jni_env([&](JNIEnv *env) {
        jclass viewCls = env->GetObjectClass(g_view);
        jmethodID getContext = env->GetMethodID(viewCls, "getContext", "()Landroid/content/Context;");
        jobject ctx = getContext ? env->CallObjectMethod(g_view, getContext) : nullptr;
        jclass activityCls = env->FindClass("android/app/Activity");
        if (ctx && activityCls && env->IsInstanceOf(ctx, activityCls)) {
            jmethodID finish = env->GetMethodID(activityCls, "finishAffinity", "()V");
            if (finish) env->CallVoidMethod(ctx, finish);
        }
        if (activityCls) env->DeleteLocalRef(activityCls);
        if (ctx) env->DeleteLocalRef(ctx);
        env->DeleteLocalRef(viewCls);
    });
    // RunnerCore is a process-wide singleton that cannot be re-initialised, so
    // the next launch needs a fresh process. Give the activity time to close.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    _exit(0);
}

} // namespace pvz_tv

extern "C" {

// ============================================================================
// com.transmension.mobile.NativeApp
// ============================================================================

JNIEXPORT jlong JNICALL Java_com_transmension_mobile_NativeApp_loadNativeApp(
    JNIEnv *env, jclass clazz,
    jstring jpath, jstring jfuncname,
    jobject activity, jobject view,
    jstring juserDataPath, jstring jexternalDataPath,
    jint sdkVersion, jobject assetManager) {

    const char *path = env->GetStringUTFChars(jpath, nullptr);
    const char *userDataPath = juserDataPath ? env->GetStringUTFChars(juserDataPath, nullptr) : nullptr;

    LOGI("loadNativeApp called: path=%s, userData=%s, sdk=%d",
         path ? path : "null",
         userDataPath ? userDataPath : "null",
         (int)sdkVersion);

    env->GetJavaVM(&g_vm);
    if (view && !g_view) g_view = env->NewGlobalRef(view);

    bool ok = RunnerCore::instance().init(path, userDataPath);

    if (userDataPath) env->ReleaseStringUTFChars(juserDataPath, userDataPath);
    if (path) env->ReleaseStringUTFChars(jpath, path);

    if (!ok) {
        LOGE("RunnerCore::init failed!");
        return 0;
    }

    if (!RunnerCore::instance().start()) {
        LOGE("RunnerCore::start failed!");
        return 0;
    }

    return (jlong)1;
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_unloadNativeApp(
    JNIEnv *env, jclass clazz, jlong handle) {
    LOGI("unloadNativeApp called");
    RunnerCore::instance().stop();
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onPauseNative(
    JNIEnv *env, jclass clazz, jlong handle) {
    LOGI("onPauseNative called");
    RunnerCore::instance().pause();
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onResumeNative(
    JNIEnv *env, jclass clazz, jlong handle) {
    LOGI("onResumeNative called");
    RunnerCore::instance().resume();
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onStartNative(
    JNIEnv *env, jclass clazz, jlong handle) {
    LOGI("onStartNative called");
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onStopNative(
    JNIEnv *env, jclass clazz, jlong handle) {
    LOGI("onStopNative called");
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onConfigurationChangedNative(
    JNIEnv *env, jclass clazz, jlong handle) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onLowMemoryNative(
    JNIEnv *env, jclass clazz, jlong handle) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_processWorksNative(
    JNIEnv *env, jclass clazz, jlong handle) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onNewIntentNative(
    JNIEnv *env, jclass clazz, jlong handle) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onNetworkConnectivityChanged(
    JNIEnv *env, jclass clazz, jlong handle, jboolean connected, jint type) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onHeadsetStateChanged(
    JNIEnv *env, jclass clazz, jlong handle, jstring str, jint i1, jint i2) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeApp_onMessageBoxButtonClickedNative(
    JNIEnv *env, jclass clazz, jlong handle, jint i1, jint i2) {
}

// ============================================================================
// com.transmension.mobile.NativeView
// ============================================================================

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onSurfaceCreatedNative(
    JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
    LOGI("onSurfaceCreatedNative called");
    ANativeWindow *win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    android_runner_set_window(win);
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onSurfaceChangedNative(
    JNIEnv *env, jobject thiz, jlong handle, jobject surface, jint format, jint width, jint height) {
    LOGI("onSurfaceChangedNative called: %dx%d", (int)width, (int)height);
    ANativeWindow *win = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    android_runner_set_window(win);
    pvz2_surface_set((uint32_t)width, (uint32_t)height);
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onSurfaceDestroyedNative(
    JNIEnv *env, jobject thiz, jlong handle) {
    LOGI("onSurfaceDestroyedNative called");
    android_runner_destroy_window();
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onSurfaceRedrawNeededNative(
    JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onContentRectChangedNative(
    JNIEnv *env, jobject thiz, jlong handle, jint x, jint y, jint w, jint h) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onKeyboardFrameNative(
    JNIEnv *env, jobject thiz, jlong handle, jint frame) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onTextChangedNative(
    JNIEnv *env, jobject thiz, jlong handle, jstring str, jint start, jint len, jlong cookie) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onTextInputNative(
    JNIEnv *env, jobject thiz, jlong handle, jstring str) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onTextInputNative2(
    JNIEnv *env, jobject thiz, jstring str) {
    // Empty text (Cancel / back) is reported to the guest as a cancelled input.
    std::string text;
    if (str) {
        const char *s = env->GetStringUTFChars(str, nullptr);
        if (s) {
            text = s;
            env->ReleaseStringUTFChars(str, s);
        }
    }
    LOGI("onTextInputNative2: \"%s\"", text.c_str());
    android_runner_queue_text(text, text.empty());
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeView_onWindowFocusChangedNative(
    JNIEnv *env, jobject thiz, jlong handle, jboolean focus) {
}

// ============================================================================
// com.transmension.mobile.NativeInputManager
// ============================================================================

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeInputManager_onTouchEventNative(
    JNIEnv *env, jclass clazz, jlong handle, jobject inputManager, jobject pointerEvent) {
    if (!pointerEvent) return;

    jclass clsInputEvent = env->GetObjectClass(pointerEvent);
    jfieldID fidAction = env->GetFieldID(clsInputEvent, "mAction", "I");
    if (!fidAction) return;

    jint rawAction = env->GetIntField(pointerEvent, fidAction);
    int actionMasked = rawAction & 0xFF;
    int actionIndex = (rawAction >> 8) & 0xFF;

    jfieldID fidPointers = env->GetFieldID(clsInputEvent, "mPointers", "Ljava/util/List;");
    if (!fidPointers) return;

    jobject listObj = env->GetObjectField(pointerEvent, fidPointers);
    if (!listObj) return;

    jclass clsList = env->GetObjectClass(listObj);
    jmethodID midSize = env->GetMethodID(clsList, "size", "()I");
    jmethodID midGet = env->GetMethodID(clsList, "get", "(I)Ljava/lang/Object;");

    jint count = env->CallIntMethod(listObj, midSize);
    for (jint i = 0; i < count; ++i) {
        jobject ptr = env->CallObjectMethod(listObj, midGet, i);
        if (!ptr) continue;

        jclass clsPtr = env->GetObjectClass(ptr);
        jfieldID fidId = env->GetFieldID(clsPtr, "mId", "I");
        jfieldID fidX = env->GetFieldID(clsPtr, "mX", "F");
        jfieldID fidY = env->GetFieldID(clsPtr, "mY", "F");

        jint id = fidId ? env->GetIntField(ptr, fidId) : 0;
        jfloat x = fidX ? env->GetFloatField(ptr, fidX) : 0.0f;
        jfloat y = fidY ? env->GetFloatField(ptr, fidY) : 0.0f;

        int runnerAction = 3; // POINTER_MOVE
        if (actionMasked == 0 /* ACTION_DOWN */) {
            runnerAction = 2; // POINTER_DOWN
        } else if (actionMasked == 5 /* ACTION_POINTER_DOWN */) {
            runnerAction = (i == actionIndex) ? 2 : 3;
        } else if (actionMasked == 1 /* ACTION_UP */) {
            runnerAction = 4; // POINTER_UP
        } else if (actionMasked == 6 /* ACTION_POINTER_UP */) {
            runnerAction = (i == actionIndex) ? 4 : 3;
        } else if (actionMasked == 3 /* ACTION_CANCEL */) {
            runnerAction = 5; // POINTER_CANCEL
        }

        android_runner_queue_touch(runnerAction, x, y, id);
        env->DeleteLocalRef(ptr);
    }
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeInputManager_onKeyInputEventNative(
    JNIEnv *env, jclass clazz, jlong handle, jobject inputManager, jobject keyInputEvent) {
    if (!keyInputEvent) return;

    jclass clsKey = env->GetObjectClass(keyInputEvent);
    jfieldID fidAction = env->GetFieldID(clsKey, "mAction", "I");
    jfieldID fidKeyCode = env->GetFieldID(clsKey, "mKeyCode", "I");

    if (fidAction && fidKeyCode) {
        jint action = env->GetIntField(keyInputEvent, fidAction);
        jint keyCode = env->GetIntField(keyInputEvent, fidKeyCode);
        android_runner_queue_key((int)action, (int)keyCode);
    }
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeInputManager_onJoystickEventNative(
    JNIEnv *env, jclass clazz, jlong handle, jobject inputManager, jobject joystickEvent) {
}

JNIEXPORT void JNICALL Java_com_transmension_mobile_NativeInputManager_onSensorInputEventNative(
    JNIEnv *env, jclass clazz, jlong handle, jobject inputManager, jobject sensorInputEvent) {
}

} // extern "C"
