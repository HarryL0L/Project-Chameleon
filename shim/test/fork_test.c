/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A forked child's first calls to functions our libraries override (dup2
 * in libchameleon_udev.so, sigaction in libchameleon.so) must not look
 * anything up in the dynamic linker: on Android its lock may have been held
 * by another thread at the fork (Firefox launching a content process while a
 * GPU driver thread loads a library), and the child then waits forever.
 * glibc protects that lock across fork, so instead of hanging this checks
 * that no lookup happens: run with LD_BIND_NOW=1 LD_DEBUG=symbols
 * LD_DEBUG_OUTPUT=PREFIX, both libraries preloaded, and PREFIX as argv[1];
 * glibc's lines carry the pid of the process looking a symbol up.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"

/* Lookups of `name` the process `pid` made, in the files PREFIX.<pid>. */
static int lookups(const char *prefix, pid_t pid, const char *name)
{
    char dir[256], want[64], line[512];
    snprintf(dir, sizeof dir, "%s", prefix);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return -1;
    *slash = 0;
    const char *base = slash + 1;
    snprintf(want, sizeof want, "symbol=%s;", name);
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (strncmp(e->d_name, base, strlen(base)) != 0)
            continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        while (f && fgets(line, sizeof line, f)) {
            int who = 0;
            if (sscanf(line, " %d:", &who) == 1 && who == pid && strstr(line, want))
                n++;
        }
        if (f)
            fclose(f);
    }
    if (d)
        closedir(d);
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "run via run-host-test.sh\n");
        return 2;
    }
    printf("a forked child's first overridden calls\n");
    pid_t pid = fork();
    if (pid == 0) {
        struct sigaction sa = {.sa_handler = SIG_DFL};
        _exit(dup2(1, 100) == 100 && sigaction(SIGUSR1, &sa, NULL) == 0 ? 0 : 1);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "dup2 and sigaction work in the child");
    int dup2s = lookups(argv[1], pid, "dup2"), sigactions = lookups(argv[1], pid, "sigaction");
    CHECK(dup2s == 0 && sigactions == 0,
          "...without a symbol lookup in the dynamic linker (dup2 %d, sigaction %d)", dup2s, sigactions);
    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
