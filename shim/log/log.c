/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-log FILE [BYTES]: the recorder of `chameleon --debug-log`.
 *
 * Copies what it reads (everything the session's programs print) to its
 * output unchanged and to FILE with the time since it started in front of
 * each line. FILE stays under BYTES (28 MiB if not given): once it would
 * grow past that, the start of the session and the most recent part are
 * kept and the middle is left out, with a line saying how much.
 *
 * It ends the file when its input ends or, two seconds after SIGTERM (the
 * launcher's, when the session's command has ended: the service's last lines
 * still come), says where the file is, and then only passes on what programs
 * that outlive the session still print.
 *
 * While it runs the recent part is in FILE.0 and FILE.1 next to FILE.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_term;
static void on_term(int sig) { (void)sig; g_term = 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void write_all(int fd, const char *data, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, data, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return; /* a closed terminal or a full disk: the rest still goes on */
        data += w;
        n -= (size_t)w;
    }
}

static char g_head[4096], g_older[4100], g_newer[4100];
static uint64_t g_head_max, g_part_max; /* the start's size; each of the two recent parts' */
static uint64_t g_head_size, g_part_size, g_older_size, g_left_out;
static int g_file = -1, g_part = -1;

/* One whole line, with its time, into the file. */
static void record(const char *line, size_t n)
{
    if (g_part < 0 && g_head_size + n <= g_head_max) {
        write_all(g_file, line, n);
        g_head_size += n;
        return;
    }
    if (g_part >= 0 && g_part_size + n > g_part_max) { /* the older part goes, this one becomes it */
        g_left_out += g_older_size;
        close(g_part);
        g_part = -1;
        rename(g_newer, g_older);
        g_older_size = g_part_size;
    }
    if (g_part < 0) {
        g_part = open(g_newer, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        g_part_size = 0;
        if (g_part < 0)
            return;
    }
    write_all(g_part, line, n);
    g_part_size += n;
}

static void append(const char *path)
{
    char buf[65536];
    ssize_t n;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        write_all(g_file, buf, (size_t)n);
    close(fd);
    unlink(path);
}

static void finish(void)
{
    char line[160];
    if (g_part >= 0) {
        close(g_part);
        if (g_left_out) {
            int n = snprintf(line, sizeof line,
                             "[...] %llu bytes of the session are left out here, to keep this file small\n",
                             (unsigned long long)g_left_out);
            write_all(g_file, line, (size_t)n);
        }
        if (g_older_size)
            append(g_older);
        append(g_newer);
    }
    off_t size = lseek(g_file, 0, SEEK_END);
    close(g_file);
    char say[4400];
    int n = snprintf(say, sizeof say,
                     "chameleon: the debug log is %s (%.1f %s). Read it before sharing it: it holds everything "
                     "the session's programs printed.\n",
                     g_head, size < 1048576 ? (double)size / 1024.0 : (double)size / 1048576.0,
                     size < 1048576 ? "KiB" : "MiB");
    write_all(1, say, (size_t)n);
}

int main(int argc, char **argv)
{
    if (argc < 2 || strlen(argv[1]) >= sizeof g_head) {
        fprintf(stderr, "usage: %s FILE [BYTES]\n", argv[0]);
        return 2;
    }
    uint64_t max = argc > 2 ? strtoull(argv[2], NULL, 10) : 28ull << 20;
    g_head_max = max / 7;
    g_part_max = max * 3 / 7 - 128; /* less the line that says what is left out */
    snprintf(g_head, sizeof g_head, "%s", argv[1]);
    snprintf(g_older, sizeof g_older, "%s.0", argv[1]);
    snprintf(g_newer, sizeof g_newer, "%s.1", argv[1]);
    g_file = open(g_head, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (g_file < 0) {
        fprintf(stderr, "chameleon: cannot write the debug log %s: %s\n", g_head, strerror(errno));
        /* the session still runs: pass its output on */
    }
    /* The terminal's ^C and its closing are for the session; this ends with it. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction term = {.sa_handler = on_term};
    sigaction(SIGTERM, &term, NULL);

    double start = now(), stop_at = 0;
    static char in[65536], line[4096 + 32];
    size_t len = 0;       /* of the line being gathered, after its time */
    int stamp = 0;        /* the time's length at the line's start */
    int recording = g_file >= 0;
    for (;;) {
        if (recording && g_term && !stop_at)
            stop_at = now() + 2;
        if (recording && stop_at && now() >= stop_at)
            break;
        struct pollfd p = {0, POLLIN, 0};
        if (poll(&p, 1, recording ? 200 : -1) <= 0)
            continue;
        ssize_t n = read(0, in, sizeof in);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        write_all(1, in, (size_t)n);
        if (!recording)
            continue;
        for (ssize_t i = 0; i < n; i++) {
            if (!stamp)
                stamp = snprintf(line, sizeof line, "[%9.3f] ", now() - start);
            line[stamp + len++] = in[i];
            if (in[i] == '\n' || len == 4096) { /* a longer line goes on as a next one */
                if (in[i] != '\n')
                    line[stamp + len++] = '\n';
                record(line, (size_t)stamp + len);
                len = 0;
                stamp = 0;
            }
        }
    }
    if (!recording)
        return 0;
    if (len) {
        line[stamp + len++] = '\n';
        record(line, (size_t)stamp + len);
    }
    finish();
    /* Programs that outlive the session keep the pipe: their lines still
     * reach the terminal, from a process nobody waits for. */
    if (fork() != 0)
        return 0;
    ssize_t n;
    while ((n = read(0, in, sizeof in)) > 0 || (n < 0 && errno == EINTR))
        if (n > 0)
            write_all(1, in, (size_t)n);
    return 0;
}
