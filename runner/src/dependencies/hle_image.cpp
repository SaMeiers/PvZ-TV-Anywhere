/* The game's PNG loader, decoded on the host.
 *
 * GetPNGImage(const std::string &) opens the file through the game's own file
 * system (p_fopen, which also reaches inside paks), runs libpng 1.5.9 under
 * the JIT and returns a new ImageLib::Image. Decoding was the largest single
 * cost of loading on a slow phone even with libpng's NEON filters, so the
 * whole function is replaced (see hle.h): the bytes still come through
 * p_fopen/p_fread, and the result is built exactly as the original builds it --
 *
 *   - libpng was told to expand palettes and low bit depths, turn tRNS into
 *     alpha, strip 16-bit samples to their high byte, turn gray into RGB, add
 *     an opaque alpha where there is none and emit BGR: 32-bit pixels laid
 *     out B, G, R, A, i.e. 0xAARRGGBB read as a little-endian word;
 *   - the pixels come from new uint32[w * h + 1];
 *   - the object is operator new(0x420) + ImageLib::Image::Image(), with
 *     width at +4, height at +8, the pixels at +0xC and the file name, a
 *     std::string, at +0x10.
 *
 * The decoder here inflates with the host's zlib and writes straight into the
 * guest's pixel buffer; stb_image's own inflate was slow enough on a
 * Cortex-A53 to eat most of the gain. stb_image stays as the fallback for
 * anything this one does not take. Anything that fails returns 0, as the
 * original does for a missing or unreadable file. */
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/dependencies/hle.h>

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace pvz_tv {
namespace {

/* Guest functions the replacement still relies on, resolved once from the
 * game's main module. */
struct GuestImageApi {
    std::uint32_t p_fopen = 0, p_fread = 0, p_fclose = 0;
    std::uint32_t operator_new = 0, image_ctor = 0;
    std::uint32_t mode_rb = 0; /* "rb" in guest memory */
    bool ok = false;
};

const GuestImageApi &api(GuestCall &c) {
    static const GuestImageApi a = [&c] {
        GuestImageApi r;
        const pvz2_elf_module_t *m = &c.img->modules[0];
        r.p_fopen = pvz2_elf_find_symbol_in(m, "_Z7p_fopenPKcS0_");
        r.p_fread = pvz2_elf_find_symbol_in(m, "_Z7p_freadPviiP5PFILE");
        r.p_fclose = pvz2_elf_find_symbol_in(m, "_Z8p_fcloseP5PFILE");
        r.operator_new = pvz2_elf_find_symbol_in(m, "_Znwj");
        r.image_ctor = pvz2_elf_find_symbol_in(m, "_ZN8ImageLib5ImageC1Ev");
        r.mode_rb = c.dup_cstr("rb");
        r.ok = r.p_fopen && r.p_fread && r.p_fclose && r.operator_new && r.image_ctor && r.mode_rb;
        return r;
    }();
    return a;
}

/* Scratch kept per thread, so a few hundred images do not each map and unmap
 * a few megabytes -- that showed up as page faults in the profile. */
struct Scratch {
    std::vector<std::uint8_t> file, idat, raw, row_zero;
};
thread_local Scratch t_scratch;

/* Reads a whole file through the game's file system. */
bool read_guest_file(GuestCall &c, const GuestImageApi &a, std::uint32_t name, std::vector<std::uint8_t> &out) {
    out.clear();
    const std::uint32_t open_args[2] = {name, a.mode_rb};
    const std::uint32_t file = c.call_guest(a.p_fopen, open_args, 2);
    if (!file) return false;

    constexpr std::uint32_t kChunk = 64 * 1024;
    const std::uint32_t buf = c.rt->heap.alloc(kChunk);
    if (buf) {
        for (;;) {
            const std::uint32_t read_args[4] = {buf, 1, kChunk, file};
            const std::uint32_t n = c.call_guest(a.p_fread, read_args, 4);
            if (n == 0 || n > kChunk) break;
            const auto *p = static_cast<const std::uint8_t *>(c.ptr(buf, n));
            if (!p) break;
            out.insert(out.end(), p, p + n);
        }
        c.rt->heap.free_ptr(buf);
    }
    const std::uint32_t close_args[1] = {file};
    c.call_guest(a.p_fclose, close_args, 1);
    return buf != 0 && !out.empty();
}

/* ---- PNG ---------------------------------------------------------------- */

std::uint32_t be32(const std::uint8_t *p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

struct Png {
    std::uint32_t width = 0, height = 0;
    int depth = 0, color = 0, interlace = 0, channels = 0;
    std::uint8_t palette[256][4] = {}; /* R, G, B, A */
    int palette_size = 0;
    bool has_key = false;               /* tRNS on gray or RGB: one fully transparent colour */
    std::uint16_t key[3] = {};
};

bool unfilter(std::uint8_t type, std::uint8_t *row, const std::uint8_t *prev, std::size_t len, std::size_t bpp) {
    switch (type) {
        case 0:
            return true;
        case 1:
            for (std::size_t i = bpp; i < len; ++i) row[i] = static_cast<std::uint8_t>(row[i] + row[i - bpp]);
            return true;
        case 2:
            for (std::size_t i = 0; i < len; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prev[i]);
            return true;
        case 3:
            for (std::size_t i = 0; i < bpp && i < len; ++i) row[i] = static_cast<std::uint8_t>(row[i] + (prev[i] >> 1));
            for (std::size_t i = bpp; i < len; ++i)
                row[i] = static_cast<std::uint8_t>(row[i] + ((row[i - bpp] + prev[i]) >> 1));
            return true;
        case 4:
            for (std::size_t i = 0; i < bpp && i < len; ++i) row[i] = static_cast<std::uint8_t>(row[i] + prev[i]);
            for (std::size_t i = bpp; i < len; ++i) {
                const int a = row[i - bpp], b = prev[i], cc = prev[i - bpp];
                const int p = a + b - cc;
                const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - cc);
                const int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : cc);
                row[i] = static_cast<std::uint8_t>(row[i] + pred);
            }
            return true;
        default:
            return false;
    }
}

/* One sample of a packed or 8/16-bit row, at its original precision. */
inline std::uint32_t sample(const std::uint8_t *row, std::uint32_t index, int depth) {
    switch (depth) {
        case 16: return (std::uint32_t{row[index * 2]} << 8) | row[index * 2 + 1];
        case 8: return row[index];
        default: {
            const std::uint32_t bit = index * static_cast<std::uint32_t>(depth);
            return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1u);
        }
    }
}

/* To 8 bits the way libpng does: 16-bit keeps the high byte, low gray depths
 * are scaled up to fill 0..255. */
inline std::uint8_t to8(std::uint32_t v, int depth) {
    switch (depth) {
        case 16: return static_cast<std::uint8_t>(v >> 8);
        case 8: return static_cast<std::uint8_t>(v);
        default: return static_cast<std::uint8_t>(v * 255u / ((1u << depth) - 1u));
    }
}

/* Writes `count` pixels of one decoded row as B, G, R, A at `out`, `step`
 * pixels apart (1 normally, the Adam7 pass spacing otherwise). */
void emit_row(const Png &png, const std::uint8_t *row, std::uint32_t count, std::uint8_t *out, std::uint32_t step) {
    const int d = png.depth;
    if (png.color == 6 && d == 8 && step == 1) { /* the common case: RGBA8 */
        for (std::uint32_t x = 0; x < count; ++x) {
            std::uint32_t p;
            std::memcpy(&p, row + x * 4, 4); /* bytes R G B A */
            p = (p & 0xFF00FF00u) | ((p >> 16) & 0xFFu) | ((p & 0xFFu) << 16);
            std::memcpy(out + x * 4, &p, 4);
        }
        return;
    }
    for (std::uint32_t x = 0; x < count; ++x) {
        std::uint8_t r, g, b, a = 255;
        switch (png.color) {
            case 0: { /* gray */
                const std::uint32_t v = sample(row, x, d);
                r = g = b = to8(v, d);
                if (png.has_key && v == png.key[0]) a = 0;
                break;
            }
            case 2: { /* RGB */
                const std::uint32_t vr = sample(row, x * 3, d), vg = sample(row, x * 3 + 1, d), vb = sample(row, x * 3 + 2, d);
                r = to8(vr, d), g = to8(vg, d), b = to8(vb, d);
                if (png.has_key && vr == png.key[0] && vg == png.key[1] && vb == png.key[2]) a = 0;
                break;
            }
            case 3: { /* palette */
                const std::uint32_t i = sample(row, x, d);
                const std::uint8_t *e = png.palette[i & 0xFF];
                r = e[0], g = e[1], b = e[2], a = e[3];
                break;
            }
            case 4: /* gray + alpha */
                r = g = b = to8(sample(row, x * 2, d), d);
                a = to8(sample(row, x * 2 + 1, d), d);
                break;
            default: /* 6: RGBA */
                r = to8(sample(row, x * 4, d), d), g = to8(sample(row, x * 4 + 1, d), d);
                b = to8(sample(row, x * 4 + 2, d), d), a = to8(sample(row, x * 4 + 3, d), d);
                break;
        }
        std::uint8_t *o = out + static_cast<std::size_t>(x) * step * 4;
        o[0] = b, o[1] = g, o[2] = r, o[3] = a;
    }
}

/* Decodes into `out` (width * height B,G,R,A pixels, allocated by
 * `alloc_out` once the size is known). Returns false for anything it does not
 * handle, leaving the fallback to try. */
template <typename Alloc>
bool decode_png(const std::uint8_t *data, std::size_t size, Png &png, Alloc alloc_out) {
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (size < 8 || std::memcmp(data, kSig, 8) != 0) return false;
    Scratch &s = t_scratch;
    s.idat.clear();
    for (int i = 0; i < 256; ++i) png.palette[i][3] = 255;

    bool have_header = false;
    std::size_t at = 8;
    while (at + 12 <= size) {
        const std::uint32_t len = be32(data + at);
        const std::uint8_t *type = data + at + 4, *body = data + at + 8;
        if (len > size - at - 12) return false;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) return false;
            png.width = be32(body), png.height = be32(body + 4);
            png.depth = body[8], png.color = body[9], png.interlace = body[12];
            if (body[10] != 0 || body[11] != 0 || png.interlace > 1) return false;
            have_header = true;
        } else if (!std::memcmp(type, "PLTE", 4)) {
            png.palette_size = static_cast<int>(len / 3 > 256 ? 256 : len / 3);
            for (int i = 0; i < png.palette_size; ++i) {
                png.palette[i][0] = body[i * 3], png.palette[i][1] = body[i * 3 + 1], png.palette[i][2] = body[i * 3 + 2];
            }
        } else if (!std::memcmp(type, "tRNS", 4)) {
            if (png.color == 3) {
                for (std::uint32_t i = 0; i < len && i < 256; ++i) png.palette[i][3] = body[i];
            } else if (png.color == 0 && len >= 2) {
                png.has_key = true, png.key[0] = static_cast<std::uint16_t>((body[0] << 8) | body[1]);
            } else if (png.color == 2 && len >= 6) {
                png.has_key = true;
                for (int i = 0; i < 3; ++i) png.key[i] = static_cast<std::uint16_t>((body[i * 2] << 8) | body[i * 2 + 1]);
            }
        } else if (!std::memcmp(type, "IDAT", 4)) {
            s.idat.insert(s.idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + static_cast<std::size_t>(len);
    }
    if (!have_header || s.idat.empty() || png.width == 0 || png.height == 0) return false;

    switch (png.color) {
        case 0: png.channels = 1; break;
        case 2: png.channels = 3; break;
        case 3: png.channels = 1; break;
        case 4: png.channels = 2; break;
        case 6: png.channels = 4; break;
        default: return false;
    }
    const int d = png.depth;
    const bool depth_ok = (png.color == 3) ? (d == 1 || d == 2 || d == 4 || d == 8)
                        : (png.color == 0) ? (d == 1 || d == 2 || d == 4 || d == 8 || d == 16)
                                           : (d == 8 || d == 16);
    if (!depth_ok || png.width > 16384 || png.height > 16384) return false;

    const std::size_t bits_per_pixel = static_cast<std::size_t>(png.channels) * d;
    const std::size_t bpp = bits_per_pixel >= 8 ? bits_per_pixel / 8 : 1;
    auto row_bytes = [&](std::uint32_t w) { return (w * bits_per_pixel + 7) / 8; };

    /* Adam7, or the whole image as one pass. */
    struct Pass { std::uint32_t x0, y0, dx, dy; };
    static const Pass kAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static const Pass kWhole[1] = {{0, 0, 1, 1}};
    const Pass *passes = png.interlace ? kAdam7 : kWhole;
    const int pass_count = png.interlace ? 7 : 1;

    std::size_t raw_size = 0;
    for (int p = 0; p < pass_count; ++p) {
        const Pass &ps = passes[p];
        const std::uint32_t pw = (png.width - ps.x0 + ps.dx - 1) / ps.dx * (png.width > ps.x0);
        const std::uint32_t ph = (png.height - ps.y0 + ps.dy - 1) / ps.dy * (png.height > ps.y0);
        if (pw && ph) raw_size += static_cast<std::size_t>(ph) * (row_bytes(pw) + 1);
    }

    s.raw.resize(raw_size);
    uLongf got = static_cast<uLongf>(raw_size);
    if (uncompress(s.raw.data(), &got, s.idat.data(), static_cast<uLong>(s.idat.size())) != Z_OK || got != raw_size) {
        return false;
    }

    std::uint8_t *out = alloc_out(png.width, png.height);
    if (!out) return false;

    s.row_zero.assign(row_bytes(png.width), 0);
    std::uint8_t *cursor = s.raw.data();
    for (int p = 0; p < pass_count; ++p) {
        const Pass &ps = passes[p];
        const std::uint32_t pw = (png.width - ps.x0 + ps.dx - 1) / ps.dx * (png.width > ps.x0);
        const std::uint32_t ph = (png.height - ps.y0 + ps.dy - 1) / ps.dy * (png.height > ps.y0);
        if (!pw || !ph) continue;
        const std::size_t rb = row_bytes(pw);
        const std::uint8_t *prev = s.row_zero.data();
        for (std::uint32_t y = 0; y < ph; ++y) {
            std::uint8_t *row = cursor + 1;
            if (!unfilter(cursor[0], row, prev, rb, bpp)) return false;
            const std::size_t oy = static_cast<std::size_t>(ps.y0 + y * ps.dy);
            emit_row(png, row, pw, out + (oy * png.width + ps.x0) * 4, ps.dx);
            prev = row;
            cursor += rb + 1;
        }
    }
    return true;
}

/* A fresh libstdc++ (COW) std::string holding `len` bytes at `src`, with its
 * own rep: header (length, capacity, refcount 0 = one owner), then the text. */
std::uint32_t make_cow_string(GuestCall &c, std::uint32_t src, std::uint32_t len) {
    const std::uint32_t rep = c.rt->heap.alloc(len + 13);
    if (!rep) return 0;
    c.write32(rep + 0, len);
    c.write32(rep + 4, len);
    c.write32(rep + 8, 0);
    if (len) {
        const void *from = c.ptr(src, len);
        void *to = c.ptr(rep + 12, len);
        if (!from || !to) return 0;
        std::memcpy(to, from, len);
    }
    c.write8(rep + 12 + len, 0);
    return rep + 12;
}

/* ImageLib::Image *GetPNGImage(const std::string &theFileName) */
void get_png_image(GuestCall &c) {
    const GuestImageApi &a = api(c);
    const std::uint32_t name_obj = c.arg(0);
    const std::uint32_t name = c.read32(name_obj); /* the std::string's data pointer */
    std::vector<std::uint8_t> &file = t_scratch.file;
    if (!a.ok || !name || !read_guest_file(c, a, name, file)) {
        c.set_result(0);
        return;
    }

    std::uint32_t bits = 0, width = 0, height = 0;
    auto alloc_bits = [&](std::uint32_t w, std::uint32_t h) -> std::uint8_t * {
        const std::uint64_t pixels = std::uint64_t{w} * h;
        if (pixels >= 0x10000000u) return nullptr;
        bits = c.rt->heap.alloc(static_cast<std::uint32_t>(pixels + 1) * 4);
        if (!bits) return nullptr;
        width = w, height = h;
        return static_cast<std::uint8_t *>(c.ptr(bits, static_cast<std::uint32_t>(pixels) * 4));
    };

    Png png;
    if (!decode_png(file.data(), file.size(), png, alloc_bits)) {
        if (bits) c.rt->heap.free_ptr(bits), bits = 0;
        /* Whatever this decoder does not take, stb_image might. */
        int w = 0, h = 0, comp = 0;
        stbi_uc *rgba = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &comp, 4);
        std::uint8_t *out = rgba ? alloc_bits(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)) : nullptr;
        if (!out) {
            c.log("[hle] PNG decode failed for %s: %s", c.cstr(name, 256).c_str(), rgba ? "out of memory" : stbi_failure_reason());
            if (rgba) stbi_image_free(rgba);
            if (bits) c.rt->heap.free_ptr(bits);
            c.set_result(0);
            return;
        }
        Png as_rgba;
        as_rgba.color = 6, as_rgba.depth = 8;
        emit_row(as_rgba, rgba, width * height, out, 1);
        stbi_image_free(rgba);
    }

    const std::uint32_t new_args[1] = {0x420};
    const std::uint32_t image = c.call_guest(a.operator_new, new_args, 1);
    if (!image) {
        c.rt->heap.free_ptr(bits);
        c.set_result(0);
        return;
    }
    const std::uint32_t ctor_args[1] = {image};
    c.call_guest(a.image_ctor, ctor_args, 1);

    const std::uint32_t name_len = c.read32(name - 12);
    if (name_len) {
        if (const std::uint32_t copy = make_cow_string(c, name, name_len)) c.write32(image + 0x10, copy);
    }
    c.write32(image + 0x4, width);
    c.write32(image + 0x8, height);
    c.write32(image + 0xC, bits);
    c.set_result(image);
}

}  // namespace

void register_hle_image(GuestOverrides &o) {
    o.add("_Z11GetPNGImageRKSs", get_png_image);
}

}  // namespace pvz_tv
