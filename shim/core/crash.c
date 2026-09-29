/*
 * Crash reporter for kwin_wayland under the shim. On SIGSEGV/SIGBUS it writes
 * the fault, the last GL entry point the EGL vendor forwarded, the last
 * texture upload and a symbolized backtrace to stderr (kwin.log), then hands
 * the signal on to whoever had it before (debuggerd's tombstone, KCrash).
 *
 * The backtrace is a frame-pointer walk plus a stack scan for return
 * addresses (words just after a BL/BLR), since vendor drivers often have no
 * frame pointers. CHAMELEON_CRASH=0 disables all of it.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "internal.h"

/* Set by the glvnd vendor library once glvnd loads it. */
static const struct cham_crash_breadcrumbs *volatile g_crumbs;

CHAM_EXPORT void cham_crash_attach(const struct cham_crash_breadcrumbs *crumbs)
{
    g_crumbs = crumbs;
}

static int (*real_sigaction)(int, const struct sigaction *, struct sigaction *);
static struct sigaction g_prev[2];  /* what was installed before us */
static struct sigaction g_chain[2]; /* installed after us (e.g. KCrash) */
static int g_have_chain[2];
static int g_installed;
static int g_probe_pipe[2] = {-1, -1};

static int slot(int sig)
{
    return sig == SIGSEGV ? 0 : sig == SIGBUS ? 1 : -1;
}

/* ---- async-signal-safe output ---- */

static void out(const char *s)
{
    size_t n = strlen(s);
    while (n) {
        ssize_t w = write(2, s, n);
        if (w <= 0)
            return;
        s += w;
        n -= (size_t)w;
    }
}

static void hex(char *buf, uintptr_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[2 + 2 * sizeof v + 1];
    int i = (int)sizeof tmp - 1;
    tmp[i] = 0;
    do {
        tmp[--i] = digits[v & 15];
        v >>= 4;
    } while (v);
    tmp[--i] = 'x';
    tmp[--i] = '0';
    strcpy(buf, tmp + i);
}

static void dec(char *buf, long v)
{
    char tmp[24];
    int i = (int)sizeof tmp - 1, neg = v < 0;
    unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
    tmp[i] = 0;
    do {
        tmp[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg)
        tmp[--i] = '-';
    strcpy(buf, tmp + i);
}

static void out_hex(const char *label, uintptr_t v)
{
    char b[24];
    hex(b, v);
    out(label);
    out(b);
}

static void out_dec(const char *label, long v)
{
    char b[24];
    dec(b, v);
    out(label);
    out(b);
}

/* Reads `len` bytes at `addr` without faulting: write() returns EFAULT for
 * unreadable memory. */
static int safe_read(uintptr_t addr, void *dst, size_t len)
{
    if (g_probe_pipe[0] < 0 || addr < 4096)
        return 0;
    if (write(g_probe_pipe[1], (const void *)addr, len) != (ssize_t)len)
        return 0;
    return read(g_probe_pipe[0], dst, len) == (ssize_t)len;
}

static uintptr_t strip(uintptr_t pc)
{
#if defined(__aarch64__)
    return pc & 0x00ffffffffffffffull; /* top byte / PAC-free enough for dladdr */
#else
    return pc;
#endif
}

__attribute__((unused)) static void out_frame(const char *kind, int n, uintptr_t pc)
{
    pc = strip(pc);
    out("chameleon:   ");
    out(kind);
    out_dec(" #", n);
    out_hex(" ", pc);
    Dl_info info;
    if (dladdr((void *)pc, &info) && info.dli_fname) {
        const char *base = strrchr(info.dli_fname, '/');
        out(" ");
        out(base ? base + 1 : info.dli_fname);
        out_hex("+", pc - (uintptr_t)info.dli_fbase);
        if (info.dli_sname) {
            out(" (");
            out(info.dli_sname);
            out_hex("+", pc - (uintptr_t)info.dli_saddr);
            out(")");
        }
    }
    out("\n");
}

/* Is `ret` a return address, i.e. does the instruction before it call? */
__attribute__((unused)) static int is_return_address(uintptr_t ret)
{
    ret = strip(ret);
    if (ret & 3)
        return 0;
    uint32_t insn;
    if (!safe_read(ret - 4, &insn, sizeof insn))
        return 0;
#if defined(__aarch64__)
    return (insn & 0xfc000000u) == 0x94000000u      /* BL */
           || (insn & 0xfffffc1fu) == 0xd63f0000u   /* BLR */
           || (insn & 0xfffff800u) == 0xd63f0800u;  /* BLRAA/BLRAB (and Z) */
#else
    return 1;
#endif
}

static void report(int sig, siginfo_t *si, void *ucv)
{
    out(sig == SIGBUS ? "chameleon: crash: SIGBUS" : "chameleon: crash: SIGSEGV");
    out_dec(" code ", si->si_code);
    out_hex(" fault addr ", (uintptr_t)si->si_addr);
    out_dec(" pid ", getpid());
    out_dec(" tid ", (long)syscall(SYS_gettid));
    out(syscall(SYS_gettid) == getpid() ? " (main thread)\n" : " (not the main thread)\n");
    const struct cham_crash_breadcrumbs *c = g_crumbs;
    if (c) {
        const char *gl = *c->last_call;
        out("chameleon: last GL call forwarded: ");
        out(gl ? gl : "(none)");
        out("\nchameleon: last texture upload: ");
        out(c->note[0] ? c->note : "(none)");
        out("\n");

        /* Oldest first, runs of the same call folded into "name xN". */
        unsigned end = *c->pos, start = end > CHAM_CALL_RING ? end - CHAM_CALL_RING : 0;
        out_dec("chameleon: last GL/EGL calls (of ", (long)end);
        out("):");
        for (unsigned i = start; i < end;) {
            const char *name = c->ring[i % CHAM_CALL_RING];
            unsigned run = 1;
            while (i + run < end && c->ring[(i + run) % CHAM_CALL_RING] == name)
                run++;
            out(" ");
            out(name ? name : "?");
            if (run > 1)
                out_dec(" x", (long)run);
            i += run;
        }
        out("\n");
    } else {
        out("chameleon: no GL/EGL breadcrumbs (the Chameleon EGL vendor was not loaded)\n");
    }

#if defined(__aarch64__)
    ucontext_t *uc = ucv;
    uintptr_t pc = uc->uc_mcontext.pc, lr = uc->uc_mcontext.regs[30];
    uintptr_t fp = uc->uc_mcontext.regs[29], sp = uc->uc_mcontext.sp;
    out_hex("chameleon:   x0 ", uc->uc_mcontext.regs[0]);
    out_hex(" x1 ", uc->uc_mcontext.regs[1]);
    out_hex(" x2 ", uc->uc_mcontext.regs[2]);
    out_hex(" x3 ", uc->uc_mcontext.regs[3]);
    out_hex(" sp ", sp);
    out_hex(" fp ", fp);
    out("\n");
    out_frame("pc", 0, pc);
    out_frame("lr", 0, lr);

    /* Frame-pointer walk: [fp] = caller's fp, [fp + 8] = return address. */
    for (int n = 1; n <= 32 && fp && !(fp & 7); n++) {
        uintptr_t rec[2];
        if (!safe_read(fp, rec, sizeof rec) || !rec[1])
            break;
        out_frame("fp", n, rec[1]);
        if (rec[0] <= fp)
            break;
        fp = rec[0];
    }

    /* Stack scan: catches frames the walk missed (no frame pointers). */
    int shown = 0;
    uintptr_t last = 0;
    for (uintptr_t a = sp & ~(uintptr_t)7; a < sp + 16384 && shown < 40; a += 8) {
        uintptr_t v;
        if (!safe_read(a, &v, sizeof v))
            break;
        if (v == last || v < 4096 || !is_return_address(v))
            continue;
        Dl_info info;
        if (!dladdr((void *)strip(v), &info) || !info.dli_fname)
            continue;
        out_frame("stack", shown++, v);
        last = v;
    }
#else
    (void)ucv;
#endif
    out("chameleon: end of crash report\n");
}

static void handler(int sig, siginfo_t *si, void *uc)
{
    static volatile int in_handler;
    int s = slot(sig);
    if (!in_handler) {
        in_handler = 1;
        report(sig, si, uc);
    }
    /* Put back the next handler and return: the faulting instruction runs
     * again and the signal goes to it (debuggerd writes the tombstone). */
    if (s >= 0)
        real_sigaction(sig, g_have_chain[s] ? &g_chain[s] : &g_prev[s], NULL);
}

/* Handlers installed after ours (KCrash) run after the report. */
CHAM_EXPORT int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    if (!real_sigaction)
        *(void **)&real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    int s = slot(sig);
    if (!g_installed || s < 0)
        return real_sigaction(sig, act, oldact);
    if (oldact)
        *oldact = g_have_chain[s] ? g_chain[s] : g_prev[s];
    if (act) {
        g_chain[s] = *act;
        g_have_chain[s] = 1;
    }
    return 0;
}

__attribute__((constructor)) static void install(void)
{
    const char *env = getenv("CHAMELEON_CRASH");
    if (env && strcmp(env, "0") == 0)
        return;
    if (!real_sigaction)
        *(void **)&real_sigaction = dlsym(RTLD_NEXT, "sigaction");
    if (!real_sigaction || pipe2(g_probe_pipe, O_CLOEXEC) != 0)
        return;
    /* Warm up dladdr's lazy state outside the handler. */
    Dl_info info;
    dladdr((void *)install, &info);

    static char altstack[64 * 1024];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    real_sigaction(SIGSEGV, &sa, &g_prev[0]);
    real_sigaction(SIGBUS, &sa, &g_prev[1]);
    g_installed = 1;
}
