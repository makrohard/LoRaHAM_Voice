/*
 * The GUI half of the band lock (README "Usage"): the settings dialog's band
 * combo, built by band_combo_new(), is selectable without the socket options
 * and not selectable with either of them. The voice source is compiled in with
 * LORAHAM_VOICE_NO_MAIN and GTK; it needs a display, which CI gives it through
 * xvfb-run: `xvfb-run -a ./run_tests.sh gui` (it fails without a display).
 */
#define LORAHAM_VOICE_NO_MAIN
#include "../loraham_voice_v107.c"

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

/* A fresh combo for these arguments: its sensitivity and its active band. */
static void combo_for(int argc, char **argv, int *sensitive, int *active)
{
    g_data_socket_opt = NULL;
    g_conf_socket_opt = NULL;
    if (voice_parse_args(argc, argv) != 0) {
        *sensitive = *active = -1;
        return;
    }
    GtkWidget *c = band_combo_new();
    g_object_ref_sink(c);
    *sensitive = gtk_widget_get_sensitive(c);
    *active = gtk_combo_box_get_active(GTK_COMBO_BOX(c));
    g_object_unref(c);
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) {
        printf("[FAIL] gtk_init_check: no display (run it under xvfb-run)\n");
        return 1;
    }
    int sens, act;
    char *a0[] = { "voice", NULL };
    char *a1[] = { "voice", "--data-socket", "/x/lora-t1.sock", NULL };
    char *a2[] = { "voice", "--conf-socket", "/x/loraconf-t1.sock", NULL };
    char *a3[] = { "voice", "--data-socket", "/x/d", "--conf-socket", "/x/c", NULL };

    CFG.active_band = 0;
    combo_for(1, a0, &sens, &act);
    expect("no options: the band combo is selectable", sens == 1);
    expect("no options: it shows the active band (433)", act == 0);
    CFG.active_band = 1;
    combo_for(1, a0, &sens, &act);
    expect("no options, band 868: selectable, shows 868", sens == 1 && act == 1);
    CFG.active_band = 0;
    combo_for(3, a1, &sens, &act);
    expect("--data-socket: the band combo is not selectable", sens == 0);
    expect("--data-socket: it still shows the active band", act == 0);
    combo_for(3, a2, &sens, &act);
    expect("--conf-socket: the band combo is not selectable", sens == 0);
    combo_for(5, a3, &sens, &act);
    expect("both options: the band combo is not selectable", sens == 0);

    printf("%d OK, %d FAIL\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
