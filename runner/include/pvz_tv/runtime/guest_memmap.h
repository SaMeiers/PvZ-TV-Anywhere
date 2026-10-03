#ifndef PVZ2NATIVE_RUNTIME_GUEST_MEMMAP_H
#define PVZ2NATIVE_RUNTIME_GUEST_MEMMAP_H

#include <cstdint>

#include <pvz_tv/elf32/elf32_loader.h>

/* Everything the harness fabricates in guest memory below the loaded image.
 *
 * The .so is placed at PVZ2_SO_BASE (0x00100000) and the import trampolines at
 * PVZ2_TRAMPOLINE_BASE (0x00001000). The ~1MB gap between them is free real
 * estate, and the harness fills it with structures the guest must be able to
 * read and call: the fake JNIEnv and JavaVM vtables, opaque JNI handles, scratch
 * for fabricated jstrings, and the placeholder objects passed to
 * Native_GameAppInitialize.
 *
 * Those addresses used to be declared in six different files -- dex/dex.h,
 * runtime/guest_runtime.h, dependencies/libc_unistd.cpp, engine/boot.cpp,
 * engine/lifecycle.cpp and the loader's own private #defines -- so no single
 * place said what the layout was, and nothing checked that two regions did not
 * overlap. Two of them were already only 0x800 apart with no note saying so.
 * Collecting them here costs nothing at runtime and makes an overlap a build
 * error instead of memory corruption with no plausible symptom.
 *
 * Everything here is an ADDRESS in the emulated space. The opaque token
 * namespaces (guest fd tokens, FILE* cookies) are NOT addresses even though they
 * are also small integers -- see kFdTokenBase in guest_runtime.h. They are never
 * dereferenced, so they may numerically overlap this map without meaning
 * anything, and they deliberately do.
 */

namespace pvz_tv {
namespace runtime {
namespace memmap {

/* --- the loader's own two anchors ---------------------------------------- */

constexpr std::uint32_t kTrampolineBase = PVZ2_TRAMPOLINE_BASE;
constexpr std::uint32_t kTrampolineEnd = PVZ2_TRAMPOLINE_BASE + PVZ2_TRAMPOLINE_MAX * PVZ2_TRAMPOLINE_STRIDE;
constexpr std::uint32_t kImageBase = PVZ2_SO_BASE;

/* --- opaque JNI handles (jclass / jmethodID / jfieldID / jobject) --------- *
 *
 * Bump-allocated. The guest never dereferences these, only passes them back into
 * other JNI calls, so a unique non-null address is all that is needed. */
constexpr std::uint32_t kFakeHandleBase = 0x00005000;
constexpr std::uint32_t kFakeHandleEnd = 0x00005800;

/* --- the fake JNIEnv ------------------------------------------------------ *
 *
 * JNIEnv* is really a `JNINativeInterface**`: a pointer to a pointer to a table
 * of function pointers. Both levels live here, with every slot wired to its own
 * SVC trampoline, so a JNI call traps back to the host knowing which slot it was.
 * kJniTableSlots bounds both the table and the stub block. */
constexpr std::uint32_t kJniTableAddr = 0x00006000;
constexpr std::uint32_t kJniStubsAddr = 0x00006800;
constexpr std::uint32_t kJniEnvPtrAddr = 0x00007000;
/* Ceiling the two blocks above are sized against. The real table is ~233 entries
 * (jni_slot_count()); this is the largest it may grow to before it would run
 * into the next region, and dex::install() is what fills it. */
constexpr std::uint32_t kJniTableSlots = 512;

/* --- the fake JavaVM ------------------------------------------------------ *
 *
 * What GetJavaVM hands back. Eight slots: reserved0-2, DestroyJavaVM,
 * AttachCurrentThread, DetachCurrentThread, GetEnv, AttachCurrentThreadAsDaemon. */
constexpr std::uint32_t kJavaVmTableAddr = 0x00008000;
constexpr std::uint32_t kJavaVmStubsAddr = 0x00008100;
constexpr std::uint32_t kJavaVmPtrAddr = 0x00008200;
constexpr std::uint32_t kJavaVmSlots = 8;

/* --- scratch -------------------------------------------------------------- *
 *
 * Fabricated argument payloads that outlive the call taking them: the jstring
 * bytes for EAIO's documents path, and anything else make_fake_jstring needs a
 * home for during boot. */
constexpr std::uint32_t kScratchAddr = 0x00009000;
constexpr std::uint32_t kScratchSize = 0x00001000;

/* --- Native_GameAppInitialize's placeholder objects ----------------------- *
 *
 * One block per declared argument plus one for `thiz`. 9.6.1 declares seven
 * objects, 1.6 and 4.5.2 eight, so kFakeObjMax is the ceiling the layout is
 * checked against rather than any one build's count. */
constexpr std::uint32_t kFakeObjBase = 0x0000A000;
constexpr std::uint32_t kFakeObjStride = 0x40;
constexpr std::uint32_t kFakeObjMax = 16;
constexpr std::uint32_t kFakeObjEnd = kFakeObjBase + kFakeObjMax * kFakeObjStride;

/* --- the layout is consistent, checked at build time ---------------------- */

static_assert(kTrampolineEnd <= kFakeHandleBase,
              "the import trampoline table now runs into the JNI handle pool -- raising "
              "PVZ2_TRAMPOLINE_MAX means moving kFakeHandleBase up too");
static_assert(kFakeHandleBase < kFakeHandleEnd, "empty JNI handle pool");
static_assert(kFakeHandleEnd <= kJniTableAddr, "JNI handle pool overlaps the JNIEnv vtable");
static_assert(kJniTableAddr + kJniTableSlots * 4 <= kJniStubsAddr,
              "the JNIEnv function table overlaps its SVC stub block");
static_assert(kJniStubsAddr + kJniTableSlots * 4 <= kJniEnvPtrAddr,
              "the JNIEnv SVC stubs overlap the JNIEnv* word");
static_assert(kJniEnvPtrAddr + 4 <= kJavaVmTableAddr, "the JNIEnv* word overlaps the JavaVM");
static_assert(kJavaVmTableAddr + kJavaVmSlots * 4 <= kJavaVmStubsAddr,
              "the JavaVM function table overlaps its SVC stub block");
static_assert(kJavaVmStubsAddr + kJavaVmSlots * 4 <= kJavaVmPtrAddr,
              "the JavaVM SVC stubs overlap the JavaVM* word");
static_assert(kJavaVmPtrAddr + 4 <= kScratchAddr, "the JavaVM* word overlaps the scratch area");
static_assert(kScratchAddr + kScratchSize <= kFakeObjBase,
              "the scratch area overlaps the GameAppInitialize placeholder objects");
static_assert(kFakeObjEnd <= kImageBase,
              "the placeholder objects run into the loaded .so image");

}  // namespace memmap
}  // namespace runtime
}  // namespace pvz_tv

#endif
