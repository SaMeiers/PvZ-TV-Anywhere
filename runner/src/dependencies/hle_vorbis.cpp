/* The vorbisfile API, decoded on the host.
 *
 * The game links Tremor (libvorbisidec) statically and audiere decodes every sound
 * effect through it while the resources load. Running that decoder under the
 * JIT was about a third of loading on a slow phone. The game's ov_* entry
 * points are redirected here instead (see install_guest_overrides), and the
 * stream is decoded by stb_vorbis at native speed.
 *
 * Only audiere's OGGInputStream uses this API, through ov_open_callbacks,
 * ov_info, ov_comment, ov_read, ov_pcm_total, ov_pcm_tell, ov_seekable,
 * ov_pcm_seek(_page) and ov_clear, and it never reads the OggVorbis_File
 * itself. So the guest's struct is only a key: the decoder state lives here,
 * and ov_info / ov_comment hand back small structs allocated for it in guest
 * memory. Entry points that open a stream any other way refuse, so a stream
 * can never be half guest-decoded and half host-decoded. */
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/dependencies/hle.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define STB_VORBIS_NO_STDIO 1
#define STB_VORBIS_NO_PUSHDATA_API 1
#include "stb_vorbis.c"

namespace pvz_tv {
namespace {

constexpr std::int32_t OV_FALSE = -1;
constexpr std::int32_t OV_EINVAL = -131;
constexpr std::int32_t OV_ENOTVORBIS = -132;

/* Guest layouts (libvorbis 1.x, 32-bit). */
constexpr std::uint32_t kVorbisInfoSize = 32;    /* version, channels, rate, 4 bitrates, codec_setup */
constexpr std::uint32_t kVorbisCommentSize = 16; /* user_comments, comment_lengths, comments, vendor */

struct Stream {
    std::vector<std::uint8_t> data; /* the whole file: stb_vorbis decodes from memory */
    stb_vorbis *vorbis = nullptr;
    int channels = 0;
    std::uint32_t rate = 0;
    std::uint64_t position = 0; /* in samples per channel */
    std::uint32_t guest_block = 0; /* vorbis_info, vorbis_comment, empty vendor string */
    std::uint32_t datasource = 0;
    std::uint32_t close_func = 0;
};

std::mutex g_lock;
std::unordered_map<std::uint32_t, std::unique_ptr<Stream>> g_streams; /* key: the guest's OggVorbis_File* */

Stream *find(std::uint32_t vf) {
    std::lock_guard<std::mutex> lk(g_lock);
    auto it = g_streams.find(vf);
    return it == g_streams.end() ? nullptr : it->second.get();
}

std::uint64_t arg64(const GuestCall &c, int first) { /* an even-aligned register/stack pair */
    return std::uint64_t{c.arg(first)} | (std::uint64_t{c.arg(first + 1)} << 32);
}

/* int ov_open_callbacks(void *datasource, OggVorbis_File *vf, const char *initial,
 *                       long ibytes, ov_callbacks callbacks)
 * ov_callbacks is passed by value: read_func, seek_func, close_func, tell_func
 * arrive as stack words 4..7. */
void ov_open_callbacks(GuestCall &c) {
    const std::uint32_t datasource = c.arg(0), vf = c.arg(1), initial = c.arg(2), ibytes = c.arg(3);
    const std::uint32_t read_func = c.arg(4), close_func = c.arg(6);

    auto s = std::make_unique<Stream>();
    s->datasource = datasource;
    s->close_func = close_func;
    if (initial && ibytes) {
        const auto *p = static_cast<const std::uint8_t *>(c.ptr(initial, ibytes));
        if (p) s->data.assign(p, p + ibytes);
    }

    /* Pull the whole stream through the game's own read callback. */
    constexpr std::uint32_t kChunk = 64 * 1024;
    const std::uint32_t buf = c.rt->heap.alloc(kChunk);
    if (!buf) {
        c.set_result(static_cast<std::uint32_t>(OV_EINVAL));
        return;
    }
    for (;;) {
        const std::uint32_t args[4] = {buf, 1, kChunk, datasource};
        const std::uint32_t n = c.call_guest(read_func, args, 4);
        if (n == 0 || n > kChunk) break;
        const auto *p = static_cast<const std::uint8_t *>(c.ptr(buf, n));
        if (!p) break;
        s->data.insert(s->data.end(), p, p + n);
    }
    c.rt->heap.free_ptr(buf);

    int error = 0;
    s->vorbis = stb_vorbis_open_memory(s->data.data(), static_cast<int>(s->data.size()), &error, nullptr);
    if (!s->vorbis) {
        c.set_result(static_cast<std::uint32_t>(OV_ENOTVORBIS));
        return;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(s->vorbis);
    s->channels = info.channels;
    s->rate = info.sample_rate;

    s->guest_block = c.rt->heap.alloc(kVorbisInfoSize + kVorbisCommentSize + 8);
    if (!s->guest_block) {
        stb_vorbis_close(s->vorbis);
        c.set_result(static_cast<std::uint32_t>(OV_EINVAL));
        return;
    }
    if (void *p = c.ptr(s->guest_block, kVorbisInfoSize + kVorbisCommentSize + 8)) {
        std::memset(p, 0, kVorbisInfoSize + kVorbisCommentSize + 8);
    }
    const std::uint32_t vi = s->guest_block, vc = vi + kVorbisInfoSize, vendor = vc + kVorbisCommentSize;
    c.write32(vi + 4, static_cast<std::uint32_t>(s->channels));
    c.write32(vi + 8, s->rate);
    c.write32(vc + 12, vendor); /* comments = 0, vendor = "" */

    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_streams[vf] = std::move(s);
    }
    c.set_result(0);
}

/* vorbis_info *ov_info(OggVorbis_File *vf, int link) */
void ov_info(GuestCall &c) {
    Stream *s = find(c.arg(0));
    c.set_result(s ? s->guest_block : 0);
}

/* vorbis_comment *ov_comment(OggVorbis_File *vf, int link) */
void ov_comment(GuestCall &c) {
    Stream *s = find(c.arg(0));
    c.set_result(s ? s->guest_block + kVorbisInfoSize : 0);
}

/* long ov_read(OggVorbis_File *vf, char *buffer, int length, int *bitstream)
 *
 * Tremor's signature, which is what the game links: no endianness, word size
 * or signedness arguments -- the output is always 16-bit signed little-endian.
 * (Reading libvorbisfile's seven arguments here took the bitstream pointer for
 * bigendianp, refused every call, and audiere retried forever.) */
void ov_read(GuestCall &c) {
    Stream *s = find(c.arg(0));
    const std::uint32_t buffer = c.arg(1), length = c.arg(2), bitstream = c.arg(3);
    if (!s || s->channels <= 0) {
        c.set_result(static_cast<std::uint32_t>(OV_EINVAL));
        return;
    }
    const int frame_bytes = 2 * s->channels;
    const int frames = static_cast<int>(length) / frame_bytes;
    auto *out = static_cast<short *>(c.ptr(buffer, static_cast<std::uint32_t>(frames * frame_bytes)));
    if (!out || frames <= 0) {
        c.set_result(0);
        return;
    }
    const int got = stb_vorbis_get_samples_short_interleaved(s->vorbis, s->channels, out, frames * s->channels);
    s->position += static_cast<std::uint64_t>(got);
    if (bitstream) c.write32(bitstream, 0);
    c.set_result(static_cast<std::uint32_t>(got * frame_bytes));
}

/* ogg_int64_t ov_pcm_total(OggVorbis_File *vf, int i) */
void ov_pcm_total(GuestCall &c) {
    Stream *s = find(c.arg(0));
    if (!s) {
        c.set_result64(static_cast<std::uint64_t>(static_cast<std::int64_t>(OV_EINVAL)));
        return;
    }
    c.set_result64(stb_vorbis_stream_length_in_samples(s->vorbis));
}

/* ogg_int64_t ov_pcm_tell(OggVorbis_File *vf) */
void ov_pcm_tell(GuestCall &c) {
    Stream *s = find(c.arg(0));
    c.set_result64(s ? s->position : static_cast<std::uint64_t>(static_cast<std::int64_t>(OV_EINVAL)));
}

/* long ov_seekable(OggVorbis_File *vf) */
void ov_seekable(GuestCall &c) {
    c.set_result(find(c.arg(0)) ? 1 : 0);
}

/* int ov_pcm_seek(OggVorbis_File *vf, ogg_int64_t pos), and ov_pcm_seek_page.
 * The 64-bit position takes the r2:r3 pair. */
void ov_pcm_seek(GuestCall &c) {
    Stream *s = find(c.arg(0));
    const std::uint64_t pos = arg64(c, 2);
    if (!s || pos > 0xFFFFFFFFu || !stb_vorbis_seek(s->vorbis, static_cast<unsigned>(pos))) {
        c.set_result(static_cast<std::uint32_t>(OV_EINVAL));
        return;
    }
    s->position = pos;
    c.set_result(0);
}

/* int ov_clear(OggVorbis_File *vf) */
void ov_clear(GuestCall &c) {
    const std::uint32_t vf = c.arg(0);
    std::unique_ptr<Stream> s;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_streams.find(vf);
        if (it == g_streams.end()) {
            c.set_result(0);
            return;
        }
        s = std::move(it->second);
        g_streams.erase(it);
    }
    stb_vorbis_close(s->vorbis);
    if (s->guest_block) c.rt->heap.free_ptr(s->guest_block);
    /* libvorbisfile closes the data source on clear when a close callback
     * was given; audiere relies on that. */
    if (s->close_func) {
        const std::uint32_t args[1] = {s->datasource};
        c.call_guest(s->close_func, args, 1);
    }
    c.set_result(0);
}

/* Ways into the library that would set up a stream we do not track. */
void ov_refuse(GuestCall &c) {
    c.set_result(static_cast<std::uint32_t>(OV_FALSE));
}

}  // namespace

void register_hle_vorbis(GuestOverrides &o) {
    o.add("ov_open_callbacks", ov_open_callbacks);
    o.add("ov_info", ov_info);
    o.add("ov_comment", ov_comment);
    o.add("ov_read", ov_read);
    o.add("ov_pcm_total", ov_pcm_total);
    o.add("ov_pcm_tell", ov_pcm_tell);
    o.add("ov_seekable", ov_seekable);
    o.add("ov_pcm_seek", ov_pcm_seek);
    o.add("ov_pcm_seek_page", ov_pcm_seek);
    o.add("ov_clear", ov_clear);
    for (const char *name : {"ov_open", "ov_fopen", "ov_test", "ov_test_callbacks", "ov_test_open"}) {
        o.add(name, ov_refuse);
    }
}

}  // namespace pvz_tv
