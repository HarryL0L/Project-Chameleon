/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-kernel: what the kernel would be for a Chameleon session, in one
 * process the launcher starts (shim/chameleon) before the session's command:
 * the card (kms.c, served to every process by card.c), the input devices and
 * udev monitors (evdev.c), the link to the app (link.c), the input thread
 * (input.c). Programs reach it through standard interfaces only, by way of
 * libchameleon.so (the card node) and libchameleon_udev.so (udev, evdev).
 *
 *   chameleon-kernel SESSION_PID
 *
 * returns once its sockets answer, and keeps running in the background until
 * SESSION_PID is gone.
 *
 *   chameleon-kernel --report
 *
 * asks the session's running service for its report (report.c) and prints it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <ucontext.h>
#include <unistd.h>

#include "../../common/cham_io.h"
#include "internal.h"

int real_fstat(int fd, struct stat *st)
{
    return fstat(fd, st);
}

/* A crash of the service ends the session's screen and input without a word
 * otherwise: say which signal and where (an offset addr2line takes), then let
 * the signal do what it does (Android's tombstone). */
extern char __executable_start[];

static void put(const char *text, uintptr_t value)
{
    char line[96];
    size_t n = 0;
    while (*text && n < sizeof line - 20)
        line[n++] = *text++;
    for (int shift = 60; shift >= 0; shift -= 4)
        line[n++] = "0123456789abcdef"[value >> shift & 15];
    line[n++] = '\n';
    if (write(2, line, n) < 0)
        return;
}

static void crashed(int sig, siginfo_t *info, void *context)
{
    uintptr_t pc = 0;
#if defined(__aarch64__)
    pc = (uintptr_t)((ucontext_t *)context)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (uintptr_t)((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
#else
    (void)context;
#endif
    put("chameleon: chameleon-kernel crashed: signal 0x", (uintptr_t)sig);
    put("chameleon:   at address 0x", (uintptr_t)info->si_addr);
    put("chameleon:   pc - start of chameleon-kernel 0x", pc - (uintptr_t)__executable_start);
    /* SA_RESETHAND: returning runs into a fault again, with the default
     * action; a signal a program sent (abort, kill) is sent again */
    if (info->si_code <= 0)
        raise(sig);
}

static int print_report(void)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    struct cham_msg m = {CARD_REPORT, 0, 0, 0};
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0), fd = -1;
    if (!dir || s < 0 ||
        snprintf(addr.sun_path, sizeof addr.sun_path, "%s/%s", dir, CHAM_CARD_SOCKET_NAME) >=
            (int)sizeof addr.sun_path ||
        connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "chameleon: no session is running (nothing answers at %s/%s)\n", dir ? dir : "",
                CHAM_CARD_SOCKET_NAME);
        return 1;
    }
    if (cham_send(s, &m, -1) != 0 || cham_recv(s, &m, &fd) != 1 || fd < 0) {
        fprintf(stderr, "chameleon: the session's service gave no report (an older chameleon-kernel?)\n");
        return 1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write(1, buf, (size_t)n) != n)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--report"))
        return print_report();
    pid_t session = argc > 1 ? (pid_t)atoi(argv[1]) : 0;
    if (session <= 0) {
        fprintf(stderr, "usage: %s SESSION_PID\n", argv[0]);
        return 2;
    }
    int ready[2];
    if (pipe(ready) != 0)
        return 1;
    pid_t pid = fork();
    if (pid < 0)
        return 1;
    if (pid > 0) { /* the launcher's process: wait until the service answers */
        close(ready[1]);
        char c;
        return read(ready[0], &c, 1) == 1 ? 0 : 1;
    }
    close(ready[0]);
    setsid(); /* the terminal's ^C is for the session's command */
    signal(SIGPIPE, SIG_IGN); /* a program closing its end must not end the service */
    struct sigaction crash = {.sa_sigaction = crashed, .sa_flags = SA_SIGINFO | SA_RESETHAND};
    static const int k_fatal[] = {SIGSEGV, SIGBUS, SIGABRT, SIGFPE, SIGILL};
    for (size_t i = 0; i < sizeof k_fatal / sizeof k_fatal[0]; i++)
        sigaction(k_fatal[i], &crash, NULL);
    report_start();
    evdev_start();
    if (card_start() != 0)
        return 1;
    if (write(ready[1], "", 1) != 1)
        return 1;
    close(ready[1]);
    while (kill(session, 0) == 0 || errno == EPERM)
        sleep(1);
    if (report_debug())
        report_write(stderr);
    cham_log("the session ended: chameleon-kernel stops");
    return 0;
}
