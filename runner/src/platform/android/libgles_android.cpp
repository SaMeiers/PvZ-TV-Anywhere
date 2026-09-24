/* libGLESv2.so / libGLESv1_CM.so -- the OpenGL ES entry points.
 *
 * PvZ2 uses BOTH: fixed-function GLES1 calls (glMatrixMode, glLoadMatrixf,
 * glShadeModel, glTexCoordPointer) alongside the GLES2 shader pipeline, which
 * is why the host context is a desktop GL compatibility profile rather than a
 * pure ES one.
 *
 * Each handler unpacks the guest registers and calls the host GL entry point
 * directly. There used to be a gfx/gles_compat layer in between: 114 functions
 * of which 112 were `gl_bind_texture(t,x) { glBindTexture(t,x); }` -- a rename
 * and nothing more -- so following a GL call meant hopping through a file that
 * never did any work, and 41 of those wrappers were never called at all. The
 * two that looked like translation only cast float to double, which the
 * compiler already does at the call site. Desktop GL IS the translation target
 * here; the only genuine ES-vs-desktop work in this port is the GLSL dialect
 * rewrite (see gl_glShaderSource) and the viewport fix (see gl_glViewport).
 */

#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/config.h>
#include <pvz_tv/surface.h>

#include <GLES/gl.h>
#include <GLES/glext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace pvz_tv {
namespace {

/* Which framebuffer the guest currently has bound. The engine renders its scene
 * into an offscreen FBO at its fixed resolution, then binds framebuffer 0 -- the
 * window -- to composite. Tracking this lets gl_glViewport force the composite
 * to fill the whole window: without it the composite keeps the engine's
 * render-size viewport and the picture sits in one corner of a larger or
 * maximised window. Only the composite draws to framebuffer 0. */
std::atomic<GLuint> g_bound_fbo{0};

/* Drains the host GL error queue after an operation the guest never checks
 * itself. Budgeted, so a per-frame error cannot flood the log. */
void report_gl_error(GuestCall &c, const char *what) {
    static std::atomic<std::uint32_t> budget{24};
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return;
    if (budget.load(std::memory_order_relaxed) == 0) return;
    budget.fetch_sub(1, std::memory_order_relaxed);
    c.trace("[gl] %s -> GL error 0x%04x", what, (unsigned)err);
}

/* Same, but says whether there WAS an error, so a draw can follow up with the
 * full state dump. Kept separate because it must not consume the error queue
 * silently once the report budget runs out. */
bool gl_peek_error_for_draw(GuestCall &c, const char *what) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return false;
    static std::atomic<std::uint32_t> budget{24};
    if (budget.load(std::memory_order_relaxed) > 0) {
        budget.fetch_sub(1, std::memory_order_relaxed);
        c.trace("[gl] %s -> GL error 0x%04x", what, (unsigned)err);
    }
    return true;
}

void gl_glActiveTexture(GuestCall &c) {
    glActiveTexture(c.arg(0));
}

/* [gl] debug_clear=1 forces a loud clear colour. The engine clears to opaque
 * black, which is indistinguishable from "the frame never reached the window"
 * -- and those two have completely different causes. If the window turns this
 * colour, presentation works and the problem is that nothing is drawn INTO the
 * frame; if it stays black, the frame is not reaching the screen at all. */
bool debug_clear_enabled() { return pvz2_config()->gl_debug_clear != 0; }



void gl_glDrawArrays(GuestCall &c) {
    glDrawArrays(c.arg(0), c.arg(1), c.arg(2));
}

void gl_glDrawElements(GuestCall &c) {
    glDrawElements(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
    report_gl_error(c, "glDrawElements");
}

void gl_glViewport(GuestCall &c) {
    std::uint32_t x = c.arg(0), y = c.arg(1), w = c.arg(2), h = c.arg(3);

    /* A zero-width viewport makes every subsequent draw a no-op the driver
     * accepts without error, which is how a whole pass can silently disappear.
     *
     * Every frame does exactly this: the scene is rendered into an FBO with a
     * correct viewport (0,0,1365,768), then the engine binds framebuffer 0 --
     * the window -- with viewport (0,0,0,1280) and composites. Width 0, so
     * nothing ever reaches the screen. The values come from sub_ABC650, which
     * reads x/y/w/h out of a render-state cache at stride 124; w lands on what
     * should be y and h on what should be w, i.e. the cached entry is one slot
     * out. Where that shift comes from is still unknown.
     *
     * Overriding with the drawable size was tried once BEFORE the boot was
     * fixed and dismissed as "changed nothing visible" -- correctly, back then:
     * no resource group had loaded, so there was nothing to composite. Now the
     * menu's textures do load, which makes the substitution worth having.
     * [gl] no_viewport_fix=1 restores the uncorrected behaviour. */
    const bool disabled = pvz2_config()->gl_no_viewport_fix != 0;
    const std::uint32_t dw = pvz2_surface_width();
    const std::uint32_t dh = pvz2_surface_height();
    const bool have_drawable = !disabled && dw != 0 && dh != 0;

    /* Any viewport on framebuffer 0 is a composite to the window, and it must
     * fill the window whatever size it is now -- otherwise a maximised or
     * dragged window shows the fixed-resolution picture in one corner. The
     * engine sizes this viewport from its own render resolution (or leaves it
     * zero-width, the original bug below), neither of which tracks the real
     * window, so override it with the current drawable size. Scene passes into
     * an offscreen FBO keep their own viewport untouched. */
    if (have_drawable && g_bound_fbo.load(std::memory_order_relaxed) == 0) {
        if (x != 0 || y != 0 || w != dw || h != dh) {
            static std::atomic<std::uint32_t> budget{3};
            if (budget.load(std::memory_order_relaxed) > 0) {
                budget.fetch_sub(1, std::memory_order_relaxed);
                c.trace("[gl] window viewport(%u,%u,%u,%u) -> filling drawable %ux%u", x, y, w, h, dw, dh);
            }
        }
        x = 0;
        y = 0;
        w = dw;
        h = dh;
    } else if (w == 0 || h == 0) {
        /* A zero-width viewport on an FBO would make the pass a silent no-op.
         * Unlikely off framebuffer 0, but substitute defensively. */
        static std::atomic<std::uint32_t> budget{3};
        if (have_drawable && budget.load(std::memory_order_relaxed) > 0) {
            budget.fetch_sub(1, std::memory_order_relaxed);
            c.trace("[gl] degenerate viewport(%u,%u,%u,%u) lr=0x%08x -- substituting the drawable size",
                  x, y, w, h, c.lr());
        }
        if (have_drawable) { x = 0; y = 0; w = dw; h = dh; }
    }
    glViewport(x, y, w, h);
}

void gl_glAlphaFunc(GuestCall &c) {
    glAlphaFunc(c.arg(0), c.argf(1));
}

void gl_glAttachShader(GuestCall &c) {
    glAttachShader(c.arg(0), c.arg(1));
}

void gl_glBindAttribLocation(GuestCall &c) {
    glBindAttribLocation(c.arg(0), c.arg(1), (const GLchar *)c.ptr(c.arg(2)));
}

/* Reports what an offscreen framebuffer actually contains, at the moment the
 * engine stops rendering into it.
 *
 * The screen is black even though the GL error queue is clean, the viewport is
 * correct and SDL_GL_SwapWindow runs every frame -- and forcing a loud clear
 * colour DOES show through, so presentation works and the composite is
 * covering the window with something black. The question left is whether the
 * scene FBO it samples has any content. glReadPixels on the FBO answers that
 * directly instead of by inference: it also names the colour attachment, so a
 * composite sampling the wrong texture is visible too. */
void gl_glBindFramebuffer(GuestCall &c) {
    g_bound_fbo.store((GLuint)c.arg(1), std::memory_order_relaxed);
    glBindFramebuffer(c.arg(0), c.arg(1));
}

void gl_glBindTexture(GuestCall &c) {
    glBindTexture(c.arg(0), c.arg(1));
}

void gl_glBlendFunc(GuestCall &c) {
    glBlendFunc(c.arg(0), c.arg(1));
}

void gl_glCheckFramebufferStatus(GuestCall &c) {
    c.regs[0] = glCheckFramebufferStatus(c.arg(0));
}

void gl_glClear(GuestCall &c) {
    glClear(c.arg(0));
}

void gl_glClearColor(GuestCall &c) {
    if (debug_clear_enabled()) {
        glClearColor(0.2f, 0.0f, 0.6f, 1.0f); /* purple: nothing in the game is this colour */
        return;
    }
    glClearColor(c.argf(0), c.argf(1), c.argf(2), c.argf(3));
}

void gl_glClearDepthf(GuestCall &c) {
    glClearDepthf(c.argf(0));
}

void gl_glClientActiveTexture(GuestCall &c) {
    glClientActiveTexture(c.arg(0));
}

void gl_glColorMask(GuestCall &c) {
    glColorMask((GLboolean)c.arg(0), (GLboolean)c.arg(1), (GLboolean)c.arg(2), (GLboolean)c.arg(3));
}

void gl_glColorPointer(GuestCall &c) {
    glColorPointer(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
}

void gl_glCompileShader(GuestCall &c) {
    GLuint shader = c.arg(0);
    glCompileShader(shader);
    /* The engine's shaders are GLSL ES 1.00 and the host context is desktop GL,
     * which rejects them unless the driver accepts ES syntax. The engine never
     * reads the info log, so a rejected shader would silently produce a black
     * screen with draw calls still being issued -- exactly the symptom. */
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLchar log[1024] = {0};
        GLsizei len = 0;
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log) - 1, &len, log);
        std::lock_guard<std::mutex> lg(c.rt->log_lock);
        std::printf("pvz2: [gl] SHADER %u FAILED TO COMPILE: %s\n", shader, log);
    }
}

void gl_glLinkProgram(GuestCall &c) {
    GLuint program = c.arg(0);
    glLinkProgram(program);
    /* Same reasoning as gl_glCompileShader: the engine never checks, so a failed
     * link is otherwise invisible and reads as a black screen. */
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        GLchar log[1024] = {0};
        GLsizei len = 0;
        glGetProgramInfoLog(program, (GLsizei)sizeof(log) - 1, &len, log);
        std::lock_guard<std::mutex> lg(c.rt->log_lock);
        std::printf("pvz2: [gl] PROGRAM %u FAILED TO LINK: %s\n", program, log);
    }
}

/* --- ETC1 software decode -------------------------------------------------
 *
 * PvZ2 uploads every non-RGBA texture as ETC1 (GL_ETC1_RGB8_OES, 0x8D64) -- the
 * universal Android compressed format. Desktop GL drivers frequently do NOT
 * expose ETC1: they expose ETC2 (its superset) or nothing, so the pass-through
 * upload fails with GL_INVALID_ENUM and the texture is black. Confirmed on an
 * AMD HD 5450 (lists ETC2 0x9274 but not ETC1) and reported the same on Intel
 * Bay Trail. The dev machine happened to accept ETC1, which is why it looked
 * fine there and nowhere else.
 *
 * Decoding on the CPU and uploading plain RGB8 removes the dependency on the
 * driver supporting ANY compressed format -- it works everywhere, including the
 * weak GPUs this is for. ETC1 is a subset of ETC2, so re-tagging as ETC2 would
 * fix the AMD but not a driver that lacks ETC2 too; software decode is the only
 * universally correct fix. The separate-alpha texture PvZ2 pairs with these is
 * already uploaded uncompressed (GL_ALPHA), so only the RGB block needs this.
 *
 * Reference: the ETC1 block format (Khronos GL_OES_compressed_ETC1_RGB8_texture),
 * matching Android's etc1.cpp. 4x4 pixels per 8-byte block, two 2x4/4x2 subblocks
 * each with a base colour and a 3-bit modifier-table codeword; per-pixel 2-bit
 * index picks the modifier added to all three channels. */
const int kEtc1Modifier[8][4] = {
    {2, 8, -2, -8},      {5, 17, -5, -17},    {9, 29, -9, -29},    {13, 42, -13, -42},
    {18, 60, -18, -60},  {24, 80, -24, -80},  {33, 106, -33, -106}, {47, 183, -47, -183},
};

inline int etc1_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
inline int etc1_ext4(int v) { return (v << 4) | v; }               /* 4 bits -> 8 */
inline int etc1_ext5(int v) { v &= 0x1f; return (v << 3) | (v >> 2); } /* 5 bits -> 8 */
inline int etc1_diff3(int v) { v &= 7; return v < 4 ? v : v - 8; }  /* 3-bit signed delta */

void etc1_decode_block(const std::uint8_t *b, std::uint8_t *out, int width, int height, int ox0,
                       int oy0) {
    const std::uint32_t hi = (std::uint32_t)b[0] << 24 | (std::uint32_t)b[1] << 16 |
                             (std::uint32_t)b[2] << 8 | b[3];
    const std::uint32_t lo = (std::uint32_t)b[4] << 24 | (std::uint32_t)b[5] << 16 |
                             (std::uint32_t)b[6] << 8 | b[7];
    const bool diff = (hi & 2) != 0;
    const bool flip = (hi & 1) != 0;

    int r1, g1, b1, r2, g2, b2;
    if (diff) {
        const int r = (hi >> 27) & 0x1f, g = (hi >> 19) & 0x1f, bl = (hi >> 11) & 0x1f;
        r1 = etc1_ext5(r);            g1 = etc1_ext5(g);            b1 = etc1_ext5(bl);
        r2 = etc1_ext5(r + etc1_diff3(hi >> 24));
        g2 = etc1_ext5(g + etc1_diff3(hi >> 16));
        b2 = etc1_ext5(bl + etc1_diff3(hi >> 8));
    } else {
        r1 = etc1_ext4((hi >> 28) & 0xf); r2 = etc1_ext4((hi >> 24) & 0xf);
        g1 = etc1_ext4((hi >> 20) & 0xf); g2 = etc1_ext4((hi >> 16) & 0xf);
        b1 = etc1_ext4((hi >> 12) & 0xf); b2 = etc1_ext4((hi >> 8) & 0xf);
    }
    const int *t1 = kEtc1Modifier[(hi >> 5) & 7];
    const int *t2 = kEtc1Modifier[(hi >> 2) & 7];

    for (int px = 0; px < 4; ++px) {
        for (int py = 0; py < 4; ++py) {
            const int k = py + px * 4; /* pixel bit index: column-major */
            const int idx = (int)(((lo >> k) & 1) | (((lo >> (k + 16)) & 1) << 1));
            const bool sub2 = flip ? (py >= 2) : (px >= 2);
            const int mod = sub2 ? t2[idx] : t1[idx];
            const int ox = ox0 + px, oy = oy0 + py;
            if (ox >= width || oy >= height) continue;
            std::uint8_t *p = out + ((std::size_t)oy * width + ox) * 3;
            p[0] = (std::uint8_t)etc1_clamp8((sub2 ? r2 : r1) + mod);
            p[1] = (std::uint8_t)etc1_clamp8((sub2 ? g2 : g1) + mod);
            p[2] = (std::uint8_t)etc1_clamp8((sub2 ? b2 : b1) + mod);
        }
    }
}

/* Decodes a full ETC1 image (width*height, row-major RGB8). Returns false when
 * the source is too small for the dimensions, so the caller can fall back to the
 * raw upload rather than read past the buffer. */
bool etc1_decode(const std::uint8_t *src, int imageSize, int width, int height,
                 std::vector<std::uint8_t> &out) {
    if (src == nullptr || width <= 0 || height <= 0) return false;
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if ((long long)bw * bh * 8 > imageSize) return false;
    out.assign((std::size_t)width * height * 3, 0);
    const std::uint8_t *p = src;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx, p += 8)
            etc1_decode_block(p, out.data(), width, height, bx * 4, by * 4);
    return true;
}

/* A readable name for the compressed formats PvZ2 could plausibly ship, so the
 * log names the format instead of printing a bare hex enum. */
const char *compressed_format_name(GLenum fmt) {
    switch (fmt) {
        case 0x8D64: return "ETC1_RGB8_OES";
        case 0x9274: return "COMPRESSED_RGB8_ETC2";
        case 0x9278: return "COMPRESSED_RGBA8_ETC2_EAC";
        case 0x9276: return "COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2";
        case 0x8C00: return "PVRTC_RGB_4BPPV1";
        case 0x8C01: return "PVRTC_RGB_2BPPV1";
        case 0x8C02: return "PVRTC_RGBA_4BPPV1";
        case 0x8C03: return "PVRTC_RGBA_2BPPV1";
        case 0x83F0: return "S3TC_DXT1_RGB";
        case 0x83F1: return "S3TC_DXT1_RGBA";
        case 0x83F2: return "S3TC_DXT3_RGBA";
        case 0x83F3: return "S3TC_DXT5_RGBA";
        default:     return "unknown";
    }
}

/* Once, on the first compressed upload: dump the compressed formats the HOST
 * driver actually advertises. This is the crux of the "black on some PCs"
 * report -- PvZ2 uploads Android-format compressed textures (ETC1 and friends)
 * straight through, and a driver that does not list the format rejects the
 * upload, leaving the texture black. On the dev machine the format is present
 * (or software-decoded by the driver) so nothing is wrong there; a weaker Intel
 * GPU with an older driver lists neither ETC nor PVRTC and every background and
 * plant -- everything not uploaded as plain RGBA -- comes out black, while the
 * UI built from uncompressed atlases stays visible. Listing what the driver
 * supports next to what the engine asks for turns that guess into a fact. */
void log_compressed_support_once(GuestCall &c) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    GLint n = 0;
    glGetIntegerv(0x86A2 /* GL_NUM_COMPRESSED_TEXTURE_FORMATS */, &n);
    if (n < 0) n = 0;
    if (n > 128) n = 128;
    std::vector<GLint> formats((std::size_t)n, 0);
    if (n > 0) glGetIntegerv(0x86A3 /* GL_COMPRESSED_TEXTURE_FORMATS */, formats.data());
    const GLubyte *renderer = glGetString(0x1F01 /* GL_RENDERER */);
    c.trace("[gl] host renderer: %s", renderer ? (const char *)renderer : "?");
    c.trace("[gl] host advertises %d compressed texture format(s):", (int)n);
    for (GLint i = 0; i < n; ++i) {
        c.trace("[gl]   0x%04x %s", (unsigned)formats[i], compressed_format_name((GLenum)formats[i]));
    }
}

void gl_glCompressedTexImage2D(GuestCall &c) {
    const GLenum internalformat = (GLenum)c.arg(2);
    const GLsizei width = (GLsizei)c.arg(3);
    const GLsizei height = (GLsizei)c.arg(4);

    log_compressed_support_once(c);
    /* Name each distinct format the engine uploads once, so we see the working
     * set without a per-texture flood. */
    {
        static std::mutex fmt_lock;
        static std::set<GLenum> seen;
        bool first;
        {
            std::lock_guard<std::mutex> lk(fmt_lock);
            first = seen.insert(internalformat).second;
        }
        if (first) {
            c.trace("[gl] engine uploads compressed format 0x%04x %s (first seen %dx%d)",
                  (unsigned)internalformat, compressed_format_name(internalformat), (int)width,
                  (int)height);
        }
    }

    const GLint level = (GLint)c.arg(1);
    const GLsizei imageSize = (GLsizei)c.arg(6);
    const void *data = c.ptr(c.arg(7));

    /* ETC1: decode to RGB8 on the CPU and upload uncompressed, so it no longer
     * matters whether this driver exposes ETC1/ETC2 at all. Every non-RGBA PvZ2
     * texture takes this path -- backgrounds, plants, the lot -- and passing ETC1
     * straight through is exactly what left them black on drivers that reject the
     * format. A per-thread scratch buffer avoids re-allocating a few MB on every
     * texture; GL runs on one thread, but thread_local costs nothing anyway. */
    if (internalformat == 0x8D64 /* GL_ETC1_RGB8_OES */) {
        static thread_local std::vector<std::uint8_t> rgb;
        if (etc1_decode((const std::uint8_t *)data, imageSize, width, height, rgb)) {
            glTexImage2D(c.arg(0), level, 0x8051 /* GL_RGB8 */, width, height, 0,
                         0x1907 /* GL_RGB */, 0x1401 /* GL_UNSIGNED_BYTE */, rgb.data());
            report_gl_error(c, "glTexImage2D(ETC1->RGB8)");
            return;
        }
        /* Undersized source -- fall through to the raw upload, which at worst
         * reproduces the old behaviour rather than reading OOB. */
    }

    glCompressedTexImage2D(c.arg(0), level, internalformat, width, height, c.arg(5), imageSize,
                           data);

    /* Did the driver accept it? A GL_INVALID_ENUM here is the whole bug: the
     * format is not supported and this texture is now black. */
    report_gl_error(c, "glCompressedTexImage2D");
}

void gl_glCreateProgram(GuestCall &c) {
    c.regs[0] = glCreateProgram();
}

void gl_glCreateShader(GuestCall &c) {
    c.regs[0] = glCreateShader(c.arg(0));
}

void gl_glCullFace(GuestCall &c) {
    glCullFace(c.arg(0));
}

void gl_glDeleteFramebuffers(GuestCall &c) {
    glDeleteFramebuffers(c.arg(0), (GLuint *)c.ptr(c.arg(1)));
}

void gl_glDeleteProgram(GuestCall &c) {
    glDeleteProgram(c.arg(0));
}

void gl_glDeleteShader(GuestCall &c) {
    glDeleteShader(c.arg(0));
}

void gl_glDeleteTextures(GuestCall &c) {
    glDeleteTextures(c.arg(0), (const GLuint *)c.ptr(c.arg(1)));
}

void gl_glDepthFunc(GuestCall &c) {
    glDepthFunc(c.arg(0));
}

void gl_glDepthMask(GuestCall &c) {
    glDepthMask((GLboolean)c.arg(0));
}

void gl_glDepthRangef(GuestCall &c) {
    glDepthRangef(c.argf(0), c.argf(1));
}

void gl_glDisable(GuestCall &c) {
    glDisable(c.arg(0));
}

void gl_glDisableClientState(GuestCall &c) {
    glDisableClientState(c.arg(0));
}

void gl_glDisableVertexAttribArray(GuestCall &c) {
    glDisableVertexAttribArray(c.arg(0));
}

void gl_glEnable(GuestCall &c) {
    glEnable(c.arg(0));
}

void gl_glEnableClientState(GuestCall &c) {
    glEnableClientState(c.arg(0));
}

void gl_glEnableVertexAttribArray(GuestCall &c) {
    glEnableVertexAttribArray(c.arg(0));
}

void gl_glFramebufferTexture2D(GuestCall &c) {
    glFramebufferTexture2D(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4));
}

void gl_glFrontFace(GuestCall &c) {
    glFrontFace(c.arg(0));
}

void gl_glGenFramebuffers(GuestCall &c) {
    glGenFramebuffers(c.arg(0), (GLuint *)c.ptr(c.arg(1)));
}

void gl_glGenTextures(GuestCall &c) {
    glGenTextures(c.arg(0), (GLuint *)c.ptr(c.arg(1)));
}

void gl_glGetError(GuestCall &c) {
    c.regs[0] = glGetError();
}

void gl_glGetIntegerv(GuestCall &c) {
    glGetIntegerv(c.arg(0), (GLint *)c.ptr(c.arg(1)));
}

void gl_glGetProgramInfoLog(GuestCall &c) {
    glGetProgramInfoLog(c.arg(0), c.arg(1), (GLsizei *)c.ptr(c.arg(2)), (GLchar *)c.ptr(c.arg(3)));
}

void gl_glGetProgramiv(GuestCall &c) {
    glGetProgramiv(c.arg(0), c.arg(1), (GLint *)c.ptr(c.arg(2)));
}

void gl_glGetShaderInfoLog(GuestCall &c) {
    glGetShaderInfoLog(c.arg(0), c.arg(1), (GLsizei *)c.ptr(c.arg(2)), (GLchar *)c.ptr(c.arg(3)));
}

void gl_glGetShaderiv(GuestCall &c) {
    glGetShaderiv(c.arg(0), c.arg(1), (GLint *)c.ptr(c.arg(2)));
}

void gl_glGetString(GuestCall &c) {
    const GLubyte *s = glGetString(c.arg(0));
    const char *cs = s ? (const char *)s : "";
    __android_log_print(ANDROID_LOG_INFO, "RunnerGLES", "glGetString(0x%x) -> %.64s...", c.arg(0), cs);
    uint32_t len = (uint32_t)std::strlen(cs) + 1;
    uint32_t addr = c.rt->heap.alloc(len);
    /* A driver string, not guest memory -- must be copied into the guest's own
     * address space before handing its address back, unlike every other pointer
     * here which already lives in c.img->mem and can be passed through as-is. */
    if (addr) std::memcpy(&c.img->mem[addr], cs, len);
    c.regs[0] = addr;
}

void gl_glIsProgram(GuestCall &c) {
    c.regs[0] = glIsProgram(c.arg(0));
}

void gl_glIsShader(GuestCall &c) {
    c.regs[0] = glIsShader(c.arg(0));
}

void gl_glIsTexture(GuestCall &c) {
    c.regs[0] = glIsTexture(c.arg(0));
}

void gl_glLineWidth(GuestCall &c) {
    glLineWidth(c.argf(0));
}

void gl_glLoadIdentity(GuestCall &c) {
    glLoadIdentity();
}

void gl_glLoadMatrixf(GuestCall &c) {
    glLoadMatrixf((const GLfloat *)c.ptr(c.arg(0)));
}

void gl_glMatrixMode(GuestCall &c) {
    glMatrixMode(c.arg(0));
}

void gl_glNormalPointer(GuestCall &c) {
    glNormalPointer(c.arg(0), c.arg(1), c.ptr(c.arg(2)));
}

void gl_glPixelStorei(GuestCall &c) {
    glPixelStorei(c.arg(0), c.arg(1));
}

void gl_glPopMatrix(GuestCall &c) {
    glPopMatrix();
}

void gl_glPushMatrix(GuestCall &c) {
    glPushMatrix();
}

void gl_glScalef(GuestCall &c) {
    glScalef(c.argf(0), c.argf(1), c.argf(2));
}

void gl_glScissor(GuestCall &c) {
    glScissor(c.arg(0), c.arg(1), c.arg(2), c.arg(3));
}

void gl_glShadeModel(GuestCall &c) {
    glShadeModel(c.arg(0));
}

/* Removes every `precision <qualifier> <type>;` statement from a shader body.
 * GLSL 1.30 accepts them as no-ops, but 1.20 (a 2.1 host) does not know the
 * concept at all, so they have to go. Only a whole-word `precision` at a token
 * boundary and followed by whitespace is treated as a statement, and it is
 * erased up to and including its terminating ';' -- a precision statement never
 * contains another semicolon, so this cannot swallow real code. */
static void strip_precision_statements(std::string &src) {
    std::size_t pos = 0;
    while ((pos = src.find("precision", pos)) != std::string::npos) {
        const std::size_t after = pos + 9; /* strlen("precision") */
        const bool at_word_start =
            pos == 0 || !(std::isalnum((unsigned char)src[pos - 1]) || src[pos - 1] == '_');
        const bool followed_by_space =
            after < src.size() && std::isspace((unsigned char)src[after]);
        if (at_word_start && followed_by_space) {
            const std::size_t semi = src.find(';', after);
            if (semi == std::string::npos) break;
            src.erase(pos, semi - pos + 1);
        } else {
            pos = after;
        }
    }
}

static void adapt_shader_source(std::string &src) {
    (void)src;
}

void gl_glShaderSource(GuestCall &c) {
    /* The guest ships GLSL ES 1.00, which desktop GL rejects outright:
     *   "syntax error, unexpected identifier ... at token \"lowp\""
     * Every shader failed, every program failed to link, and the engine -- which
     * never reads the info log -- kept issuing draw calls against no valid
     * program, i.e. a black screen.
     *
     * adapt_shader_source() rewrites it for whatever the host context actually
     * is: a 3.0+ context just gets a "#version 130" prefix (1.30 accepts the ES
     * qualifiers as no-ops), a 2.1 one gets a "#version 120" prefix plus the
     * precision-qualifier rewrite. Which of the two is chosen by
     * pvz2_gl_target_glsl(), latched at startup, so this layer holds no version
     * policy of its own. */
    GLuint shader = c.arg(0);
    GLsizei count = (GLsizei)c.arg(1);
    uint32_t strings_ptr = c.arg(2);
    uint32_t length_ptr = c.arg(3);

    std::string src;
    for (GLsizei i = 0; i < count; ++i) {
        uint32_t guest_str_addr = c.read32(strings_ptr + (uint32_t)i * 4);
        const char *chunk = (const char *)c.ptr(guest_str_addr);
        if (chunk == nullptr) continue;
        if (length_ptr) {
            GLint len = (GLint)c.read32(length_ptr + (uint32_t)i * 4);
            if (len >= 0) { src.append(chunk, (size_t)len); continue; }
        }
        src.append(chunk);
    }

    adapt_shader_source(src);

    /* [gl] flat_fragment=1 replaces every fragment shader's body with a constant
     * magenta output, keeping its declarations so it still links against the same
     * attributes and uniforms.
     *
     * This is a bisection, not a fix. The composite draw has had every input
     * verified -- geometry, matrix, attribute indices, vertex data, source
     * texture, sampler, shader source, blend, scissor, depth, stencil, cull,
     * colour mask, frame order, GL errors -- and still puts nothing on the
     * window, while an identical draw earlier in the run does. Emitting a
     * constant splits what is left in two: a magenta window means the draw
     * rasterises and the fault is in the sampling; a black one means it produces
     * no fragments at all. */
    const bool flat = pvz2_config()->gl_flat_fragment != 0;
    if (flat && src.find("gl_FragColor") != std::string::npos) {
        const size_t body = src.find("void main");
        if (body != std::string::npos) {
            src.resize(body);
            src += "void main() { gl_FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";
        }
    }

    const GLchar *one = src.c_str();
    GLint one_len = (GLint)src.size();
    glShaderSource(shader, 1, &one, &one_len);
}

void gl_glTexCoordPointer(GuestCall &c) {
    glTexCoordPointer(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
}

void gl_glTexEnvf(GuestCall &c) {
    glTexEnvf(c.arg(0), c.arg(1), c.argf(2));
}

void gl_glTexImage2D(GuestCall &c) {
    glTexImage2D(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5),
                              c.arg(6), c.arg(7), c.ptr(c.arg(8)));
}

void gl_glTexParameteri(GuestCall &c) {
    glTexParameteri(c.arg(0), c.arg(1), c.arg(2));
}

void gl_glTexSubImage2D(GuestCall &c) {
    glTexSubImage2D(c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5),
                                  c.arg(6), c.arg(7), c.ptr(c.arg(8)));
}

void gl_glUniform1i(GuestCall &c) {
    glUniform1i(c.arg(0), c.arg(1));
}

void gl_glUniform4fv(GuestCall &c) {
    glUniform4fv(c.arg(0), c.arg(1), (const GLfloat *)c.ptr(c.arg(2)));
}

/* The location of a program's single mat4 uniform, or -1 if it has none or
 * more than one. Cached because it costs a full uniform enumeration and is
 * fixed for the life of a link. */
GLint sole_mat4_location(GLuint program) {
    static std::mutex lock;
    static std::unordered_map<GLuint, GLint> cache;
    {
        std::lock_guard<std::mutex> lk(lock);
        auto it = cache.find(program);
        if (it != cache.end()) return it->second;
    }

    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    GLint found = -1;
    for (GLint i = 0; i < count; ++i) {
        char nm[128] = {0};
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(nm) - 1, &len, &size, &type, nm);
        if (type != GL_FLOAT_MAT4) continue;
        if (found != -1) { found = -1; break; } /* ambiguous: leave it alone */
        found = glGetUniformLocation(program, nm);
    }
    std::lock_guard<std::mutex> lk(lock);
    cache[program] = found;
    return found;
}

void gl_glUniformMatrix4fv(GuestCall &c) {
    GLint loc = (GLint)c.arg(0);

    /* sub_ABBCEC uploads the screen matrix to TWO shaders back to back, using
     * locations it cached per shader, and without a glUseProgram between them
     * -- so both uploads land on whichever program is currently bound. That is
     * only harmless if every program agrees on where its mat4 lives, which is
     * true on the Android GLES driver (screenMatrix comes out at location 0
     * everywhere) and false here: the desktop compiler orders uniforms so that
     * programs declaring Tex0/Tex1 first put screenMatrix at 2. The mismatched
     * upload is rejected with INVALID_OPERATION, the projection matrix never
     * arrives, and the frame composites with garbage -- a black screen.
     *
     * Re-resolving against the bound program fixes it without touching the
     * engine's caching. Only done when the program has exactly one mat4, so a
     * shader with several is never second-guessed. */
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program != 0) {
        const GLint actual = sole_mat4_location((GLuint)program);
        if (actual >= 0 && actual != loc) {
            static std::atomic<std::uint32_t> budget{4};
            if (budget.load(std::memory_order_relaxed) > 0) {
                budget.fetch_sub(1, std::memory_order_relaxed);
                c.trace("[gl] program %d: remapping mat4 uniform location %d -> %d "
                      "(engine cached another program's location)", program, loc, actual);
            }
            loc = actual;
        }
    }

    const GLfloat *m = (const GLfloat *)c.ptr(c.arg(3));

    /* Reject a non-finite projection matrix, and reuse the last good one.
     *
     * This is the actual reason the screen is black. Read back from the
     * program at draw time, screenMatrix holds
     *     [ inf  nan  nan -inf ]
     *     [ nan -inf  nan  inf ]
     * Every vertex transformed by it becomes NaN, so the rasteriser emits no
     * fragments at all -- which is why even a constant-colour fragment shader
     * could not put anything on the window, while glClear still could.
     *
     * The inf comes from the same corrupt value as the zero-width viewport
     * handled in gl_glViewport: an orthographic projection is built as
     * 2/width, and 2/0 is inf. Both are symptoms of a width of 0 arriving from
     * the engine's render-state cache (sub_ABC650 reads x/y/w/h at stride 124
     * and gets them one slot out); the real fix is upstream, in whatever
     * corrupts that cache. Until then, holding the last finite matrix keeps
     * the projection usable instead of poisoning every vertex. */
    if (m != nullptr && c.arg(1) == 1) {
        bool finite = true;
        for (int i = 0; i < 16; ++i) {
            const float v = m[i];
            if (v != v || v > 3.0e38f || v < -3.0e38f) { finite = false; break; }
        }
        static std::mutex mtx_lock;
        static std::unordered_map<GLint, std::array<GLfloat, 16>> last_good;
        std::lock_guard<std::mutex> lk(mtx_lock);
        if (finite) {
            std::array<GLfloat, 16> copy{};
            std::memcpy(copy.data(), m, sizeof(copy));
            last_good[program] = copy;
        } else {
            auto it = last_good.find(program);
            static std::atomic<std::uint32_t> budget{4};
            if (budget.load(std::memory_order_relaxed) > 0) {
                budget.fetch_sub(1, std::memory_order_relaxed);
                c.trace("[gl] program %d: screenMatrix upload is non-finite (inf/nan) -- %s",
                      program, it != last_good.end() ? "substituting the last finite one"
                                                     : "no finite matrix seen yet, dropping the upload");
            }
            if (it != last_good.end()) {
                /* Magnitudes from the last good matrix, SIGNS from the one the
                 * engine just tried to upload.
                 *
                 * An infinity keeps the sign of the division that produced it,
                 * so the corrupt matrix still says what the engine meant. It
                 * carries x-scale +inf, y-scale -inf, x-translate -inf,
                 * y-translate +inf: an orthographic projection with the Y axis
                 * FLIPPED, which is what a 2D engine with y-down screen
                 * coordinates needs. The last good matrix happens to be an
                 * unflipped one, and using it verbatim renders the whole game
                 * upside down. Transferring the signs fixes that; NaN carries
                 * no sign, so those entries are left as they are. */
                std::array<GLfloat, 16> fixed = it->second;
                for (int i = 0; i < 16; ++i) {
                    const float bad = m[i];
                    if (bad != bad) continue;                    /* NaN: no sign to take */
                    if (bad <= 3.0e38f && bad >= -3.0e38f) continue; /* finite: nothing to fix */
                    const bool want_negative = bad < 0.0f;
                    if (want_negative != (fixed[i] < 0.0f)) fixed[i] = -fixed[i];
                }
                glUniformMatrix4fv(loc, 1, (GLboolean)c.arg(2), fixed.data());
            }
            return; /* never hand inf/nan to the driver */
        }
    }

    /* The window is pure black, yet a forced clear colour DOES show through --
     * which means the composite quad is not covering the screen at all, rather
     * than covering it with black. So the projection matrix it is drawn with is
     * the thing to look at. Logged once per program, column-major as GL takes
     * it. A sane screen matrix for a 1280x720 target has ~2/1280 and ~-2/720 on
     * the diagonal (or 1.0s if the engine feeds clip-space coordinates). */
    if (m != nullptr) {
        static std::mutex seen_lock;
        static std::set<GLint> seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(seen_lock);
            first = seen.size() < 8 && seen.insert(program).second;
        }
        if (first) {
            c.trace("[gl] screenMatrix for program %d (loc %d):", program, loc);
            for (int row = 0; row < 4; ++row) {
                c.trace("[gl]   % .5f % .5f % .5f % .5f", m[row], m[row + 4], m[row + 8], m[row + 12]);
            }
        }
    }

    glUniformMatrix4fv(loc, c.arg(1), (GLboolean)c.arg(2), m);
}

void gl_glUseProgram(GuestCall &c) {
    glUseProgram(c.arg(0));
}

void gl_glVertexAttribPointer(GuestCall &c) {
    glVertexAttribPointer(c.arg(0), c.arg(1), c.arg(2), (GLboolean)c.arg(3), c.arg(4),
                                      c.ptr(c.arg(5)));
}

void gl_glVertexPointer(GuestCall &c) {
    glVertexPointer(c.arg(0), c.arg(1), c.arg(2), c.ptr(c.arg(3)));
}

/* glGetUniformLocation was the one GLES symbol with no implementation at all:
 * it returns the uniform's location, and returning 0 unconditionally would
 * silently alias every uniform onto slot 0. */
void gl_glGetUniformLocation(GuestCall &c) {
    std::string nm = c.cstr(c.arg(1), 256);
    const GLint loc = glGetUniformLocation(c.arg(0), nm.c_str());
    /* The engine ends up calling glUniformMatrix4fv with location 0, which is
     * a sampler2D -- so either it never asks for the matrix's location, or it
     * asks and does not get 2. Logging every query answers which, and a failed
     * lookup (-1) shows up here rather than as a mystery INVALID_OPERATION
     * three calls later. */
    if (gl_strict_enabled()) {
        static std::atomic<std::uint32_t> budget{40};
        if (budget.load(std::memory_order_relaxed) > 0) {
            budget.fetch_sub(1, std::memory_order_relaxed);
            c.trace("[gl-strict] glGetUniformLocation(program=%u, \"%s\") -> %d", c.arg(0),
                  nm.c_str(), (int)loc);
        }
    }
    c.set_result((std::uint32_t)loc);
}

}  // namespace

bool gl_strict_enabled() { return pvz2_config()->gl_strict != 0; }

void gl_check_error_after(GuestCall &c, const char *name) {
    GLenum err = glGetError();
    if (err == GL_NO_ERROR) return;
    static std::atomic<std::uint32_t> budget{20};
    if (budget.load(std::memory_order_relaxed) == 0) return;
    budget.fetch_sub(1, std::memory_order_relaxed);
    c.trace("[gl] %s -> GL error 0x%04x (r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x) lr=0x%08x",
          name, (unsigned)err, c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.lr());
}


void gl_glColor4f(GuestCall &c) {
    glColor4f(c.argf(0), c.argf(1), c.argf(2), c.argf(3));
}

void gl_glOrthof(GuestCall &c) {
    glOrthof(c.argf(0), c.argf(1), c.argf(2), c.argf(3), c.argf(4), c.argf(5));
}

void gl_glReadPixels(GuestCall &c) {
    GLint x = (GLint)c.arg(0);
    GLint y = (GLint)c.arg(1);
    GLsizei w = (GLsizei)c.arg(2);
    GLsizei h = (GLsizei)c.arg(3);
    GLenum format = c.arg(4);
    GLenum type = c.arg(5);
    void *pixels = c.ptr(c.arg(6), (uint32_t)(w * h * 4));
    if (pixels) {
        glReadPixels(x, y, w, h, format, type, pixels);
    }
}

void gl_glDetachShader(GuestCall &c) {
    glDetachShader(c.arg(0), c.arg(1));
}

void register_libgles(ImportTable &t) {
    t.add("glGetUniformLocation", gl_glGetUniformLocation);
    t.add("glDrawArrays", gl_glDrawArrays);
    t.add("glDrawElements", gl_glDrawElements);
    t.add("glViewport", gl_glViewport);
    t.add("glActiveTexture", gl_glActiveTexture);
    t.add("glAlphaFunc", gl_glAlphaFunc);
    t.add("glAttachShader", gl_glAttachShader);
    t.add("glBindAttribLocation", gl_glBindAttribLocation);
    t.add("glBindFramebuffer", gl_glBindFramebuffer);
    t.add("glBindFramebufferOES", gl_glBindFramebuffer);
    t.add("glBindTexture", gl_glBindTexture);
    t.add("glBlendFunc", gl_glBlendFunc);
    t.add("glCheckFramebufferStatus", gl_glCheckFramebufferStatus);
    t.add("glCheckFramebufferStatusOES", gl_glCheckFramebufferStatus);
    t.add("glClear", gl_glClear);
    t.add("glClearColor", gl_glClearColor);
    t.add("glClearDepthf", gl_glClearDepthf);
    t.add("glClientActiveTexture", gl_glClientActiveTexture);
    t.add("glColorMask", gl_glColorMask);
    t.add("glColorPointer", gl_glColorPointer);
    t.add("glCompileShader", gl_glCompileShader);
    t.add("glLinkProgram", gl_glLinkProgram);
    t.add("glCompressedTexImage2D", gl_glCompressedTexImage2D);
    t.add("glCreateProgram", gl_glCreateProgram);
    t.add("glCreateShader", gl_glCreateShader);
    t.add("glCullFace", gl_glCullFace);
    t.add("glDeleteFramebuffers", gl_glDeleteFramebuffers);
    t.add("glDeleteFramebuffersOES", gl_glDeleteFramebuffers);
    t.add("glDeleteProgram", gl_glDeleteProgram);
    t.add("glDeleteShader", gl_glDeleteShader);
    t.add("glDeleteTextures", gl_glDeleteTextures);
    t.add("glDepthFunc", gl_glDepthFunc);
    t.add("glDepthMask", gl_glDepthMask);
    t.add("glDepthRangef", gl_glDepthRangef);
    t.add("glDisable", gl_glDisable);
    t.add("glDisableClientState", gl_glDisableClientState);
    t.add("glDisableVertexAttribArray", gl_glDisableVertexAttribArray);
    t.add("glEnable", gl_glEnable);
    t.add("glEnableClientState", gl_glEnableClientState);
    t.add("glEnableVertexAttribArray", gl_glEnableVertexAttribArray);
    t.add("glFramebufferTexture2D", gl_glFramebufferTexture2D);
    t.add("glFramebufferTexture2DOES", gl_glFramebufferTexture2D);
    t.add("glFrontFace", gl_glFrontFace);
    t.add("glGenFramebuffers", gl_glGenFramebuffers);
    t.add("glGenFramebuffersOES", gl_glGenFramebuffers);
    t.add("glGenTextures", gl_glGenTextures);
    t.add("glGetError", gl_glGetError);
    t.add("glGetIntegerv", gl_glGetIntegerv);
    t.add("glGetProgramInfoLog", gl_glGetProgramInfoLog);
    t.add("glGetProgramiv", gl_glGetProgramiv);
    t.add("glGetShaderInfoLog", gl_glGetShaderInfoLog);
    t.add("glGetShaderiv", gl_glGetShaderiv);
    t.add("glGetString", gl_glGetString);
    t.add("glIsProgram", gl_glIsProgram);
    t.add("glIsShader", gl_glIsShader);
    t.add("glIsTexture", gl_glIsTexture);
    t.add("glLineWidth", gl_glLineWidth);
    t.add("glLoadIdentity", gl_glLoadIdentity);
    t.add("glLoadMatrixf", gl_glLoadMatrixf);
    t.add("glMatrixMode", gl_glMatrixMode);
    t.add("glNormalPointer", gl_glNormalPointer);
    t.add("glPixelStorei", gl_glPixelStorei);
    t.add("glPopMatrix", gl_glPopMatrix);
    t.add("glPushMatrix", gl_glPushMatrix);
    t.add("glScalef", gl_glScalef);
    t.add("glScissor", gl_glScissor);
    t.add("glShadeModel", gl_glShadeModel);
    t.add("glShaderSource", gl_glShaderSource);
    t.add("glTexCoordPointer", gl_glTexCoordPointer);
    t.add("glTexEnvf", gl_glTexEnvf);
    t.add("glTexImage2D", gl_glTexImage2D);
    t.add("glTexParameteri", gl_glTexParameteri);
    t.add("glTexSubImage2D", gl_glTexSubImage2D);
    t.add("glUniform1i", gl_glUniform1i);
    t.add("glUniform4fv", gl_glUniform4fv);
    t.add("glUniformMatrix4fv", gl_glUniformMatrix4fv);
    t.add("glUseProgram", gl_glUseProgram);
    t.add("glVertexAttribPointer", gl_glVertexAttribPointer);
    t.add("glVertexPointer", gl_glVertexPointer);
    t.add("glColor4f", gl_glColor4f);
    t.add("glOrthof", gl_glOrthof);
    t.add("glReadPixels", gl_glReadPixels);
    t.add("glDetachShader", gl_glDetachShader);
}

}  // namespace pvz_tv
