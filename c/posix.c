/*
 * POSIX sockets for libhttp. The per-platform bits Echo should not spell:
 * sockaddr_in, getaddrinfo, kqueue / epoll, errno.
 *
 * Every fd is CLOEXEC. Accepted sockets stay blocking (workers write
 * under SO_SNDTIMEO). The reactor reads with MSG_DONTWAIT. Listen and
 * the wake pipe are non-blocking. A response the reactor finishes for a
 * slow client is written with MSG_DONTWAIT on linux and the BSDs; darwin
 * ignores that flag for sends, so there the socket is O_NONBLOCK while the
 * reactor has it, and blocking again after. A file it finishes always goes
 * O_NONBLOCK: sendfile takes no flags.
 *
 * The lingering close: shut write, drain, then close. A plain close with
 * unread bytes is a RST. The reactor does it without blocking, with
 * http_shut_write and http_drain_now; http_finish is the blocking one, for
 * when there is no reactor left to hand the fd to.
 */

#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

/* glibc hides syscall(2), which the futex needs, under a bare _POSIX_C_SOURCE */
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define HTTP_KQUEUE 1
#else
#include <sys/epoll.h>
#endif

#if defined(__APPLE__)
#include <os/os_sync_wait_on_address.h>
#include <stdint.h>

/* before macOS 14.4 the same wait has only its private spelling */
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
/* <sys/socket.h> hides it under _POSIX_C_SOURCE, which the rest of this file wants.
   the struct is declared first so the prototype does not depend on _DARWIN_C_SOURCE */
struct sf_hdtr;
extern int sendfile(int, int, off_t, off_t *, struct sf_hdtr *, int);

#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/sendfile.h>
#include <sys/syscall.h>
#endif

#define DRAIN_MS 2000
#define IO_TIMEOUT_SECS 10
#define RECV_AGAIN (-3)
#define DRAIN_CAP (4 * 1024 * 1024)
#define DRAIN_STEP (256 * 1024)

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

/*
 * Binds and listens with room for `backlog` connections the kernel has
 * accepted and nobody has taken yet. The kernel caps it at its own limit
 * (somaxconn) without saying so.
 */
int http_listen(const char *host, int port, int backlog)
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

    if (backlog < 1) {
        backlog = 1;
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

        if (listen(fd, backlog) != 0) {
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

/*
 * An errno that says sendfile cannot serve this file or socket at all: a
 * filesystem without it (ENOTSUP on an SMB or NFS mount on macOS, EINVAL
 * on some FUSE and special filesystems on linux), or a kernel without it.
 */
static int sendfile_unsupported(int err)
{
    return err == EINVAL || err == ENOSYS || err == ENOTSUP || err == EOPNOTSUPP;
}

/*
 * Up to `count` bytes of the file `file` from `offset` onto the socket
 * `sock`, in the kernel: no copy through user space. The file's own offset
 * does not move. Answers how many went (a blocking socket under its send
 * timeout may take only part), 0 when the file ended first, RECV_AGAIN when
 * the socket took nothing before its timeout, -1 on an error, and -2 where
 * sendfile cannot serve this file (no sendfile here, or a filesystem
 * without it), so the caller copies instead.
 *
 * linux raises SIGPIPE for a socket the peer has closed, and sendfile has no
 * MSG_NOSIGNAL: the signal is blocked around the call, and one it raised is
 * taken off the thread before it is unblocked. One already pending before
 * the call is someone else's and is left alone, raised one or not: taking
 * it would hide it from the application. linux takes a thread's own signal
 * before a process-wide one, so the one taken is the one this call raised.
 * macOS honours SO_NOSIGPIPE.
 */
ssize_t http_sendfile(int sock, int file, long long offset, size_t count)
{
#if defined(__linux__)
    sigset_t pipe_set;
    sigset_t old;
    sigset_t pending;
    off_t off = (off_t)offset;
    ssize_t put;
    int err;
    int had;

    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old);

    had = sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE);

    do {
        put = sendfile(sock, file, &off, count);
    } while (put < 0 && errno == EINTR);

    err = errno;

    if (put < 0 && err == EPIPE && !had && !sigismember(&old, SIGPIPE)) {
        struct timespec zero = {0, 0};

        while (sigtimedwait(&pipe_set, NULL, &zero) < 0 && errno == EINTR) {
        }
    }

    pthread_sigmask(SIG_SETMASK, &old, NULL);
    errno = err;

    if (put < 0 && would_block()) {
        return RECV_AGAIN;
    }

    if (put < 0 && sendfile_unsupported(err)) {
        return -2;
    }

    return put;
#elif defined(__APPLE__)
    off_t len = (off_t)count;
    int rc;

    for (;;) {
        len = (off_t)count;
        rc = sendfile(file, sock, (off_t)offset, &len, NULL, 0);

        // a timeout or a signal still says how much went
        if (rc == 0 || len > 0) {
            return (ssize_t)len;
        }

        if (errno != EINTR) {
            break;
        }
    }

    if (would_block()) {
        return RECV_AGAIN;
    }

    if (sendfile_unsupported(errno)) {
        return -2;
    }

    return -1;
#else
    (void)sock;
    (void)file;
    (void)offset;
    (void)count;
    errno = ENOSYS;
    return -2;
#endif
}

/*
 * Whether MSG_DONTWAIT makes a send on a blocking socket non-blocking.
 * linux and the BSDs: yes. darwin honours it for receives only; a send
 * waits for room anyway, so a send that must not wait needs O_NONBLOCK on
 * the socket.
 */
int http_dontwait_sends(void)
{
#if defined(__APPLE__)
    return 0;
#else
    return 1;
#endif
}

/*
 * A second fd for the same open file, CLOEXEC, or -1. A half-sent file goes
 * to the reactor as this copy: the worker's own handle stays the worker's,
 * so no reference count is shared between threads.
 */
int http_dup(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}

/*
 * O_NONBLOCK on (`on` 0) or off (`on` 1), the way http_accept_one leaves it
 * off. One FIONBIO ioctl rather than fcntl's read and write of the flags:
 * a worker does this twice for every big response, and a handed-over
 * connection coming back is made blocking again this way. The send and
 * receive timeouts and SO_NOSIGPIPE are left as accept set them: a taker
 * must not change them (setting them again on every resume cost three
 * syscalls a request on a proxy's path).
 */
int http_set_blocking(int fd, int on)
{
    int nb = on ? 0 : 1;

    return ioctl(fd, FIONBIO, &nb);
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

/*
 * Arms `fd` for writing, once: reported when the socket can take more, then
 * left disabled until armed again. On epoll this replaces the read interest
 * (one registration per fd); on kqueue it is a filter of its own beside a
 * read filter that stays as it was. Either way, a connection the reactor is
 * writing to is armed for nothing else.
 */
int http_ev_arm_write(int ev, int fd)
{
#ifdef HTTP_KQUEUE
    struct kevent change;

    EV_SET(&change, (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, NULL);
    return kevent(ev, &change, 1, NULL, 0, NULL) < 0 ? -1 : 0;
#else
    struct epoll_event event;

    memset(&event, 0, sizeof event);
    event.events = EPOLLOUT | EPOLLONESHOT;
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

/*
 * Adds the listen socket, shared by every reactor, level-triggered. On
 * linux with EPOLLEXCLUSIVE, so a new connection wakes one waiting reactor
 * instead of all of them; a kernel older than 4.5 refuses the flag and gets
 * the plain arm. kqueue has no such flag: each kqueue that holds the socket
 * hears of every connection, and the reactors that lose the race to accept
 * it see EAGAIN. Arming one already armed is not an error.
 */
int http_ev_listen(int ev, int fd)
{
#ifdef HTTP_KQUEUE
    return http_ev_arm(ev, fd, 0);
#else
    struct epoll_event event;

    memset(&event, 0, sizeof event);
#ifdef EPOLLEXCLUSIVE
    event.events = EPOLLIN | EPOLLEXCLUSIVE;
    event.data.fd = fd;

    if (epoll_ctl(ev, EPOLL_CTL_ADD, fd, &event) == 0 || errno == EEXIST) {
        return 0;
    }

    if (errno != EINVAL) {
        return -1;
    }
#endif
    event.events = EPOLLIN;
    event.data.fd = fd;

    if (epoll_ctl(ev, EPOLL_CTL_ADD, fd, &event) == 0 || errno == EEXIST) {
        return 0;
    }

    return -1;
#endif
}

int http_ev_drop(int ev, int fd)
{
#ifdef HTTP_KQUEUE
    struct kevent changes[2];
    struct kevent results[2];
    int n;
    int i;

    // both filters: http_ev_arm_write leaves a write one behind, disabled,
    // that must not stay with an fd leaving this reactor. EV_RECEIPT answers
    // each one, so a filter that was never added (ENOENT) stops neither
    EV_SET(&changes[0], (uintptr_t)fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, NULL);
    EV_SET(&changes[1], (uintptr_t)fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, NULL);

    n = kevent(ev, changes, 2, results, 2, NULL);
    if (n < 0) {
        return -1;
    }

    for (i = 0; i < n; i++) {
        if ((results[i].flags & EV_ERROR) && results[i].data != 0 && results[i].data != ENOENT) {
            return -1;
        }
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

int http_shut_write(int fd)
{
    return shutdown(fd, SHUT_WR);
}

/*
 * Both directions shut, the fd still open: a thread blocked on it wakes, and
 * nothing else can be handed the same number while it does. A tunnel's close.
 */
int http_shut_both(int fd)
{
    return shutdown(fd, SHUT_RDWR);
}

/*
 * Waits up to ms for fd to be readable (events 1) or writable (2): 1 when it
 * is, or has hung up or failed so the next call says how, 0 when the time ran
 * out, -1 on an error of poll itself.
 */
int http_wait(int fd, int events, int ms)
{
    struct pollfd pfd;
    struct timespec start;
    int rc;

    pfd.fd = fd;
    pfd.events = (short)((events & 1 ? POLLIN : 0) | (events & 2 ? POLLOUT : 0));
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        long left = ms - elapsed_ms(&start);

        if (left < 0) {
            left = 0;
        }

        pfd.revents = 0;
        rc = poll(&pfd, 1, (int)left);

        if (rc < 0 && errno == EINTR) {
            continue;
        }

        if (rc < 0) {
            return -1;
        }

        return rc > 0 ? 1 : 0;
    }
}

/*
 * Reads and drops what has arrived, without waiting. The count dropped
 * while the peer is still there, or -1 once it has hung up or the socket
 * broke. Stops after DRAIN_STEP so one sender cannot hold the reactor.
 */
long http_drain_now(int fd)
{
    char buf[16384];
    long drained = 0;

    while (drained < DRAIN_STEP) {
        ssize_t got = recv(fd, buf, sizeof buf, MSG_DONTWAIT);

        if (got < 0 && errno == EINTR) {
            continue;
        }

        if (got < 0 && would_block()) {
            return drained;
        }

        if (got <= 0) {
            return -1;
        }

        drained += (long)got;
    }

    return drained;
}

int http_is_fd_limit(void)
{
    return errno == EMFILE || errno == ENFILE;
}

int http_is_badfd(void)
{
    return errno == EBADF;
}

/*
 * A word threads sleep on. `http_futex_wait` sleeps only while `*word` still
 * holds `seen`, and a wake after a bump can never be missed: the check and
 * the sleep are one step in the kernel. What a mutex and condition give,
 * without the mutex every waker and sleeper would queue on.
 */
void http_futex_wait(int *word, int seen)
{
#if defined(__APPLE__)
    if (__builtin_available(macOS 14.4, *)) {
        os_sync_wait_on_address(word, (uint64_t)(uint32_t)seen, sizeof(int), OS_SYNC_WAIT_ON_ADDRESS_NONE);
    } else {
        __ulock_wait(UL_COMPARE_AND_WAIT, word, (uint64_t)(uint32_t)seen, 0);
    }
#elif defined(__linux__)
    syscall(SYS_futex, word, FUTEX_WAIT_PRIVATE, seen, NULL, NULL, 0);
#else
    (void)word;
    (void)seen;
#endif
}

void http_futex_wake(int *word, int all)
{
#if defined(__APPLE__)
    if (__builtin_available(macOS 14.4, *)) {
        if (all) {
            os_sync_wake_by_address_all(word, sizeof(int), OS_SYNC_WAKE_BY_ADDRESS_NONE);
        } else {
            os_sync_wake_by_address_any(word, sizeof(int), OS_SYNC_WAKE_BY_ADDRESS_NONE);
        }
    } else {
        __ulock_wake(UL_COMPARE_AND_WAIT | (all ? ULF_WAKE_ALL : 0), word, 0);
    }
#elif defined(__linux__)
    syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, all ? 0x7fffffff : 1, NULL, NULL, 0);
#else
    (void)word;
    (void)all;
#endif
}

/*
 * One beat of a spin-wait: tells the core this is a busy loop, without
 * the syscall sched_yield would be.
 */
void http_cpu_relax(void)
{
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ volatile("isb" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause" ::: "memory");
#endif
}

const char *http_errstr(void)
{
    const char *s = strerror(errno);

    snprintf(g_err, sizeof g_err, "%s", s != NULL ? s : "unknown");
    return g_err;
}
