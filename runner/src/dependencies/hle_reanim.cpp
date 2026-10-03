/* Reanimation track lookups, answered on the host.
 *
 * Reanimation::FindTrackIndex(const char *) walks the definition's tracks
 * calling strcasecmp on each name, and strcasecmp is an import: every track
 * compared is a round trip out of the JIT and back. Plants and zombies look
 * their tracks up by name every frame, so with a full lawn this was a large
 * part of each frame on a slow phone. The replacements below do the same walk
 * in one call. GetFramesForLayer(const char *, int &, int &) is the main
 * caller and is cheap to take over along with it.
 *
 * Layouts, from the original code:
 *   Reanimation           +0x1C ReanimatorDefinition *
 *   ReanimatorDefinition  +0x00 ReanimatorTrack *tracks, +0x04 int count
 *   ReanimatorTrack       0x14 bytes: +0x04 const char *name,
 *                         +0x08 ReanimatorTransform *, +0x0C int count
 *   ReanimatorTransform   0x30 bytes: +0x18 float frame */
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/dependencies/hle.h>

#include <cstdint>
#include <cstring>

namespace pvz_tv {
namespace {

constexpr std::uint32_t kDefinition = 0x1C;
constexpr std::uint32_t kTrackSize = 0x14;
constexpr std::uint32_t kTrackName = 0x04;
constexpr std::uint32_t kTrackTransforms = 0x08;
constexpr std::uint32_t kTrackTransformCount = 0x0C;
constexpr std::uint32_t kTransformSize = 0x30;
constexpr std::uint32_t kTransformFrame = 0x18;

unsigned char ascii_lower(unsigned char ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }

/* strcasecmp(a, b) == 0 for two guest strings, as bionic's C locale has it. */
bool same_name(const GuestCall &c, std::uint32_t a, std::uint32_t b) {
    const std::uint32_t size = c.img->mem_size;
    const std::uint8_t *mem = c.img->mem;
    for (;; ++a, ++b) {
        if (a >= size || b >= size) return false;
        const unsigned char x = ascii_lower(mem[a]);
        if (x != ascii_lower(mem[b])) return false;
        if (x == 0) return true;
    }
}

std::int32_t find_track_index(const GuestCall &c, std::uint32_t reanim, std::uint32_t name) {
    const std::uint32_t def = c.read32(reanim + kDefinition);
    const std::uint32_t tracks = c.read32(def);
    const std::int32_t count = static_cast<std::int32_t>(c.read32(def + 4));
    for (std::int32_t i = 0; i < count; ++i) {
        if (same_name(c, c.read32(tracks + i * kTrackSize + kTrackName), name)) return i;
    }
    return -1;
}

float transform_frame(const GuestCall &c, std::uint32_t transforms, std::int32_t i) {
    const std::uint32_t bits = c.read32(transforms + i * kTransformSize + kTransformFrame);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

/* int Reanimation::FindTrackIndex(const char *theTrackName) */
void reanim_find_track_index(GuestCall &c) {
    c.set_result(static_cast<std::uint32_t>(find_track_index(c, c.arg(0), c.arg(1))));
}

/* void Reanimation::GetFramesForLayer(const char *theTrackName,
 *                                     int &theFrameStart, int &theFrameCount)
 *
 * The first frame of the track that is not hidden (frame >= 0) starts the
 * layer, the last one ends it. A NaN frame counts as hidden, as the
 * original's compares have it. */
void reanim_get_frames_for_layer(GuestCall &c) {
    const std::uint32_t reanim = c.arg(0);
    const std::uint32_t start_at = c.arg(2);
    const std::uint32_t count_at = c.arg(3);
    const std::uint32_t def = c.read32(reanim + kDefinition);
    if (c.read32(def + 4) == 0) {
        c.write32(start_at, 0);
        c.write32(count_at, 0);
        return;
    }
    c.write32(start_at, 0);
    c.write32(count_at, 1);
    const std::int32_t index = find_track_index(c, reanim, c.arg(1));
    if (index < 0) return;
    const std::uint32_t track = c.read32(def) + index * kTrackSize;
    const std::int32_t n = static_cast<std::int32_t>(c.read32(track + kTrackTransformCount));
    const std::uint32_t transforms = c.read32(track + kTrackTransforms);
    std::int32_t first = 0;
    while (first < n && !(transform_frame(c, transforms, first) >= 0.0f)) ++first;
    if (first >= n) return;
    c.write32(start_at, static_cast<std::uint32_t>(first));
    for (std::int32_t i = first; i < n; ++i) {
        if (transform_frame(c, transforms, i) >= 0.0f) c.write32(count_at, static_cast<std::uint32_t>(i - first + 1));
    }
}

}  // namespace

void register_hle_reanim(GuestOverrides &o) {
    o.add("_ZN11Reanimation14FindTrackIndexEPKc", reanim_find_track_index);
    o.add("_ZN11Reanimation17GetFramesForLayerEPKcRiS2_", reanim_get_frames_for_layer);
}

}  // namespace pvz_tv
