/*
 * POSIX sockets for libhttp. The per-platform bits Echo should not spell:
 * sockaddr_in, getaddrinfo, kqueue / epoll, errno.
 *
 * Every fd is CLOEXEC. Accepted sockets stay blocking (workers write
 * under SO_SNDTIMEO). The reactor reads with MSG_DONTWAIT. Listen and
 * the wake pipe are non-blocking.
 *
 * http_finish is the lingering close: shut write, drain, then close.
 * A plain close with unread bytes is a RST.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define HTTP_KQUEUE 1
#else
#include <sys/epoll.h>
#endif

#define DRAIN_MS 2000
#define IO_TIMEOUT_SECS 10
#define RECV_AGAIN (-3)
#define DRAIN_CAP (4 * 1024 * 1024)

static __thread char g_err[128];

static int cloexec(int fd)
{
    int flags;

    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }

    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;

    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
    return 0;
#endif
}

static int iotimeout(int fd)
{
    struct timeval tv;

    tv.tv_sec = IO_TIMEOUT_SECS;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        return -1;
    }

    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

static int reuseaddr(int fd)
{
    int one = 1;

    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
}

static int blocking(int fd, int on)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags < 0) {
        return -1;
    }

    if (on) {
        flags &= ~O_NONBLOCK;
    } else {
        flags |= O_NONBLOCK;
    }

    return fcntl(fd, F_SETFL, flags);
}

/*
 * A keep-alive response is often a head and a body in two sends. With Nagle
 * on, the second waits for the client's delayed ACK: 40ms per request.
 */
static int nodelay(int fd)
{
    int one = 1;

    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

static int would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

int http_listen(const char *host, int port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    char portstr[16];
    int fd = -1;
    int rc;

    if (port < 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    snprintf(portstr, sizeof portstr, "%d", port);

    if (host != NULL && host[0] == '\0') {
        host = NULL;
    }

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        errno = EINVAL;
        return -1;
    }

    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (cloexec(fd) != 0 || reuseaddr(fd) != 0 || blocking(fd, 0) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (bind(fd, it->ai_addr, it->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (listen(fd, 128) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        break;
    }

    freeaddrinfo(res);

    if (fd < 0 && errno == 0) {
        errno = EADDRNOTAVAIL;
    }

    return fd;
}

int http_port(int fd)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;

    memset(&addr, 0, sizeof addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        return -1;
    }

    return (int)ntohs(addr.sin_port);
}

int http_pipe_open(int *rd, int *wr)
{
    int fds[2];

    if (rd == NULL || wr == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(fds) != 0) {
        return -1;
    }

    if (cloexec(fds[0]) != 0 || cloexec(fds[1]) != 0 || blocking(fds[0], 0) != 0 || blocking(fds[1], 0) != 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    *rd = fds[0];
    *wr = fds[1];
    return 0;
}

int http_wake(int wr)
{
    char b = 1;
    ssize_t n;

    do {
        n = write(wr, &b, 1);
    } while (n < 0 && errno == EINTR);

    // a full pipe already holds a wake the reactor has not read
    if (n < 0 && !would_block()) {
        return -1;
    }

    return 0;
}

int http_drain(int rd)
{
    char buf[256];
    ssize_t n;

    for (;;) {
        n = read(rd, buf, sizeof buf);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return 0;
        }
    }
}

/*
 * One pending connection off a non-blocking listen socket, or RECV_AGAIN when
 * there is none left. The accepted socket is made blocking explicitly: BSD
 * hands O_NONBLOCK down from the listen socket, linux does not.
 */
int http_accept_one(int listen_fd, char *peer, int peerlen)
{
    int fd;
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    char ip[INET_ADDRSTRLEN];

    if (peer != NULL && peerlen > 0) {
        peer[0] = '\0';
    }

    memset(&addr, 0, sizeof addr);

    do {
        fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
    } while (fd < 0 && errno == EINTR);

    if (fd < 0) {
        if (would_block() || errno == ECONNABORTED) {
            return RECV_AGAIN;
        }

        return -1;
    }

    if (cloexec(fd) != 0 || blocking(fd, 1) != 0) {
        close(fd);
        return RECV_AGAIN;
    }

    (void)nosigpipe(fd);
    (void)iotimeout(fd);
    (void)nodelay(fd);

    if (peer != NULL && peerlen > 0) {
        if (inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip) == NULL) {
            snprintf(peer, (size_t)peerlen, "0.0.0.0:0");
        } else {
            snprintf(peer, (size_t)peerlen, "%s:%u", ip, (unsigned)ntohs(addr.sin_port));
        }
    }

    return fd;
}

int http_connect(const char *host, int port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it;
    char portstr[16];
    int fd = -1;
    int rc;

    if (host == NULL || port < 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof portstr, "%d", port);

    rc = getaddrinfo(host, portstr, &hints, &res);
    if (rc != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    for (it = res; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }

        if (cloexec(fd) != 0 || nosigpipe(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        (void)iotimeout(fd);

        do {
            rc = connect(fd, it->ai_addr, it->ai_addrlen);
        } while (rc != 0 && errno == EINTR);

        if (rc != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        break;
    }

    freeaddrinfo(res);
    return fd;
}

ssize_t http_recv(int fd, void *buf, size_t n)
{
    ssize_t got;

    do {
        got = recv(fd, buf, n, 0);
    } while (got < 0 && errno == EINTR);

    return got;
}

static long elapsed_ms(const struct timespec *from);

ssize_t http_send(int fd, const void *buf, size_t n)
{
    ssize_t put;
    int flags = 0;

#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif

    do {
        put = send(fd, buf, n, flags);
    } while (put < 0 && errno == EINTR);

    return put;
}

ssize_t http_recv_now(int fd, void *buf, size_t n)
{
    ssize_t got;

    do {
        got = recv(fd, buf, n, MSG_DONTWAIT);
    } while (got < 0 && errno == EINTR);

    if (got < 0 && would_block()) {
        return RECV_AGAIN;
    }

    return got;
}

ssize_t http_send_now(int fd, const void *buf, size_t n)
{
    ssize_t put;
    int flags = MSG_DONTWAIT;

#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    do {
        put = send(fd, buf, n, flags);
    } while (put < 0 && errno == EINTR);

    if (put < 0 && would_block()) {
        return RECV_AGAIN;
    }

    return put;
}

int http_ev_open(void)
{
    int ev;

#ifdef HTTP_KQUEUE
    ev = kqueue();
#else
    ev = epoll_create1(EPOLL_CLOEXEC);
#endif

    if (ev >= 0 && cloexec(ev) != 0) {
        close(ev);
        return -1;
    }

    return ev;
}

/*
 * Adds `fd` for reading, or re-arms it. `oneshot` reports it once and then
 * leaves it disabled until the next arm.
 */
int http_ev_arm(int ev, int fd, int oneshot)
{
#ifdef HTTP_KQUEUE
    struct kevent change;
    unsigned short flags = EV_ADD | EV_ENABLE;

    if (oneshot) {
        flags |= EV_DISPATCH;
    }

    EV_SET(&change, (uintptr_t)fd, EVFILT_READ, flags, 0, 0, NULL);
    return kevent(ev, &change, 1, NULL, 0, NULL) < 0 ? -1 : 0;
#else
    struct epoll_event event;

    memset(&event, 0, sizeof event);
    event.events = EPOLLIN | (oneshot ? EPOLLONESHOT : 0);
    event.data.fd = fd;

    if (epoll_ctl(ev, EPOLL_CTL_MOD, fd, &event) == 0) {
        return 0;
    }

    if (errno != ENOENT) {
        return -1;
    }

    return epoll_ctl(ev, EPOLL_CTL_ADD, fd, &event);
#endif
}

int http_ev_drop(int ev, int fd)
{
#ifdef HTTP_KQUEUE
    struct kevent change;

    EV_SET(&change, (uintptr_t)fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    if (kevent(ev, &change, 1, NULL, 0, NULL) < 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
#else
    struct epoll_event event;

    memset(&event, 0, sizeof event);
    if (epoll_ctl(ev, EPOLL_CTL_DEL, fd, &event) != 0 && errno != ENOENT) {
        return -1;
    }

    return 0;
#endif
}

/*
 * Waits at most `ms` (negative is forever) and writes the ready fds into
 * `fds`. Answers how many, 0 on a timeout or a signal, -1 on error.
 */
int http_ev_wait(int ev, int *fds, int max, int ms)
{
    int n;
    int i;

    if (max <= 0) {
        return 0;
    }

    if (max > 256) {
        max = 256;
    }

#ifdef HTTP_KQUEUE
    struct kevent events[256];
    struct timespec ts;
    struct timespec *tsp = NULL;

    if (ms >= 0) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        tsp = &ts;
    }

    n = kevent(ev, NULL, 0, events, max, tsp);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }

    for (i = 0; i < n; i++) {
        fds[i] = (int)events[i].ident;
    }
#else
    struct epoll_event events[256];

    n = epoll_wait(ev, events, max, ms);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }

    for (i = 0; i < n; i++) {
        fds[i] = events[i].data.fd;
    }
#endif

    return n;
}

int http_close(int fd)
{
    if (fd < 0) {
        return 0;
    }

    return close(fd);
}

static long elapsed_ms(const struct timespec *from)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)(now.tv_sec - from->tv_sec) * 1000 + (now.tv_nsec - from->tv_nsec) / 1000000;
}

int http_finish(int fd)
{
    char buf[4096];
    size_t drained = 0;
    struct timespec start;

    if (fd < 0) {
        return 0;
    }

    if (shutdown(fd, SHUT_WR) != 0) {
        return close(fd);
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    while (drained < DRAIN_CAP) {
        struct pollfd pfd;
        long left = DRAIN_MS - elapsed_ms(&start);
        int rc;
        ssize_t got;

        if (left <= 0) {
            break;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        rc = poll(&pfd, 1, (int)left);
        if (rc < 0 && errno == EINTR) {
            continue;
        }

        if (rc <= 0) {
            break;
        }

        got = recv(fd, buf, sizeof buf, 0);
        if (got < 0 && errno == EINTR) {
            continue;
        }

        if (got <= 0) {
            break;
        }

        drained += (size_t)got;
    }

    return close(fd);
}

int http_is_fd_limit(void)
{
    return errno == EMFILE || errno == ENFILE;
}

int http_is_badfd(void)
{
    return errno == EBADF;
}

const char *http_errstr(void)
{
    const char *s = strerror(errno);

    snprintf(g_err, sizeof g_err, "%s", s != NULL ? s : "unknown");
    return g_err;
}
