#include "osc_lan.h"
#include <algorithm>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>

#ifdef _WIN32
// Link with -lws2_32 (MinGW) / Ws2_32.lib (MSVC)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
static const socket_t INVALID_FD = INVALID_SOCKET;
static void shutdown_socket(socket_t fd) { ::shutdown(fd, SD_BOTH); ::closesocket(fd); }
static bool set_nonblocking(socket_t fd, bool active) {
    u_long flag = active ? 1UL : 0UL;
    return ioctlsocket(fd, FIONBIO, &flag) == 0;
}
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <sys/time.h>
#  include <netdb.h>
#  include <netinet/tcp.h>
#  include <fcntl.h>
#  include <unistd.h>
#  include <errno.h>
using socket_t = int;
static const socket_t INVALID_FD = -1;
static void shutdown_socket(socket_t fd) { ::shutdown(fd, SHUT_RDWR); ::close(fd); }
static bool set_nonblocking(socket_t fd, bool active) {
    int flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) return false;
    return fcntl(fd, F_SETFL, active ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}
#endif

struct osclan::Impl {
    socket_t fd = INVALID_FD;
    std::vector<char> unread;
    size_t next = 0;
#ifdef _WIN32
    bool wsa_started = false;
#endif

    // A buffered read is necessary because TCP may merge multiple messages
    // or split the SCPI block header/payload at arbitrary offsets.
    int fill() {
        if(fd == INVALID_FD) return -1;
        char temp[16384];
        const int n = ::recv(fd, temp, sizeof(temp), 0);
        if(n <= 0) {
            // Diagnostic only: 10060=timeout, 10054=connection reset on Windows.
#ifdef _WIN32
            const int socket_error = (n < 0) ? WSAGetLastError() : 0;
            std::fprintf(stderr,
                         "DS2302 LAN-DIAG: recv() returned %d, WSA error=%d (10060=timeout, 10054=reset)\n",
                         n, socket_error);
#else
            std::fprintf(stderr, "DS2302 LAN-DIAG: recv() returned %d, errno=%d\n",
                         n, (n < 0) ? errno : 0);
#endif
            std::fflush(stderr);
            return n == 0 ? 0 : -1;
        }
        unread.assign(temp, temp + n);
        next = 0;
        return n;
    }
};

osclan *ocs = nullptr;

osclan::osclan() : p(new Impl()) {
#ifdef _WIN32
    WSADATA wsa;
    p->wsa_started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    if(!p->wsa_started) std::fprintf(stderr, "WSAStartup failed\n");
#endif
}

osclan::~osclan() {
    close();
#ifdef _WIN32
    if(p->wsa_started) WSACleanup();
#endif
    delete p;
}

int osclan::open(const char *host_or_ip) {
    close();
#ifdef _WIN32
    if(!p->wsa_started) return -1;
#endif
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *addresses = nullptr;
    if(getaddrinfo(host_or_ip, "5555", &hints, &addresses) != 0) {
        std::fprintf(stderr, "Could not resolve bridge host: %s\n", host_or_ip);
        return -1;
    }

    for(addrinfo *a = addresses; a; a = a->ai_next) {
        socket_t fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if(fd == INVALID_FD) continue;
        if(!set_nonblocking(fd, true)) { shutdown_socket(fd); continue; }
        int r = ::connect(fd, a->ai_addr, static_cast<int>(a->ai_addrlen));
        bool connected = (r == 0);
        if(!connected) {
#ifdef _WIN32
            const int err = WSAGetLastError();
            const bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
            const bool in_progress = (errno == EINPROGRESS);
#endif
            if(in_progress) {
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(fd, &wfds);
                timeval tv = { 5, 0 };
                r = ::select(static_cast<int>(fd) + 1, nullptr, &wfds, nullptr, &tv);
                if(r > 0) {
                    int errcode = 1;
#ifdef _WIN32
                    int len = sizeof(errcode);
#else
                    socklen_t len = sizeof(errcode);
#endif
                    connected = getsockopt(fd, SOL_SOCKET, SO_ERROR,
#ifdef _WIN32
                                           reinterpret_cast<char*>(&errcode),
#else
                                           &errcode,
#endif
                                           &len) == 0 && errcode == 0;
                }
            }
        }
        if(!connected || !set_nonblocking(fd, false)) { shutdown_socket(fd); continue; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
                   reinterpret_cast<const char*>(&one),
#else
                   &one,
#endif
                   sizeof(one));
#ifdef _WIN32
        DWORD timeout_ms = 10000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval timeout = { 10, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
        p->fd = fd;
        break;
    }
    freeaddrinfo(addresses);
    if(p->fd == INVALID_FD) {
        std::fprintf(stderr, "TCP connect to %s:5555 failed\n", host_or_ip);
        return -1;
    }
    std::fprintf(stderr, "TCP bridge connected: %s:5555\n", host_or_ip);
    return 0;
}

void osclan::close() {
    if(p->fd != INVALID_FD) {
        shutdown_socket(p->fd);
        p->fd = INVALID_FD;
    }
    p->unread.clear();
    p->next = 0;
}

int osclan::write(const char *data, int len) {
    if(p->fd == INVALID_FD || len < 0) return -1;
    int sent = 0;
    while(sent < len) {
        int n = ::send(p->fd, data + sent, len - sent, 0);
        if(n <= 0) return -1;
        sent += n;
    }
    return sent;
}

int osclan::read(char *data, int len) {
    if(len <= 0) return -1;
    if(p->next == p->unread.size()) {
        int n = p->fill();
        if(n <= 0) return n;
    }
    int n = std::min<int>(len, static_cast<int>(p->unread.size() - p->next));
    std::memcpy(data, p->unread.data() + p->next, n);
    p->next += n;
    return n;
}

int osclan::readExact(char *data, int len) {
    if(len < 0) return -1;
    int received = 0;
    while(received < len) {
        int n = read(data + received, len - received);
        if(n <= 0) {
            std::fprintf(stderr,
                         "DS2302 LAN-DIAG: readExact interrupted after %d/%d bytes; read returned %d\n",
                         received, len, n);
            std::fflush(stderr);
            return -1;
        }
        received += n;
    }
    return received;
}

int osclan::readLine(char *data, int max_len) {
    if(max_len <= 0) return -1;
    int got = 0;
    while(got < max_len) {
        if(p->next == p->unread.size()) {
            if(p->fill() <= 0) return -1;
        }
        auto begin = p->unread.begin() + static_cast<std::ptrdiff_t>(p->next);
        auto newline = std::find(begin, p->unread.end(), '\n');
        int to_copy = static_cast<int>(newline - begin);
        if(newline != p->unread.end()) ++to_copy;
        if(got + to_copy > max_len) return -1;
        std::memcpy(data + got, p->unread.data() + p->next, to_copy);
        p->next += to_copy;
        got += to_copy;
        if(newline != p->unread.end()) return got;
    }
    return -1; // maximum line length exceeded
}

void osclan::flush() {
    // Discard only already received/queued input, never send anything.
    p->unread.clear();
    p->next = 0;
    if(p->fd == INVALID_FD) return;
    while(true) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(p->fd, &rfds);
        timeval tv = {0, 0};
        int n = ::select(static_cast<int>(p->fd) + 1, &rfds, nullptr, nullptr, &tv);
        if(n <= 0) break;
        char scratch[8192];
        if(::recv(p->fd, scratch, sizeof(scratch), 0) <= 0) break;
    }
}
