/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-init: the session's command runs below it, a process that
 * detaches from its parent becomes init's child and is reaped by it, signals
 * and the exit status pass through.
 *
 *   init_test PATH/chameleon-init
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"

static void on_term(int sig)
{
    (void)sig;
    _exit(9);
}

/* The command chameleon-init runs in the tests. */
static int session(const char *what)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(what, "detach")) {
        printf("init %s parent %d\n", getenv("CHAMELEON_INIT"), (int)getppid());
        int report[2];
        if (pipe(report) != 0)
            return 1;
        pid_t middle = fork();
        if (middle == 0) {
            if (fork() == 0) { /* the detached program: who is my parent once the middle one is gone? */
                pid_t was = getppid();
                for (int i = 0; i < 2000 && getppid() == was; i++)
                    usleep(1000);
                int mine[2] = {getpid(), getppid()};
                if (write(report[1], mine, sizeof mine) != sizeof mine)
                    _exit(1);
                _exit(0);
            }
            _exit(0);
        }
        waitpid(middle, NULL, 0);
        int detached[2] = {0, 0};
        if (read(report[0], detached, sizeof detached) != sizeof detached)
            return 1;
        /* Reaped: no such process. Left as a zombie it would still be there. */
        int gone = 0;
        for (int i = 0; i < 2000 && !gone; i++) {
            gone = kill(detached[0], 0) != 0 && errno == ESRCH;
            usleep(1000);
        }
        printf("detached-parent %d reaped %d\n", detached[1], gone);
        return 7;
    }
    if (!strcmp(what, "term")) {
        signal(SIGTERM, on_term);
        printf("ready\n");
        for (;;)
            pause();
    }
    if (!strcmp(what, "killed"))
        raise(SIGKILL);
    return 1;
}

/* Runs chameleon-init with a command; its output comes through *out. */
static pid_t start(const char *init, const char *command, const char *arg, int *out)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        unsetenv("CHAMELEON_INIT");
        execl(init, init, command, "session", arg, (char *)NULL);
        _exit(126);
    }
    close(fds[1]);
    *out = fds[0];
    return pid;
}

static int status_of(pid_t pid)
{
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "session"))
        return session(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: %s PATH/chameleon-init\n", argv[0]);
        return 2;
    }
    const char *init = argv[1];
    printf("chameleon-init\n");

    int out;
    pid_t pid = start(init, argv[0], "detach", &out);
    char text[256] = "";
    for (ssize_t n, len = 0; (n = read(out, text + len, sizeof text - 1 - len)) > 0;)
        len += n;
    close(out);
    int named = 0, parent = 0, adopter = 0, reaped = 0;
    sscanf(text, "init %d parent %d detached-parent %d reaped %d", &named, &parent, &adopter, &reaped);
    CHECK(parent == pid, "the command runs below init (parent %d, init %d)", parent, (int)pid);
    CHECK(named == pid, "CHAMELEON_INIT names init (%d)", named);
    CHECK(adopter == pid, "a program that detached is init's child (parent %d)", adopter);
    CHECK(reaped == 1, "init reaped it when it exited");
    CHECK(status_of(pid) == 7, "the command's exit status is init's");

    pid = start(init, argv[0], "term", &out);
    char ready[8] = "";
    CHECK(read(out, ready, 6) == 6 && !strcmp(ready, "ready\n"), "a second command runs");
    kill(pid, SIGINT);
    usleep(100000);
    CHECK(waitpid(pid, NULL, WNOHANG) == 0, "the terminal's interrupt leaves init to the command");
    kill(pid, SIGTERM);
    CHECK(status_of(pid) == 9, "SIGTERM for init reaches the command");
    close(out);

    pid = start(init, argv[0], "killed", &out);
    CHECK(status_of(pid) == 128 + SIGKILL, "a killed command: status 128 + the signal");
    close(out);

    pid = start(init, "/nonexistent/command", "x", &out);
    CHECK(status_of(pid) == 127, "a command that can't run: 127");
    close(out);

    printf(failures ? "init_test: %d FAILED\n" : "init_test: PASSED\n", failures);
    return failures ? 1 : 0;
}
