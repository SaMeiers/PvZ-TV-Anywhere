# The runner: loads the game's original 32-bit ARM libraries and executes them
# on a Dynarmic JIT. Everything here is shared between the Android app and the
# desktop player; only the graphics/audio backends differ, and those live in
# src/platform/<android|desktop>.
#
# Consumers add this directory and link the `pvztv_runner` target.

# On Android the runner *is* libnative_code.so, the library the game's Java
# code loads and calls into; elsewhere it is a static library the desktop
# player links.
if(ANDROID)
    set(PVZTV_RUNNER_KIND SHARED)
else()
    set(PVZTV_RUNNER_KIND STATIC)
endif()

add_library(pvztv_runner ${PVZTV_RUNNER_KIND}
    src/config.cpp
    src/diagnostics.cpp
    src/surface.cpp
    src/elf32/elf32_loader.c
    src/runtime/guest_heap.cpp
    src/runtime/guest_runtime.cpp
    src/runtime/guest_zlib.cpp
    src/dependencies/dependency.cpp
    src/dependencies/libandroid.cpp
    src/dependencies/libc_ctype.cpp
    src/dependencies/libc_extra.cpp
    src/dependencies/libc_locale.cpp
    src/dependencies/libc_misc.cpp
    src/dependencies/libc_pthread.cpp
    src/dependencies/libc_socket.cpp
    src/dependencies/libc_stdio.cpp
    src/dependencies/libc_stdlib.cpp
    src/dependencies/libc_string.cpp
    src/dependencies/libc_time.cpp
    src/dependencies/libc_unistd.cpp
    src/dependencies/libc_wchar.cpp
    src/dependencies/libdl.cpp
    src/dependencies/liblog.cpp
    src/dependencies/libm.cpp
    src/dependencies/libstdcxx.cpp
    src/dependencies/libz.cpp
    src/dependencies/vfs.cpp
)

# The diagnostic build traces every call the guest makes into the host; the
# normal build compiles that commentary out entirely. See pvz_tv/diagnostics.h.
option(PVZTV_DIAGNOSTICS "Build with the full guest-call tracing" OFF)
if(PVZTV_DIAGNOSTICS)
    target_compile_definitions(pvztv_runner PUBLIC PVZTV_DIAGNOSTICS=1)
endif()

target_include_directories(pvztv_runner PUBLIC include)
target_include_directories(pvztv_runner PRIVATE src)
target_compile_features(pvztv_runner PUBLIC cxx_std_20)
set_target_properties(pvztv_runner PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pvztv_runner PUBLIC dynarmic)

if(ANDROID)
    # The Android backends draw through the system EGL/GLES and play through
    # OpenSL ES; runner_jni.cpp is the bridge to the Java activity.
    target_sources(pvztv_runner PRIVATE
        src/platform/android/libegl_android.cpp
        src/platform/android/libgles_android.cpp
        src/platform/android/libaudio_android.cpp
        src/platform/android/runner_core.cpp
        src/platform/android/runner_jni.cpp
    )
    target_include_directories(pvztv_runner PUBLIC src/platform/android)
    set_target_properties(pvztv_runner PROPERTIES OUTPUT_NAME native_code)

    foreach(lib android EGL GLESv1_CM GLESv2 log OpenSLES z)
        find_library(ANDROID_${lib}_LIB ${lib})
        target_link_libraries(pvztv_runner PUBLIC ${ANDROID_${lib}_LIB})
    endforeach()
else()
    # The desktop backends draw through SDL2 + glad and play through SDL audio.
    target_sources(pvztv_runner PRIVATE
        src/platform/desktop/libegl.cpp
        src/platform/desktop/libgles.cpp
        src/platform/desktop/libaudio.cpp
        src/platform/desktop/gl_requirements.c
    )
    target_include_directories(pvztv_runner PUBLIC src/platform/desktop)
    target_link_libraries(pvztv_runner PUBLIC glad zlibstatic SDL2::SDL2 SDL2main)
    if(WIN32)
        target_link_libraries(pvztv_runner PUBLIC winmm ws2_32 iphlpapi)
    endif()
endif()
