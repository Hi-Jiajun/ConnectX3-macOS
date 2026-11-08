/*
 * cx3-phase-watch -- persistent off-machine sampler for Cx3Mlx4's phase properties.
 *
 * The shell watcher this replaces started `ioreg` for every sample. Under the load produced by
 * the CX3 bring-up chain that fell behind by about three seconds and missed the fatal selector
 * call entirely. This process resolves the service, reads six registry properties directly,
 * samples at 10 ms, sends every change over UDP immediately, and reports partial/stale reads
 * instead of turning a missing property into a plausible zero.
 */

#include <arpa/inet.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <pthread/qos.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint64_t call;
    uint64_t phase;
    uint64_t step;
    uint64_t hook;
    uint64_t worker;
    uint64_t open_phase;
} Sample;

enum {
    kPropCall   = 1u << 0,
    kPropPhase  = 1u << 1,
    kPropStep   = 1u << 2,
    kPropHook   = 1u << 3,
    kPropWorker = 1u << 4,
    kPropOpen   = 1u << 5,
    kPropAll    = kPropCall | kPropPhase | kPropStep | kPropHook | kPropWorker | kPropOpen
};

static volatile sig_atomic_t g_stop;
static unsigned long g_udp_errors;
static bool g_udp_error_reported;

static void stop_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

static bool write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static void timestamp(char *out, size_t size)
{
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(out, size, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
}

static void emit_line(int logfd, int sockfd, const struct sockaddr_in *peer,
                      const char *fmt, ...)
{
    char timestamp_buf[32];
    char message[512];
    char line[600];
    va_list ap;

    timestamp(timestamp_buf, sizeof(timestamp_buf));
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    snprintf(line, sizeof(line), "watch: %s %s\n", timestamp_buf, message);
    if (!write_all(logfd, line, strlen(line)))
        fprintf(stderr, "phase-watch: local log write failed: %s\n", strerror(errno));
    if (!write_all(STDOUT_FILENO, line, strlen(line)))
        fprintf(stderr, "phase-watch: stdout write failed: %s\n", strerror(errno));
    if (sockfd >= 0 && peer != NULL) {
        ssize_t sent = sendto(sockfd, line, strlen(line), 0,
                              (const struct sockaddr *)peer, sizeof(*peer));
        if (sent < 0) {
            g_udp_errors++;
            if (!g_udp_error_reported) {
                fprintf(stderr, "phase-watch: UDP send failed: %s (further failures counted)\n",
                        strerror(errno));
                g_udp_error_reported = true;
            }
        }
    }
}

static bool read_u64(io_service_t service, CFStringRef key, uint64_t *value)
{
    CFTypeRef property;
    int64_t signed_value = 0;

    property = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
    if (property == NULL)
        return false;
    if (CFGetTypeID(property) != CFNumberGetTypeID()) {
        CFRelease(property);
        return false;
    }
    if (!CFNumberGetValue((CFNumberRef)property, kCFNumberSInt64Type, &signed_value)) {
        CFRelease(property);
        return false;
    }
    CFRelease(property);
    *value = (uint64_t)signed_value;
    return true;
}

static bool same_sample(const Sample *a, const Sample *b)
{
    return a->call == b->call && a->phase == b->phase &&
           a->step == b->step && a->hook == b->hook && a->worker == b->worker &&
           a->open_phase == b->open_phase;
}

static bool resolve_udp(const char *host, const char *port,
                        struct sockaddr_in *peer)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(host, port, &hints, &result);
    if (rc != 0 || result == NULL)
        return false;
    memcpy(peer, result->ai_addr, sizeof(*peer));
    freeaddrinfo(result);
    return true;
}

int main(int argc, char **argv)
{
    io_service_t service = IO_OBJECT_NULL;
    int logfd;
    int sockfd = -1;
    struct sockaddr_in peer;
    bool have_peer;
    bool require_udp;
    uint64_t seconds;
    uint64_t deadline;
    uint64_t last_heartbeat;
    bool have_last = false;
    bool have_last_read = false;
    bool warned_missing = false;
    uint32_t last_seen = 0;
    Sample last = {0};
    struct timespec now;
    struct sigaction sa;
    CFStringRef key_call = CFSTR("cx3ClientSelector");
    CFStringRef key_phase = CFSTR("cx3ClientPhase");
    CFStringRef key_step = CFSTR("cx3NetupStep");
    CFStringRef key_hook = CFSTR("cx3AttachHook");
    CFStringRef key_open = CFSTR("cx3OpenPhase");

    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <seconds> <udp-host> <udp-port> <output-log>\n",
                argv[0]);
        return 2;
    }

    {
        char *end = NULL;
        errno = 0;
        seconds = strtoull(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || seconds == 0) {
            fprintf(stderr, "phase-watch: invalid duration: %s\n", argv[1]);
            return 2;
        }
    }
    require_udp = getenv("CX3_WATCH_REQUIRE_UDP") != NULL &&
                  strcmp(getenv("CX3_WATCH_REQUIRE_UDP"), "1") == 0;
    logfd = open(argv[4], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logfd < 0) {
        perror("open output log");
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    if (geteuid() == 0 && setpriority(PRIO_PROCESS, 0, -10) != 0)
        fprintf(stderr, "phase-watch: setpriority(-10) failed: %s\n", strerror(errno));

    have_peer = resolve_udp(argv[2], argv[3], &peer);
    if (have_peer) {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockfd < 0)
            have_peer = false;
    }
    if (!have_peer && require_udp) {
        fprintf(stderr, "phase-watch: UDP endpoint is unreachable: %s:%s\n", argv[2], argv[3]);
        close(logfd);
        return 3;
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    deadline = (uint64_t)now.tv_sec + seconds;
    last_heartbeat = (uint64_t)now.tv_sec;
    emit_line(logfd, have_peer ? sockfd : -1, have_peer ? &peer : NULL,
              "start (cx3-phase-watch, %" PRIu64 "s, 10ms, udp=%s:%s%s)",
              seconds, argv[2], argv[3], have_peer ? "" : ", unavailable");

    while (!g_stop) {
        Sample current = {0};
        uint64_t value;
        uint32_t seen = 0;
        bool read_ok = false;
        bool changed;

        if (service == IO_OBJECT_NULL) {
            io_service_t found = IOServiceGetMatchingService(
                kIOMainPortDefault, IOServiceMatching("Cx3Mlx4"));
            if (found != IO_OBJECT_NULL)
                service = found;
        }
        if (service != IO_OBJECT_NULL) {
            read_ok = true;
            if (read_u64(service, key_call, &value)) {
                current.call = value;
                seen |= kPropCall;
            }
            if (read_u64(service, key_phase, &value)) {
                current.phase = value;
                seen |= kPropPhase;
            }
            if (read_u64(service, key_step, &value)) {
                current.step = value;
                seen |= kPropStep;
            }
            if (read_u64(service, key_hook, &value)) {
                current.hook = value;
                seen |= kPropHook;
            }
            if (read_u64(service, CFSTR("cx3AttachWorker"), &value)) {
                current.worker = value;
                seen |= kPropWorker;
            }
            if (read_u64(service, key_open, &value)) {
                current.open_phase = value;
                seen |= kPropOpen;
            }
            /*
             * If this service previously answered and now exposes none of the
             * properties, the held registry object is stale. Drop it and
             * resolve Cx3Mlx4 again on the next pass.
             */
            if (seen == 0) {
                if (have_last_read) IOObjectRelease(service);
                service = IO_OBJECT_NULL;
                read_ok = false;
            } else if (seen != 0) {
                have_last_read = true;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!read_ok) {
            if (!warned_missing || (uint64_t)now.tv_sec != last_heartbeat) {
                emit_line(logfd, have_peer ? sockfd : -1, have_peer ? &peer : NULL,
                          "read=EMPTY service=Cx3Mlx4 (no readable properties)");
                warned_missing = true;
                last_heartbeat = (uint64_t)now.tv_sec;
            }
        } else {
            changed = !have_last || seen != last_seen || !same_sample(&last, &current);
            if (changed || (uint64_t)now.tv_sec != last_heartbeat) {
                emit_line(logfd, have_peer ? sockfd : -1, have_peer ? &peer : NULL,
                          "read=ok call=%" PRIu64 " phase=%" PRIu64
                          " step=%" PRIu64 " hook=%" PRIu64 " worker=%" PRIu64
                          " open=%" PRIu64
                          " seen=0x%x missing=0x%x",
                          current.call, current.phase, current.step, current.hook,
                          current.worker, current.open_phase, seen, kPropAll & ~seen);
                last = current;
                have_last = true;
                last_seen = seen;
                last_heartbeat = (uint64_t)now.tv_sec;
            }
        }
        if ((uint64_t)now.tv_sec >= deadline)
            break;

        {
            struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&delay, NULL);
        }
    }

    emit_line(logfd, have_peer ? sockfd : -1, have_peer ? &peer : NULL, "end");
    if (g_udp_errors)
        fprintf(stderr, "phase-watch: %lu UDP send failure(s) during run\n", g_udp_errors);
    if (sockfd >= 0)
        close(sockfd);
    close(logfd);
    if (service != IO_OBJECT_NULL)
        IOObjectRelease(service);
    return 0;
}
