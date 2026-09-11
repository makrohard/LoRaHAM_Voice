/******************************************************************************
 * Copyright (C) 2026  [LoRaHAM / Alexander Walter]
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <https://www.gnu.org/licenses/>.
 *****************************************************************************/


/*
 * loraham_voice_v107.c – LoRa Codec2 Voice für LoRaHAM Daemon
 *
 * Abhängigkeiten installieren:
 *
     sudo apt update
     sudo apt install libcodec2-dev
     sudo apt install libasound2-dev
     sudo apt install libgtk-3-dev
     sudo apt install libncurses5-dev

 * Kompilieren:
 *
    gcc -o loraham_voice loraham_voice.c `pkg-config --cflags --libs gtk+-3.0` -lcodec2 -lasound -lncurses -lpthread -lm

 * Starten:  ./loraham_voice        (Auto: GUI wenn DISPLAY gesetzt)
 *           ./loraham_voice --cli   (CLI erzwingen)
 *           ./loraham_voice --gui   (GUI erzwingen)
 */

/* ============================================================
 * SENDEVERHALTEN – hier anpassen, neu kompilieren
 * ============================================================
 * TARGET_AUDIO_MS:   Ziel-Puffergröße pro LoRa-Paket (ms)
 *                    Beeinflusst Latenz und n_frames/Paket
 * PTT_RELEASE_MS:    CLI-PTT: ms nach letztem Space-Tastendruck
 *                    bis PTT automatisch endet (Auto-Repeat)
 * TX_EXTRA_DELAY_MS: Zusätzliche Pause nach Airtime (Daemon-Puffer)
 * AIRTIME_MARGIN_PCT:Prozentualer Aufschlag auf berechnete Airtime
 * ============================================================ */
#define TARGET_AUDIO_MS     250
#define PTT_RELEASE_MS      400
#define TX_EXTRA_DELAY_MS    50
#define AIRTIME_MARGIN_PCT   10

/* GTK muss vor ncurses includiert werden.
 * -DNO_GTK baut eine reine ncurses-CLI-Variante ohne GTK/X11-Abhaengigkeit
 * (Headless-/Lite-Systeme):
 *   gcc -DNO_GTK -o loraham_voice loraham_voice_v107.c -lcodec2 -lasound -lncurses -lpthread -lm */
#ifndef NO_GTK
#include <gtk/gtk.h>
#endif

/* ncurses: _XOPEN_SOURCE_EXTENDED verhindert Kollisionen */
#define _XOPEN_SOURCE_EXTENDED 1
#include <ncurses.h>

#include <codec2/codec2.h>
#include <alsa/asoundlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <libgen.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

/* ================================================================
 * Protokoll
 * ================================================================ */
#define MAGIC_0   0xC0
#define MAGIC_1   0xDE
#define MAGIC_2   0xC2
#define PKT_HDR   0x01
#define PKT_VOICE 0x02
#define PKT_BRK   0x03   /* Übertragung abgebrochen (Abbrechen-Button) */
#define PKT_FIN   0x04   /* Übertragung normal beendet (PTT losgelassen) */

#pragma pack(push,1)
typedef struct {
    uint8_t magic[3];      /* C0 DE C2              */
    uint8_t pkt_type;      /* 0x01                  */
    uint8_t codec_mode;    /* Index in C2_MODES[]   */
    char    callsign[12];
    char    ident[14];     /* "LoRaHAM Voice\0"     */
    uint8_t reserved;
} PktHdr_t;   /* 31 Bytes */

typedef struct {
    uint8_t  magic[3];
    uint8_t  pkt_type;     /* 0x02                  */
    uint16_t seq_nr;       /* LE, wraps             */
    uint8_t  n_frames;
    uint8_t  data[252];    /* max Codec2-Frames     */
} PktVoice_t;  /* max 259 Bytes */

/* PKT_BRK und PKT_FIN teilen dieselbe Struktur (16 Bytes) */
typedef struct {
    uint8_t magic[3];
    uint8_t pkt_type;      /* 0x03 oder 0x04        */
    char    callsign[12];
} PktEnd_t;   /* 16 Bytes */
#pragma pack(pop)

#define PKT_VOICE_OVERHEAD 7  /* magic+type+seq+n_frames */
#define PKT_HDR_SIZE       ((int)sizeof(PktHdr_t))
#define HDR_INTERVAL       10 /* Header alle N Voice-Pakete */

/* ================================================================
 * Codec2-Modus-Tabelle
 * ================================================================ */
typedef struct {
    int         id;
    const char *name;
} C2ModeEntry_t;

static const C2ModeEntry_t C2_MODES[] = {
    { CODEC2_MODE_3200,  "Codec2-3200"   },
    { CODEC2_MODE_2400,  "Codec2-2400"   },
    { CODEC2_MODE_1600,  "Codec2-1600"   },
    { CODEC2_MODE_1400,  "Codec2-1400"   },
    { CODEC2_MODE_1300,  "Codec2-1300"   },
    { CODEC2_MODE_1200,  "Codec2-1200"   },
    { CODEC2_MODE_700C,  "Codec2-700C"   },
#ifdef CODEC2_MODE_700B
    { CODEC2_MODE_700B,  "Codec2-700B"   },
#endif
#ifdef CODEC2_MODE_700
    { CODEC2_MODE_700,   "Codec2-700"    },
#endif
#ifdef CODEC2_MODE_450
    { CODEC2_MODE_450,   "Codec2-450"    },
#endif
#ifdef CODEC2_MODE_450PWB
    { CODEC2_MODE_450PWB,"Codec2-450PWB" },
#endif
};
#define N_C2_MODES ((int)(sizeof(C2_MODES)/sizeof(C2_MODES[0])))
#define C2_SAMPLE_RATE 8000

/* ================================================================
 * LoRa-Parameter + Konfiguration
 * ================================================================ */
typedef struct {
    double  freq;
    int     sf, cr, crc, preamble, power;
    double  bw;
    uint8_t sync;
    int     ldro;    /* 0=AUS 1=AN 2=AUTO */
} LoRaP_t;

typedef struct {
    char    callsign[13];
    int     active_band;       /* 0=433  1=868 */
    int     codec_mode_idx;
    char    audio_capture[64];
    char    audio_playback[64];
    int     duplex;
    LoRaP_t lora433;
    LoRaP_t lora868;
} Config_t;

static Config_t CFG = {
    .callsign       = "DC0AAA",
    .active_band    = 0,
    .codec_mode_idx = 0,
    .audio_capture  = "default",
    .audio_playback = "default",
    .duplex         = 0,
    .lora433 = { .freq=434.700,.sf=7,.bw=125.0,.cr=5,
                 .crc=1,.preamble=8,.sync=0x12,.power=17,.ldro=2 },
    .lora868 = { .freq=869.525,.sf=11,.bw=250.0,.cr=5,
                 .crc=1,.preamble=16,.sync=0x2B,.power=10,.ldro=2 },
};

static char conf_path[512] = "loraham_voice.conf";

/* ================================================================
 * Globaler Zustand
 * ================================================================ */
static int  data_fd    = -1;
static int  conf_fd    = -1;
static int  use_gui    = 0;
static int  app_run    = 1;

static volatile int ptt_active    = 0;
static volatile int ptt_in_config = 0; /* PTT sperren im Konfig-Dialog */
static volatile int tone_test     = 0; /* 1 = 600Hz Testton statt Mikrofon */

static pthread_mutex_t mtx_sock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mtx_hist  = PTHREAD_MUTEX_INITIALIZER;

/* Codec2-Handles (werden in c2_init gesetzt) */
static struct CODEC2 *c2_enc = NULL;
static struct CODEC2 *c2_dec = NULL;
static int c2_spf   = 0;   /* TX: samples_per_frame  */
static int c2_bpf   = 0;   /* TX: bits_per_frame     */
static int c2_Bpf   = 0;   /* TX: bytes_per_frame    */
static int c2_fms   = 0;   /* TX: frame duration ms  */
static int n_frames_per_pkt = 0;

/* Empfangs-Codec: wird beim PKT_HDR auf den gesendeten Modus umgeschaltet.
 * Nur vom RX-Thread beschrieben → kein Mutex nötig.                        */
static struct CODEC2 *c2_rx_dec  = NULL;
static int            c2_rx_spf  = 0;
static int            c2_rx_Bpf  = 0;
static volatile int   c2_rx_mode = -1;  /* Index in C2_MODES[], -1=unbekannt */

/* Sende-Sequenznummer */
static uint16_t tx_seq = 0;
static int      tx_hdr_ctr = 0;

/* ALSA Handles */
static snd_pcm_t *pcm_cap = NULL;
static snd_pcm_t *pcm_play = NULL;

/* RSSI (von conf socket) */
static float last_rssi = -200.0f;

/* RX-Übertragungs-Tracking:
 * Verhindert dass wiederholte PKT_HDR (alle 10 Pakete) als neue
 * Übertragung in die History eingetragen werden.                 */
static char rx_cur_call[13] = "";  /* Rufzeichen des aktuellen Senders  */
static int  rx_in_tx        = 0;   /* 1 = Sender sendet gerade          */

/* ================================================================
 * History (scrollbar, max 200 Einträge, kein Log)
 * ================================================================ */
#define MAX_HIST 200
typedef struct {
    char ts[10];      /* HH:MM:SS   */
    char call[13];
    char codec[20];
    int  rssi;
} HistEntry_t;

static HistEntry_t  g_hist[MAX_HIST];
static int          g_hist_head  = 0;
static int          g_hist_count = 0;

static void hist_add(const char *call, const char *codec, int rssi) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    pthread_mutex_lock(&mtx_hist);
    HistEntry_t *e = &g_hist[g_hist_head % MAX_HIST];
    snprintf(e->ts,   sizeof(e->ts),    "%02d:%02d:%02d",
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    strncpy(e->call,  call,  sizeof(e->call)-1);
    strncpy(e->codec, codec, sizeof(e->codec)-1);
    e->rssi = rssi;
    g_hist_head  = (g_hist_head + 1) % MAX_HIST;
    if (g_hist_count < MAX_HIST) g_hist_count++;
    pthread_mutex_unlock(&mtx_hist);
}

static void hist_get(int idx, HistEntry_t *out) {
    /* idx=0 = ältester, idx=g_hist_count-1 = neuester */
    pthread_mutex_lock(&mtx_hist);
    int real = (g_hist_head - g_hist_count + idx + MAX_HIST * 2) % MAX_HIST;
    *out = g_hist[real];
    pthread_mutex_unlock(&mtx_hist);
}

/* ================================================================
 * Locale-sichere Float-Helfer
 * ================================================================ */
static void flt2str(char *buf, size_t sz, const char *fmt, double v) {
    snprintf(buf, sz, fmt, v);
    for (char *p = buf; *p; p++) if (*p == ',') *p = '.';
}
static double str2flt(const char *s) {
    /* Vollständig locale-unabhängiger Parser.
     * atof()/strtod() versagen in German-Locale bei Punkt als Trenner.
     * Diese Funktion akzeptiert sowohl '.' als auch ',' als Dezimaltrenner. */
    const char *p = s;
    while (*p==' '||*p=='\t') p++;
    double sign=1.0;
    if (*p=='-'){sign=-1.0;p++;} else if(*p=='+') p++;
    double result=0.0, frac=0.0, fdiv=1.0;
    int in_frac=0;
    for(;*p;p++){
        if (*p>='0'&&*p<='9'){
            if(in_frac){frac=frac*10+(*p-'0');fdiv*=10;}
            else result=result*10+(*p-'0');
        } else if((*p=='.'||*p==',')&&!in_frac){ in_frac=1; }
        else break;
    }
    return sign*(result+frac/fdiv);
}

/* ================================================================
 * Airtime-Berechnung (Semtech AN1200.13, gleich wie LoRaSST)
 * ================================================================ */
static uint32_t lora_airtime_us(const LoRaP_t *lp, int pl_bytes) {
    double bw_hz = lp->bw * 1000.0;
    double t_sym = (double)(1 << lp->sf) / bw_hz;
    int de = (lp->ldro==1)?1:(lp->ldro==0)?0:(t_sym>=0.016)?1:0;
    int cr4 = lp->cr - 4;
    double num = 8.0*pl_bytes - 4.0*lp->sf + 28.0 + 16.0*lp->crc;
    double den = 4.0*(double)(lp->sf - 2*de);
    int np = (den>0)?(int)ceil(num/den):0;
    if (np<0) np=0;
    np = np*(cr4+4)+8;
    double t = ((double)lp->preamble + 4.25 + np) * t_sym;
    return (uint32_t)(t * (1.0 + AIRTIME_MARGIN_PCT/100.0) * 1e6);
}
static uint32_t total_airtime_us(const LoRaP_t *lp, int psz) {
    uint32_t tot=0;
    while (psz>0) { int c=(psz>255)?255:psz; tot+=lora_airtime_us(lp,c); psz-=c; }
    return tot;
}

/* ================================================================
 * Modus-Validierung: gibt 0=OK, 1=verboten-simplex, 2=verboten-duplex
 * Berechnet auch n_frames_per_pkt für das aktuelle Setup.
 * ================================================================ */
static int check_voice_mode(const LoRaP_t *lp, int duplex, int *nf_out) {
    if (c2_fms <= 0) { if(nf_out) *nf_out=1; return 0; }
    int n = (TARGET_AUDIO_MS + c2_fms - 1) / c2_fms;
    if (n < 1) n = 1;
    int psz = PKT_VOICE_OVERHEAD + n * c2_Bpf;
    uint32_t at_us  = total_airtime_us(lp, psz);
    int      at_ms  = (int)(at_us / 1000) + TX_EXTRA_DELAY_MS;
    int      aud_ms = n * c2_fms;
    if (nf_out) *nf_out = n;
    if (at_ms >= aud_ms)       return 1;  /* simplex verboten */
    if (duplex && at_ms*2 >= aud_ms) return 2;  /* duplex verboten */
    return 0;
}

/* ================================================================
 * Konfiguration laden / speichern
 * Achtung! %s/ nicht vergessen, sonst schreibt er es in den
 * momentanen Ordner und nicht dort wo die App liegt!
 * ================================================================ */
static void config_init_path(const char *argv0) {
    char tmp[512]; strncpy(tmp, argv0, 511);
    snprintf(conf_path, sizeof(conf_path), "%s/loraham_voice.conf", dirname(tmp));
}

static void lp_save(FILE *f, const char *prefix, const LoRaP_t *lp) {
    char fs[32], bs[32];
    flt2str(fs, sizeof(fs), "%.3f", lp->freq);
    flt2str(bs, sizeof(bs), "%.1f", lp->bw);
    fprintf(f, "%s_freq=%s\n",     prefix, fs);
    fprintf(f, "%s_sf=%d\n",       prefix, lp->sf);
    fprintf(f, "%s_bw=%s\n",       prefix, bs);
    fprintf(f, "%s_cr=%d\n",       prefix, lp->cr);
    fprintf(f, "%s_crc=%d\n",      prefix, lp->crc);
    fprintf(f, "%s_preamble=%d\n", prefix, lp->preamble);
    fprintf(f, "%s_sync=0x%02X\n", prefix, lp->sync);
    fprintf(f, "%s_power=%d\n",    prefix, lp->power);
    fprintf(f, "%s_ldro=%d\n",     prefix, lp->ldro);
}

static void config_save(void) {
    FILE *f = fopen(conf_path, "w");
    if (!f) return;
    fprintf(f, "# LoRaHAM Voice Konfiguration\n");
    fprintf(f, "callsign=%s\n",      CFG.callsign);
    fprintf(f, "active_band=%d\n",   CFG.active_band);
    fprintf(f, "codec_mode=%d\n",    CFG.codec_mode_idx);
    fprintf(f, "audio_capture=%s\n", CFG.audio_capture);
    fprintf(f, "audio_playback=%s\n",CFG.audio_playback);
    fprintf(f, "duplex=%d\n",        CFG.duplex);
    lp_save(f, "433", &CFG.lora433);
    lp_save(f, "868", &CFG.lora868);
    fclose(f);
}

static void lp_load(const char *prefix, const char *key, const char *val, LoRaP_t *lp) {
    char k[64];
    snprintf(k,sizeof(k),"%s_freq",prefix);  if(!strcmp(key,k)){lp->freq=str2flt(val);return;}
    snprintf(k,sizeof(k),"%s_sf",prefix);    if(!strcmp(key,k)){lp->sf=atoi(val);return;}
    snprintf(k,sizeof(k),"%s_bw",prefix);    if(!strcmp(key,k)){lp->bw=str2flt(val);return;}
    snprintf(k,sizeof(k),"%s_cr",prefix);    if(!strcmp(key,k)){lp->cr=atoi(val);return;}
    snprintf(k,sizeof(k),"%s_crc",prefix);   if(!strcmp(key,k)){lp->crc=atoi(val);return;}
    snprintf(k,sizeof(k),"%s_preamble",prefix);if(!strcmp(key,k)){lp->preamble=atoi(val);return;}
    snprintf(k,sizeof(k),"%s_sync",prefix);  if(!strcmp(key,k)){lp->sync=(uint8_t)strtoul(val,NULL,0);return;}
    snprintf(k,sizeof(k),"%s_power",prefix); if(!strcmp(key,k)){lp->power=atoi(val);return;}
    snprintf(k,sizeof(k),"%s_ldro",prefix);  if(!strcmp(key,k)){lp->ldro=atoi(val);return;}
}

static void config_load(void) {
    FILE *f = fopen(conf_path, "r");
    if (!f) return;
    char line[128], key[64], val[64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='#') continue;
        if (sscanf(line,"%63[^=]=%63s",key,val)!=2) continue;
        for (char *p=val;*p;p++) if(*p==',') *p='.';
        if (!strcmp(key,"callsign"))     { strncpy(CFG.callsign,val,12); }
        if (!strcmp(key,"active_band"))  { CFG.active_band=atoi(val); }
        if (!strcmp(key,"codec_mode"))   { CFG.codec_mode_idx=atoi(val); }
        if (!strcmp(key,"audio_capture")){ strncpy(CFG.audio_capture,val,63); }
        if (!strcmp(key,"audio_playback")){ strncpy(CFG.audio_playback,val,63); }
        if (!strcmp(key,"duplex"))       { CFG.duplex=atoi(val); }
        lp_load("433",key,val,&CFG.lora433);
        lp_load("868",key,val,&CFG.lora868);
    }
    fclose(f);
    if (CFG.codec_mode_idx<0||CFG.codec_mode_idx>=N_C2_MODES) CFG.codec_mode_idx=0;
    if (CFG.active_band<0||CFG.active_band>1) CFG.active_band=0;
}

/* ================================================================
 * LoRa-Parameter an Daemon senden
 * ================================================================ */
static void apply_lora_params(void) {
    if (conf_fd<0) return;
    const LoRaP_t *lp = CFG.active_band ? &CFG.lora868 : &CFG.lora433;
    char ldro[8];
    if      (lp->ldro==2) snprintf(ldro,sizeof(ldro),"AUTO"); // Hier muss 0,1,2 auf Text 0, 1, AUTO gewandelt werden, da der Daemon den Text interpretiert
    else if (lp->ldro==1) snprintf(ldro,sizeof(ldro),"1");
    else                  snprintf(ldro,sizeof(ldro),"0");
    char fs[32], bs[32];
    flt2str(fs,sizeof(fs),"%.3f",lp->freq);
    flt2str(bs,sizeof(bs),"%.1f",lp->bw);
    char cmd[256];
    snprintf(cmd,sizeof(cmd),
        "SET FREQ=%s SF=%d BW=%s CR=%d CRC=%d PREAMBLE=%d SYNC=0x%02X POWER=%d LDRO=%s\n",
        fs,lp->sf,bs,lp->cr,lp->crc,lp->preamble,lp->sync,lp->power,ldro);
    pthread_mutex_lock(&mtx_sock);
    if (conf_fd>=0) (void)write(conf_fd,cmd,strlen(cmd));
    pthread_mutex_unlock(&mtx_sock);
}

/* ================================================================
 * Socket verbinden
 * ================================================================ */
static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd<0) return -1;
    struct sockaddr_un a;
    memset(&a,0,sizeof(a));
    a.sun_family=AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path)-1);
    if (connect(fd,(struct sockaddr*)&a,sizeof(a))<0) { close(fd); return -1; }
    return fd;
}
/* Daemon-Socket-Pfadwahl: systemd-Deployments servieren die Sockets unter /run/loraham, direkte/
 * Benutzer-Starts unter /tmp (LORAHAM_SOCKET_DIR). Nimm den Pfad, unter dem der Daemon-Socket
 * tatsaechlich existiert, sonst den /tmp-Fallback. */
static const char *loraham_sockpath(const char *runp, const char *tmpp) {
    struct stat st;
    return (stat(runp, &st) == 0 && S_ISSOCK(st.st_mode)) ? runp : tmpp;
}
static int sockets_connect(void) {
    const char *dsock = CFG.active_band
        ? loraham_sockpath("/run/loraham/lora868.sock", "/tmp/lora868.sock")
        : loraham_sockpath("/run/loraham/lora433.sock", "/tmp/lora433.sock");
    const char *csock = CFG.active_band
        ? loraham_sockpath("/run/loraham/loraconf868.sock", "/tmp/loraconf868.sock")
        : loraham_sockpath("/run/loraham/loraconf433.sock", "/tmp/loraconf433.sock");
    data_fd = connect_unix(dsock);
    if (data_fd<0) return 0;
    fcntl(data_fd, F_SETFL, fcntl(data_fd,F_GETFL,0)|O_NONBLOCK);
    conf_fd = connect_unix(csock);
    if (conf_fd<0) { close(data_fd); data_fd=-1; return 0; }
    return 1;
}

/* ================================================================
 * Codec2 initialisieren
 * ================================================================ */
static int c2_init(int mode_idx) {
    if (c2_enc) { codec2_destroy(c2_enc); c2_enc=NULL; }
    if (c2_dec) { codec2_destroy(c2_dec); c2_dec=NULL; }
    if (mode_idx<0||mode_idx>=N_C2_MODES) return 0;
    c2_enc = codec2_create(C2_MODES[mode_idx].id);
    c2_dec = codec2_create(C2_MODES[mode_idx].id);
    if (!c2_enc||!c2_dec) return 0;
    c2_spf = codec2_samples_per_frame(c2_enc);
    c2_bpf = codec2_bits_per_frame(c2_enc);
    c2_Bpf = (c2_bpf + 7) / 8;
    c2_fms = c2_spf * 1000 / C2_SAMPLE_RATE;
    check_voice_mode(CFG.active_band ? &CFG.lora868 : &CFG.lora433,
                     CFG.duplex, &n_frames_per_pkt);
    return 1;
}

/* ================================================================
 * ALSA Hilfsfunktionen
 * ================================================================ */
static int alsa_list_devices(char devs[][64], int max) {
    void **hints=NULL, **h;
    int n=0;
    if (snd_device_name_hint(-1,"pcm",&hints)<0) return 0;
    for (h=hints; *h && n<max; h++) {
        char *name = snd_device_name_get_hint(*h,"NAME");
        char *ioid = snd_device_name_get_hint(*h,"IOID");
        /* Nur Geräte die Capture können, oder keine Einschränkung */
        if (name && (!ioid||!strcmp(ioid,"Input")||!strcmp(ioid,""))) {
            strncpy(devs[n],name,63); n++;
        }
        if (name) free(name);
        if (ioid) free(ioid);
    }
    snd_device_name_free_hint(hints);
    return n;
}

static snd_pcm_t* alsa_open(const char *dev, snd_pcm_stream_t dir, int period) {
    snd_pcm_t *pcm=NULL;
    snd_pcm_hw_params_t *hw=NULL;
    if (snd_pcm_open(&pcm,dev,dir,0)<0) return NULL;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm,hw);
    snd_pcm_hw_params_set_access(pcm,hw,SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm,hw,SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm,hw,1);
    unsigned int rate = C2_SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm,hw,&rate,0);
    snd_pcm_uframes_t per = (snd_pcm_uframes_t)period;
    snd_pcm_hw_params_set_period_size_near(pcm,hw,&per,0);
    if (snd_pcm_hw_params(pcm,hw)<0) { snd_pcm_close(pcm); return NULL; }
    return pcm;
}

static int alsa_read(snd_pcm_t *pcm, int16_t *buf, int frames) {
    int n = (int)snd_pcm_readi(pcm,buf,frames);
    if (n==-EPIPE) { snd_pcm_prepare(pcm); return 0; }
    return (n>0)?n:0;
}
static int alsa_write(snd_pcm_t *pcm, const int16_t *buf, int frames) {
    int n = (int)snd_pcm_writei(pcm,buf,frames);
    if (n==-EPIPE) { snd_pcm_prepare(pcm); return 0; }
    return (n>0)?n:0;
}

/* ALSA-Capture-Buffer komplett leeren (altes Audio verwerfen).
 * Wird beim PTT-Start und PTT-Ende aufgerufen. */
static void alsa_flush_cap(void) {
    if (!pcm_cap) return;
    snd_pcm_drop(pcm_cap);
    snd_pcm_prepare(pcm_cap);
}

/* Überschüssige Samples im ALSA-Capture-Buffer lesen und verwerfen.
 * Verhindert Stau während der Airtime-Pause: ALSA füllt den Buffer
 * mit ~8 Samples/ms weiter – nach 150ms Airtime sind das ~1200
 * veraltete Samples die sonst beim nächsten Capture auftauchen. */
static void alsa_drain_excess(void) {
    if (!pcm_cap) return;
    snd_pcm_sframes_t avail = snd_pcm_avail_update(pcm_cap);
    if (avail < 0) { snd_pcm_recover(pcm_cap,(int)avail,0); return; }
    /* Alles oberhalb eines halben Frames verwerfen */
    int16_t drain[960];  /* >= max codec2 samples_per_frame */
    while (avail > (snd_pcm_sframes_t)(c2_spf/2)) {
        int r = (int)snd_pcm_readi(pcm_cap, drain,
                    (avail > c2_spf) ? c2_spf : (int)avail);
        if (r <= 0) break;
        avail -= r;
    }
}

/* ================================================================
 * TX-Thread: PTT-aktiv → capture → encode → senden
 * ================================================================ */

/* conf-Zeilen-Puffer für RX-Thread (RSSI) */
static char conf_lbuf[64];
static int  conf_llen=0;

static void build_pkt_hdr(uint8_t *out) {
    PktHdr_t h;
    memset(&h,0,sizeof(h));
    h.magic[0]=MAGIC_0; h.magic[1]=MAGIC_1; h.magic[2]=MAGIC_2;
    h.pkt_type=PKT_HDR;
    h.codec_mode=(uint8_t)CFG.codec_mode_idx;
    strncpy(h.callsign,CFG.callsign,11);
    strncpy(h.ident,"LoRaHAM Voice",13);
    memcpy(out,&h,sizeof(h));
}

static void send_raw(const void *buf, int len) {
    pthread_mutex_lock(&mtx_sock);
    if (data_fd>=0) (void)write(data_fd,buf,len);
    pthread_mutex_unlock(&mtx_sock);
}

static void *tx_thread(void *arg) {
    (void)arg;
    if (!tone_test && !pcm_cap) return NULL;
    if (!c2_enc) return NULL;

    /* Maximale Puffer-Größe (n_frames_per_pkt Frames) */
    int16_t *pcmbuf = (int16_t*)malloc(c2_spf * n_frames_per_pkt * sizeof(int16_t));
    uint8_t *encbuf = (uint8_t*)malloc(c2_Bpf * n_frames_per_pkt);
    uint8_t  pkt[sizeof(PktVoice_t)];
    uint8_t  hdrbuf[PKT_HDR_SIZE];
    if (!pcmbuf||!encbuf) { free(pcmbuf); free(encbuf); return NULL; }

    const LoRaP_t *lp = CFG.active_band ? &CFG.lora868 : &CFG.lora433;

    /* ── Fix 1: ALSA-Buffer leeren beim PTT-Start ─────────────────
     * Verhindert dass im Buffer angesammeltes Audio (z.B. von der
     * letzten Übertragung oder Hintergrundgeräusche) gesendet wird. */
    if (!tone_test) alsa_flush_cap();

    double tone_phase = 0.0;
    const double tone_inc = 2.0 * M_PI * 600.0 / (double)C2_SAMPLE_RATE;
    const int16_t tone_amp = 24576;

    /* Header sofort beim PTT-Start senden */
    build_pkt_hdr(hdrbuf);
    send_raw(hdrbuf, PKT_HDR_SIZE);
    uint32_t hdr_at = total_airtime_us(lp, PKT_HDR_SIZE) + TX_EXTRA_DELAY_MS*1000U;
    usleep(hdr_at);
    tx_hdr_ctr = 0;

    while (app_run) {
        /* ── Frames aufnehmen ─────────────────────────────────────
         * Zählt wirklich kodierte Frames – bei PTT-Release wird
         * das Teilpaket noch gesendet statt verworfen.            */
        int f_done = 0;
        for (int f = 0; f < n_frames_per_pkt; f++) {
            if (!ptt_active) break;   /* sofort abbrechen bei PTT-Los */

            if (tone_test) {
                for (int s=0; s<c2_spf; s++) {
                    pcmbuf[f*c2_spf+s] = (int16_t)(tone_amp * sin(tone_phase));
                    tone_phase += tone_inc;
                    if (tone_phase >= 2.0*M_PI) tone_phase -= 2.0*M_PI;
                }
            } else {
                int got=0;
                while (got<c2_spf) {
                    if (!ptt_active) break;
                    int r = alsa_read(pcm_cap,
                                      pcmbuf + f*c2_spf + got,
                                      c2_spf - got);
                    got += r;
                    if (!r) usleep(500);
                }
                if (got < c2_spf)
                    /* Frame unvollständig → mit Stille auffüllen */
                    memset(pcmbuf + f*c2_spf + got, 0,
                           (c2_spf - got) * sizeof(int16_t));
            }
            codec2_encode(c2_enc, encbuf + f*c2_Bpf, pcmbuf + f*c2_spf);
            f_done++;
        }

        if (f_done == 0) break;   /* PTT weg, kein Frame aufgenommen */

        /* Periodisch Header senden */
        if (tx_hdr_ctr >= HDR_INTERVAL) {
            build_pkt_hdr(hdrbuf);
            send_raw(hdrbuf, PKT_HDR_SIZE);
            usleep(hdr_at);
            tx_hdr_ctr = 0;
        }

        /* ── Voice-Paket mit tatsächlicher Frame-Anzahl senden ─── */
        int psz = PKT_VOICE_OVERHEAD + f_done * c2_Bpf;
        pkt[0]=MAGIC_0; pkt[1]=MAGIC_1; pkt[2]=MAGIC_2;
        pkt[3]=PKT_VOICE;
        uint16_t sq = tx_seq++;
        memcpy(pkt+4, &sq, 2);
        pkt[6] = (uint8_t)f_done;          /* echte Frameanzahl     */
        memcpy(pkt+7, encbuf, f_done * c2_Bpf);
        send_raw(pkt, psz);
        tx_hdr_ctr++;

        /* ── Airtime-Pause – Capture-Zeit bereits eingerechnet ────
         * Die Capture-Schleife dauert f_done × c2_fms ms – das ist
         * bereits "Wartezeit". Wir schlafen nur noch den Überschuss
         * (airtime - capture_time) + TX_EXTRA_DELAY_MS.
         * Verhindert Audio-Lücken durch zu langes Schlafen:
         * z.B. Codec2-1200 + SF7 BW250: capture=280ms > airtime=103ms
         * → nur noch 50ms Pause statt 153ms → keine Unterbrechungen. */
        uint32_t capture_us = (uint32_t)f_done * (uint32_t)c2_fms * 1000U;
        uint32_t at_needed  = total_airtime_us(lp, psz);
        uint32_t sleep_us   = (uint32_t)TX_EXTRA_DELAY_MS * 1000U;
        if (at_needed > capture_us)
            sleep_us += at_needed - capture_us;
        usleep(sleep_us);

        if (!ptt_active) break;   /* PTT freigegeben → Schleife beenden */
    }

    /* ── Übertragungsende signalisieren ──────────────────────────
     * PKT_FIN: normale Beendigung (PTT losgelassen)
     * PKT_BRK: Abbruch (Abbrechen-Button, tx_stop=1)
     * Empfänger setzt rx_in_tx=0 → nächste Übertragung erzeugt
     * neuen History-Eintrag.                                      */
    {
        PktEnd_t end;
        end.magic[0]=MAGIC_0; end.magic[1]=MAGIC_1; end.magic[2]=MAGIC_2;
        end.pkt_type = PKT_FIN;
        memset(end.callsign,0,sizeof(end.callsign));
        strncpy(end.callsign,CFG.callsign,11);
        send_raw(&end, sizeof(end));
        uint32_t end_at = total_airtime_us(lp,(int)sizeof(end))
                        + (uint32_t)TX_EXTRA_DELAY_MS*1000U;
        usleep(end_at);
    }

    /* ALSA nach PTT leeren */
    if (!tone_test) alsa_flush_cap();

    free(pcmbuf); free(encbuf);
    return NULL;
}

/* ================================================================
 * RX-Thread: net lesen → dekodieren → ALSA abspielen
 * ================================================================ */
#define RXBSZ 1024
static uint8_t rxbuf[RXBSZ];
static int     rxlen=0;

static void parse_rx(void) {
    int pos=0;
    while (rxlen-pos >= 4) {
        if (rxbuf[pos]!=MAGIC_0||rxbuf[pos+1]!=MAGIC_1||rxbuf[pos+2]!=MAGIC_2)
            { pos++; continue; }
        uint8_t pt = rxbuf[pos+3];

        if (pt==PKT_HDR) {
            if (rxlen-pos < PKT_HDR_SIZE) break;
            const PktHdr_t *h = (const PktHdr_t*)(rxbuf+pos);
            char call[13]={0}; memcpy(call,h->callsign,12);
            char cname[24]={0};
            if (h->codec_mode < N_C2_MODES)
                strncpy(cname,C2_MODES[h->codec_mode].name,23);
            else snprintf(cname,sizeof(cname),"Codec2-?(%d)",h->codec_mode);

            /* History-Eintrag nur wenn neue Übertragung:
             * - Erste Übertragung (rx_in_tx==0)
             * - Anderer Sender als bisher
             * Wiederholte PKT_HDR von derselben Station → kein neuer Eintrag */
            int is_new = (!rx_in_tx || strcmp(call,rx_cur_call)!=0);
            if (is_new) {
                hist_add(call,cname,(int)last_rssi);
                strncpy(rx_cur_call,call,12);
                rx_in_tx=1;
            }

            /* RX-Decoder auf gesendeten Codec umschalten */
            int new_mode = (int)h->codec_mode;
            if (new_mode != c2_rx_mode && new_mode < N_C2_MODES) {
                if (c2_rx_dec) { codec2_destroy(c2_rx_dec); c2_rx_dec=NULL; }
                c2_rx_dec = codec2_create(C2_MODES[new_mode].id);
                if (c2_rx_dec) {
                    c2_rx_spf = codec2_samples_per_frame(c2_rx_dec);
                    c2_rx_Bpf = (codec2_bits_per_frame(c2_rx_dec)+7)/8;
                    c2_rx_mode = new_mode;
                }
            }
            pos += PKT_HDR_SIZE;

        } else if (pt==PKT_FIN || pt==PKT_BRK) {
            /* Übertragung beendet – nächster PKT_HDR erzeugt neuen Eintrag */
            if (rxlen-pos < (int)sizeof(PktEnd_t)) break;
            rx_in_tx = 0;
            rx_cur_call[0] = '\0';
            pos += (int)sizeof(PktEnd_t);

        } else if (pt==PKT_VOICE) {
            if (rxlen-pos < 7) break;
            int nf  = rxbuf[pos+6];
            /* RX-Decoder nutzen (Codec des Senders), nicht TX-Decoder */
            if (!c2_rx_dec || nf<1 || nf>32) { pos++; continue; }
            int psz = PKT_VOICE_OVERHEAD + nf*c2_rx_Bpf;
            if (rxlen-pos < psz) break;
            const uint8_t *data = rxbuf+pos+7;
            if (pcm_play) {
                int16_t *pcm=(int16_t*)malloc(nf*c2_rx_spf*sizeof(int16_t));
                if (pcm) {
                    for (int f=0;f<nf;f++)
                        codec2_decode(c2_rx_dec,pcm+f*c2_rx_spf,data+f*c2_rx_Bpf);
                    alsa_write(pcm_play, pcm, nf*c2_rx_spf);
                    free(pcm);
                }
            }
            pos += psz;
        } else { pos++; }
    }
    if (pos>0) { rxlen-=pos; if(rxlen>0) memmove(rxbuf,rxbuf+pos,rxlen); }
}

static void *rx_thread(void *arg) {
    (void)arg;
    uint8_t tmp[256];
    /* GETRSSI aktivieren */
    if (conf_fd>=0) {
        const char *cmd = "SET GETRSSI=1\n";  // ToDo: Die RSSI ist noch etwas instabil, da der Wert vor dem Empfang stammt. Das erkennt man am Sprung von z.b. -100 dBm zu -73dBm
        (void)write(conf_fd,cmd,strlen(cmd));
    }
    while (app_run) {
        if (data_fd<0 || conf_fd<0) { usleep(500000); continue; }
        fd_set fds; FD_ZERO(&fds);
        FD_SET(data_fd,&fds);
        FD_SET(conf_fd,&fds);
        int mfd=(data_fd>conf_fd?data_fd:conf_fd)+1;
        struct timeval tv={0,10000};
        if (select(mfd,&fds,NULL,NULL,&tv)<=0) continue;

        if (FD_ISSET(data_fd,&fds)) {
            ssize_t n=read(data_fd,tmp,sizeof(tmp));
            if (n>0) {
                if (rxlen+(int)n>RXBSZ) rxlen=0;
                memcpy(rxbuf+rxlen,tmp,n); rxlen+=(int)n;
                parse_rx();
            } else if (n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK)) {
                close(data_fd); data_fd=-1;
            }
        }
        if (FD_ISSET(conf_fd,&fds)) {
            char ct[64]; ssize_t cn=read(conf_fd,ct,sizeof(ct));
            if (cn>0) {
                for (int i=0;i<(int)cn;i++) {
                    if (conf_llen<63) conf_lbuf[conf_llen++]=ct[i];
                    if (ct[i]=='\n') {
                        conf_lbuf[conf_llen]='\0';
                        if (strncmp(conf_lbuf,"RSSI=",5)==0)
                            last_rssi=(float)str2flt(conf_lbuf+5);
                        conf_llen=0;
                    }
                }
            }
        }
    }
    return NULL;
}

/* ================================================================
 * CLI (ncurses)
 * ================================================================ */
static WINDOW *cli_hist_win=NULL;
static WINDOW *cli_status_win=NULL;
static WINDOW *cli_bottom_win=NULL;
static int    cli_hist_scroll=0;
static int    cli_in_config=0;

static void cli_redraw_hist(void) {
    if (!cli_hist_win) return;
    int rows,cols; getmaxyx(cli_hist_win,rows,cols); (void)cols;
    werase(cli_hist_win);
    int cnt = g_hist_count;
    int start = cnt - rows - cli_hist_scroll;
    if (start<0) start=0;
    for (int i=0; i<rows && (start+i)<cnt; i++) {
        HistEntry_t e; hist_get(start+i,&e);
        mvwprintw(cli_hist_win,i,0,"[%s] %-10s %-14s RSSI:%ddBm",
                  e.ts,e.call,e.codec,e.rssi);
    }
    wrefresh(cli_hist_win);
}

static void cli_redraw_status(void) {
    if (!cli_status_win) return;
    werase(cli_status_win);
    const LoRaP_t *lp = CFG.active_band ? &CFG.lora868 : &CFG.lora433;
    char fs[16]; flt2str(fs,sizeof(fs),"%.3f",lp->freq);
    int chk = check_voice_mode(lp,CFG.duplex,NULL);
    const char *warn = (chk==1)?" [VERBOTEN!]":(chk==2)?" [DUPLEX VERBOTEN!]":"";
    mvwprintw(cli_status_win,0,0,
        " LoRaHAM Voice | %s | %sMHz %s SF%d BW%.0f | TX:%s | RX:%s | RSSI:%.0fdBm%s",
        CFG.callsign,
        CFG.active_band?"868":"433",
        fs, lp->sf, (double)lp->bw,
        (CFG.codec_mode_idx<N_C2_MODES)?C2_MODES[CFG.codec_mode_idx].name:"?",
        (c2_rx_mode>=0&&c2_rx_mode<N_C2_MODES)?C2_MODES[c2_rx_mode].name:"–",
        (double)last_rssi, warn);
    wrefresh(cli_status_win);
}

static void cli_redraw_bottom(void) {
    if (!cli_bottom_win) return;
    werase(cli_bottom_win);
    if (ptt_active)
        mvwprintw(cli_bottom_win,0,0,"[ SENDE%s ] [C]onfig [Q]uit [PgUp/PgDn] Scrollen",
                  tone_test?" (600Hz TESTTON)":"...");
    else
        mvwprintw(cli_bottom_win,0,0,"[SPACE=PTT] [T]estton%s [C]onfig [Q]uit | %s",
                  tone_test?"=AN":"=AUS",
                  data_fd>=0?"Verbunden":"NICHT verbunden");
    wrefresh(cli_bottom_win);
}

static void cli_setup_windows(void) {
    int rows,cols; getmaxyx(stdscr,rows,cols); (void)cols;
    if (cli_status_win) { delwin(cli_status_win); }
    if (cli_hist_win)   { delwin(cli_hist_win);   }
    if (cli_bottom_win) { delwin(cli_bottom_win); }
    cli_status_win = newwin(1,     cols, 0,      0);
    cli_hist_win   = newwin(rows-3,cols, 1,      0);
    cli_bottom_win = newwin(1,     cols, rows-1, 0);
    wbkgd(cli_status_win, COLOR_PAIR(1));
    scrollok(cli_hist_win,FALSE);
}

/* ================================================================
 * CLI Konfigurationsmenü – TUI mit Pfeiltasten-Navigation
 * ================================================================ */

/* BW-Lookup-Tabelle */
static const double BW_TABLE[] =
    {7.8,10.4,15.6,20.8,31.25,41.7,62.5,125.0,250.0,500.0};
#define N_BW 10

static int bw_to_idx(double bw) {
    for(int i=0;i<N_BW;i++) if(fabs(BW_TABLE[i]-bw)<0.01) return i;
    return 7;
}

/* Feldnamen */
static const char *CFG_LABELS[] = {
    /* 0-5 Allgemein */
    "Rufzeichen","Aktives Band","Codec2-Modus",
    "Duplex","Audio Aufnahme","Audio Ausgabe",
    /* 6-14 LoRa 433 */
    "Frequenz MHz","SF","BW kHz","CR","CRC",
    "Preamble","Sync Word","Power dBm","LDRO",
    /* 15-23 LoRa 868 */
    "Frequenz MHz","SF","BW kHz","CR","CRC",
    "Preamble","Sync Word","Power dBm","LDRO",
};
#define N_CFG_FIELDS 24

/* Aktuellen Feldwert als String */
static void cfg_get_val(int f, char *buf, int sz) {
    char fs[20];
    switch(f){
    case  0: strncpy(buf,CFG.callsign,sz); break;
    case  1: snprintf(buf,sz,"%s",CFG.active_band?"868 MHz":"433 MHz"); break;
    case  2: snprintf(buf,sz,"%s",
                 CFG.codec_mode_idx<N_C2_MODES?
                 C2_MODES[CFG.codec_mode_idx].name:"?"); break;
    case  3: snprintf(buf,sz,"%s",CFG.duplex?"AN":"AUS"); break;
    case  4: strncpy(buf,CFG.audio_capture,sz); break;
    case  5: strncpy(buf,CFG.audio_playback,sz); break;
    case  6: flt2str(fs,sizeof(fs),"%.3f",CFG.lora433.freq);
             strncpy(buf,fs,sz); break;
    case  7: snprintf(buf,sz,"%d",CFG.lora433.sf); break;
    case  8: flt2str(fs,sizeof(fs),"%.1f",CFG.lora433.bw);
             strncpy(buf,fs,sz); break;
    case  9: snprintf(buf,sz,"%d",CFG.lora433.cr); break;
    case 10: snprintf(buf,sz,"%d",CFG.lora433.crc); break;
    case 11: snprintf(buf,sz,"%d",CFG.lora433.preamble); break;
    case 12: snprintf(buf,sz,"0x%02X",CFG.lora433.sync); break;
    case 13: snprintf(buf,sz,"%d",CFG.lora433.power); break;
    case 14: snprintf(buf,sz,"%s",CFG.lora433.ldro==2?"AUTO":
                 CFG.lora433.ldro==1?"AN":"AUS"); break;
    case 15: flt2str(fs,sizeof(fs),"%.3f",CFG.lora868.freq);
             strncpy(buf,fs,sz); break;
    case 16: snprintf(buf,sz,"%d",CFG.lora868.sf); break;
    case 17: flt2str(fs,sizeof(fs),"%.1f",CFG.lora868.bw);
             strncpy(buf,fs,sz); break;
    case 18: snprintf(buf,sz,"%d",CFG.lora868.cr); break;
    case 19: snprintf(buf,sz,"%d",CFG.lora868.crc); break;
    case 20: snprintf(buf,sz,"%d",CFG.lora868.preamble); break;
    case 21: snprintf(buf,sz,"0x%02X",CFG.lora868.sync); break;
    case 22: snprintf(buf,sz,"%d",CFG.lora868.power); break;
    case 23: snprintf(buf,sz,"%s",CFG.lora868.ldro==2?"AUTO":
                 CFG.lora868.ldro==1?"AN":"AUS"); break;
    default: buf[0]='\0';
    }
}

/* Ist das Feld ein Auswahlfeld (Leertaste/Enter zyklisch) */
static int cfg_is_sel(int f){
    return(f==1||f==2||f==3||f==8||f==10||f==14||f==17||f==19||f==23);
}

/* Auswahlfeld weiterschalten */
static void cfg_cycle(int f){
    switch(f){
    case  1: CFG.active_band=!CFG.active_band; break;
    case  2: CFG.codec_mode_idx=(CFG.codec_mode_idx+1)%N_C2_MODES; break;
    case  3: CFG.duplex=!CFG.duplex; break;
    case  8: { int i=(bw_to_idx(CFG.lora433.bw)+1)%N_BW;
               CFG.lora433.bw=BW_TABLE[i]; } break;
    case 10: CFG.lora433.crc=!CFG.lora433.crc; break;
    case 14: CFG.lora433.ldro=(CFG.lora433.ldro+1)%3; break;
    case 17: { int i=(bw_to_idx(CFG.lora868.bw)+1)%N_BW;
               CFG.lora868.bw=BW_TABLE[i]; } break;
    case 19: CFG.lora868.crc=!CFG.lora868.crc; break;
    case 23: CFG.lora868.ldro=(CFG.lora868.ldro+1)%3; break;
    }
}

/* Texteingabe anwenden */
static void cfg_set_val(int f, const char *s){
    int v; double d;
    switch(f){
    case  0: { char t[13]; strncpy(t,s,12); t[12]='\0';
               for(int i=0;t[i];i++) t[i]=(char)toupper((unsigned char)t[i]);
               strncpy(CFG.callsign,t,12); } break;
    case  4: strncpy(CFG.audio_capture,s,63); break;
    case  5: strncpy(CFG.audio_playback,s,63); break;
    case  6: d=str2flt(s); if(d>0) CFG.lora433.freq=d; break;
    case  7: v=atoi(s); if(v>=7&&v<=12) CFG.lora433.sf=v; break;
    case  9: v=atoi(s); if(v>=5&&v<=8)  CFG.lora433.cr=v; break;
    case 11: v=atoi(s); if(v>=6)         CFG.lora433.preamble=v; break;
    case 12: CFG.lora433.sync=(uint8_t)strtoul(s,NULL,16); break;
    case 13: v=atoi(s); if(v>=0&&v<=20)  CFG.lora433.power=v; break;
    case 15: d=str2flt(s); if(d>0) CFG.lora868.freq=d; break;
    case 16: v=atoi(s); if(v>=7&&v<=12) CFG.lora868.sf=v; break;
    case 18: v=atoi(s); if(v>=5&&v<=8)  CFG.lora868.cr=v; break;
    case 20: v=atoi(s); if(v>=6)         CFG.lora868.preamble=v; break;
    case 21: CFG.lora868.sync=(uint8_t)strtoul(s,NULL,16); break;
    case 22: v=atoi(s); if(v>=0&&v<=20)  CFG.lora868.power=v; break;
    }
}

/* Gesamten Konfig-Screen zeichnen */
static void cli_cfg_draw(WINDOW *w, int cur){
    int rows,cols; getmaxyx(w,rows,cols); (void)cols;
    werase(w);
    /* Titelzeile */
    wattron(w,A_BOLD|COLOR_PAIR(1));
    mvwhline(w,0,0,' ',cols);
    mvwprintw(w,0,2,"LoRaHAM Voice - Konfiguration");
    wattroff(w,A_BOLD|COLOR_PAIR(1));

    int y=2;
    /* --- Allgemein --- */
    wattron(w,A_BOLD);
    mvwprintw(w,y++,1,"--- Allgemein ---");
    wattroff(w,A_BOLD);
    for(int f=0;f<6;f++,y++){
        char val[28]; cfg_get_val(f,val,sizeof(val));
        if(f==cur) wattron(w,A_REVERSE);
        mvwprintw(w,y,2,"%-16s: %-26s",CFG_LABELS[f],val);
        if(f==cur) wattroff(w,A_REVERSE);
    }
    /* --- LoRa 433 MHz --- */
    y++;
    wattron(w,A_BOLD);
    mvwprintw(w,y++,1,"--- LoRa 433 MHz ---");
    wattroff(w,A_BOLD);
    for(int f=6;f<15;f++,y++){
        char val[28]; cfg_get_val(f,val,sizeof(val));
        if(f==cur) wattron(w,A_REVERSE);
        mvwprintw(w,y,2,"%-16s: %-26s",CFG_LABELS[f],val);
        if(f==cur) wattroff(w,A_REVERSE);
    }
    /* --- LoRa 868 MHz --- */
    y++;
    wattron(w,A_BOLD);
    mvwprintw(w,y++,1,"--- LoRa 868 MHz ---");
    wattroff(w,A_BOLD);
    for(int f=15;f<N_CFG_FIELDS;f++,y++){
        char val[28]; cfg_get_val(f,val,sizeof(val));
        if(f==cur) wattron(w,A_REVERSE);
        mvwprintw(w,y,2,"%-16s: %-26s",CFG_LABELS[f],val);
        if(f==cur) wattroff(w,A_REVERSE);
    }
    /* Fußzeile */
    if(rows>2){
        mvwprintw(w,rows-2,1,
            "[Pfeile] Navigieren  [Enter/Leerzeichen] Aendern");
        mvwprintw(w,rows-1,1,
            "[S] Speichern & Beenden  [ESC] Abbrechen ohne Speichern");
    }
    wrefresh(w);
}

static void cli_config_menu(void) {
    ptt_in_config=1; cli_in_config=1;
    int rows,cols; getmaxyx(stdscr,rows,cols);
    WINDOW *w=newwin(rows,cols,0,0);
    keypad(w,TRUE);
    nodelay(w,FALSE);   /* blockierendes getch für Konfig */
    cbreak(); noecho(); curs_set(0);

    int cur=0;
    cli_cfg_draw(w,cur);

    while(1){
        int ch=wgetch(w);
        if(ch==27||ch=='q'||ch=='Q') break; /* ESC = Abbrechen */

        if(ch=='s'||ch=='S'){
            c2_init(CFG.codec_mode_idx);
            apply_lora_params();
            config_save();
            mvwprintw(w,rows-1,1,"  Gespeichert!                                     ");
            wrefresh(w);
            usleep(800000);
            break;
        }
        if(ch==KEY_UP  &&cur>0)              cur--;
        if(ch==KEY_DOWN&&cur<N_CFG_FIELDS-1) cur++;

        if(ch=='\n'||ch=='\r'||ch==' '){
            if(cfg_is_sel(cur)){
                cfg_cycle(cur);
            } else {
                /* Texteingabe unten im Fenster */
                char curval[28]; cfg_get_val(cur,curval,sizeof(curval));
                mvwprintw(w,rows-1,1,"> %-20s : ",CFG_LABELS[cur]);
                wclrtoeol(w);
                echo(); curs_set(1); nocbreak();
                char buf[48]={0};
                mvwprintw(w,rows-1,26,"%s",curval);
                wmove(w,rows-1,26); wclrtoeol(w);
                wgetnstr(w,buf,40);
                noecho(); curs_set(0); cbreak();
                wmove(w,rows-1,0); wclrtoeol(w);
                if(buf[0]) cfg_set_val(cur,buf);
            }
        }
        cli_cfg_draw(w,cur);
    }
    delwin(w);
    clear(); refresh();
    cli_setup_windows();
    /* halfdelay zurücksetzen */
    halfdelay(1);
    ptt_in_config=0; cli_in_config=0;
}

static void cli_main(void) {
    initscr(); cbreak(); noecho(); keypad(stdscr,TRUE);
    start_color(); use_default_colors();
    init_pair(1,COLOR_BLACK,COLOR_CYAN);
    curs_set(0);
    halfdelay(1); /* 100ms getch-Timeout */

    cli_setup_windows();
    cli_redraw_status();
    cli_redraw_hist();
    cli_redraw_bottom();

    struct timespec last_space={0,0};
    pthread_t tid_tx={0}, tid_rx;
    pthread_create(&tid_rx,NULL,rx_thread,NULL);
    int tick=0;

    while (app_run) {
        int ch=getch();

        if (!cli_in_config) {
            if (ch==' ') {
                clock_gettime(CLOCK_MONOTONIC,&last_space);
                if (!ptt_active) {
                    ptt_active=1;
                    pthread_create(&tid_tx,NULL,tx_thread,NULL);
                    pthread_detach(tid_tx);
                }
            }
            if (ch=='q'||ch=='Q') { app_run=0; break; }
            if (ch=='c'||ch=='C') { cli_config_menu(); }
            if (ch=='t'||ch=='T') {
                tone_test = !tone_test;
                cli_redraw_bottom();
            }
            if (ch==KEY_UP)   { cli_hist_scroll++; cli_redraw_hist(); }
            if (ch==KEY_DOWN) {
                if (cli_hist_scroll>0) cli_hist_scroll--;
                cli_redraw_hist();
            }
        }

        /* PTT Auto-Release bei Leertaste nicht mehr gehalten */
        if (ptt_active && !cli_in_config) {
            struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
            long ms = (now.tv_sec-last_space.tv_sec)*1000
                     +(now.tv_nsec-last_space.tv_nsec)/1000000;
            if (ms > PTT_RELEASE_MS) ptt_active=0;
        }

        tick++;
        if (tick>=5) { /* alle 500ms Status refreshen */
            tick=0;
            cli_redraw_status();
            cli_redraw_hist();
            cli_redraw_bottom();
        }
    }
    ptt_active=0;
    endwin();
}

/* ================================================================
 * GTK UI  (komplett ausgeblendet bei -DNO_GTK)
 * ================================================================ */
#ifndef NO_GTK
static GtkWidget *g_win=NULL;
static GtkWidget *g_btn_ptt=NULL;
static GtkWidget *g_lbl_status=NULL;
static GtkWidget *g_lbl_rssi=NULL;
static GtkTextBuffer *g_hist_buf=NULL;
static GtkWidget *g_txt_hist=NULL;

/* Hilfsmakro für LoRa-Parameter-Zeile im Dialog */
static void gui_hist_append(const char *call, const char *codec, int rssi) {
    if (!g_hist_buf) return;
    time_t t=time(NULL); struct tm *tm=localtime(&t);
    char line[128];
    snprintf(line,sizeof(line),"[%02d:%02d:%02d] %-10s %-14s RSSI:%ddBm\n",
             tm->tm_hour,tm->tm_min,tm->tm_sec,call,codec,rssi);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(g_hist_buf,&end);
    gtk_text_buffer_insert(g_hist_buf,&end,line,-1);
    /* Auto-Scroll */
    gtk_text_iter_forward_to_end(&end);
    GtkTextMark *m=gtk_text_buffer_get_mark(g_hist_buf,"insert");
    if(m) gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(g_txt_hist),m);
}

/* Periodischer GUI-Update-Timer */
static gboolean gui_timer(gpointer ud) {
    (void)ud;
    if (!g_lbl_rssi) return G_SOURCE_CONTINUE;
    char rs[32]; snprintf(rs,sizeof(rs),"RSSI: %.0f dBm",last_rssi);
    gtk_label_set_text(GTK_LABEL(g_lbl_rssi),rs);

    /* History-Einträge in TextView übertragen */
    static int last_hist=-1;
    if (g_hist_count != last_hist) {
        /* Nur neue Einträge anhängen */
        int start = (last_hist<0)?0:last_hist;
        for (int i=start;i<g_hist_count;i++) {
            HistEntry_t e; hist_get(i,&e);
            gui_hist_append(e.call,e.codec,e.rssi);
        }
        last_hist=g_hist_count;
    }

    /* Status-Label */
    const LoRaP_t *lp = CFG.active_band ? &CFG.lora868 : &CFG.lora433;
    char fs[16]; flt2str(fs,sizeof(fs),"%.3f",lp->freq);
    int chk=check_voice_mode(lp,CFG.duplex,NULL);
    char st[160];
    snprintf(st,sizeof(st),"%s | %sMHz %s SF%d BW%.0f | TX:%s | RX:%s%s",
             CFG.callsign,
             CFG.active_band?"868":"433",
             fs, lp->sf, (double)lp->bw,
             (CFG.codec_mode_idx<N_C2_MODES)?C2_MODES[CFG.codec_mode_idx].name:"?",
             (c2_rx_mode>=0&&c2_rx_mode<N_C2_MODES)?C2_MODES[c2_rx_mode].name:"–",
             (chk==1)?" VERBOTEN!":(chk==2)?" DUPLEX VERBOTEN!":"");
    gtk_label_set_text(GTK_LABEL(g_lbl_status),st);
    return G_SOURCE_CONTINUE;
}

static void gui_ptt_press(GtkWidget *w, gpointer ud) {
    (void)w;(void)ud;
    if (ptt_in_config||ptt_active) return;
    if (data_fd<0) {
        gtk_label_set_text(GTK_LABEL(g_lbl_status),"Nicht verbunden!");
        return;
    }
    ptt_active=1;
    pthread_t tid; pthread_create(&tid,NULL,tx_thread,NULL); pthread_detach(tid);
    gtk_button_set_label(GTK_BUTTON(g_btn_ptt),"● SENDE...");
}
static void gui_ptt_release(GtkWidget *w, gpointer ud) {
    (void)w;(void)ud;
    ptt_active=0;
    gtk_button_set_label(GTK_BUTTON(g_btn_ptt),"SPRECHEN  [Leertaste]");
}

static gboolean gui_key_press(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w;(void)ud;
    if (ptt_in_config) return FALSE;
    if (ev->keyval==GDK_KEY_space) { gui_ptt_press(NULL,NULL); return TRUE; }
    return FALSE;
}
static gboolean gui_key_release(GtkWidget *w, GdkEventKey *ev, gpointer ud) {
    (void)w;(void)ud;
    if (ev->keyval==GDK_KEY_space) { gui_ptt_release(NULL,NULL); return TRUE; }
    return FALSE;
}

/* LoRa-Parameter-Block im Dialog */
static void dlg_lp_row(GtkWidget *grid, int row, const char *lbl,
                        GtkWidget **w_out, const char *val) {
    GtkWidget *l=gtk_label_new(lbl);
    gtk_widget_set_halign(l,GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid),l,0,row,1,1);
    *w_out=gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(*w_out),val);
    gtk_grid_attach(GTK_GRID(grid),*w_out,1,row,1,1);
}
static void dlg_lp_spin(GtkWidget *grid, int row, const char *lbl,
                         GtkWidget **w_out, double lo, double hi, double val) {
    GtkWidget *l=gtk_label_new(lbl);
    gtk_widget_set_halign(l,GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid),l,0,row,1,1);
    *w_out=gtk_spin_button_new_with_range(lo,hi,1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(*w_out),val);
    gtk_grid_attach(GTK_GRID(grid),*w_out,1,row,1,1);
}

static GtkWidget *make_lp_page(const LoRaP_t *lp,
    GtkWidget **ef, GtkWidget **sf, GtkWidget **bwf,
    GtkWidget **crf, GtkWidget **crcf, GtkWidget **pref,
    GtkWidget **syf, GtkWidget **pwf, GtkWidget **ldf) {
    GtkWidget *grid=gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid),6);
    gtk_grid_set_column_spacing(GTK_GRID(grid),10);
    gtk_container_set_border_width(GTK_CONTAINER(grid),10);
    char tmp[32];
    flt2str(tmp,sizeof(tmp),"%.3f",lp->freq);
    dlg_lp_row(grid,0,"Frequenz (MHz):",ef,tmp);
    dlg_lp_spin(grid,1,"SF (7-12):",sf,7,12,lp->sf);
    /* BW Combo */
    {GtkWidget *l=gtk_label_new("Bandwidth (kHz):");
    gtk_widget_set_halign(l,GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid),l,0,2,1,1);
    *bwf=gtk_combo_box_text_new();
    const char *bws[]={"7.8","10.4","15.6","20.8","31.25","41.7","62.5","125.0","250.0","500.0",NULL};
    int sel=7;
    for(int i=0;bws[i];i++){
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*bwf),bws[i]);
        if(fabs(atof(bws[i])-lp->bw)<0.01) sel=i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(*bwf),sel);
    gtk_grid_attach(GTK_GRID(grid),*bwf,1,2,1,1);}
    dlg_lp_spin(grid,3,"CR (5-8):",crf,5,8,lp->cr);
    dlg_lp_spin(grid,4,"CRC:",crcf,0,1,lp->crc);
    dlg_lp_spin(grid,5,"Präambel:",pref,6,65535,lp->preamble);
    snprintf(tmp,sizeof(tmp),"0x%02X",lp->sync);
    dlg_lp_row(grid,6,"Sync Word:",syf,tmp);
    dlg_lp_spin(grid,7,"Power (0-20):",pwf,0,20,lp->power);
    /* LDRO */
    {GtkWidget *l=gtk_label_new("LDRO:");
    gtk_widget_set_halign(l,GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid),l,0,8,1,1);
    *ldf=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*ldf),"0 – AUS");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*ldf),"1 – AN");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(*ldf),"AUTO");
    gtk_combo_box_set_active(GTK_COMBO_BOX(*ldf),lp->ldro>=0&&lp->ldro<=2?lp->ldro:2);
    gtk_grid_attach(GTK_GRID(grid),*ldf,1,8,1,1);}
    return grid;
}

static void lp_from_dlg(LoRaP_t *lp,
    GtkWidget *ef,GtkWidget *sf,GtkWidget *bwf,
    GtkWidget *crf,GtkWidget *crcf,GtkWidget *pref,
    GtkWidget *syf,GtkWidget *pwf,GtkWidget *ldf) {
    lp->freq=str2flt(gtk_entry_get_text(GTK_ENTRY(ef)));
    lp->sf=(int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(sf));
    gchar *bs=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(bwf));
    lp->bw=bs?str2flt(bs):125.0; g_free(bs);
    lp->cr=(int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(crf));
    lp->crc=(int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(crcf));
    lp->preamble=(int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pref));
    lp->sync=(uint8_t)strtoul(gtk_entry_get_text(GTK_ENTRY(syf)),NULL,16);
    lp->power=(int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(pwf));
    lp->ldro=gtk_combo_box_get_active(GTK_COMBO_BOX(ldf));
}

static void gui_config_dialog(void) {
    ptt_in_config=1;
    GtkWidget *dlg=gtk_dialog_new_with_buttons(
        "LoRaHAM Voice Konfiguration",GTK_WINDOW(g_win),
        GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK",GTK_RESPONSE_OK,"_Abbrechen",GTK_RESPONSE_CANCEL,NULL);
    gtk_window_set_resizable(GTK_WINDOW(dlg),FALSE);
    GtkWidget *nb=gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb),8);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       nb,TRUE,TRUE,0);

    /* --- Allgemein --- */
    GtkWidget *gen=gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(gen),6);
    gtk_grid_set_column_spacing(GTK_GRID(gen),10);
    gtk_container_set_border_width(GTK_CONTAINER(gen),10);
    int r=0;
    #define GL(txt,row) { GtkWidget *_l=gtk_label_new(txt); \
        gtk_widget_set_halign(_l,GTK_ALIGN_END); \
        gtk_grid_attach(GTK_GRID(gen),_l,0,row,1,1); }
    GL("Rufzeichen:",r);
    GtkWidget *e_call=gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(e_call),12);
    gtk_entry_set_text(GTK_ENTRY(e_call),CFG.callsign);
    gtk_grid_attach(GTK_GRID(gen),e_call,1,r++,1,1);
    GL("Aktives Band:",r);
    GtkWidget *c_band=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_band),"433 MHz");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_band),"868 MHz");
    gtk_combo_box_set_active(GTK_COMBO_BOX(c_band),CFG.active_band);
    gtk_grid_attach(GTK_GRID(gen),c_band,1,r++,1,1);
    GL("Codec2-Modus:",r);
    GtkWidget *c_codec=gtk_combo_box_text_new();
    for(int i=0;i<N_C2_MODES;i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_codec),C2_MODES[i].name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c_codec),CFG.codec_mode_idx);
    gtk_grid_attach(GTK_GRID(gen),c_codec,1,r++,1,1);
    GL("Duplex (gleiche Freq.):",r);
    GtkWidget *chk_dup=gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_dup),CFG.duplex);
    gtk_grid_attach(GTK_GRID(gen),chk_dup,1,r++,1,1);
    GL("Audio Capture:",r);
    GtkWidget *c_cap=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_cap),"default");
    { char devs[16][64]; int nd=alsa_list_devices(devs,16);
      for(int i=0;i<nd;i++) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_cap),devs[i]); }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c_cap),0);
    gtk_grid_attach(GTK_GRID(gen),c_cap,1,r++,1,1);
    #undef GL
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),gen,gtk_label_new("Allgemein"));

    /* --- 433 MHz Tab --- */
    GtkWidget *ef4,*sf4,*bwf4,*crf4,*crcf4,*pref4,*syf4,*pwf4,*ldf4;
    GtkWidget *pg433=make_lp_page(&CFG.lora433,
        &ef4,&sf4,&bwf4,&crf4,&crcf4,&pref4,&syf4,&pwf4,&ldf4);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),pg433,gtk_label_new("433 MHz"));

    /* --- 868 MHz Tab --- */
    GtkWidget *ef8,*sf8,*bwf8,*crf8,*crcf8,*pref8,*syf8,*pwf8,*ldf8;
    GtkWidget *pg868=make_lp_page(&CFG.lora868,
        &ef8,&sf8,&bwf8,&crf8,&crcf8,&pref8,&syf8,&pwf8,&ldf8);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb),pg868,gtk_label_new("868 MHz"));

    gtk_widget_show_all(dlg);

    if (gtk_dialog_run(GTK_DIALOG(dlg))==GTK_RESPONSE_OK) {
        /* Allgemein */
        const char *cs=gtk_entry_get_text(GTK_ENTRY(e_call));
        strncpy(CFG.callsign,cs,12);
        for(int i=0;CFG.callsign[i];i++)
            CFG.callsign[i]=(char)toupper((unsigned char)CFG.callsign[i]);
        CFG.active_band=gtk_combo_box_get_active(GTK_COMBO_BOX(c_band));
        CFG.codec_mode_idx=gtk_combo_box_get_active(GTK_COMBO_BOX(c_codec));
        if(CFG.codec_mode_idx<0) CFG.codec_mode_idx=0;
        CFG.duplex=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk_dup))?1:0;
        gchar *cap=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c_cap));
        if(cap){strncpy(CFG.audio_capture,cap,63);g_free(cap);}
        /* LoRa 433 */
        lp_from_dlg(&CFG.lora433,ef4,sf4,bwf4,crf4,crcf4,pref4,syf4,pwf4,ldf4);
        /* LoRa 868 */
        lp_from_dlg(&CFG.lora868,ef8,sf8,bwf8,crf8,crcf8,pref8,syf8,pwf8,ldf8);

        c2_init(CFG.codec_mode_idx);
        apply_lora_params();
        config_save();
    }
    gtk_widget_destroy(dlg);
    ptt_in_config=0;
}

static gboolean gui_delete(GtkWidget *w, GdkEvent *ev, gpointer ud) {
    (void)w;(void)ev;(void)ud;
    app_run=0; ptt_active=0;
    config_save();
    gtk_main_quit();
    return TRUE;  /* TRUE = Event selbst behandelt, Fenster bleibt bis gtk_main_quit */
}

static void gui_audio_select_dialog(void) {
    char devs[16][64];
    int nd = alsa_list_devices(devs,16);
    if (nd<=1) return; /* nichts zu wählen */

    GtkWidget *dlg=gtk_dialog_new_with_buttons(
        "Audiogerät wählen",GTK_WINDOW(g_win),
        GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK",GTK_RESPONSE_OK,NULL);
    gtk_window_set_resizable(GTK_WINDOW(dlg),FALSE);
    GtkWidget *grid=gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid),8);
    gtk_grid_set_column_spacing(GTK_GRID(grid),10);
    gtk_container_set_border_width(GTK_CONTAINER(grid),16);
    gtk_box_pack_start(GTK_BOX(
        gtk_dialog_get_content_area(GTK_DIALOG(dlg))),grid,FALSE,FALSE,0);

    GtkWidget *lbl=gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl),
        "<b>Audiogerät auswählen</b>\n"
        "Tipp: <i>pulse</i> → PulseAudio/Bluetooth-Headset");
    gtk_grid_attach(GTK_GRID(grid),lbl,0,0,2,1);

    gtk_grid_attach(GTK_GRID(grid),gtk_label_new("Aufnahme:"),0,1,1,1);
    GtkWidget *c_cap=gtk_combo_box_text_new();
    for(int i=0;i<nd;i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_cap),devs[i]);
    /* Standard auf "pulse" oder ersten Eintrag */
    int sel_cap=0;
    for(int i=0;i<nd;i++) if(!strcmp(devs[i],"pulse")){sel_cap=i;break;}
    gtk_combo_box_set_active(GTK_COMBO_BOX(c_cap),sel_cap);
    gtk_grid_attach(GTK_GRID(grid),c_cap,1,1,1,1);

    gtk_grid_attach(GTK_GRID(grid),gtk_label_new("Wiedergabe:"),0,2,1,1);
    GtkWidget *c_play=gtk_combo_box_text_new();
    for(int i=0;i<nd;i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(c_play),devs[i]);
    int sel_play=sel_cap;
    gtk_combo_box_set_active(GTK_COMBO_BOX(c_play),sel_play);
    gtk_grid_attach(GTK_GRID(grid),c_play,1,2,1,1);

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));

    gchar *cap =gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c_cap));
    gchar *play=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c_play));
    if(cap) { strncpy(CFG.audio_capture, cap, 63);  g_free(cap);  }
    if(play){ strncpy(CFG.audio_playback,play,63);  g_free(play); }
    gtk_widget_destroy(dlg);
    config_save();
}

static void gui_tone_toggled(GtkToggleButton *b, gpointer ud) {
    (void)ud;
    tone_test = gtk_toggle_button_get_active(b) ? 1 : 0;
}

static void gui_main(int *argc, char ***argv) {
    gtk_init(argc,argv);
    g_win=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win),"LoRaHAM Voice");
    gtk_window_set_default_size(GTK_WINDOW(g_win),520,420);
    gtk_window_set_position(GTK_WINDOW(g_win),GTK_WIN_POS_CENTER);
    g_signal_connect(g_win,"delete-event",G_CALLBACK(gui_delete),NULL);
    g_signal_connect(g_win,"key-press-event",G_CALLBACK(gui_key_press),NULL);
    g_signal_connect(g_win,"key-release-event",G_CALLBACK(gui_key_release),NULL);

    GtkWidget *vbox=gtk_box_new(GTK_ORIENTATION_VERTICAL,6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox),8);
    gtk_container_add(GTK_CONTAINER(g_win),vbox);

    /* Status + RSSI */
    GtkWidget *hst=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    g_lbl_status=gtk_label_new("Initialisiere…");
    gtk_widget_set_halign(g_lbl_status,GTK_ALIGN_START);
    gtk_widget_set_hexpand(g_lbl_status,TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(g_lbl_status),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(hst),g_lbl_status,TRUE,TRUE,0);
    g_lbl_rssi=gtk_label_new("RSSI: – dBm");
    gtk_box_pack_start(GTK_BOX(hst),g_lbl_rssi,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(vbox),hst,FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,2);

    /* History */
    GtkWidget *sw=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    g_txt_hist=gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(g_txt_hist),FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(g_txt_hist),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(g_txt_hist),GTK_WRAP_WORD_CHAR);
    g_hist_buf=gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_txt_hist));
    {GtkTextIter it; gtk_text_buffer_get_end_iter(g_hist_buf,&it);
     gtk_text_buffer_create_mark(g_hist_buf,"end",&it,FALSE);}
    gtk_container_add(GTK_CONTAINER(sw),g_txt_hist);
    gtk_widget_set_vexpand(sw,TRUE);
    gtk_box_pack_start(GTK_BOX(vbox),sw,TRUE,TRUE,0);

    gtk_box_pack_start(GTK_BOX(vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,2);

    /* PTT + Config */
    GtkWidget *hb=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    g_btn_ptt=gtk_button_new_with_label("SPRECHEN  [Leertaste]");
    gtk_widget_set_size_request(g_btn_ptt,220,60);
    g_signal_connect(g_btn_ptt,"pressed", G_CALLBACK(gui_ptt_press),NULL);
    g_signal_connect(g_btn_ptt,"released",G_CALLBACK(gui_ptt_release),NULL);
    gtk_box_pack_start(GTK_BOX(hb),g_btn_ptt,TRUE,TRUE,0);

    GtkWidget *chk_tone=gtk_check_button_new_with_label("600Hz Testton");
    g_signal_connect(chk_tone,"toggled",G_CALLBACK(gui_tone_toggled),NULL);
    gtk_box_pack_start(GTK_BOX(hb),chk_tone,FALSE,FALSE,0);
    GtkWidget *btn_cfg=gtk_button_new_with_label("⚙ Konfiguration");
    g_signal_connect(btn_cfg,"clicked",G_CALLBACK(gui_config_dialog),NULL);
    gtk_box_pack_start(GTK_BOX(hb),btn_cfg,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(vbox),hb,FALSE,FALSE,0);

    gtk_widget_show_all(g_win);

    /* Audiogerät wählen (nach GTK-Init, nicht davor!) */
    gui_audio_select_dialog();

    /* ALSA erst jetzt öffnen – nach GTK-Init und Geräteauswahl */
    pcm_cap  = alsa_open(CFG.audio_capture,  SND_PCM_STREAM_CAPTURE,  c2_spf);
    pcm_play = alsa_open(CFG.audio_playback, SND_PCM_STREAM_PLAYBACK, c2_spf);
    if (!pcm_cap)
        gtk_label_set_text(GTK_LABEL(g_lbl_status),
            "WARNUNG: Capture-Gerät nicht öffenbar – kein Senden möglich!");
    if (!pcm_play)
        gtk_label_set_text(GTK_LABEL(g_lbl_status),
            "WARNUNG: Playback-Gerät nicht öffenbar – kein Empfang möglich!");

    pthread_t tid_rx; pthread_create(&tid_rx,NULL,rx_thread,NULL);
    g_timeout_add(500,gui_timer,NULL);
    gtk_main();
    app_run=0;
}
#endif /* NO_GTK */

/* ================================================================
 * Audio-Gerät auswählen (gemeinsam CLI + GUI)
 * ================================================================ */
static void select_audio_device(void) {
    char devs[16][64];
    int  nd = alsa_list_devices(devs,16);
    if (nd==0) { fprintf(stderr,"Kein ALSA-Gerät gefunden!\n"); return; }
    if (nd==1) {
        strncpy(CFG.audio_capture, devs[0],63);
        strncpy(CFG.audio_playback,devs[0],63);
        return;
    }
    /* Mehrere Geräte: User muss wählen (CLI-Auswahl) */
    if (!use_gui) {
        printf("Verfügbare Audiogeräte:\n");
        for (int i=0;i<nd;i++) printf("  %d: %s\n",i,devs[i]);
        printf("Aufnahme [0]: "); fflush(stdout);
        int ci=0; if(scanf("%d",&ci)!=1) ci=0;
        if(ci<0||ci>=nd) ci=0;
        strncpy(CFG.audio_capture,devs[ci],63);
        printf("Wiedergabe [0]: "); fflush(stdout);
        int pi=0; if(scanf("%d",&pi)!=1) pi=0;
        if(pi<0||pi>=nd) pi=0;
        strncpy(CFG.audio_playback,devs[pi],63);
    }
    /* GUI: Auswahl erfolgt im Konfig-Dialog */
}

/* ================================================================
 * main()
 * ================================================================ */
int main(int argc, char **argv) {
    setlocale(LC_NUMERIC,"C");
    signal(SIGPIPE, SIG_IGN);  /* verhindert Absturz bei gebrochenem Socket */
    config_init_path(argv[0]);
    config_load();

#ifdef NO_GTK
    use_gui = 0;   /* ohne GTK gebaut: immer ncurses-CLI (auch mit --gui) */
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--gui"))
            fprintf(stderr,"Hinweis: ohne GTK gebaut - starte CLI\n");
    }
#else
    use_gui = (getenv("DISPLAY")!=NULL || getenv("WAYLAND_DISPLAY")!=NULL);
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--cli")) use_gui=0;
        if (!strcmp(argv[i],"--gui")) use_gui=1;
    }
#endif

    /* Codec2 initialisieren */
    if (!c2_init(CFG.codec_mode_idx)) {
        fprintf(stderr,"Codec2 Init fehlgeschlagen!\n"); return 1;
    }

    /* Audio-Gerät bestimmen */
    select_audio_device();

    /* Sockets verbinden */
    if (!sockets_connect())
        fprintf(stderr,"WARNUNG: Daemon nicht erreichbar – Reconnect im Hintergrund\n");
    else
        apply_lora_params();

#ifndef NO_GTK
    if (use_gui) {
        gui_main(&argc,&argv);
    } else
#endif
    {
        /* CLI: ALSA vor ncurses öffnen */
        pcm_cap  = alsa_open(CFG.audio_capture,  SND_PCM_STREAM_CAPTURE,  c2_spf);
        pcm_play = alsa_open(CFG.audio_playback, SND_PCM_STREAM_PLAYBACK, c2_spf);
        if (!pcm_cap)  fprintf(stderr,"WARNUNG: Capture '%s' nicht öffenbar!\n", CFG.audio_capture);
        if (!pcm_play) fprintf(stderr,"WARNUNG: Playback '%s' nicht öffenbar!\n",CFG.audio_playback);
        cli_main();
    }

    /* Aufräumen */
    ptt_active=0; app_run=0;
    config_save();
    if (pcm_cap)  snd_pcm_close(pcm_cap);
    if (pcm_play) snd_pcm_close(pcm_play);
    if (c2_enc)    codec2_destroy(c2_enc);
    if (c2_dec)    codec2_destroy(c2_dec);
    if (c2_rx_dec) codec2_destroy(c2_rx_dec);
    if (data_fd>=0) close(data_fd);
    if (conf_fd>=0) close(conf_fd);
    return 0;
}
