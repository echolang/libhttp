/*
 * Process CPU time and resident memory, for the load benchmarks in
 * tests/load.eco. Test target only.
 */

#define _DEFAULT_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

/* user plus system time of the whole process, in microseconds */
int64_t http_bench_cpu_us(void)
{
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return -1;
    }

    return (int64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + (int64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
}

/* resident set size right now, in KiB */
int64_t http_bench_rss_kb(void)
{
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;

    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) {
        return -1;
    }

    return (int64_t)(info.resident_size / 1024);
#else
    long pages = 0;
    long resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");

    if (f == NULL) {
        return -1;
    }

    if (fscanf(f, "%ld %ld", &pages, &resident) != 2) {
        resident = -1;
    }

    fclose(f);
    return resident < 0 ? -1 : (int64_t)resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>

/*
 * A blocking loopback connection with a receive buffer of `bytes`, set
 * before the connect so the window it offers is small from the start: a
 * slow reader the kernel cannot take a whole big response off the server
 * for. The fd, or -1.
 */
int http_bench_dial(int port, int bytes)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

#ifdef SO_NOSIGPIPE
    {
        int one = 1;

        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    }
#endif

    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes) != 0) {
        close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}
