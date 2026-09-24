/* libc.so -- <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>
 * Real host socket networking implementation for Homura netplay & multiplayer.
 */

#include <pvz_tv/diagnostics.h>
#include <pvz_tv/dependencies/dependency.h>
#include <pvz_tv/runtime/guest_runtime.h>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <dlfcn.h>
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#define closesocket(s) close(s)
#endif

#ifdef ifc_buf
#undef ifc_buf
#endif
#ifdef ifc_len
#undef ifc_len
#endif
#ifdef ifc_req
#undef ifc_req
#endif

namespace pvz_tv {

/* Linux / Bionic errno numbers */
constexpr std::uint32_t kEPERM = 1;
constexpr std::uint32_t kENOENT = 2;
constexpr std::uint32_t kEINTR = 4;
constexpr std::uint32_t kEBADF = 9;
constexpr std::uint32_t kEAGAIN = 11;
constexpr std::uint32_t kEWOULDBLOCK = 11;
constexpr std::uint32_t kENOMEM = 12;
constexpr std::uint32_t kEACCES = 13;
constexpr std::uint32_t kEFAULT = 14;
constexpr std::uint32_t kEINVAL = 22;
constexpr std::uint32_t kEMFILE = 24;
constexpr std::uint32_t kENOTTY = 25;
constexpr std::uint32_t kENOTSOCK = 88;
constexpr std::uint32_t kEDESTADDRREQ = 89;
constexpr std::uint32_t kEMSGSIZE = 90;
constexpr std::uint32_t kEPROTOTYPE = 91;
constexpr std::uint32_t kENOPROTOOPT = 92;
constexpr std::uint32_t kEPROTONOSUPPORT = 93;
constexpr std::uint32_t kEAFNOSUPPORT = 97;
constexpr std::uint32_t kEADDRINUSE = 98;
constexpr std::uint32_t kEADDRNOTAVAIL = 99;
constexpr std::uint32_t kENETDOWN = 100;
constexpr std::uint32_t kENETUNREACH = 101;
constexpr std::uint32_t kENETRESET = 102;
constexpr std::uint32_t kECONNABORTED = 103;
constexpr std::uint32_t kECONNRESET = 104;
constexpr std::uint32_t kENOBUFS = 105;
constexpr std::uint32_t kEISCONN = 106;
constexpr std::uint32_t kENOTCONN = 107;
constexpr std::uint32_t kESHUTDOWN = 108;
constexpr std::uint32_t kETIMEDOUT = 110;
constexpr std::uint32_t kECONNREFUSED = 111;
constexpr std::uint32_t kEHOSTUNREACH = 113;
constexpr std::uint32_t kEALREADY = 114;
constexpr std::uint32_t kEINPROGRESS = 115;

constexpr std::uint32_t kMinusOne = (std::uint32_t)-1;

#if defined(_WIN32)
uint32_t wsa_to_linux_errno(int werr) {
    switch (werr) {
        case 0: return 0;
        case WSAEWOULDBLOCK:    return kEWOULDBLOCK;
        case WSAEINPROGRESS:    return kEINPROGRESS;
        case WSAEALREADY:       return kEALREADY;
        case WSAENOTSOCK:       return kENOTSOCK;
        case WSAEDESTADDRREQ:   return kEDESTADDRREQ;
        case WSAEMSGSIZE:       return kEMSGSIZE;
        case WSAEPROTOTYPE:     return kEPROTOTYPE;
        case WSAENOPROTOOPT:    return kENOPROTOOPT;
        case WSAEPROTONOSUPPORT:return kEPROTONOSUPPORT;
        case WSAEAFNOSUPPORT:   return kEAFNOSUPPORT;
        case WSAEADDRINUSE:     return kEADDRINUSE;
        case WSAEADDRNOTAVAIL:  return kEADDRNOTAVAIL;
        case WSAENETDOWN:       return kENETDOWN;
        case WSAENETUNREACH:    return kENETUNREACH;
        case WSAENETRESET:      return kENETRESET;
        case WSAECONNABORTED:   return kECONNABORTED;
        case WSAECONNRESET:     return kECONNRESET;
        case WSAENOBUFS:        return kENOBUFS;
        case WSAEISCONN:        return kEISCONN;
        case WSAENOTCONN:       return kENOTCONN;
        case WSAESHUTDOWN:      return kESHUTDOWN;
        case WSAETIMEDOUT:      return kETIMEDOUT;
        case WSAECONNREFUSED:   return kECONNREFUSED;
        case WSAEHOSTUNREACH:   return kEHOSTUNREACH;
        case WSAEINTR:          return kEINTR;
        case WSAEBADF:          return kEBADF;
        case WSAEACCES:         return kEACCES;
        case WSAEFAULT:         return kEFAULT;
        case WSAEINVAL:         return kEINVAL;
        case WSAEMFILE:         return kEMFILE;
        default:                return kEINVAL;
    }
}
#endif

namespace {

void init_networking() {
    static bool initialized = false;
    if (!initialized) {
        initialized = true;
#if defined(_WIN32)
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    }
}

static bool get_host_socket(GuestCall &c, uint32_t token, SOCKET &out_sock) {
    std::lock_guard<std::mutex> lk(c.rt->files_lock);
    auto it = c.rt->host_sockets.find(token);
    if (it == c.rt->host_sockets.end()) {
        return false;
    }
    out_sock = (SOCKET)it->second;
    return true;
}

static std::string addr_to_str(const struct sockaddr_in &sa) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, (void*)&sa.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(sa.sin_port));
}

/* int socket(int domain, int type, int protocol) */
void s_socket(GuestCall &c) {
    init_networking();

    int domain = (int)c.arg(0);
    int type = (int)c.arg(1);
    int protocol = (int)c.arg(2);

    int base_type = type & 0xF;
    bool is_nonblock = (type & 0x800) != 0; // Linux SOCK_NONBLOCK

#if defined(_WIN32)
    SOCKET s = ::socket(domain, base_type, protocol);
    if (s == INVALID_SOCKET) {
        int werr = WSAGetLastError();
        c.set_errno(wsa_to_linux_errno(werr));
        c.set_result(kMinusOne);
        diag::report("[net] socket(d=%d, t=0x%x, p=%d) -> FAIL (wsa=%d)", domain, type, protocol, werr);
        return;
    }

    if (base_type == SOCK_DGRAM) {
        DWORD dwBytesReturned = 0;
        BOOL bNewBehavior = FALSE;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        WSAIoctl(s, SIO_UDP_CONNRESET, &bNewBehavior, sizeof(bNewBehavior), NULL, 0, &dwBytesReturned, NULL, NULL);
    }

    if (is_nonblock) {
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
    }

    std::lock_guard<std::mutex> lk(c.rt->files_lock);
    uint32_t token = c.rt->alloc_fd_token();
    c.rt->host_sockets[token] = (uintptr_t)s;
    if (is_nonblock) {
        c.rt->nonblocking_sockets.insert(token);
    }
    PVZTV_TRACE("[net] socket(d=%d, t=0x%x, p=%d) -> fd=%u (host=%llu%s)",
                domain, type, protocol, token, (unsigned long long)s, is_nonblock ? ", nonblock" : "");
    c.set_result(token);
#else
    int s = ::socket(domain, base_type, protocol);
    if (s < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    if (is_nonblock) {
        int cur = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, cur | O_NONBLOCK);
    }
    std::lock_guard<std::mutex> lk(c.rt->files_lock);
    uint32_t token = c.rt->alloc_fd_token();
    c.rt->host_sockets[token] = s;
    if (is_nonblock) {
        c.rt->nonblocking_sockets.insert(token);
    }
    c.set_result(token);
#endif
}

/* int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) */
void s_connect(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t addr_ptr = c.arg(1);
    uint32_t addrlen = c.arg(2);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(addr_ptr, sizeof(struct sockaddr_in))) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in sa{};
    std::memcpy(&sa, &c.img->mem[addr_ptr], sizeof(sa));
    std::string endpoint = addr_to_str(sa);

#if defined(_WIN32)
    int ret = ::connect(s, (const struct sockaddr*)&sa, sizeof(sa));
    if (ret < 0) {
        int werr = WSAGetLastError();
        uint32_t lerr = (werr == WSAEWOULDBLOCK) ? kEINPROGRESS : wsa_to_linux_errno(werr);
        c.set_errno(lerr);
        c.set_result(kMinusOne);
        diag::report("[net] connect(fd=%u, %s) -> ret=-1 (wsa=%d, errno=%u)", token, endpoint.c_str(), werr, lerr);
        return;
    }
    PVZTV_TRACE("[net] connect(fd=%u, %s) -> 0 (immediate)", token, endpoint.c_str());
    c.set_result(0);
#else
    int ret = ::connect((int)s, (const struct sockaddr*)&sa, sizeof(sa));
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#endif
}

/* int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) */
void s_bind(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t addr_ptr = c.arg(1);
    uint32_t addrlen = c.arg(2);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(addr_ptr, sizeof(struct sockaddr_in))) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in sa{};
    std::memcpy(&sa, &c.img->mem[addr_ptr], sizeof(sa));
    std::string endpoint = addr_to_str(sa);

#if defined(_WIN32)
    int ret = ::bind(s, (const struct sockaddr*)&sa, sizeof(sa));
    if (ret < 0) {
        int werr = WSAGetLastError();
        uint32_t lerr = wsa_to_linux_errno(werr);
        c.set_errno(lerr);
        c.set_result(kMinusOne);
        diag::report("[net] bind(fd=%u, %s) -> ret=-1 (wsa=%d, errno=%u)", token, endpoint.c_str(), werr, lerr);
        return;
    }
    PVZTV_TRACE("[net] bind(fd=%u, %s) -> 0", token, endpoint.c_str());
    c.set_result(0);
#else
    int ret = ::bind((int)s, (const struct sockaddr*)&sa, sizeof(sa));
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#endif
}

/* int listen(int sockfd, int backlog) */
void s_listen(GuestCall &c) {
    uint32_t token = c.arg(0);
    int backlog = (int)c.arg(1);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

#if defined(_WIN32)
    int ret = ::listen(s, backlog);
    if (ret < 0) {
        int werr = WSAGetLastError();
        uint32_t lerr = wsa_to_linux_errno(werr);
        c.set_errno(lerr);
        c.set_result(kMinusOne);
        diag::report("[net] listen(fd=%u, b=%d) -> ret=-1 (wsa=%d, errno=%u)", token, backlog, werr, lerr);
        return;
    }
    PVZTV_TRACE("[net] listen(fd=%u, b=%d) -> 0", token, backlog);
    c.set_result(0);
#else
    int ret = ::listen((int)s, backlog);
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#endif
}

/* int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) */
void s_accept(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t addr_ptr = c.arg(1);
    uint32_t addrlen_ptr = c.arg(2);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in client_sa{};
    int slen = sizeof(client_sa);

#if defined(_WIN32)
    SOCKET client = ::accept(s, (struct sockaddr*)&client_sa, &slen);
    if (client == INVALID_SOCKET) {
        int werr = WSAGetLastError();
        if (werr == WSAEWOULDBLOCK) {
            c.set_errno(kEWOULDBLOCK);
        } else {
            c.set_errno(wsa_to_linux_errno(werr));
            diag::report("[net] accept(fd=%u) -> ret=-1 (wsa=%d)", token, werr);
        }
        c.set_result(kMinusOne);
        return;
    }

    uint32_t newtok = 0;
    {
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        newtok = c.rt->alloc_fd_token();
        c.rt->host_sockets[newtok] = (uintptr_t)client;
    }

    if (addr_ptr != 0 && c.in_bounds(addr_ptr, sizeof(client_sa))) {
        std::memcpy(&c.img->mem[addr_ptr], &client_sa, sizeof(client_sa));
    }
    if (addrlen_ptr != 0 && c.in_bounds(addrlen_ptr, 4)) {
        c.write32(addrlen_ptr, sizeof(client_sa));
    }

    PVZTV_TRACE("[net] accept(fd=%u) -> new_client_fd=%u (%s)", token, newtok, addr_to_str(client_sa).c_str());
    c.set_result(newtok);
#else
    socklen_t ulen = sizeof(client_sa);
    int client = ::accept((int)s, (struct sockaddr*)&client_sa, &ulen);
    if (client < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }

    uint32_t newtok = 0;
    {
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        newtok = c.rt->alloc_fd_token();
        c.rt->host_sockets[newtok] = client;
    }

    if (addr_ptr != 0 && c.in_bounds(addr_ptr, sizeof(client_sa))) {
        std::memcpy(&c.img->mem[addr_ptr], &client_sa, sizeof(client_sa));
    }
    if (addrlen_ptr != 0 && c.in_bounds(addrlen_ptr, 4)) {
        c.write32(addrlen_ptr, sizeof(client_sa));
    }

    c.set_result(newtok);
#endif
}

/* ssize_t send(int sockfd, const void *buf, size_t len, int flags) */
void s_send(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t src = c.arg(1);
    uint32_t len = c.arg(2);
    int flags = (int)c.arg(3) & ~0x40; // Strip Linux MSG_DONTWAIT (0x40)

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(src, len)) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

#if defined(_WIN32)
    int n = ::send(s, (const char*)&c.img->mem[src], (int)len, flags);
    if (n < 0) {
        int werr = WSAGetLastError();
        if (werr == WSAEWOULDBLOCK || werr == WSAENOTCONN || werr == WSAEINPROGRESS) {
            c.set_errno(kEWOULDBLOCK);
        } else {
            c.set_errno(wsa_to_linux_errno(werr));
        }
        c.set_result(kMinusOne);
        return;
    }
    c.set_result((uint32_t)n);
#else
    ssize_t n = ::send((int)s, &c.img->mem[src], len, flags);
    if (n < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result((uint32_t)n);
#endif
}

/* ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
 *                const struct sockaddr *dest_addr, socklen_t addrlen) */
void s_sendto(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t src = c.arg(1);
    uint32_t len = c.arg(2);
    int flags = (int)c.arg(3) & ~0x40;
    uint32_t addr_ptr = c.arg(4);
    uint32_t addrlen = c.arg(5);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(src, len)) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in dest_sa{};
    const struct sockaddr *psa = nullptr;
    int slen = 0;
    if (addr_ptr != 0) {
        // Parenthesised so <windows.h>'s min macro cannot eat the call.
        size_t to_copy = (std::min)((size_t)addrlen, sizeof(dest_sa));
        if (to_copy == 0) to_copy = sizeof(dest_sa);
        if (c.in_bounds(addr_ptr, (uint32_t)to_copy)) {
            std::memcpy(&dest_sa, &c.img->mem[addr_ptr], to_copy);
            psa = (const struct sockaddr*)&dest_sa;
            slen = sizeof(dest_sa);
        }
    }

#if defined(_WIN32)
    int n = ::sendto(s, (const char*)&c.img->mem[src], (int)len, flags, psa, slen);
    if (n < 0) {
        int werr = WSAGetLastError();
        if (werr == WSAEWOULDBLOCK || werr == WSAENOTCONN || werr == WSAEINPROGRESS) {
            c.set_errno(kEWOULDBLOCK);
        } else {
            c.set_errno(wsa_to_linux_errno(werr));
            if (psa) {
                diag::report("[net] sendto(fd=%u, len=%u, %s) -> -1 (wsa=%d)",
                             token, len, addr_to_str(dest_sa).c_str(), werr);
            }
        }
        c.set_result(kMinusOne);
        return;
    }
    c.set_result((uint32_t)n);
#else
    ssize_t n = ::sendto((int)s, &c.img->mem[src], len, flags, psa, slen);
    if (n < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result((uint32_t)n);
#endif
}

/* ssize_t recv(int sockfd, void *buf, size_t len, int flags) */
void s_recv(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t dst = c.arg(1);
    uint32_t len = c.arg(2);
    int flags = (int)c.arg(3) & ~0x40; // Strip MSG_DONTWAIT

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(dst, len)) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

#if defined(_WIN32)
    int n = ::recv(s, (char*)&c.img->mem[dst], (int)len, flags);
    if (n < 0) {
        int werr = WSAGetLastError();
        if (werr == WSAEWOULDBLOCK || werr == WSAENOTCONN || werr == WSAEINPROGRESS) {
            c.set_errno(kEWOULDBLOCK);
        } else {
            c.set_errno(wsa_to_linux_errno(werr));
        }
        c.set_result(kMinusOne);
        return;
    }
    c.set_result((uint32_t)n);
#else
    ssize_t n = ::recv((int)s, &c.img->mem[dst], len, flags);
    if (n < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result((uint32_t)n);
#endif
}

/* ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
 *                  struct sockaddr *src_addr, socklen_t *addrlen) */
void s_recvfrom(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t dst = c.arg(1);
    uint32_t len = c.arg(2);
    int flags = (int)c.arg(3) & ~0x40;
    uint32_t addr_ptr = c.arg(4);
    uint32_t addrlen_ptr = c.arg(5);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(dst, len)) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in from_sa{};
    int slen = sizeof(from_sa);

#if defined(_WIN32)
    int n = ::recvfrom(s, (char*)&c.img->mem[dst], (int)len, flags,
                       addr_ptr ? (struct sockaddr*)&from_sa : nullptr,
                       addr_ptr ? &slen : nullptr);
    if (n < 0) {
        int werr = WSAGetLastError();
        if (werr == WSAEWOULDBLOCK || werr == WSAENOTCONN || werr == WSAEINPROGRESS) {
            c.set_errno(kEWOULDBLOCK);
        } else {
            c.set_errno(wsa_to_linux_errno(werr));
        }
        c.set_result(kMinusOne);
        return;
    }

    if (addr_ptr != 0 && c.in_bounds(addr_ptr, sizeof(from_sa))) {
        std::memcpy(&c.img->mem[addr_ptr], &from_sa, sizeof(from_sa));
    }
    if (addrlen_ptr != 0 && c.in_bounds(addrlen_ptr, 4)) {
        c.write32(addrlen_ptr, sizeof(from_sa));
    }

    c.set_result((uint32_t)n);
#else
    socklen_t ulen = sizeof(from_sa);
    ssize_t n = ::recvfrom((int)s, &c.img->mem[dst], len, flags,
                           addr_ptr ? (struct sockaddr*)&from_sa : nullptr,
                           addr_ptr ? &ulen : nullptr);
    if (n < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }

    if (addr_ptr != 0 && c.in_bounds(addr_ptr, sizeof(from_sa))) {
        std::memcpy(&c.img->mem[addr_ptr], &from_sa, sizeof(from_sa));
    }
    if (addrlen_ptr != 0 && c.in_bounds(addrlen_ptr, 4)) {
        c.write32(addrlen_ptr, sizeof(from_sa));
    }

    c.set_result((uint32_t)n);
#endif
}

/* int shutdown(int sockfd, int how) */
void s_shutdown(GuestCall &c) {
    uint32_t token = c.arg(0);
    int how = (int)c.arg(1);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

#if defined(_WIN32)
    int ret = ::shutdown(s, how);
    if (ret < 0) {
        c.set_errno(wsa_to_linux_errno(WSAGetLastError()));
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#else
    int ret = ::shutdown((int)s, how);
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#endif
}

/* int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) */
void s_getsockopt(GuestCall &c) {
    uint32_t token = c.arg(0);
    int level = (int)c.arg(1);
    int optname = (int)c.arg(2);
    uint32_t optval_ptr = c.arg(3);
    uint32_t optlen_ptr = c.arg(4);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

#if defined(_WIN32)
    int host_level = level;
    int host_optname = optname;

    if (level == 1 /* Linux SOL_SOCKET */) {
        host_level = SOL_SOCKET;
        switch (optname) {
            case 2: host_optname = SO_REUSEADDR; break;
            case 4: host_optname = SO_ERROR; break;
            case 6: host_optname = SO_BROADCAST; break;
            case 9: host_optname = SO_KEEPALIVE; break;
            case 15: /* Linux SO_REUSEPORT */
                if (optval_ptr != 0 && c.in_bounds(optval_ptr, 4)) c.write32(optval_ptr, 1);
                if (optlen_ptr != 0 && c.in_bounds(optlen_ptr, 4)) c.write32(optlen_ptr, 4);
                c.set_result(0);
                return;
            default: break;
        }
    }

    if (host_level == SOL_SOCKET && host_optname == SO_ERROR) {
        int werr = 0;
        int wlen = sizeof(werr);
        int res = ::getsockopt(s, host_level, host_optname, (char*)&werr, &wlen);
        if (res == 0) {
            int linux_err = wsa_to_linux_errno(werr);
            if (optval_ptr != 0 && c.in_bounds(optval_ptr, 4)) {
                c.write32(optval_ptr, linux_err);
            }
            if (optlen_ptr != 0 && c.in_bounds(optlen_ptr, 4)) {
                c.write32(optlen_ptr, 4);
            }
            diag::report("[net] getsockopt(fd=%u, SO_ERROR) -> wsa=%d (errno=%d)", token, werr, linux_err);
            c.set_result(0);
            return;
        }
        int werr_fail = WSAGetLastError();
        c.set_errno(wsa_to_linux_errno(werr_fail));
        c.set_result(kMinusOne);
        diag::report("[net] getsockopt(fd=%u, SO_ERROR) -> FAIL (wsa=%d)", token, werr_fail);
        return;
    }

    char buf[128]{};
    int len = sizeof(buf);
    int res = ::getsockopt(s, host_level, host_optname, buf, &len);
    if (res < 0) {
        c.set_errno(wsa_to_linux_errno(WSAGetLastError()));
        c.set_result(kMinusOne);
        return;
    }
    if (optval_ptr != 0 && c.in_bounds(optval_ptr, (uint32_t)len)) {
        std::memcpy(&c.img->mem[optval_ptr], buf, len);
    }
    if (optlen_ptr != 0 && c.in_bounds(optlen_ptr, 4)) {
        c.write32(optlen_ptr, (uint32_t)len);
    }
    c.set_result(0);
#else
    socklen_t len = 0;
    if (optlen_ptr != 0 && c.in_bounds(optlen_ptr, 4)) {
        len = (socklen_t)c.read32(optlen_ptr);
    }
    std::vector<char> buf(len > 0 ? len : 128);
    int res = ::getsockopt((int)s, level, optname, buf.data(), &len);
    if (res < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    if (optval_ptr != 0 && c.in_bounds(optval_ptr, len)) {
        std::memcpy(&c.img->mem[optval_ptr], buf.data(), len);
    }
    if (optlen_ptr != 0 && c.in_bounds(optlen_ptr, 4)) {
        c.write32(optlen_ptr, len);
    }
    c.set_result(0);
#endif
}

/* int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) */
void s_setsockopt(GuestCall &c) {
    uint32_t token = c.arg(0);
    int level = (int)c.arg(1);
    int optname = (int)c.arg(2);
    uint32_t optval_ptr = c.arg(3);
    uint32_t optlen = c.arg(4);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    if (!c.in_bounds(optval_ptr, optlen)) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }

#if defined(_WIN32)
    int host_level = level;
    int host_optname = optname;

    if (level == 1 /* Linux SOL_SOCKET */) {
        host_level = SOL_SOCKET;
        switch (optname) {
            case 2: host_optname = SO_REUSEADDR; break;
            case 6: host_optname = SO_BROADCAST; break;
            case 9: host_optname = SO_KEEPALIVE; break;
            case 15: /* Linux SO_REUSEPORT */
                // On Windows Winsock, SO_REUSEADDR covers port reuse and SO_REUSEPORT does not exist.
                c.set_result(0);
                return;
            default: break;
        }
    } else if (level == 6 /* Linux IPPROTO_TCP */) {
        host_level = IPPROTO_TCP;
        switch (optname) {
            case 1: host_optname = TCP_NODELAY; break;
            case 4: /* TCP_KEEPIDLE */
            case 5: /* TCP_KEEPINTVL */
            case 6: /* TCP_KEEPCNT */
                // On Windows TCP keepalive interval/count is configured via WSAIoctl,
                // so we safely accept and return 0
                c.set_result(0);
                return;
            default: break;
        }
    }

    int ret = ::setsockopt(s, host_level, host_optname, (const char*)&c.img->mem[optval_ptr], (int)optlen);
    if (ret < 0) {
        c.set_errno(wsa_to_linux_errno(WSAGetLastError()));
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#else
    int ret = ::setsockopt((int)s, level, optname, &c.img->mem[optval_ptr], optlen);
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    c.set_result(0);
#endif
}

/* int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) */
void s_getsockname(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t addr_ptr = c.arg(1);
    uint32_t addrlen_ptr = c.arg(2);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in sa{};
    int slen = sizeof(sa);

#if defined(_WIN32)
    int ret = ::getsockname(s, (struct sockaddr*)&sa, &slen);
    if (ret < 0) {
        c.set_errno(wsa_to_linux_errno(WSAGetLastError()));
        c.set_result(kMinusOne);
        return;
    }
#else
    socklen_t ulen = sizeof(sa);
    int ret = ::getsockname((int)s, (struct sockaddr*)&sa, &ulen);
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    slen = (int)ulen;
#endif

    if (addr_ptr != 0 && c.in_bounds(addr_ptr, sizeof(sa))) {
        std::memcpy(&c.img->mem[addr_ptr], &sa, sizeof(sa));
    }
    if (addrlen_ptr != 0 && c.in_bounds(addrlen_ptr, 4)) {
        c.write32(addrlen_ptr, (uint32_t)slen);
    }
    c.set_result(0);
}

/* int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) */
void s_getpeername(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t addr_ptr = c.arg(1);
    uint32_t addrlen_ptr = c.arg(2);

    SOCKET s = INVALID_SOCKET;
    if (!get_host_socket(c, token, s)) {
        c.set_errno(kEBADF);
        c.set_result(kMinusOne);
        return;
    }

    struct sockaddr_in sa{};
    int slen = sizeof(sa);

#if defined(_WIN32)
    int ret = ::getpeername(s, (struct sockaddr*)&sa, &slen);
    if (ret < 0) {
        c.set_errno(wsa_to_linux_errno(WSAGetLastError()));
        c.set_result(kMinusOne);
        return;
    }
#else
    socklen_t ulen = sizeof(sa);
    int ret = ::getpeername((int)s, (struct sockaddr*)&sa, &ulen);
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
    slen = (int)ulen;
#endif

    if (addr_ptr != 0 && c.in_bounds(addr_ptr, sizeof(sa))) {
        std::memcpy(&c.img->mem[addr_ptr], &sa, sizeof(sa));
    }
    if (addrlen_ptr != 0 && c.in_bounds(addrlen_ptr, 4)) {
        c.write32(addrlen_ptr, (uint32_t)slen);
    }
    c.set_result(0);
}

/* Linux ARM32 fd_set helpers */
static bool guest_fd_isset(uint32_t fd, const uint8_t *set_mem) {
    if (fd >= 1024) return false;
    uint32_t word = *(const uint32_t*)(set_mem + (fd / 32) * 4);
    return (word & (1u << (fd % 32))) != 0;
}

static void guest_fd_set(uint32_t fd, uint8_t *set_mem) {
    if (fd >= 1024) return;
    uint32_t *word_ptr = (uint32_t*)(set_mem + (fd / 32) * 4);
    *word_ptr |= (1u << (fd % 32));
}

/* select(nfds, readfds, writefds, exceptfds, timeout) */
void s_select(GuestCall &c) {
    uint32_t nfds = c.arg(0);
    uint32_t read_ptr = c.arg(1);
    uint32_t write_ptr = c.arg(2);
    uint32_t except_ptr = c.arg(3);
    uint32_t timeout_ptr = c.arg(4);

    if (nfds > 1024) nfds = 1024;

    uint32_t clear_len = ((nfds + 31) / 32) * 4;
    if (clear_len > 128) clear_len = 128;

    fd_set host_readfds, host_writefds, host_exceptfds;
    int max_host_fd = -1; // the host nfds: guest tokens and host fds are unrelated numbers
    FD_ZERO(&host_readfds);
    FD_ZERO(&host_writefds);
    FD_ZERO(&host_exceptfds);

    std::vector<uint32_t> tested_reads;
    std::vector<uint32_t> tested_writes;
    std::vector<uint32_t> tested_excepts;

    {
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        for (uint32_t i = 0; i < nfds; ++i) {
            auto it = c.rt->host_sockets.find(i);
            if (it == c.rt->host_sockets.end()) continue;
            SOCKET s = (SOCKET)it->second;
#if !defined(_WIN32)
            if ((int)s >= FD_SETSIZE) continue; // cannot be represented in an fd_set
            if ((int)s > max_host_fd) max_host_fd = (int)s;
#endif

            if (read_ptr != 0 && c.in_bounds(read_ptr, 128) && guest_fd_isset(i, &c.img->mem[read_ptr])) {
                FD_SET(s, &host_readfds);
                tested_reads.push_back(i);
            }
            if (write_ptr != 0 && c.in_bounds(write_ptr, 128) && guest_fd_isset(i, &c.img->mem[write_ptr])) {
                FD_SET(s, &host_writefds);
                tested_writes.push_back(i);
#if defined(_WIN32)
                // In Winsock, non-blocking connect failure is reported in exceptfds, NOT writefds.
                // In POSIX, connect completion (both success and failure) is reported in writefds.
                // We MUST monitor exceptfds for all writable-tested sockets!
                FD_SET(s, &host_exceptfds);
#endif
            }
            if (except_ptr != 0 && c.in_bounds(except_ptr, 128) && guest_fd_isset(i, &c.img->mem[except_ptr])) {
                FD_SET(s, &host_exceptfds);
                tested_excepts.push_back(i);
            }
        }
    }

    struct timeval tv;
    struct timeval *ptv = nullptr;
    if (timeout_ptr != 0 && c.in_bounds(timeout_ptr, 8)) {
        tv.tv_sec = (long)c.read32(timeout_ptr);
        tv.tv_usec = (long)c.read32(timeout_ptr + 4);
        ptv = &tv;
    }

    // Windows Winsock requires at least one socket in select().
    // If all sets are empty, POSIX uses select() as a portable sleep.
    if (tested_reads.empty() && tested_writes.empty() && tested_excepts.empty()) {
        if (ptv) {
            uint64_t ms = (uint64_t)ptv->tv_sec * 1000 + ptv->tv_usec / 1000;
            if (ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            } else {
                std::this_thread::yield();
            }
        } else {
            std::this_thread::yield();
        }
        if (read_ptr != 0 && c.in_bounds(read_ptr, clear_len)) {
            std::memset(&c.img->mem[read_ptr], 0, clear_len);
        }
        if (write_ptr != 0 && c.in_bounds(write_ptr, clear_len)) {
            std::memset(&c.img->mem[write_ptr], 0, clear_len);
        }
        if (except_ptr != 0 && c.in_bounds(except_ptr, clear_len)) {
            std::memset(&c.img->mem[except_ptr], 0, clear_len);
        }
        c.set_result(0);
        return;
    }

#if defined(_WIN32)
    int ret = ::select(0,
                       tested_reads.empty() ? nullptr : &host_readfds,
                       tested_writes.empty() ? nullptr : &host_writefds,
                       (tested_excepts.empty() && tested_writes.empty()) ? nullptr : &host_exceptfds,
                       ptv);
    if (ret < 0) {
        int werr = WSAGetLastError();
        c.set_errno(wsa_to_linux_errno(werr));
        c.set_result(kMinusOne);
        diag::report("[net] select(nfds=%u) -> ret=-1 (wsa=%d)", nfds, werr);
        return;
    }
#else
    int ret = ::select(max_host_fd + 1, &host_readfds, &host_writefds, &host_exceptfds, ptv);
    if (ret < 0) {
        c.set_errno(errno);
        c.set_result(kMinusOne);
        return;
    }
#endif

    uint32_t ready_count = 0;

    if (read_ptr != 0 && c.in_bounds(read_ptr, clear_len)) {
        std::memset(&c.img->mem[read_ptr], 0, clear_len);
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        for (uint32_t tok : tested_reads) {
            auto it = c.rt->host_sockets.find(tok);
            if (it != c.rt->host_sockets.end() && FD_ISSET((SOCKET)it->second, &host_readfds)) {
                guest_fd_set(tok, &c.img->mem[read_ptr]);
                ++ready_count;
            }
        }
    }
    if (write_ptr != 0 && c.in_bounds(write_ptr, clear_len)) {
        std::memset(&c.img->mem[write_ptr], 0, clear_len);
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        for (uint32_t tok : tested_writes) {
            auto it = c.rt->host_sockets.find(tok);
            if (it != c.rt->host_sockets.end()) {
                SOCKET s = (SOCKET)it->second;
                bool is_w = FD_ISSET(s, &host_writefds);
#if defined(_WIN32)
                bool is_e = FD_ISSET(s, &host_exceptfds);
                if (is_w || is_e) {
                    guest_fd_set(tok, &c.img->mem[write_ptr]);
                    ++ready_count;
                }
#else
                if (is_w) {
                    guest_fd_set(tok, &c.img->mem[write_ptr]);
                    ++ready_count;
                }
#endif
            }
        }
    }
    if (except_ptr != 0 && c.in_bounds(except_ptr, clear_len)) {
        std::memset(&c.img->mem[except_ptr], 0, clear_len);
        std::lock_guard<std::mutex> lk(c.rt->files_lock);
        for (uint32_t tok : tested_excepts) {
            auto it = c.rt->host_sockets.find(tok);
            if (it != c.rt->host_sockets.end() && FD_ISSET((SOCKET)it->second, &host_exceptfds)) {
                guest_fd_set(tok, &c.img->mem[except_ptr]);
                ++ready_count;
            }
        }
    }

    c.set_result(ready_count);
}

/* in_addr_t inet_addr(const char *cp) */
void s_inet_addr(GuestCall &c) {
    const std::string text = c.cstr(c.arg(0), 64);
    unsigned parts[4];
    char extra = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3],
                    &extra) != 4) {
        c.set_result(kMinusOne);
        return;
    }
    std::uint32_t addr = 0;
    for (int i = 0; i < 4; ++i) {
        if (parts[i] > 255) {
            c.set_result(kMinusOne);
            return;
        }
        addr |= (std::uint32_t)parts[i] << (8 * i);
    }
    c.set_result(addr);
}

/* int inet_pton(int af, const char *src, void *dst) */
void s_inet_pton(GuestCall &c) {
    constexpr std::uint32_t kAF_INET = 2;
    const std::uint32_t af = c.arg(0), dst = c.arg(2);
    if (af != kAF_INET) {
        c.set_errno(kEAFNOSUPPORT);
        c.set_result(kMinusOne);
        return;
    }
    const std::string text = c.cstr(c.arg(1), 64);
    unsigned parts[4];
    char extra = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3],
                    &extra) != 4) {
        c.set_result(0);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (parts[i] > 255) {
            c.set_result(0);
            return;
        }
    }
    if (dst != 0 && c.in_bounds(dst, 4)) {
        for (int i = 0; i < 4; ++i) c.write8(dst + (std::uint32_t)i, (std::uint8_t)parts[i]);
    }
    c.set_result(1);
}

/* const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) */
void s_inet_ntop(GuestCall &c) {
    constexpr std::uint32_t kAF_INET = 2;
    const std::uint32_t af = c.arg(0), src = c.arg(1), dst = c.arg(2), size = c.arg(3);
    if (af != kAF_INET) {
        c.set_errno(kEAFNOSUPPORT);
        c.set_result(0);
        return;
    }
    if (!c.in_bounds(src, 4)) {
        c.set_errno(kEFAULT);
        c.set_result(0);
        return;
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", c.read8(src), c.read8(src + 1),
                  c.read8(src + 2), c.read8(src + 3));
    if (dst == 0 || size < std::strlen(text) + 1 || !c.in_bounds(dst, (uint32_t)(std::strlen(text) + 1))) {
        c.set_errno(28 /* ENOSPC */);
        c.set_result(0);
        return;
    }
    c.put_cstr(dst, text);
    c.set_result(dst);
}

/* int gethostname(char *name, size_t len) */
void s_gethostname(GuestCall &c) {
    init_networking();
    const std::uint32_t buf = c.arg(0);
    const std::uint32_t len = c.arg(1);
    if (buf == 0 || len == 0 || !c.in_bounds(buf, len)) {
        c.set_errno(kEFAULT);
        c.set_result(kMinusOne);
        return;
    }
    std::vector<char> hname(len);
    if (::gethostname(hname.data(), (int)len) != 0) {
        c.put_cstr(buf, "localhost");
        c.set_result(0);
        return;
    }
    c.put_cstr(buf, hname.data());
    c.set_result(0);
}

/* struct hostent *gethostbyname(const char *name) */
void s_gethostbyname(GuestCall &c) {
    init_networking();
    std::string name = c.cstr(c.arg(0), 256);
    struct hostent *he = ::gethostbyname(name.c_str());
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        c.set_result(0);
        return;
    }

    // Allocate guest struct hostent in guest heap
    // struct hostent:
    //   h_name (4)
    //   h_aliases (4)
    //   h_addrtype (4)
    //   h_length (4)
    //   h_addr_list (4)
    static uint32_t guest_he = 0;
    static uint32_t guest_name = 0;
    static uint32_t guest_addr_list = 0;
    static uint32_t guest_addr = 0;

    if (guest_he == 0) {
        guest_he = c.rt->heap.alloc(32);
        guest_name = c.rt->heap.alloc(256);
        guest_addr_list = c.rt->heap.alloc(16);
        guest_addr = c.rt->heap.alloc(16);
    }

    c.put_cstr(guest_name, he->h_name ? he->h_name : name.c_str());
    std::memcpy(&c.img->mem[guest_addr], he->h_addr_list[0], 4);

    c.write32(guest_addr_list, guest_addr);
    c.write32(guest_addr_list + 4, 0);

    c.write32(guest_he + 0, guest_name);
    c.write32(guest_he + 4, 0); // h_aliases
    c.write32(guest_he + 8, 2); // AF_INET
    c.write32(guest_he + 12, 4); // length
    c.write32(guest_he + 16, guest_addr_list);

    c.set_result(guest_he);
}

void s_getaddrinfo(GuestCall &c) {
    init_networking();
    // Non-zero is failure for getaddrinfo
    if (c.arg(3) != 0 && c.in_bounds(c.arg(3), 4)) {
        c.write32(c.arg(3), 0);
    }
    c.set_result(4 /* EAI_FAIL */);
}

void s_getnameinfo(GuestCall &c) {
    c.set_result(4 /* EAI_FAIL */);
}

void s_freeaddrinfo(GuestCall &c) {}

void s_gai_strerror(GuestCall &c) {
    static std::uint32_t slot = 0;
    if (slot == 0) slot = c.rt->heap.alloc(64);
    c.put_cstr(slot, "Network resolution unavailable");
    c.set_result(slot);
}

void s_if_nametoindex(GuestCall &c) {
    c.set_result(0);
}

void s_socketpair(GuestCall &c) {
    c.set_errno(kEAFNOSUPPORT);
    c.set_result(kMinusOne);
}

struct NetIfInfo {
    std::string name;
    uint32_t ip;      // in network byte order
    uint32_t mask;    // in network byte order
    uint32_t bcast;   // in network byte order
    uint16_t flags;
};

static std::vector<NetIfInfo> query_host_interfaces() {
    std::vector<NetIfInfo> list;
    int wlan_idx = 0;
    int eth_idx = 0;

#if defined(_WIN32)
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = GetAdaptersAddresses(AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        ret = GetAdaptersAddresses(AF_INET, flags, NULL, (IP_ADAPTER_ADDRESSES*)buf.data(), &bufLen);
    }
    if (ret == NO_ERROR) {
        IP_ADAPTER_ADDRESSES* pCurr = (IP_ADAPTER_ADDRESSES*)buf.data();
        while (pCurr) {
            if (pCurr->OperStatus == IfOperStatusUp && pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                for (IP_ADAPTER_UNICAST_ADDRESS* pUni = pCurr->FirstUnicastAddress; pUni; pUni = pUni->Next) {
                    if (pUni->Address.lpSockaddr && pUni->Address.lpSockaddr->sa_family == AF_INET) {
                        sockaddr_in* sin = (sockaddr_in*)pUni->Address.lpSockaddr;
                        uint32_t ip = sin->sin_addr.s_addr;
                        if (ip == 0 || (ip & 0xFF) == 127) continue;

                        uint8_t prefix = pUni->OnLinkPrefixLength;
                        uint32_t mask = 0;
                        if (prefix > 0 && prefix <= 32) {
                            uint32_t host_mask = (prefix == 32) ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
                            mask = htonl(host_mask);
                        } else {
                            mask = htonl(0xFFFFFF00u);
                        }
                        uint32_t bcast = (ip & mask) | ~mask;

                        NetIfInfo info;
                        if (pCurr->IfType == IF_TYPE_IEEE80211) {
                            info.name = "wlan" + std::to_string(wlan_idx++);
                        } else {
                            info.name = "eth" + std::to_string(eth_idx++);
                        }
                        info.ip = ip;
                        info.mask = mask;
                        info.bcast = bcast;
                        // IFF_UP(1) | IFF_BROADCAST(2) | IFF_RUNNING(0x40) | IFF_MULTICAST(0x1000) = 0x1043
                        info.flags = 0x1043;
                        list.push_back(info);
                    }
                }
            }
            pCurr = pCurr->Next;
        }
    }
#else
    // getifaddrs is API 24+ while minSdk is 21, so resolve it at runtime.
    using getifaddrs_fn = int (*)(struct ifaddrs **);
    using freeifaddrs_fn = void (*)(struct ifaddrs *);
    static auto p_getifaddrs = (getifaddrs_fn)dlsym(RTLD_DEFAULT, "getifaddrs");
    static auto p_freeifaddrs = (freeifaddrs_fn)dlsym(RTLD_DEFAULT, "freeifaddrs");
    struct ifaddrs *ifa_list = nullptr;
    if (p_getifaddrs && p_freeifaddrs && p_getifaddrs(&ifa_list) == 0) {
        for (struct ifaddrs *ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
            if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
            uint32_t ip = ((sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
            if (ip == 0) continue;
            uint32_t mask = ifa->ifa_netmask ? ((sockaddr_in *)ifa->ifa_netmask)->sin_addr.s_addr
                                             : htonl(0xFFFFFF00u);
            NetIfInfo info;
            info.name = ifa->ifa_name ? ifa->ifa_name : "eth0";
            if (info.name.rfind("wlan", 0) == 0) ++wlan_idx;
            info.ip = ip;
            info.mask = mask;
            info.bcast = (ip & mask) | ~mask;
            info.flags = (uint16_t)(ifa->ifa_flags & 0xFFFF);
            // Real Wi-Fi first: Homura broadcasts on the first usable interface
            // and mobile data (rmnet/ccmni) cannot reach the LAN anyway.
            if (info.name.rfind("wlan", 0) == 0) list.insert(list.begin(), info);
            else list.push_back(info);
        }
        p_freeifaddrs(ifa_list);
    }
#endif

    if (list.empty()) {
        char hostname[256]{};
        uint32_t ip = 0;
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            struct hostent *he = gethostbyname(hostname);
            if (he && he->h_addr_list && he->h_addr_list[0]) {
                ip = *(uint32_t*)he->h_addr_list[0];
            }
        }
        if (ip == 0 || (ip & 0xFF) == 127) {
            ip = inet_addr("192.168.1.100");
        }
        uint32_t mask = inet_addr("255.255.255.0");
        uint32_t bcast = (ip & mask) | ~mask;
        NetIfInfo def_wlan;
        def_wlan.name = "wlan0";
        def_wlan.ip = ip;
        def_wlan.mask = mask;
        def_wlan.bcast = bcast;
        def_wlan.flags = 0x1043;
        list.push_back(def_wlan);
    } else if (wlan_idx == 0) {
        NetIfInfo alias = list.front();
        alias.name = "wlan0";
        list.insert(list.begin(), alias);
    }

    return list;
}

/* int ioctl(int fd, unsigned long request, ...) */
void s_ioctl(GuestCall &c) {
    uint32_t token = c.arg(0);
    uint32_t request = c.arg(1);
    uint32_t argp = c.arg(2);

    constexpr uint32_t kSIOCGIFCONF    = 0x8912;
    constexpr uint32_t kSIOCGIFFLAGS   = 0x8913;
    constexpr uint32_t kSIOCGIFADDR    = 0x8915;
    constexpr uint32_t kSIOCGIFBRDADDR = 0x8919;
    constexpr uint32_t kSIOCGIFNETMASK = 0x891b;
    constexpr uint32_t kFIONBIO        = 0x5421;

    switch (request) {
        case kSIOCGIFCONF: {
            if (argp == 0 || !c.in_bounds(argp, 8)) {
                c.set_errno(kEFAULT);
                c.set_result(kMinusOne);
                return;
            }
            int32_t ifc_len = (int32_t)c.read32(argp);
            uint32_t ifc_buf = c.read32(argp + 4);

            auto ifaces = query_host_interfaces();
            uint32_t total_needed = (uint32_t)(ifaces.size() * 32);

            if (ifc_buf == 0 || ifc_len <= 0) {
                c.write32(argp, total_needed);
                c.set_result(0);
                return;
            }

            uint32_t written = 0;
            for (size_t i = 0; i < ifaces.size(); ++i) {
                if ((uint32_t)(written + 32) > (uint32_t)ifc_len) break;
                uint32_t entry = ifc_buf + written;
                if (!c.in_bounds(entry, 32)) break;

                // Clear 32 bytes of ifreq
                std::memset(&c.img->mem[entry], 0, 32);

                // ifr_name (16 bytes)
                std::string ifname = ifaces[i].name;
                if (ifname.size() > 15) ifname.resize(15);
                std::memcpy(&c.img->mem[entry], ifname.c_str(), ifname.size() + 1);

                // ifr_addr (sockaddr_in at offset 16)
                c.write16(entry + 16, 2 /* AF_INET */);
                c.write16(entry + 18, 0 /* port */);
                c.write32(entry + 20, ifaces[i].ip);

                written += 32;
            }

            c.write32(argp, written);
            PVZTV_TRACE("[ioctl] SIOCGIFCONF returned %u bytes (%zu interfaces available)", written, ifaces.size());
            c.set_result(0);
            return;
        }

        case kSIOCGIFFLAGS: {
            if (argp == 0 || !c.in_bounds(argp, 32)) {
                c.set_errno(kEFAULT);
                c.set_result(kMinusOne);
                return;
            }
            char ifname[16]{};
            std::memcpy(ifname, &c.img->mem[argp], 15);
            ifname[15] = '\0';

            auto ifaces = query_host_interfaces();
            uint16_t flags = 0x1043; // default UP | BROADCAST | RUNNING | MULTICAST
            for (const auto &iface : ifaces) {
                if (iface.name == ifname) {
                    flags = iface.flags;
                    break;
                }
            }

            c.write16(argp + 16, flags);
            c.set_result(0);
            return;
        }

        case kSIOCGIFADDR: {
            if (argp == 0 || !c.in_bounds(argp, 32)) {
                c.set_errno(kEFAULT);
                c.set_result(kMinusOne);
                return;
            }
            char ifname[16]{};
            std::memcpy(ifname, &c.img->mem[argp], 15);
            ifname[15] = '\0';

            auto ifaces = query_host_interfaces();
            uint32_t ip = ifaces.empty() ? 0 : ifaces.front().ip;
            for (const auto &iface : ifaces) {
                if (iface.name == ifname) {
                    ip = iface.ip;
                    break;
                }
            }

            c.write16(argp + 16, 2 /* AF_INET */);
            c.write16(argp + 18, 0);
            c.write32(argp + 20, ip);
            std::memset(&c.img->mem[argp + 24], 0, 8);
            c.set_result(0);
            return;
        }

        case kSIOCGIFBRDADDR: {
            if (argp == 0 || !c.in_bounds(argp, 32)) {
                c.set_errno(kEFAULT);
                c.set_result(kMinusOne);
                return;
            }
            char ifname[16]{};
            std::memcpy(ifname, &c.img->mem[argp], 15);
            ifname[15] = '\0';

            auto ifaces = query_host_interfaces();
            uint32_t bcast = ifaces.empty() ? 0 : ifaces.front().bcast;
            for (const auto &iface : ifaces) {
                if (iface.name == ifname) {
                    bcast = iface.bcast;
                    break;
                }
            }

            c.write16(argp + 16, 2 /* AF_INET */);
            c.write16(argp + 18, 0);
            c.write32(argp + 20, bcast);
            std::memset(&c.img->mem[argp + 24], 0, 8);
            c.set_result(0);
            return;
        }

        case kSIOCGIFNETMASK: {
            if (argp == 0 || !c.in_bounds(argp, 32)) {
                c.set_errno(kEFAULT);
                c.set_result(kMinusOne);
                return;
            }
            char ifname[16]{};
            std::memcpy(ifname, &c.img->mem[argp], 15);
            ifname[15] = '\0';

            auto ifaces = query_host_interfaces();
            uint32_t mask = ifaces.empty() ? 0 : ifaces.front().mask;
            for (const auto &iface : ifaces) {
                if (iface.name == ifname) {
                    mask = iface.mask;
                    break;
                }
            }

            c.write16(argp + 16, 2 /* AF_INET */);
            c.write16(argp + 18, 0);
            c.write32(argp + 20, mask);
            std::memset(&c.img->mem[argp + 24], 0, 8);
            c.set_result(0);
            return;
        }

        case kFIONBIO: {
            SOCKET s = INVALID_SOCKET;
            if (!get_host_socket(c, token, s)) {
                c.set_errno(kEBADF);
                c.set_result(kMinusOne);
                return;
            }
            if (argp == 0 || !c.in_bounds(argp, 4)) {
                c.set_errno(kEFAULT);
                c.set_result(kMinusOne);
                return;
            }
            uint32_t val = c.read32(argp);
            u_long mode = (val != 0) ? 1 : 0;
#if defined(_WIN32)
            ioctlsocket(s, FIONBIO, &mode);
#else
            ioctl((int)s, FIONBIO, &mode);
#endif
            std::lock_guard<std::mutex> lk(c.rt->files_lock);
            if (mode != 0) {
                c.rt->nonblocking_sockets.insert(token);
            } else {
                c.rt->nonblocking_sockets.erase(token);
            }
            c.set_result(0);
            return;
        }

        default: {
            diag::report("[ioctl] unhandled request 0x%08X on token %u", request, token);
            c.set_errno(kENOTTY);
            c.set_result(kMinusOne);
            return;
        }
    }
}

}  // namespace

void register_libc_socket(ImportTable &t) {
    t.add("socket", s_socket);
    t.add("accept", s_accept);
    t.add("bind", s_bind);
    t.add("connect", s_connect);
    t.add("getsockname", s_getsockname);
    t.add("getpeername", s_getpeername);
    t.add("listen", s_listen);
    t.add("recv", s_recv);
    t.add("recvfrom", s_recvfrom);
    t.add("send", s_send);
    t.add("sendto", s_sendto);
    t.add("setsockopt", s_setsockopt);
    t.add("getsockopt", s_getsockopt);
    t.add("shutdown", s_shutdown);

    t.add("ioctl", s_ioctl);
    t.add("select", s_select);
    t.add("inet_addr", s_inet_addr);
    t.add("inet_pton", s_inet_pton);
    t.add("inet_ntop", s_inet_ntop);
    t.add("gethostname", s_gethostname);

    t.add("gethostbyname", s_gethostbyname);
    t.add("getaddrinfo", s_getaddrinfo);
    t.add("getnameinfo", s_getnameinfo);
    t.add("freeaddrinfo", s_freeaddrinfo);
    t.add("gai_strerror", s_gai_strerror);
    t.add("if_nametoindex", s_if_nametoindex);
    t.add("socketpair", s_socketpair);
}

}  // namespace pvz_tv
