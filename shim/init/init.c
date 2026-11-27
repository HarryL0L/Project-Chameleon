/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * chameleon-init COMMAND [ARG...]: the session's init.
 *
 * Runs the command and adopts every process below it whose parent exits: a
 * daemon that detaches, a service the session bus starts. On a Linux system
 * init does that, and `systemd --user` for a desktop session, by the same
 * means (PR_SET_CHILD_SUBREAPER). Without it such a process becomes a child
 * of Android's init, and some phones' process cleaners kill those (ColorOS:
 * "orphan process"): chameleon-kernel, the portals, the sound server.
 *
 * It reaps what it adopted, passes SIGTERM and SIGHUP on to the command and
 * exits with the command's status. What still runs then goes to Android's
 * init, as it did before.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_command;

static void pass_on(int sig)
{
    kill(g_command, sig);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s COMMAND [ARG...]\n", argv[0]);
        return 2;
    }
    /* The launcher runs through here once: it finds its init by this. */
    char self[16];
    snprintf(self, sizeof self, "%d", (int)getpid());
    setenv("CHAMELEON_INIT", self, 1);

    /* Held back until the command's pid is known and the handlers are set. */
    sigset_t passed, before;
    sigemptyset(&passed);
    sigaddset(&passed, SIGTERM);
    sigaddset(&passed, SIGHUP);
    sigprocmask(SIG_BLOCK, &passed, &before);

    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
        fprintf(stderr, "chameleon: init: can't adopt the session's detached programs: %s\n", strerror(errno));
    else if ((g_command = fork()) < 0)
        fprintf(stderr, "chameleon: init: fork: %s\n", strerror(errno));
    if (g_command <= 0) { /* the command; in this process when there is nothing to stay for */
        sigprocmask(SIG_SETMASK, &before, NULL);
        execvp(argv[1], argv + 1);
        fprintf(stderr, "chameleon: init: %s: %s\n", argv[1], strerror(errno));
        _exit(127);
    }

    struct sigaction pass = {.sa_handler = pass_on};
    sigaction(SIGTERM, &pass, NULL);
    sigaction(SIGHUP, &pass, NULL);
    /* The terminal's ^C and ^\ reach the command by themselves; init waits for it. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    sigprocmask(SIG_SETMASK, &before, NULL);

    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0 && errno != EINTR)
            return 1;
        if (pid == g_command)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
}
