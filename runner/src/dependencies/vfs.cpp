/* Guest path translation -- see vfs.h for why this layer exists at all. */

#include <pvz_tv/dependencies/vfs.h>

#include <pvz_tv/config.h>
#include <pvz_tv/runtime/jit_tuning.h>

#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#endif

namespace pvz_tv {
namespace vfs {

/* Real on-disk resources (doc section 9.22). The game's own native loader
 * (ResStreamsManager/ResourceManager: sub_2BE120 -> sub_867740/sub_864950)
 * opens "main.rsb" via libc fopen/fread and decodes the outer "1bsr" RSB plus
 * the inner "pgsr"/RTON formats ITSELF -- so none of those formats are
 * reimplemented here. It gets real file I/O over the real .obb, which literally
 * IS a "1bsr" archive (magic confirmed). */
/* Both come from config.ini (see pvz2_config_load): the host path is used
 * verbatim, and the guest path is the same with a leading '/' so
 * ResStreamsManager::LoadRSB takes its verbatim branch (see vfs.h). The guest
 * form is cached on first use -- the config path is fixed for the run. */
const char *obb_host_path() { return pvz2_config()->obb_path; }

const char *obb_guest_path() {
    static const std::string guest = std::string("/") + pvz2_config()->obb_path;
    return guest.c_str();
}

static std::unordered_map<std::string, std::string> s_vfs_index;
static std::once_flag s_vfs_index_once;

static void collapse_slashes(std::string &s) {
    std::string out;
    out.reserve(s.size());
    bool last_slash = false;
    for (char c : s) {
        if (c == '\\' || c == '/') {
            if (!last_slash) {
                out.push_back('/');
                last_slash = true;
            }
        } else {
            out.push_back(c);
            last_slash = false;
        }
    }
    s = std::move(out);
}

static std::string to_lower_str(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

/* The walk runs from inside a guest file call, so nothing here may throw: an
 * entry that cannot be read -- or, on Windows, whose name the narrow encoding
 * cannot represent -- is skipped and the rest of the tree is still indexed. */
static void index_dir(const std::string &dir_path, const std::string &prefix,
                      std::initializer_list<const char *> skip_top = {}) {
    std::error_code ec;
    if (!std::filesystem::exists(dir_path, ec)) return;
    std::string root = std::filesystem::path(dir_path).generic_string();
    collapse_slashes(root);
    try {
        std::filesystem::recursive_directory_iterator walk(
            dir_path, std::filesystem::directory_options::skip_permission_denied, ec);
        for (auto it = std::filesystem::begin(walk); it != std::filesystem::end(walk); it.increment(ec)) {
            if (ec) break;
            try {
                if (it.depth() == 0 && it->is_directory(ec)) {
                    const std::string name = it->path().filename().generic_string();
                    for (const char *skip : skip_top) {
                        if (name == skip) {
                            it.disable_recursion_pending();
                            break;
                        }
                    }
                    continue;
                }
                if (!it->is_regular_file(ec)) continue;

                std::string full = it->path().generic_string();
                collapse_slashes(full);
                /* Every path the walk yields starts with dir_path, so the
                 * relative part is a substring. std::filesystem::relative()
                 * canonicalises both sides with a stat per path component --
                 * for every one of ~20000 files. */
                std::string rel = full.size() > root.size() ? full.substr(root.size()) : std::string();
                if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);

                std::string rel_lower = to_lower_str(rel);
                s_vfs_index[rel_lower] = full;
                s_vfs_index["/" + rel_lower] = full;

                if (!prefix.empty()) {
                    std::string with_p = prefix + "/" + rel;
                    collapse_slashes(with_p);
                    std::string with_p_lower = to_lower_str(with_p);
                    s_vfs_index[with_p_lower] = full;
                    s_vfs_index["/" + with_p_lower] = full;
                }
            } catch (const std::exception &) {
                continue;
            }
        }
    } catch (const std::exception &) {
        /* Whatever was collected before the failure stays usable. */
    }
}

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "RunnerVFS", __VA_ARGS__)
#else
#define LOGI(...)
#endif

static void ensure_vfs_indexed() {
    std::call_once(s_vfs_index_once, []() {
        index_dir("assets", "assets");
        /* The APK keeps the game data one level down, in assets/files; the
         * Android extractor flattens that away, while a desktop install can
         * hold the asset directory exactly as it is packed. Indexing it as a
         * root as well makes both layouts resolve the same guest paths. */
        index_dir("assets/files", "assets");
        index_dir("userdata", "userdata");
        index_dir("pseudo_fs", "");
        /* What a pak or an imported config drops next to the assets, which
         * wins over them because it is indexed last. The cwd is the data
         * directory, so the asset tree is under it too: walking it again here
         * (and once more through absolute paths that named the very same
         * directory) was most of the time this took. data/ and pseudo_fs/
         * are answered from the filesystem anyway. */
        index_dir(".", "", {"assets", "userdata", "data", "pseudo_fs"});
        LOGI("VFS indexed %zu entries across asset directories", s_vfs_index.size());
    });
}

constexpr char kAssetScheme[] = "ASSET:";
constexpr std::size_t kAssetSchemeLen = 6;

std::string translate(GuestRuntime *rt, std::string p) {
    ensure_vfs_indexed();

    if (p.compare(0, kAssetSchemeLen, kAssetScheme) == 0) p.erase(0, kAssetSchemeLen);
    for (char &c : p) if (c == '\\') c = '/';
    collapse_slashes(p);

    if (p.size() >= 3 && p[0] == '/' && std::isalpha((unsigned char)p[1]) && p[2] == ':') {
        p.erase(0, 1);
    }

    // Map save data folder
    static const char kEngineDataDir[] = "No_Backup";
    static const char kHostDataDir[] = "userdata";
    for (std::size_t at = p.find(kEngineDataDir); at != std::string::npos;
         at = p.find(kEngineDataDir, at + sizeof(kHostDataDir) - 1)) {
        p.replace(at, sizeof(kEngineDataDir) - 1, kHostDataDir);
    }

    // Map pseudo filesystem (/proc/...)
    if (p.rfind("/proc/", 0) == 0 || p.rfind("proc/", 0) == 0) {
        std::string rel = p;
        if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        std::string target = "pseudo_fs/" + rel;
        return target;
    }

    // Fast in-memory lookup
    std::string p_lower = to_lower_str(p);
    auto it = s_vfs_index.find(p_lower);
    if (it != s_vfs_index.end()) {
        return it->second;
    }

    if (!p_lower.empty() && p_lower[0] == '/') {
        auto it2 = s_vfs_index.find(p_lower.substr(1));
        if (it2 != s_vfs_index.end()) return it2->second;
    }

    std::size_t fpos = p_lower.find("files/");
    if (fpos != std::string::npos) {
        std::string sub = p_lower.substr(fpos + 6);
        auto it3 = s_vfs_index.find(sub);
        if (it3 != s_vfs_index.end()) return it3->second;
    }

    return p;
}

/* The directories the game writes into, where the index cannot be trusted to
 * be complete because the contents appear while it runs. */
static bool is_writable_area(const std::string &key) {
    for (const char *dir : {"data", "userdata", "pseudo_fs"}) {
        const std::size_t len = std::strlen(dir);
        if (key.compare(0, len, dir) == 0 && (key.size() == len || key[len] == '/')) {
            return true;
        }
    }
    return false;
}

bool exists(GuestRuntime *rt, const std::string &guest_path, std::string &out_host) {
    ensure_vfs_indexed();
    out_host = translate(rt, guest_path);
    std::string key = to_lower_str(out_host);
    collapse_slashes(key);
    if (s_vfs_index.find(key) != s_vfs_index.end()) {
        return true;
    }
    // Also check guest path itself in index
    std::string g_key = to_lower_str(guest_path);
    collapse_slashes(g_key);
    if (s_vfs_index.find(g_key) != s_vfs_index.end()) {
        out_host = s_vfs_index[g_key];
        return true;
    }

    /* The index is a snapshot taken at startup, so nothing the game creates
     * while it runs can be in it: its profile under data/, a save. Those
     * directories have to be answered from the filesystem -- not doing so made
     * the game give up on its own first launch, having created a profile it
     * then could not see.
     *
     * Everywhere else the index is the authority. The assets do not change
     * while the game runs, and loading probes thousands of paths that simply
     * are not there (one per extension it is willing to accept); turning each
     * of those misses into a stat() is enough to make loading crawl. */
    if (is_writable_area(key) || is_writable_area(g_key)) {
        if (!parent_dir_exists(out_host)) return false;
        std::error_code ec;
        return std::filesystem::exists(out_host, ec);
    }
    return false;
}

static std::mutex s_dir_lock;
static std::unordered_map<std::string, bool> s_dir_exists;

bool parent_dir_exists(const std::string &host_path) {
    const std::size_t slash = host_path.find_last_of("/\\");
    if (slash == std::string::npos) return true; /* the cwd itself */
    const std::string dir = host_path.substr(0, slash);
    if (dir.empty()) return true;               /* the root */
    {
        std::lock_guard<std::mutex> lk(s_dir_lock);
        auto it = s_dir_exists.find(dir);
        if (it != s_dir_exists.end()) return it->second;
    }
    std::error_code ec;
    const bool present = std::filesystem::is_directory(dir, ec);
    std::lock_guard<std::mutex> lk(s_dir_lock);
    s_dir_exists[dir] = present;
    return present;
}

void invalidate_dir_cache() {
    std::lock_guard<std::mutex> lk(s_dir_lock);
    s_dir_exists.clear();
}

void write_pseudo_cpuinfo() {
    std::error_code ec;
    std::filesystem::create_directories("pseudo_fs/proc", ec);
    std::FILE *f = std::fopen("pseudo_fs/proc/cpuinfo", "w");
    if (!f) return;
    unsigned cpus = std::thread::hardware_concurrency();
    if (cpus == 0) cpus = 4;
    const char *neon = (jit_tuning() & kJitGuestNeon) ? "neon " : "";
    for (unsigned i = 0; i < cpus; ++i) {
        std::fprintf(f,
                     "processor\t: %u\n"
                     "model name\t: ARMv7 Processor rev 4 (v7l)\n"
                     "BogoMIPS\t: 26.00\n"
                     "Features\t: half thumb fastmult vfp edsp %svfpv3 tls vfpv4 idiva idivt lpae evtstrm\n"
                     "CPU implementer\t: 0x41\n"
                     "CPU architecture: 7\n"
                     "CPU variant\t: 0x0\n"
                     "CPU part\t: 0xd03\n"
                     "CPU revision\t: 4\n\n",
                     i, neon);
    }
    std::fprintf(f, "Hardware\t: pvztv-runner\n");
    std::fclose(f);
}

void ensure_writable_dirs() {
    std::error_code ec;
    for (const char *dir : {"data", "data/userdata", "userdata"}) {
        std::filesystem::create_directories(dir, ec);
    }
}

int translate_open_flags(std::uint32_t g) {
    int h = (int)(g & 3); /* O_RDONLY/O_WRONLY/O_RDWR share values 0/1/2 */
#if defined(_WIN32)
    if (g & 0x40)  h |= _O_CREAT;
    if (g & 0x80)  h |= _O_EXCL;
    if (g & 0x200) h |= _O_TRUNC;
    if (g & 0x400) h |= _O_APPEND;
    h |= _O_BINARY;
#else
    if (g & 0x40)  h |= O_CREAT;
    if (g & 0x80)  h |= O_EXCL;
    if (g & 0x200) h |= O_TRUNC;
    if (g & 0x400) h |= O_APPEND;
#endif
    return h;
}

}  // namespace vfs
}  // namespace pvz_tv
