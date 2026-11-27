/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the card was asked and what it answered, counted for the session:
 * every request by its name with the time it took, each kind of refusal with
 * the program that first got it, and every reason a commit was refused.
 * `chameleon --report` prints it; in a session started with
 * `chameleon --debug-log` (CHAMELEON_DEBUG_LOG is set) the first of each
 * request and refusal, and the slow ones, are also logged as they happen,
 * and the whole report at the session's end.
 *
 * Nothing a program drew or a person typed is in it: names of requests and
 * programs, numbers, and the card's own reasons.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

#define REQUESTS 256 /* an ioctl's number is a byte */
#define ANSWERS 96
#define REASONS 48
#define SLOW_NS 8000000ull /* a request the caller waited a 120 Hz frame for */
#define SLOW_LINES 200

static pthread_mutex_t g_report_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_debug;
static uint64_t g_start_ns;

static struct {
    const char *name;
    uint64_t count, refused, slow, ns, max_ns;
} g_requests[REQUESTS];
/* Each different refusal of a request, and (count 0 with ret 0) its first use. */
static struct {
    unsigned nr;
    int ret;
    uint64_t count;
    char comm[20];
} g_answers[ANSWERS];
static int g_nanswers, g_answers_full;
static struct {
    int kind, err;
    uint64_t count;
    char text[160];
} g_reasons[REASONS];
static int g_nreasons, g_reasons_full;
static uint64_t g_counts[REPORT_COUNTS];
static int g_slow_lines;

void report_start(void)
{
    g_start_ns = now_ns();
    g_debug = getenv("CHAMELEON_DEBUG_LOG") != NULL;
}

void report_count(int what)
{
    __atomic_add_fetch(&g_counts[what], 1, __ATOMIC_RELAXED);
}

static void comm_of(pid_t pid, char *out, size_t size)
{
    char path[40];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    snprintf(out, size, "pid %d", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    ssize_t n = read(fd, out, size - 1);
    close(fd);
    if (n > 0) {
        out[n] = 0;
        out[strcspn(out, "\n")] = 0;
    } else {
        snprintf(out, size, "pid %d", (int)pid);
    }
}

static const char *request_name(unsigned nr, char *buf, size_t size)
{
    if (g_requests[nr].name)
        return g_requests[nr].name;
    snprintf(buf, size, "ioctl 0x%02x (not one of the card's)", nr);
    return buf;
}

void report_request(unsigned nr, const char *name, int waits, int ret, uint64_t ns)
{
    char comm[20] = "", buf[48];
    int first = 0, slow;
    nr &= REQUESTS - 1;
    if (ret > 0)
        ret = 0;
    slow = !waits && ns >= SLOW_NS;
    pthread_mutex_lock(&g_report_lock);
    if (name)
        g_requests[nr].name = name;
    g_requests[nr].count++;
    g_requests[nr].refused += ret < 0;
    g_requests[nr].slow += slow;
    if (!waits) {
        g_requests[nr].ns += ns;
        if (ns > g_requests[nr].max_ns)
            g_requests[nr].max_ns = ns;
    }
    int i;
    for (i = 0; i < g_nanswers; i++)
        if (g_answers[i].nr == nr && g_answers[i].ret == ret)
            break;
    if (i == g_nanswers && g_nanswers < ANSWERS) {
        g_nanswers++;
        g_answers[i].nr = nr;
        g_answers[i].ret = ret;
        g_answers[i].count = 0;
        comm_of(card_caller_pid(), g_answers[i].comm, sizeof g_answers[i].comm);
        first = 1;
    }
    if (i < g_nanswers) {
        g_answers[i].count++;
        memcpy(comm, g_answers[i].comm, sizeof comm);
    } else {
        g_answers_full = 1;
    }
    int say_slow = g_debug && slow && g_slow_lines < SLOW_LINES;
    g_slow_lines += say_slow;
    pthread_mutex_unlock(&g_report_lock);
    if (!g_debug)
        return;
    if (first && ret < 0)
        cham_log("debug: %s: %s refused: %s (the first time)", comm, request_name(nr, buf, sizeof buf),
                 strerror(-ret));
    else if (first)
        cham_log("debug: %s: %s, the first one (%.2f ms)", comm, request_name(nr, buf, sizeof buf), ns / 1e6);
    if (say_slow) {
        comm_of(card_caller_pid(), comm, sizeof comm);
        cham_log("debug: slow: %s waited %.1f ms for %s", comm, ns / 1e6, request_name(nr, buf, sizeof buf));
    }
}

void report_refusal(int kind, int err, const char *fmt, ...)
{
    char text[sizeof g_reasons[0].text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    int first = 0;
    pthread_mutex_lock(&g_report_lock);
    int i;
    for (i = 0; i < g_nreasons; i++)
        if (g_reasons[i].kind == kind && g_reasons[i].err == err && !strcmp(g_reasons[i].text, text))
            break;
    if (i == g_nreasons && g_nreasons < REASONS) {
        g_nreasons++;
        g_reasons[i].kind = kind;
        g_reasons[i].err = err;
        g_reasons[i].count = 0;
        memcpy(g_reasons[i].text, text, sizeof text);
        first = 1;
    }
    if (i < g_nreasons)
        g_reasons[i].count++;
    else
        g_reasons_full = 1;
    pthread_mutex_unlock(&g_report_lock);
    /* a real commit's refusal is logged by its caller, always */
    if (g_debug && first && kind != REPORT_COMMIT)
        cham_log("debug: a %s was refused (%s): %s (the first time)",
                 kind == REPORT_TEST ? "test commit" : "legacy call", strerror(-err), text);
}

void report_write(FILE *out)
{
    static const char *const k_kinds[] = {"commit", "test commit", "legacy call"};
    char buf[48], comm[20];
    uint32_t w, h;
    uint64_t all = 0, refused = 0;
    kms_mode_size(&w, &h);
    pid_t master = kms_master_pid();
    fprintf(out, "chameleon report: chameleon-kernel has run for %.0f s\n", (now_ns() - g_start_ns) / 1e9);
    if (master) {
        comm_of(master, comm, sizeof comm);
        fprintf(out, "screen: mode %ux%u, set by %s (the DRM master)\n", w, h, comm);
    } else {
        fprintf(out, "screen: no DRM master now (last mode %ux%u)\n", w, h);
    }
    link_report(out);
    fprintf(out, "input: %llu messages dropped because the input thread was busy\n",
            (unsigned long long)__atomic_load_n(&g_counts[REPORT_INPUT_DROPPED], __ATOMIC_RELAXED));
    pthread_mutex_lock(&g_report_lock);
    for (int nr = 0; nr < REQUESTS; nr++) {
        all += g_requests[nr].count;
        refused += g_requests[nr].refused;
    }
    fprintf(out, "card requests: %llu, %llu refused\n", (unsigned long long)all, (unsigned long long)refused);
    fprintf(out, "  %-28s %10s %8s %8s %8s %6s\n", "request", "count", "refused", "avg ms", "max ms", "slow");
    for (int nr = 0; nr < REQUESTS; nr++) {
        if (!g_requests[nr].count)
            continue;
        fprintf(out, "  %-28s %10llu %8llu %8.3f %8.2f %6llu\n", request_name((unsigned)nr, buf, sizeof buf),
                (unsigned long long)g_requests[nr].count, (unsigned long long)g_requests[nr].refused,
                g_requests[nr].ns / 1e6 / (double)g_requests[nr].count, g_requests[nr].max_ns / 1e6,
                (unsigned long long)g_requests[nr].slow);
    }
    fprintf(out, "  (slow: the caller waited %d ms or more; requests that wait by design aren't timed)\n",
            (int)(SLOW_NS / 1000000));
    fprintf(out, "refused requests, by answer and the program that got it first:\n");
    int any = 0;
    for (int i = 0; i < g_nanswers; i++) {
        if (g_answers[i].ret >= 0)
            continue;
        any = 1;
        fprintf(out, "  %8llu x %s: %s (%s)\n", (unsigned long long)g_answers[i].count,
                request_name(g_answers[i].nr, buf, sizeof buf), strerror(-g_answers[i].ret), g_answers[i].comm);
    }
    if (!any)
        fprintf(out, "  none\n");
    if (g_answers_full)
        fprintf(out, "  (more kinds than this list holds)\n");
    fprintf(out, "refused commits, by the card's reason:\n");
    for (int i = 0; i < g_nreasons; i++)
        fprintf(out, "  %8llu x %s, %s: %s\n", (unsigned long long)g_reasons[i].count, k_kinds[g_reasons[i].kind],
                strerror(-g_reasons[i].err), g_reasons[i].text);
    if (!g_nreasons)
        fprintf(out, "  none\n");
    if (g_reasons_full)
        fprintf(out, "  (more reasons than this list holds)\n");
    pthread_mutex_unlock(&g_report_lock);
    fflush(out);
}

int report_debug(void)
{
    return g_debug;
}
