/*
 * The BUILT voice CLI binary (-DNO_GTK; path in argv[1]) and the GTK binary (argv[2]), as separate processes.
 *
 * Refusals: run without a terminal (setsid, stdin /dev/null): an unknown
 * argument, an empty path, a missing path and a 108-byte path -> exit 1 with
 * the reason on stderr; --help -> exit 0 with the usage on stdout. A run that
 * does not exit within 5 s fails its case and is killed.
 *
 * The start, on a pseudo-terminal (stdin/stdout), stderr on a pipe. The binary
 * is copied into a temporary directory with a loraham_voice.conf beside it
 * (the config path is the binary's directory) that saves audio_capture=null and
 * audio_playback=null, and the terminal's EOF character is sent before the
 * start, so the audio device choice can never wait for input:
 *   - no listener on the given sockets: stderr says that the daemon is not
 *     reachable and that the app does not reconnect (no "Reconnect im
 *     Hintergrund"); read up to that line, then the child is killed (it would
 *     go on to the ALSA open and the CLI);
 *   - two listeners on the given sockets: the CONF listener receives the
 *     "SET FREQ=" line, and stderr carries no "not reachable" line.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_ok, g_fail;
static char g_bin[256];
static char g_dir[64];

static void expect(const char *name, int cond)
{
    if (cond) {
        g_ok++;
        printf("[ OK ] %s\n", name);
    } else {
        g_fail++;
        printf("[FAIL] %s\n", name);
    }
    fflush(stdout);
}

static int ms_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (int)((t1.tv_sec - t0->tv_sec) * 1000 + (t1.tv_nsec - t0->tv_nsec) / 1000000);
}

/* Run the binary without a terminal; the exit code (-1: killed after 5 s),
 * stderr in err, stdout in out. Both pipes are read together. The child closes
 * its pipes while it exits, a moment before it can be reaped: its exit is
 * awaited on a pidfd within the same 5 s, never judged by one WNOHANG. */
static int run_notty(char *const args[], char *err, size_t err_size, char *out, size_t out_size)
{
    int pe[2], po[2];
    if (pipe(pe))
        return -2;
    if (pipe(po)) {                               /* the first pair is not left open */
        close(pe[0]);
        close(pe[1]);
        return -2;
    }
    pid_t pid = fork();
    if (pid < 0) {                                /* no child: nothing to read, wait for or signal */
        close(pe[0]);
        close(pe[1]);
        close(po[0]);
        close(po[1]);
        return -2;
    }
    if (pid == 0) {
        setsid();
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(po[1], 1);
        dup2(pe[1], 2);
        execv(g_bin, args);
        _exit(127);
    }
    close(pe[1]);
    close(po[1]);
    int pfd = pidfd_open(pid, 0);
    size_t ne = 0, no = 0;
    int open_fds = 2;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct pollfd p[2] = { { pe[0], POLLIN, 0 }, { po[0], POLLIN, 0 } };
    while (open_fds > 0) {
        int left = 5000 - ms_since(&t0);
        if (left <= 0 || poll(p, 2, left) <= 0)
            break;
        for (int k = 0; k < 2; k++) {
            if (p[k].fd < 0 || !(p[k].revents & (POLLIN | POLLHUP)))
                continue;
            char *buf = k == 0 ? err : out;
            size_t *n = k == 0 ? &ne : &no;
            size_t cap = k == 0 ? err_size : out_size;
            char sink[512];
            ssize_t r = *n + 1 < cap ? read(p[k].fd, buf + *n, cap - 1 - *n) : read(p[k].fd, sink, sizeof(sink));
            if (r <= 0) {
                close(p[k].fd);
                p[k].fd = -1;
                open_fds--;
            } else if (*n + 1 < cap) {
                *n += (size_t)r;
            }
        }
    }
    err[ne] = '\0';
    out[no] = '\0';
    int st = 0, killed = 0, left = 5000 - ms_since(&t0);
    struct pollfd px = { pfd, POLLIN, 0 };
    if (pfd < 0 || poll(&px, 1, left > 0 ? left : 0) != 1) {
        kill(pid, SIGKILL);
        killed = 1;
    }
    waitpid(pid, &st, 0);
    if (pfd >= 0)
        close(pfd);
    for (int k = 0; k < 2; k++)
        if (p[k].fd >= 0)
            close(p[k].fd);
    return killed ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int listen_at(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || listen(fd, 8)) {
        close(fd);                                /* no socket left open on a failure, */
        unlink(path);                             /* and no path left bound */
        return -1;
    }
    return fd;
}

/* The voice CLI on a pseudo-terminal, stderr on a pipe. */
struct child {
    pid_t pid;
    int master, err;
};

/* End a child that start_on_pty() started; a no-op for the invalid one. */
static void stop_child(struct child *c)
{
    if (c->pid <= 0)
        return;
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
    close(c->master);
    close(c->err);
    c->pid = -1;
}

/* 0 = started; -1 = no child left behind (c is then the invalid one, which the
 * readers are never given). */
static int start_on_pty(char *const args[], struct child *c)
{
    int pe[2];
    c->pid = -1;
    c->master = c->err = -1;
    if (pipe(pe))
        return -1;
    c->pid = forkpty(&c->master, NULL, NULL, NULL);
    if (c->pid < 0) {
        close(pe[0]);
        close(pe[1]);
        c->pid = -1;
        return -1;
    }
    if (c->pid == 0) {
        dup2(pe[1], 2);
        close(pe[0]);
        execv(g_bin, args);
        _exit(127);
    }
    close(pe[1]);
    c->err = pe[0];
    /* The terminal's EOF character at the start of a line: a read of stdin
     * returns 0, so the audio device choice takes its default. */
    struct termios t;
    char eof = 4;
    if (tcgetattr(c->master, &t) == 0)
        eof = (char)t.c_cc[VEOF];
    if (write(c->master, &eof, 1) != 1) {
        stop_child(c);                            /* the started child is ended, not left behind */
        return -1;
    }
    return 0;
}


/* Read stderr until `want` and the end of its line is seen, or ms passed; the
 * terminal side is drained alongside so the child never blocks on it. */
static int read_err_until(struct child *c, const char *want, char *buf, size_t size, int ms)
{
    size_t n = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    buf[0] = '\0';
    for (;;) {
        int left = ms - ms_since(&t0);
        if (left <= 0)
            return 0;
        struct pollfd p[2] = { { c->err, POLLIN, 0 }, { c->master, POLLIN, 0 } };
        if (poll(p, 2, left) <= 0)
            return 0;
        if (p[1].revents & POLLIN) {
            char sink[512];
            (void)read(c->master, sink, sizeof(sink));
        }
        if (p[0].revents & (POLLIN | POLLHUP)) {
            ssize_t r = read(c->err, buf + n, size - 1 - n);
            if (r <= 0)
                return 0;
            n += (size_t)r;
            buf[n] = '\0';
            char *hit = strstr(buf, want);
            if (hit && strchr(hit, '\n'))
                return 1;
            if (n + 1 >= size)
                return 0;
        }
    }
}

/* Accept on lfd within ms (the pty drained meanwhile), then read until `want` or ms. */
static int accept_read_until(struct child *c, int lfd, const char *want, char *buf, size_t size, int ms)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    buf[0] = '\0';
    int cfd = -1;
    size_t n = 0;
    for (;;) {
        int left = ms - ms_since(&t0);
        if (left <= 0)
            break;
        int watch = cfd >= 0 ? cfd : lfd;
        struct pollfd p[2] = { { watch, POLLIN, 0 }, { c->master, POLLIN, 0 } };
        if (poll(p, 2, left) <= 0)
            break;
        if (p[1].revents & POLLIN) {
            char sink[512];
            (void)read(c->master, sink, sizeof(sink));
        }
        if (!(p[0].revents & POLLIN))
            continue;
        if (cfd < 0) {
            cfd = accept(lfd, NULL, NULL);
            continue;
        }
        ssize_t r = read(cfd, buf + n, size - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        buf[n] = '\0';
        if (strstr(buf, want))
            break;
    }
    if (cfd >= 0)
        close(cfd);
    return strstr(buf, want) != NULL;
}

/* What stderr holds right now (non-blocking). */
static void drain_err(struct child *c, char *buf, size_t size)
{
    size_t n = strlen(buf);
    fcntl(c->err, F_SETFL, fcntl(c->err, F_GETFL) | O_NONBLOCK);
    ssize_t r;
    while (n + 1 < size && (r = read(c->err, buf + n, size - 1 - n)) > 0)
        n += (size_t)r;
    buf[n] = '\0';
}

/* Remove the test's directory: 0 when it is gone. */
static int remove_dir(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
    return system(cmd);
}

static int copy_file(const char *from, const char *to)
{
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", from, to);
    return system(cmd);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <loraham_voice_cli> <loraham_voice (GTK)>\n", argv[0]);
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "/tmp/loraham-voice-bin.XXXXXX");
    if (!mkdtemp(g_dir))
        return 1;
    /* The binary beside its own loraham_voice.conf: the audio pair saved as null. */
    snprintf(g_bin, sizeof(g_bin), "%s/loraham_voice_cli", g_dir);
    if (copy_file(argv[1], g_bin) != 0) {
        remove_dir();                             /* a failed setup leaves nothing behind either */
        return 1;
    }
    char conf[128];
    snprintf(conf, sizeof(conf), "%s/loraham_voice.conf", g_dir);
    FILE *f = fopen(conf, "w");
    if (!f) {
        remove_dir();
        return 1;
    }
    fprintf(f, "audio_capture=null\naudio_playback=null\n");
    fclose(f);

    char err[4096], out[4096];
    char longp[120];
    memset(longp, 'p', 108);
    longp[0] = '/';
    longp[108] = '\0';
    char *r1[] = { g_bin, "--bogus", NULL };
    expect("an unknown argument: exit 1", run_notty(r1, err, sizeof(err), out, sizeof(out)) == 1 &&
                                         strstr(err, "unknown argument: --bogus"));
    char *r2[] = { g_bin, "--data-socket", "", NULL };
    expect("an empty path: exit 1", run_notty(r2, err, sizeof(err), out, sizeof(out)) == 1 &&
                                   strstr(err, "--data-socket needs a path"));
    char *r3[] = { g_bin, "--conf-socket", NULL };
    expect("a missing path: exit 1", run_notty(r3, err, sizeof(err), out, sizeof(out)) == 1 &&
                                    strstr(err, "--conf-socket needs a path"));
    char *r4[] = { g_bin, "--data-socket", longp, NULL };
    expect("a 108-byte path: exit 1", run_notty(r4, err, sizeof(err), out, sizeof(out)) == 1 &&
                                     strstr(err, "path longer than 107 bytes"));
    char *r5[] = { g_bin, "--help", NULL };
    expect("--help: exit 0, the usage", run_notty(r5, err, sizeof(err), out, sizeof(out)) == 0 &&
                                       strstr(out, "--data-socket PATH") && strstr(out, "--conf-socket PATH"));
    /* --gui on the CLI build: the note as before; sockets nobody listens on, so nothing is reached (the
     * run goes on to the CLI and is ended at run_notty's 5 s bound). */
    char nd[128], nc[128];
    snprintf(nd, sizeof(nd), "%s/none.sock", g_dir);
    snprintf(nc, sizeof(nc), "%s/noneconf.sock", g_dir);
    char *r6[] = { g_bin, "--gui", "--data-socket", nd, "--conf-socket", nc, NULL };
    run_notty(r6, err, sizeof(err), out, sizeof(out));
    expect("--gui on the CLI build: the note, as before", strstr(err, "Hinweis: ohne GTK gebaut - starte CLI") != NULL);
    /* As at 9c000225 the note is printed once per --gui token, whatever follows it. */
    char *r7[] = { g_bin, "--gui", "--cli", "--data-socket", nd, "--conf-socket", nc, NULL };
    run_notty(r7, err, sizeof(err), out, sizeof(out));
    expect("--gui --cli on the CLI build: the note, as before", strstr(err, "Hinweis: ohne GTK gebaut - starte CLI") != NULL);
    char *r8[] = { g_bin, "--gui", "--gui", "--data-socket", nd, "--conf-socket", nc, NULL };
    run_notty(r8, err, sizeof(err), out, sizeof(out));
    const char *h1 = strstr(err, "Hinweis: ohne GTK gebaut - starte CLI");
    expect("--gui --gui on the CLI build: the note twice, as before",
           h1 && strstr(h1 + 1, "Hinweis: ohne GTK gebaut - starte CLI") != NULL);
    /* A socket operand that reads like a UI token is a path, not --gui: no note, and the warning names it. */
    char *r9[] = { g_bin, "--data-socket", "--gui", "--conf-socket", "--gui", NULL };
    run_notty(r9, err, sizeof(err), out, sizeof(out));
    expect("--data-socket --gui: no note, the operand is the path",
           strstr(err, "Hinweis: ohne GTK gebaut") == NULL && strstr(err, "Daemon nicht erreichbar (--gui, --gui)") != NULL);

    /* No listener: the start message says what happens. */
    char dsock[128], csock[128];
    snprintf(dsock, sizeof(dsock), "%s/lora-t1.sock", g_dir);
    snprintf(csock, sizeof(csock), "%s/loraconf-t1.sock", g_dir);
    char *s1[] = { g_bin, "--data-socket", dsock, "--conf-socket", csock, NULL };
    struct child c;
    int started = start_on_pty(s1, &c) == 0, seen = 0;
    expect("started on a pty (no listener)", started);
    if (started) {
        seen = read_err_until(&c, "WARNUNG: Daemon nicht erreichbar", err, sizeof(err), 10000);
        stop_child(&c);
    }
    expect("no listener: the not-reachable line on stderr", seen);
    expect("it names the given data socket", strstr(err, dsock) != NULL);
    expect("it says the app does not reconnect", strstr(err, "die App verbindet sich nicht neu") != NULL);
    expect("no false \"Reconnect im Hintergrund\"", strstr(err, "Reconnect im Hintergrund") == NULL);

    /* Listeners on the given sockets: connected, the CONF line arrives, no warning. */
    int ld = listen_at(dsock), lc = listen_at(csock);
    expect("test listeners up", ld >= 0 && lc >= 0);
    err[0] = '\0';
    started = start_on_pty(s1, &c) == 0;
    expect("started on a pty (listeners)", started);
    char got[512] = "";
    int conf_seen = 0;
    if (started) {
        conf_seen = accept_read_until(&c, lc, "SET FREQ=", got, sizeof(got), 10000);
        drain_err(&c, err, sizeof(err));
        stop_child(&c);
    }
    expect("listeners: the SET FREQ line arrives on the given CONF socket", conf_seen);
    expect("listeners: no not-reachable line", strstr(err, "Daemon nicht erreichbar") == NULL);
    close(ld);
    close(lc);

    /* The GTK binary, with no display: a --gui that is a socket operand must not choose the GUI (which would
     * end in GTK's "cannot open display"); the run takes the CLI path and is ended at run_notty's bound. */
    char gtk[256];
    snprintf(gtk, sizeof(gtk), "%s/loraham_voice", g_dir);              /* beside the same loraham_voice.conf */
    expect("the GTK binary copied", copy_file(argv[2], gtk) == 0);
    unsetenv("DISPLAY");
    unsetenv("WAYLAND_DISPLAY");
    char *r10[] = { gtk, "--data-socket", "--gui", "--conf-socket", nc, NULL };
    snprintf(g_bin, sizeof(g_bin), "%s", gtk);
    run_notty(r10, err, sizeof(err), out, sizeof(out));
    expect("GTK binary, no display, --data-socket --gui: the CLI path (no \"cannot open display\")",
           strstr(err, "cannot open display") == NULL && strstr(err, "Daemon nicht erreichbar (--gui, ") != NULL);

    if (remove_dir() != 0)
        expect("the temporary directory removed", 0);
    printf("%d OK, %d FAIL\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
