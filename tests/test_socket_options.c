/*
 * Voice's --data-socket / --conf-socket (README "Usage"). The voice source is
 * compiled in with LORAHAM_VOICE_NO_MAIN and -DNO_GTK, so every check runs
 * without a display, a terminal or an audio device:
 *   - no options: each open takes the band's default as before (loraham_sockpath()),
 *     for active_band 0 and 1;
 *   - each option alone and both, verbatim; --cli/--gui; -h; an empty path, a
 *     108-byte path, a missing path and an unknown argument are refused;
 *   - the band toggle (cfg_cycle field 1) switches without the options and is
 *     off with either of them;
 *   - against two listening sockets named like a daemon started with --id t1:
 *     sockets_connect() connects the data socket, and apply_lora_params()'s
 *     "SET FREQ=" line arrives on the given CONF socket.
 */
#define LORAHAM_VOICE_NO_MAIN
#ifndef NO_GTK
#define NO_GTK
#endif
#include "../loraham_voice_v107.c"

#include <poll.h>

static int g_ok = 0, g_fail = 0;

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

static int listen_at(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 4) != 0) {
        close(fd);                                /* no socket left open on a failure, */
        unlink(path);                             /* and no path left bound */
        return -1;
    }
    return fd;
}

/* Accept one client within 2 s and wait (2 s) for its bytes; -1 if none connected. */
static int accept_read(int lfd, char *buf, size_t n)
{
    struct pollfd pl = { lfd, POLLIN, 0 };
    buf[0] = '\0';
    if (poll(&pl, 1, 2000) != 1)
        return -1;
    int c = accept(lfd, NULL, NULL);
    if (c < 0)
        return -1;
    struct pollfd p = { c, POLLIN, 0 };
    (void)poll(&p, 1, 2000);
    ssize_t r = recv(c, buf, n - 1, MSG_DONTWAIT);
    buf[r > 0 ? r : 0] = '\0';
    close(c);
    return 0;
}

static int parse(int argc, char **argv)
{
    g_data_socket_opt = NULL;
    g_conf_socket_opt = NULL;
    return voice_parse_args(argc, argv);
}

int main(void)
{
    char tmpl[] = "/tmp/loraham-voice-test.XXXXXX";
    char path[160], other[160], buf[300];
    if (!mkdtemp(tmpl))
        return 1;

    /* Defaults: the band's choice, as before. */
    g_data_socket_opt = g_conf_socket_opt = NULL;
    snprintf(path, sizeof(path), "%s/run-lora433.sock", tmpl);
    expect("no socket under the /run-style dir: the /tmp fallback",
           strcmp(loraham_sockpath(path, "/tmp/lora433.sock"), "/tmp/lora433.sock") == 0);
    int l0 = listen_at(path);
    expect("a socket under the /run-style dir: that path",
           l0 >= 0 && strcmp(loraham_sockpath(path, "/tmp/lora433.sock"), path) == 0);
    close(l0);
    unlink(path);
    CFG.active_band = 0;
    expect("no options, band 433: the data default",
           strcmp(data_sock_path(), loraham_sockpath("/run/loraham/lora433.sock", "/tmp/lora433.sock")) == 0);
    expect("no options, band 433: the CONF default",
           strcmp(conf_sock_path(), loraham_sockpath("/run/loraham/loraconf433.sock", "/tmp/loraconf433.sock")) == 0);
    CFG.active_band = 1;
    expect("no options, band 868: the data default",
           strcmp(data_sock_path(), loraham_sockpath("/run/loraham/lora868.sock", "/tmp/lora868.sock")) == 0);
    expect("no options, band 868: the CONF default",
           strcmp(conf_sock_path(), loraham_sockpath("/run/loraham/loraconf868.sock", "/tmp/loraconf868.sock")) == 0);

    /* Options. */
    char *a0[] = { "voice", NULL };
    expect("no arguments parse", parse(1, a0) == 0 && !band_fixed());
    char *a1[] = { "voice", "--data-socket", "/x/lora-t1.sock", NULL };
    expect("--data-socket alone parses", parse(3, a1) == 0);
    expect("--data-socket verbatim", strcmp(data_sock_path(), "/x/lora-t1.sock") == 0);
    expect("--data-socket alone leaves the CONF default",
           strcmp(conf_sock_path(), loraham_sockpath("/run/loraham/loraconf868.sock", "/tmp/loraconf868.sock")) == 0);
    char *a2[] = { "voice", "--conf-socket", "/x/loraconf-t1.sock", NULL };
    expect("--conf-socket alone", parse(3, a2) == 0 && strcmp(conf_sock_path(), "/x/loraconf-t1.sock") == 0 &&
                                  strcmp(data_sock_path(), loraham_sockpath("/run/loraham/lora868.sock", "/tmp/lora868.sock")) == 0);
    char *a3[] = { "voice", "--cli", "--data-socket", "/x/d", "--conf-socket", "/x/c", NULL };
    expect("both, with --cli (accepted; main handles it as before)", parse(6, a3) == 0 && strcmp(data_sock_path(), "/x/d") == 0 &&
                               strcmp(conf_sock_path(), "/x/c") == 0);
    char *a4[] = { "voice", "--gui", NULL };
    expect("--gui (accepted; main handles it as before)", parse(2, a4) == 0);
    char *a5[] = { "voice", "-h", NULL };
    expect("-h shows the help (2)", parse(2, a5) == 2);
    char *a6[] = { "voice", "--data-socket", "", NULL };
    expect("an empty path is refused", parse(3, a6) == 1);
    char longp[120];
    memset(longp, 'p', 108);
    longp[0] = '/';
    longp[108] = '\0';
    char *a7[] = { "voice", "--conf-socket", longp, NULL };
    expect("a 108-byte path is refused", parse(3, a7) == 1);
    longp[107] = '\0';
    char *a8[] = { "voice", "--conf-socket", longp, NULL };
    expect("a 107-byte path is taken", parse(3, a8) == 0 && strcmp(conf_sock_path(), longp) == 0);
    char *a9[] = { "voice", "--bogus", NULL };
    expect("an unknown argument is refused", parse(2, a9) == 1);
    char *a10[] = { "voice", "--data-socket", NULL };
    expect("an option without its path is refused", parse(2, a10) == 1);

    /* The band toggle: as before without the options, off with either. */
    parse(1, a0);
    CFG.active_band = 0;
    cfg_cycle(1);
    expect("no options: the toggle switches 433 -> 868", CFG.active_band == 1);
    cfg_cycle(1);
    expect("no options: and back", CFG.active_band == 0);
    cfg_get_val(1, buf, sizeof(buf));
    expect("no options: the field reads 433 MHz", strcmp(buf, "433 MHz") == 0);
    parse(3, a1);
    CFG.active_band = 0;
    cfg_cycle(1);
    expect("--data-socket: the toggle is off", CFG.active_band == 0);
    cfg_get_val(1, buf, sizeof(buf));
    expect("--data-socket: the field says fixed", strcmp(buf, "433 MHz (fest)") == 0);
    parse(3, a2);
    CFG.active_band = 0;
    cfg_cycle(1);
    expect("--conf-socket: the toggle is off", CFG.active_band == 0);

    /* Against sockets named like a daemon with --id t1. The band (868) only
     * selects the parameter set; the sockets are the given ones. */
    snprintf(path, sizeof(path), "%s/lora-t1.sock", tmpl);
    snprintf(other, sizeof(other), "%s/loraconf-t1.sock", tmpl);
    int ld = listen_at(path), lc = listen_at(other);
    expect("test listeners up", ld >= 0 && lc >= 0);
    char *a11[] = { "voice", "--data-socket", path, "--conf-socket", other, NULL };
    parse(5, a11);
    CFG.active_band = 1;
    expect("sockets_connect() connects both given sockets", sockets_connect() == 1);
    expect("the data connection reaches lora-t1.sock", accept_read(ld, buf, sizeof(buf)) == 0);
    apply_lora_params();
    expect("apply_lora_params() reaches loraconf-t1.sock with the 868 set",
           accept_read(lc, buf, sizeof(buf)) == 0 && strncmp(buf, "SET FREQ=869.525 ", 17) == 0);
    close(data_fd);
    close(conf_fd);
    data_fd = conf_fd = -1;

    char *a12[] = { "voice", "--data-socket", "/nonexistent/lora-x.sock", "--conf-socket", other, NULL };
    parse(5, a12);
    expect("a given data path nobody listens on: no connection (0)", sockets_connect() == 0 && data_fd < 0 && conf_fd < 0);

    close(ld);
    close(lc);
    unlink(path);
    unlink(other);
    expect("the temporary directory removed (nothing left in it)", rmdir(tmpl) == 0);
    printf("%d OK, %d FAIL\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
