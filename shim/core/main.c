/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-kernel: what the kernel would be for a Chameleon session, in one
 * process the launcher starts (shim/chameleon) before the session's command:
 * the card (kms.c, served to every process by card.c), the input devices and
 * udev monitors (evdev.c), the link to the app (link.c) and the input thread
 * (input.c). Programs reach it through standard interfaces only, by way of
 * libchameleon.so (the card node) and libchameleon_udev.so (udev, evdev).
 *
 *   chameleon-kernel SESSION_PID
 *
 * returns once its sockets answer, and keeps running in the background until
 * SESSION_PID is gone.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

int real_fstat(int fd, struct stat *st)
{
    return fstat(fd, st);
}

int main(int argc, char **argv)
{
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
    evdev_start();
    if (card_start() != 0)
        return 1;
    if (write(ready[1], "", 1) != 1)
        return 1;
    close(ready[1]);
    while (kill(session, 0) == 0 || errno == EPERM)
        sleep(1);
    return 0;
}
