/* RG35XX Files + FTP: a two-pane file manager with a built-in FTP server for
 * wireless transfers. 640x480, Anbernic buttons. */
#define _GNU_SOURCE
#include <SDL.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "fileops.h"
#include "ftpd.h"
#include "ui.h"

#define APP_TITLE "Files + FTP"
#define FTP_PORT_DEFAULT 2121

/* ------------------------------------------------------------------ settings */

static struct {
    char ftp_user[32], ftp_pass[32];
    int ftp_port, ftp_auto, hidden, sort; /* sort 0 name, 1 newest, 2 largest */
    char path[2][1024];
} cfg;
static char app_dir[512];

static void cfg_load(void)
{
    snprintf(cfg.ftp_user, sizeof cfg.ftp_user, "rg");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    snprintf(cfg.ftp_pass, sizeof cfg.ftp_pass, "%04d", 1000 + rand() % 9000);
    cfg.ftp_port = FTP_PORT_DEFAULT;
    FILE *f = fopen("settings.cfg", "r");
    char line[1200];
    while (f && fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *v = strchr(line, '=');
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        if (!strcmp(line, "ftp_user") && *v) snprintf(cfg.ftp_user, sizeof cfg.ftp_user, "%s", v);
        else if (!strcmp(line, "ftp_pass") && *v) snprintf(cfg.ftp_pass, sizeof cfg.ftp_pass, "%s", v);
        else if (!strcmp(line, "ftp_port")) cfg.ftp_port = atoi(v);
        else if (!strcmp(line, "ftp_autostart")) cfg.ftp_auto = atoi(v) != 0;
        else if (!strcmp(line, "show_hidden")) cfg.hidden = atoi(v) != 0;
        else if (!strcmp(line, "sort")) cfg.sort = atoi(v);
        else if (!strcmp(line, "left")) snprintf(cfg.path[0], sizeof cfg.path[0], "%s", v);
        else if (!strcmp(line, "right")) snprintf(cfg.path[1], sizeof cfg.path[1], "%s", v);
    }
    if (f) fclose(f);
    if (cfg.ftp_port < 1 || cfg.ftp_port > 65535) cfg.ftp_port = FTP_PORT_DEFAULT;
    if (cfg.sort < 0 || cfg.sort > 2) cfg.sort = 0;
}

static void cfg_save(void)
{
    FILE *f = fopen("settings.cfg", "w");
    if (!f) return;
    fprintf(f, "# Files + FTP settings\n");
    fprintf(f, "# FTP login shown on the FTP screen (X there makes a new password)\n");
    fprintf(f, "ftp_user=%s\nftp_pass=%s\nftp_port=%d\n# 1 = start the FTP server when the app starts\nftp_autostart=%d\n",
            cfg.ftp_user, cfg.ftp_pass, cfg.ftp_port, cfg.ftp_auto);
    fprintf(f, "show_hidden=%d\nsort=%d\nleft=%s\nright=%s\n", cfg.hidden, cfg.sort, cfg.path[0], cfg.path[1]);
    fclose(f);
}

/* ------------------------------------------------------------------ locations */

typedef struct { char label[48], path[512], ftp[32]; } location;
static location locs[12];
static int nlocs;

static int is_mount(const char *p)
{
    struct stat a, b;
    char parent[600];
    snprintf(parent, sizeof parent, "%s/..", p);
    return stat(p, &a) == 0 && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}

static void add_loc(const char *label, const char *path, const char *ftp)
{
    struct stat st;
    if (nlocs >= 12 || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    for (int i = 0; i < nlocs; i++)
        if (!strcmp(locs[i].path, path)) return;
    snprintf(locs[nlocs].label, sizeof locs[0].label, "%s", label);
    snprintf(locs[nlocs].path, sizeof locs[0].path, "%s", path);
    snprintf(locs[nlocs].ftp, sizeof locs[0].ftp, "%s", ftp ? ftp : "");
    nlocs++;
}

static void find_locations(void)
{
    nlocs = 0;
#ifdef RG35XX
    if (is_mount("/mnt/sdcard")) add_loc("TF2 - SD card", "/mnt/sdcard", "TF2");
    if (is_mount("/mnt/mmc")) add_loc("TF1 - games", "/mnt/mmc", "TF1");
#else
    const char *home = getenv("HOME");
    if (home) add_loc("Home (PC test)", home, "HOME");
    add_loc("Windows D: (PC test)", "/mnt/d", "D");
#endif
    DIR *d = opendir("/mnt/usb");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512], l[48], fn[32];
        snprintf(p, sizeof p, "/mnt/usb/%.200s", e->d_name);
        if (!is_mount(p)) continue;
        snprintf(l, sizeof l, "USB - %.30s", e->d_name);
        snprintf(fn, sizeof fn, "USB-%.20s", e->d_name);
        add_loc(l, p, fn);
    }
    if (d) closedir(d);
    add_loc("This app's folder", app_dir, NULL);
    add_loc("Whole system (/)", "/", NULL);
}

static int loc_of(const char *path)
{
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < nlocs; i++) {
        size_t l = strlen(locs[i].path);
        if (!strcmp(locs[i].path, "/")) l = 0;
        if ((!strncmp(path, locs[i].path, l) && (path[l] == '/' || !path[l] || l == 0)) && (best < 0 || l > bl)) {
            best = i;
            bl = l;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ panes */

typedef struct {
    char name[256];
    int dir;
    long long size;
    time_t mtime;
    int marked;
} entry;

typedef struct {
    char path[1024];
    entry *e;
    int n, cap;
    int sel, top;
    time_t dir_mtime;
    char err[96];
} pane;

static pane P[2];
static int act; /* active pane */

static int cmp_entry(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    if (cfg.sort == 1 && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    if (cfg.sort == 2 && !x->dir && x->size != y->size) return x->size < y->size ? 1 : -1;
    return strcasecmp(x->name, y->name);
}

static void pane_load(pane *p, const char *keep_name)
{
    char keep[256] = "";
    if (keep_name) snprintf(keep, sizeof keep, "%s", keep_name);
    else if (p->n && p->sel < p->n) snprintf(keep, sizeof keep, "%s", p->e[p->sel].name);
    p->n = 0;
    p->err[0] = 0;
    DIR *d = opendir(p->path);
    if (!d) {
        snprintf(p->err, sizeof p->err, "Can't open: %s", strerror(errno));
        return;
    }
    struct stat st;
    if (stat(p->path, &st) == 0) p->dir_mtime = st.st_mtime;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (!cfg.hidden && de->d_name[0] == '.') continue;
        if (p->n == p->cap) {
            int nc = p->cap ? p->cap * 2 : 256;
            entry *ne = realloc(p->e, sizeof *ne * (size_t)nc);
            if (!ne) break;
            p->e = ne;
            p->cap = nc;
        }
        entry *e = &p->e[p->n];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", de->d_name);
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", strcmp(p->path, "/") ? p->path : "", de->d_name);
        if (stat(full, &st) == 0) {
            e->dir = S_ISDIR(st.st_mode);
            e->size = st.st_size;
            e->mtime = st.st_mtime;
        }
        p->n++;
    }
    closedir(d);
    qsort(p->e, (size_t)p->n, sizeof *p->e, cmp_entry);
    p->sel = 0;
    if (keep[0])
        for (int i = 0; i < p->n; i++)
            if (!strcmp(p->e[i].name, keep)) { p->sel = i; break; }
    if (p->sel >= p->n) p->sel = p->n ? p->n - 1 : 0;
}

static void pane_open(pane *p, const char *path, const char *keep)
{
    char np[1024];
    snprintf(np, sizeof np, "%s", path);
    size_t l = strlen(np);
    while (l > 1 && np[l - 1] == '/') np[--l] = 0;
    snprintf(p->path, sizeof p->path, "%s", np);
    p->top = 0;
    pane_load(p, keep);
}

static void full_path(const pane *p, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", strcmp(p->path, "/") ? p->path : "", name);
}

static int marked_count(const pane *p)
{
    int c = 0;
    for (int i = 0; i < p->n; i++) c += p->e[i].marked;
    return c;
}

/* ------------------------------------------------------------------ helpers */

static char toast[200];
static Uint32 toast_t;
static void show_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void show_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof toast, fmt, ap);
    va_end(ap);
    toast_t = SDL_GetTicks();
    fprintf(stderr, "[toast] %s\n", toast);
}

static void human(char *out, size_t n, long long v)
{
    if (v >= 1LL << 30) snprintf(out, n, "%.1f GB", v / 1073741824.0);
    else if (v >= 1LL << 20) snprintf(out, n, "%.1f MB", v / 1048576.0);
    else if (v >= 1024) snprintf(out, n, "%.0f KB", v / 1024.0);
    else snprintf(out, n, "%lld B", v);
}

static int is_text_name(const char *n)
{
    static const char *ext[] = { ".txt", ".cfg", ".log", ".sh", ".ini", ".json", ".md", ".xml", ".conf", ".csv",
                                 ".lst", ".m3u", ".cue", ".js", ".py", ".htm", ".html", ".nfo", NULL };
    const char *d = strrchr(n, '.');
    if (!d) return 0;
    for (int i = 0; ext[i]; i++)
        if (!strcasecmp(d, ext[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------------ battery + screen power */

static char bat_dir[256];
static int bat_pct = -1, bat_chg;

static int read_sys(const char *dir, const char *name, char *out, size_t n)
{
    char p[400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) out[0] = 0;
    fclose(f);
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    return 0;
}

static void battery_poll(void)
{
    static Uint32 last;
    if (last && SDL_GetTicks() - last < 10000) return;
    last = SDL_GetTicks();
    if (!bat_dir[0]) {
        DIR *d = opendir("/sys/class/power_supply");
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char dir[300], t[32];
            snprintf(dir, sizeof dir, "/sys/class/power_supply/%.200s", e->d_name);
            if (read_sys(dir, "type", t, sizeof t) == 0 && !strcmp(t, "Battery")) { snprintf(bat_dir, sizeof bat_dir, "%s", dir); break; }
        }
        if (d) closedir(d);
    }
    char v[32];
    if (bat_dir[0] && read_sys(bat_dir, "capacity", v, sizeof v) == 0) bat_pct = atoi(v);
    if (bat_dir[0] && read_sys(bat_dir, "status", v, sizeof v) == 0) bat_chg = !strcmp(v, "Charging") || !strcmp(v, "Full");
}

static int screen_off, wake_hold;
static __attribute__((unused)) unsigned long disp_saved;

static void backlight_set(int on)
{
#ifdef RG35XX
    int fd = open("/dev/disp", O_RDWR);
    if (fd >= 0) {
        unsigned long args[4] = { 0, 0, 0, 0 };
        if (!on) {
            int cur = ioctl(fd, 0x103, args); /* DISP_LCD_GET_BRIGHTNESS */
            if (cur > 0 && cur <= 255) disp_saved = (unsigned long)cur;
        }
        args[1] = on ? (disp_saved ? disp_saved : 128) : 0;
        ioctl(fd, 0x102, args); /* DISP_LCD_SET_BRIGHTNESS */
        close(fd);
    }
    fd = open("/dev/fb0", O_RDWR);
    if (fd >= 0) {
        ioctl(fd, FBIOBLANK, on ? FB_BLANK_UNBLANK : FB_BLANK_POWERDOWN);
        close(fd);
    }
#else
    (void)on;
#endif
}

static void screen_restore(void)
{
    if (screen_off) { screen_off = 0; backlight_set(1); }
}

static void fatal_sig(int s)
{
    screen_restore();
    signal(s, SIG_DFL);
    raise(s);
}

/* ------------------------------------------------------------------ UI state */

enum { M_FILES, M_FTP };
static int mode = M_FILES;

/* popup list (actions, locations, sort) */
typedef struct { char label[96]; int id; int dim; } item;
static struct {
    int open;
    char title[64];
    item it[24];
    int n, sel;
    int kind; /* 1 actions, 2 locations */
} pop;

/* confirm box */
static struct { int open; char text[160]; int action; } cfm;

/* on-screen keyboard */
static struct {
    int open;
    int purpose; /* 1 rename, 2 new folder */
    char text[256];
    char orig[256];
    int row, col, shift;
} osk;

/* text viewer */
static struct {
    int open;
    char title[256];
    char **lines;
    int n, top;
} view;

static int quit;

/* ------------------------------------------------------------------ actions */

enum {
    ACT_OPEN = 1, ACT_VIEW, ACT_COPY, ACT_MOVE, ACT_DELETE, ACT_RENAME, ACT_NEWDIR, ACT_INFO, ACT_MARKALL,
    ACT_UNMARK, ACT_HIDDEN, ACT_SORT, ACT_REFRESH, ACT_SAMEDIR, ACT_FTP, ACT_RUN,
};

/* Run script: /bin/sh <script> in its own folder, output to <script>.log, max 3 min. */
static volatile int script_running;
static char script_done_msg[160];
static volatile int script_done;

static void *script_thread(void *arg)
{
    char *path = arg, cmd[3000], dir[1400];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0;
    const char *dot = strrchr(path, '.');
    int stem = dot ? (int)(dot - path) : (int)strlen(path);
    snprintf(cmd, sizeof cmd, "cd '%s' && timeout 180 /bin/sh '%s' > '%.*s.log' 2>&1; echo \"[exit code $?]\" >> '%.*s.log'; sync",
             dir, path, stem, path, stem, path);
    fprintf(stderr, "[run] %s\n", path);
    int r = system(cmd);
    const char *b = strrchr(path, '/');
    snprintf(script_done_msg, sizeof script_done_msg, "%s finished%s - see the .log next to it", b ? b + 1 : path,
             r ? " (with errors)" : "");
    free(path);
    script_running = 0;
    script_done = 1;
    return NULL;
}

/* Alert when a script finishes: two short high beeps (2.4 kHz), three if it
 * failed. The sound device is opened only for the beep and closed after. */
static SDL_AudioDeviceID beep_dev;
static Uint32 beep_close_at;

static void beep(int failed)
{
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    if (!beep_dev) {
        SDL_AudioSpec w = { 0 };
        w.freq = 48000;
        w.format = AUDIO_S16SYS;
        w.channels = 1;
        w.samples = 1024;
        beep_dev = SDL_OpenAudioDevice(NULL, 0, &w, NULL, 0);
        if (!beep_dev) { fprintf(stderr, "[beep] %s\n", SDL_GetError()); return; }
    }
    static int16_t s[48000 * 3 / 2];
    int n = 0, count = failed ? 3 : 2;
    for (int b = 0; b < count; b++) {
        for (int i = 0; i < 48000 * 15 / 100; i++) { /* 150 ms tone, soft edges */
            double env = i < 480 ? i / 480.0 : i > 6720 ? (7200 - i) / 480.0 : 1;
            s[n++] = (int16_t)(14000 * env * sin(2 * M_PI * 2400 * i / 48000.0));
        }
        for (int i = 0; i < 48000 / 10; i++) s[n++] = 0; /* 100 ms gap */
    }
    SDL_ClearQueuedAudio(beep_dev);
    SDL_QueueAudio(beep_dev, s, (Uint32)n * 2);
    SDL_PauseAudioDevice(beep_dev, 0);
    beep_close_at = SDL_GetTicks() + 1500;
}

static void beep_poll(void)
{
    if (beep_dev && SDL_TICKS_PASSED(SDL_GetTicks(), beep_close_at)) {
        SDL_CloseAudioDevice(beep_dev);
        beep_dev = 0;
    }
}

/* the items an action works on: marked ones, else the selected one */
static int selection(pane *p, char ***out)
{
    int n = marked_count(p);
    char **v = calloc((size_t)(n ? n : 1), sizeof *v);
    int k = 0;
    if (n) {
        for (int i = 0; i < p->n; i++)
            if (p->e[i].marked) {
                char f[1400];
                full_path(p, p->e[i].name, f, sizeof f);
                v[k++] = strdup(f);
            }
    } else if (p->n) {
        char f[1400];
        full_path(p, p->e[p->sel].name, f, sizeof f);
        v[k++] = strdup(f);
    }
    *out = v;
    return k;
}

static void free_sel(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

static void start_job(int op)
{
    pane *p = &P[act], *o = &P[!act];
    char **v;
    int n = selection(p, &v);
    if (!n) { free(v); show_toast("Nothing selected"); return; }
    if (op != OP_DELETE && !strcmp(p->path, o->path)) {
        free_sel(v, n);
        show_toast("Both sides show the same folder - open the target folder on the other side");
        return;
    }
    if (fo_start(op, v, n, op == OP_DELETE ? NULL : o->path) != 0) show_toast("Another job is still running");
    free_sel(v, n);
    for (int i = 0; i < p->n; i++) p->e[i].marked = 0;
}

static void viewer_open(const char *path, const char *name)
{
    FILE *f = fopen(path, "rb");
    if (!f) { show_toast("Can't open the file"); return; }
    char *buf = malloc(2 * 1024 * 1024 + 1);
    size_t len = buf ? fread(buf, 1, 2 * 1024 * 1024, f) : 0;
    fclose(f);
    if (!buf) return;
    buf[len] = 0;
    view.n = 0;
    view.lines = NULL;
    int cap = 0;
    char *s = buf;
    while (*s || s == buf) {
        char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l && s[l - 1] == '\r') l--;
        /* wrap long lines at ~70 characters */
        size_t off = 0;
        do {
            size_t chunk = l - off > 70 ? 70 : l - off;
            while (chunk > 0 && off + chunk < l && ((unsigned char)s[off + chunk] & 0xC0) == 0x80) chunk--;
            if (view.n == cap) {
                cap = cap ? cap * 2 : 1024;
                char **nl = realloc(view.lines, sizeof *nl * (size_t)cap);
                if (!nl) break;
                view.lines = nl;
            }
            char *ln = malloc(chunk + 1);
            memcpy(ln, s + off, chunk);
            ln[chunk] = 0;
            for (char *c = ln; *c; c++) if (*c == '\t') *c = ' ';
            view.lines[view.n++] = ln;
            off += chunk;
        } while (off < l);
        if (!e) break;
        s = e + 1;
    }
    free(buf);
    snprintf(view.title, sizeof view.title, "%s", name);
    view.top = 0;
    view.open = 1;
}

static void viewer_close(void)
{
    for (int i = 0; i < view.n; i++) free(view.lines[i]);
    free(view.lines);
    view.lines = NULL;
    view.n = 0;
    view.open = 0;
}

static void pop_add(const char *label, int id, int dim)
{
    if (pop.n >= 24) return;
    snprintf(pop.it[pop.n].label, sizeof pop.it[0].label, "%s", label);
    pop.it[pop.n].id = id;
    pop.it[pop.n].dim = dim;
    pop.n++;
}

static void actions_open(int for_file)
{
    pane *p = &P[act];
    int mk = marked_count(p);
    memset(&pop, 0, sizeof pop);
    pop.open = 1;
    pop.kind = 1;
    char t[96];
    if (mk) snprintf(pop.title, sizeof pop.title, "%d marked", mk);
    else if (p->n) text_fit(pop.title, sizeof pop.title, p->e[p->sel].name, 18, 380);
    else snprintf(pop.title, sizeof pop.title, "Folder");
    int has = mk || p->n;
    if (for_file && !mk && p->n && is_text_name(p->e[p->sel].name)) pop_add("View text", ACT_VIEW, 0);
    if (!mk && p->n && !p->e[p->sel].dir) {
        const char *dot = strrchr(p->e[p->sel].name, '.');
        if (dot && !strcasecmp(dot, ".sh")) pop_add("Run script (output -> .log)", ACT_RUN, 0);
    }
    snprintf(t, sizeof t, "Copy to the %s side", act ? "left" : "right");
    pop_add(t, ACT_COPY, !has);
    snprintf(t, sizeof t, "Move to the %s side", act ? "left" : "right");
    pop_add(t, ACT_MOVE, !has);
    pop_add("Delete", ACT_DELETE, !has);
    pop_add("Rename", ACT_RENAME, mk || !p->n);
    pop_add("New folder here", ACT_NEWDIR, 0);
    pop_add("Info", ACT_INFO, !has);
    pop_add(mk ? "Unmark all" : "Mark all", mk ? ACT_UNMARK : ACT_MARKALL, !p->n);
    pop_add("Other side: open this folder", ACT_SAMEDIR, 0);
    snprintf(t, sizeof t, "Sort: %s", cfg.sort == 0 ? "name" : cfg.sort == 1 ? "newest first" : "largest first");
    pop_add(t, ACT_SORT, 0);
    pop_add(cfg.hidden ? "Hidden files: shown" : "Hidden files: not shown", ACT_HIDDEN, 0);
    pop_add("Refresh", ACT_REFRESH, 0);
    pop_add("Wireless transfer (FTP)", ACT_FTP, 0);
    while (pop.sel < pop.n - 1 && pop.it[pop.sel].dim) pop.sel++; /* start on something usable */
}

static void locations_open(void)
{
    find_locations();
    memset(&pop, 0, sizeof pop);
    pop.open = 1;
    pop.kind = 2;
    snprintf(pop.title, sizeof pop.title, "Go to (%s side)", act ? "right" : "left");
    int cur = loc_of(P[act].path);
    for (int i = 0; i < nlocs; i++) {
        char l[96], fr[24] = "";
        struct statvfs sv;
        if (statvfs(locs[i].path, &sv) == 0 && strcmp(locs[i].path, "/")) {
            char h[24];
            human(h, sizeof h, (long long)sv.f_bavail * (long long)sv.f_frsize);
            snprintf(fr, sizeof fr, "%s free", h);
        }
        snprintf(l, sizeof l, "%s\t%s", locs[i].label, fr);
        pop_add(l, i, 0);
        if (i == cur) pop.sel = i;
    }
}

static void do_action(int id)
{
    pane *p = &P[act];
    switch (id) {
    case ACT_VIEW: {
        char f[1400];
        full_path(p, p->e[p->sel].name, f, sizeof f);
        viewer_open(f, p->e[p->sel].name);
        break;
    }
    case ACT_COPY: start_job(OP_COPY); break;
    case ACT_MOVE: start_job(OP_MOVE); break;
    case ACT_DELETE: {
        int mk = marked_count(p);
        cfm.open = 1;
        cfm.action = ACT_DELETE;
        if (mk) snprintf(cfm.text, sizeof cfm.text, "Delete %d marked items?", mk);
        else {
            char nm[96];
            text_fit(nm, sizeof nm, p->e[p->sel].name, 18, 380);
            snprintf(cfm.text, sizeof cfm.text, "Delete \"%s\"%s?", nm, p->e[p->sel].dir ? " and everything in it" : "");
        }
        break;
    }
    case ACT_RENAME:
        memset(&osk, 0, sizeof osk);
        osk.open = 1;
        osk.purpose = 1;
        snprintf(osk.text, sizeof osk.text, "%s", p->e[p->sel].name);
        snprintf(osk.orig, sizeof osk.orig, "%s", p->e[p->sel].name);
        break;
    case ACT_NEWDIR:
        memset(&osk, 0, sizeof osk);
        osk.open = 1;
        osk.purpose = 2;
        snprintf(osk.text, sizeof osk.text, "New folder");
        break;
    case ACT_INFO: {
        char **v;
        int n = selection(p, &v);
        long long bytes = 0;
        int files = 0, dirs = 0;
        for (int i = 0; i < n; i++) {
            struct stat st;
            if (stat(v[i], &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) dirs++;
            else { files++; bytes += st.st_size; }
        }
        char h[32];
        human(h, sizeof h, bytes);
        if (n == 1) {
            struct stat st;
            stat(v[0], &st);
            char d[40];
            strftime(d, sizeof d, "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
            if (S_ISDIR(st.st_mode)) show_toast("Folder, changed %s", d);
            else show_toast("%s, changed %s", h, d);
        } else {
            show_toast("%d files (%s) and %d folders", files, h, dirs);
        }
        free_sel(v, n);
        break;
    }
    case ACT_MARKALL: for (int i = 0; i < p->n; i++) p->e[i].marked = 1; break;
    case ACT_UNMARK: for (int i = 0; i < p->n; i++) p->e[i].marked = 0; break;
    case ACT_SAMEDIR: pane_open(&P[!act], p->path, NULL); break;
    case ACT_SORT:
        cfg.sort = (cfg.sort + 1) % 3;
        pane_load(&P[0], NULL);
        pane_load(&P[1], NULL);
        cfg_save();
        break;
    case ACT_HIDDEN:
        cfg.hidden = !cfg.hidden;
        pane_load(&P[0], NULL);
        pane_load(&P[1], NULL);
        cfg_save();
        break;
    case ACT_REFRESH:
        pane_load(&P[0], NULL);
        pane_load(&P[1], NULL);
        break;
    case ACT_FTP: mode = M_FTP; break;
    case ACT_RUN: {
        if (script_running) { show_toast("A script is already running"); break; }
        char f[1400];
        full_path(p, p->e[p->sel].name, f, sizeof f);
        if (strchr(f, '\'')) { show_toast("Can't run a file with ' in its path"); break; }
        script_running = 1;
        pthread_t t;
        if (pthread_create(&t, NULL, script_thread, strdup(f)) == 0) {
            pthread_detach(t);
            show_toast("Running %s ...", p->e[p->sel].name);
        } else script_running = 0;
        break;
    }
    }
}

static void confirm_yes(void)
{
    if (cfm.action == ACT_DELETE) start_job(OP_DELETE);
    cfm.open = 0;
}

/* ---- on-screen keyboard ---- */
static const char *osk_rows[2][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.," },
    { "!@#$%^&()=", "QWERTYUIOP", "ASDFGHJKL+", "ZXCVBNM[]'" },
};
enum { OSK_KEYROWS = 4, OSK_COLS = 10 };
/* last row: SHIFT SPACE DEL CANCEL DONE */
static const char *osk_special[5] = { "Shift", "Space", "Delete", "Cancel", "Done" };

static void osk_backspace(void)
{
    size_t l = strlen(osk.text);
    while (l > 0) {
        l--;
        if (((unsigned char)osk.text[l] & 0xC0) != 0x80) break;
    }
    osk.text[l] = 0;
}

static void osk_type(char c)
{
    size_t l = strlen(osk.text);
    if (l + 1 < sizeof osk.text) { osk.text[l] = c; osk.text[l + 1] = 0; }
}

static void osk_done(void)
{
    pane *p = &P[act];
    if (!osk.text[0] || strchr(osk.text, '/') || !strcmp(osk.text, ".") || !strcmp(osk.text, "..")) {
        show_toast("That name can't be used");
        return;
    }
    char dst[1400];
    full_path(p, osk.text, dst, sizeof dst);
    struct stat st;
    if (osk.purpose == 1) {
        if (!strcmp(osk.text, osk.orig)) { osk.open = 0; return; }
        if (lstat(dst, &st) == 0) { show_toast("Something with that name is already here"); return; }
        char src[1400];
        full_path(p, osk.orig, src, sizeof src);
        if (rename(src, dst) != 0) show_toast("Can't rename: %s", strerror(errno));
        else { sync(); pane_load(p, osk.text); }
    } else {
        if (lstat(dst, &st) == 0) { show_toast("Something with that name is already here"); return; }
        if (mkdir(dst, 0755) != 0) show_toast("Can't create the folder: %s", strerror(errno));
        else { sync(); pane_load(p, osk.text); }
    }
    osk.open = 0;
}

static void osk_press(void)
{
    if (osk.row < OSK_KEYROWS) {
        osk_type(osk_rows[osk.shift][osk.row][osk.col]);
        return;
    }
    int k = osk.col / 2; /* special keys are two columns wide */
    if (k == 0) osk.shift = !osk.shift;
    else if (k == 1) osk_type(' ');
    else if (k == 2) osk_backspace();
    else if (k == 3) osk.open = 0;
    else osk_done();
}

/* ------------------------------------------------------------------ FTP control */

static ftpd_state fs;

static void ftp_roots(ftpd_root *r, int *n)
{
    find_locations();
    *n = 0;
    for (int i = 0; i < nlocs && *n < FTPD_MAX_ROOTS; i++)
        if (locs[i].ftp[0]) {
            snprintf(r[*n].name, sizeof r[0].name, "%s", locs[i].ftp);
            snprintf(r[*n].path, sizeof r[0].path, "%s", locs[i].path);
            (*n)++;
        }
}

static void ftp_toggle(void)
{
    ftpd_get(&fs);
    if (fs.running) {
        ftpd_stop();
        cfg.ftp_auto = 0;
        show_toast("FTP server stopped");
    } else {
        ftpd_root r[FTPD_MAX_ROOTS];
        int n;
        ftp_roots(r, &n);
        if (ftpd_start(cfg.ftp_port, cfg.ftp_user, cfg.ftp_pass, r, n) == 0) {
            cfg.ftp_auto = 1;
            show_toast("FTP server started");
        } else {
            ftpd_get(&fs);
            show_toast("%s", fs.error);
        }
    }
    cfg_save();
}

/* ------------------------------------------------------------------ input */

enum { A_LEFT, A_RIGHT, A_UP, A_DOWN, A_A, A_B, A_X, A_Y, A_L1, A_R1, A_L2, A_R2, A_SELECT, A_START, A_MENU, A_COUNT };
/* RG35XX Plus stock firmware button numbers (same as the SDR app) */
static const int btn_map[A_COUNT] = {
    [A_LEFT] = -1, [A_RIGHT] = -1, [A_UP] = -1, [A_DOWN] = -1,
    [A_A] = 0, [A_B] = 1, [A_X] = 3, [A_Y] = 2, [A_L1] = 4, [A_R1] = 5, [A_L2] = 9, [A_R2] = 10,
    [A_SELECT] = 6, [A_START] = 7, [A_MENU] = 8,
};
static int pressed[A_COUNT];
static Uint32 press_t[A_COUNT], rep_t[A_COUNT];
static int sel_combo;

static int list_rows(void);

static void move_sel(pane *p, int d)
{
    if (!p->n) return;
    p->sel += d;
    if (p->sel < 0) p->sel = d < -1 ? 0 : p->n - 1;
    if (p->sel >= p->n) p->sel = d > 1 ? p->n - 1 : 0;
}

static void files_action(int a, int repeat)
{
    pane *p = &P[act];
    int rows = list_rows();
    switch (a) {
    case A_UP: move_sel(p, -1); break;
    case A_DOWN: move_sel(p, 1); break;
    case A_L1: move_sel(p, -rows); break;
    case A_R1: move_sel(p, rows); break;
    case A_L2: if (p->n) p->sel = 0; break;
    case A_R2: if (p->n) p->sel = p->n - 1; break;
    case A_LEFT: if (!repeat) act = 0; break;
    case A_RIGHT: if (!repeat) act = 1; break;
    case A_A:
        if (repeat || !p->n) break;
        if (p->e[p->sel].dir) {
            char np[1400];
            full_path(p, p->e[p->sel].name, np, sizeof np);
            pane_open(p, np, NULL);
        } else {
            actions_open(1);
        }
        break;
    case A_B: {
        if (repeat || !strcmp(p->path, "/")) break;
        int l = loc_of(p->path);
        if (l >= 0 && !strcmp(p->path, locs[l].path) && strcmp(locs[l].path, "/")) { /* at a card's top */
            locations_open();
            break;
        }
        char up[1024], name[256];
        snprintf(up, sizeof up, "%s", p->path);
        char *s = strrchr(up, '/');
        snprintf(name, sizeof name, "%s", s ? s + 1 : "");
        if (s == up) up[1] = 0;
        else if (s) *s = 0;
        pane_open(p, up, name);
        break;
    }
    case A_Y:
        if (!p->n) break;
        p->e[p->sel].marked = !p->e[p->sel].marked;
        move_sel(p, 1);
        if (p->sel == 0) p->sel = p->n - 1; /* don't wrap from the last line */
        break;
    case A_X: if (!repeat) actions_open(0); break;
    case A_START: if (!repeat) mode = M_FTP; break;
    case A_MENU: if (!repeat) { cfm.open = 1; cfm.action = -1; snprintf(cfm.text, sizeof cfm.text, "Exit Files + FTP?"); } break;
    }
}

static void ftp_action(int a, int repeat)
{
    if (repeat) return;
    switch (a) {
    case A_A: ftp_toggle(); break;
    case A_X:
        snprintf(cfg.ftp_pass, sizeof cfg.ftp_pass, "%04d", 1000 + rand() % 9000);
        cfg_save();
        if (fs.running) { ftpd_stop(); ftp_toggle(); }
        show_toast("New password: %s", cfg.ftp_pass);
        break;
    case A_B: case A_START: mode = M_FILES; break;
    case A_MENU: cfm.open = 1; cfm.action = -1; snprintf(cfm.text, sizeof cfm.text, "Exit Files + FTP?"); break;
    }
}

static void do_input(int a, int repeat)
{
    if (screen_off) { /* any key wakes the screen */
        screen_off = 0;
        backlight_set(1);
        wake_hold = 1;
        return;
    }
    if (wake_hold) return;
    if (!repeat && ((a == A_START && pressed[A_SELECT]) || (a == A_SELECT && pressed[A_START]))) { sel_combo = 1; quit = 1; return; }
    if (!repeat && ((a == A_B && pressed[A_SELECT]) || (a == A_SELECT && pressed[A_B]))) {
        sel_combo = 1;
        screen_off = 1;
        backlight_set(0);
        return;
    }
    fo_state js;
    fo_get(&js);
    if (js.active) { /* a copy / delete is running: B stops it */
        if (a == A_B && !repeat) fo_cancel();
        return;
    }
    if (cfm.open) {
        if (repeat) return;
        if (a == A_A) { if (cfm.action == -1) quit = 1; else confirm_yes(); }
        else if (a == A_B) cfm.open = 0;
        return;
    }
    if (view.open) {
        int rows = 18;
        if (a == A_UP && view.top > 0) view.top--;
        else if (a == A_DOWN && view.top < view.n - rows) view.top++;
        else if (a == A_L1) view.top = view.top > rows ? view.top - rows : 0;
        else if (a == A_R1) view.top = view.top + rows < view.n - rows ? view.top + rows : (view.n > rows ? view.n - rows : 0);
        else if ((a == A_B || a == A_A) && !repeat) viewer_close();
        return;
    }
    if (osk.open) {
        if (a == A_UP) osk.row = (osk.row + OSK_KEYROWS) % (OSK_KEYROWS + 1);
        else if (a == A_DOWN) osk.row = (osk.row + 1) % (OSK_KEYROWS + 1);
        else if (a == A_LEFT) osk.col = osk.row == OSK_KEYROWS ? (osk.col / 2 * 2 + 8) % 10 : (osk.col + 9) % 10;
        else if (a == A_RIGHT) osk.col = osk.row == OSK_KEYROWS ? (osk.col / 2 * 2 + 2) % 10 : (osk.col + 1) % 10;
        else if (a == A_A) osk_press();
        else if (a == A_B) osk_backspace();
        else if (a == A_Y && !repeat) osk.shift = !osk.shift;
        else if (a == A_X && !repeat) osk_type(' ');
        else if (a == A_START && !repeat) osk_done();
        return;
    }
    if (pop.open) {
        if (a == A_UP) do pop.sel = (pop.sel + pop.n - 1) % pop.n; while (pop.it[pop.sel].dim && pop.n > 1);
        else if (a == A_DOWN) do pop.sel = (pop.sel + 1) % pop.n; while (pop.it[pop.sel].dim && pop.n > 1);
        else if (a == A_B && !repeat) pop.open = 0;
        else if (a == A_A && !repeat && pop.n && !pop.it[pop.sel].dim) {
            int id = pop.it[pop.sel].id, kind = pop.kind;
            pop.open = 0;
            if (kind == 2) pane_open(&P[act], locs[id].path, NULL);
            else do_action(id);
        }
        return;
    }
    if (a == A_SELECT) return; /* Select alone acts on release */
    if (mode == M_FTP) ftp_action(a, repeat);
    else files_action(a, repeat);
}

static void set_pressed(int a, int down)
{
    if (down && !pressed[a]) {
        pressed[a] = 1;
        press_t[a] = rep_t[a] = SDL_GetTicks();
        do_input(a, 0);
    } else if (!down && pressed[a]) {
        pressed[a] = 0;
        if (wake_hold) {
            int any = 0;
            for (int i = 0; i < A_COUNT; i++) any |= pressed[i];
            if (!any) wake_hold = 0;
            return;
        }
        if (a == A_SELECT) {
            if (sel_combo) sel_combo = 0;
            else if (!pop.open && !cfm.open && !osk.open && !view.open && !screen_off) {
                fo_state js;
                fo_get(&js);
                if (!js.active && mode == M_FILES) locations_open(); /* Select: go to a card / location */
            }
        }
    }
}

static void process_repeat(void)
{
    static const int rep[] = { A_UP, A_DOWN, A_L1, A_R1, A_LEFT, A_RIGHT, A_B };
    Uint32 now = SDL_GetTicks();
    for (unsigned i = 0; i < sizeof rep / sizeof rep[0]; i++) {
        int a = rep[i];
        if (!pressed[a] || (a == A_B && !osk.open)) continue;
        Uint32 held = now - press_t[a];
        if (held > 350 && now - rep_t[a] >= (held > 2000 ? 40u : 90u)) {
            rep_t[a] = now;
            do_input(a, 1);
        }
    }
}

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: quit = 1; break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->key.repeat) break;
        int a = -1;
        switch (e->key.keysym.sym) {
        case SDLK_LEFT: a = A_LEFT; break;
        case SDLK_RIGHT: a = A_RIGHT; break;
        case SDLK_UP: a = A_UP; break;
        case SDLK_DOWN: a = A_DOWN; break;
        case SDLK_z: a = A_A; break;
        case SDLK_x: a = A_B; break;
        case SDLK_s: a = A_X; break;
        case SDLK_a: a = A_Y; break;
        case SDLK_q: a = A_L1; break;
        case SDLK_w: a = A_R1; break;
        case SDLK_1: a = A_L2; break;
        case SDLK_2: a = A_R2; break;
        case SDLK_BACKSPACE: case SDLK_RSHIFT: a = A_SELECT; break;
        case SDLK_RETURN: a = A_START; break;
        case SDLK_ESCAPE: a = A_MENU; break;
        }
        if (a >= 0) set_pressed(a, e->type == SDL_KEYDOWN);
        break;
    }
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
        for (int a = A_A; a < A_COUNT; a++)
            if (btn_map[a] == e->jbutton.button) set_pressed(a, e->type == SDL_JOYBUTTONDOWN);
        break;
    case SDL_JOYHATMOTION: {
        int v = e->jhat.value;
        set_pressed(A_UP, (v & SDL_HAT_UP) != 0);
        set_pressed(A_DOWN, (v & SDL_HAT_DOWN) != 0);
        set_pressed(A_LEFT, (v & SDL_HAT_LEFT) != 0);
        set_pressed(A_RIGHT, (v & SDL_HAT_RIGHT) != 0);
        break;
    }
    }
}

/* ------------------------------------------------------------------ drawing */

#define TOP_H 30
#define PANE_Y 32
#define PANE_HDR 38
#define ROW_H 24
#define FOOT_Y 450

static int list_rows(void) { return (FOOT_Y - 2 - (PANE_Y + PANE_HDR)) / ROW_H; }

static void draw_top(void)
{
    fill(0, 0, W, TOP_H, C_PANEL);
    text(10, 6, 18, C_ACCENT, APP_TITLE);
    int x = W - 10;
    if (bat_pct >= 0) {
        char b[16];
        snprintf(b, sizeof b, "%s%d%%", bat_chg ? "+" : "", bat_pct);
        x -= text_w(16, b);
        text(x, 7, 16, bat_pct <= 15 ? C_BAD : C_TEXT, b);
        x -= 14;
    }
    if (fs.running) {
        char f[96];
        if (fs.xfer_active) snprintf(f, sizeof f, "FTP %s %s", fs.xfer_active == 1 ? "receiving" : "sending", "");
        else if (fs.clients) snprintf(f, sizeof f, "FTP: %d connected", fs.clients);
        else snprintf(f, sizeof f, "FTP ON  %s:%d", fs.ip[0] ? fs.ip : "no Wi-Fi", fs.port);
        int w = text_w(14, f) + 16;
        x -= w;
        fill(x, 6, w, 19, fs.xfer_active ? C_WARN : C_GOOD);
        text(x + 8, 8, 14, C_BG, f);
    }
}

static void draw_pane(int i)
{
    pane *p = &P[i];
    int x0 = i ? W / 2 + 1 : 0, pw = W / 2 - 1;
    int active = i == act;
    fill(x0, PANE_Y, pw, FOOT_Y - PANE_Y - 2, C_BG);
    /* header: location + path */
    fill(x0, PANE_Y, pw, PANE_HDR, active ? C_PANEL2 : C_PANEL);
    if (active) fill(x0, PANE_Y, pw, 2, C_ACCENT);
    int li = loc_of(p->path);
    const char *rel = p->path;
    if (li >= 0 && strcmp(locs[li].path, "/")) rel = p->path + strlen(locs[li].path);
    char lbl[80], pathfit[160];
    snprintf(lbl, sizeof lbl, "%s", li >= 0 ? locs[li].label : "");
    text(x0 + 8, PANE_Y + 3, 15, active ? C_ACCENT : C_DIM, lbl);
    text_fit(pathfit, sizeof pathfit, *rel ? rel : "/", 14, pw - 16);
    text(x0 + 8, PANE_Y + 20, 14, C_DIM, pathfit);
    int mk = marked_count(p);
    if (mk) {
        char m[24];
        snprintf(m, sizeof m, "%d marked", mk);
        text(x0 + pw - 8 - text_w(14, m), PANE_Y + 3, 14, C_MARK, m);
    }
    int y0 = PANE_Y + PANE_HDR, rows = list_rows();
    if (p->err[0]) { text(x0 + 10, y0 + 10, 15, C_BAD, p->err); return; }
    if (!p->n) { text(x0 + 10, y0 + 10, 15, C_DIM, "(empty folder)"); return; }
    if (p->sel < p->top) p->top = p->sel;
    if (p->sel >= p->top + rows) p->top = p->sel - rows + 1;
    for (int r = 0; r < rows && p->top + r < p->n; r++) {
        entry *e = &p->e[p->top + r];
        int y = y0 + r * ROW_H;
        int cur = p->top + r == p->sel;
        if (cur) fill(x0, y, pw, ROW_H, active ? C_SEL : C_SELDIM);
        if (e->marked) fill(x0, y, 4, ROW_H, C_MARK);
        /* icon */
        if (e->dir) {
            fill(x0 + 9, y + 8, 6, 3, C_FOLDER);
            fill(x0 + 9, y + 10, 14, 9, C_FOLDER);
        } else {
            fill(x0 + 11, y + 5, 10, 14, C_DIM);
            fill(x0 + 13, y + 8, 6, 1, C_BG);
            fill(x0 + 13, y + 11, 6, 1, C_BG);
            fill(x0 + 13, y + 14, 6, 1, C_BG);
        }
        char sz[24] = "";
        if (!e->dir) human(sz, sizeof sz, e->size);
        int sw = sz[0] ? text_w(13, sz) : 0;
        char nm[300];
        text_fit(nm, sizeof nm, e->name, 16, pw - 34 - (sw ? sw + 14 : 6));
        text(x0 + 30, y + 3, 16, e->marked ? C_MARK : e->dir ? C_TEXT : 0xFFC9D4DEu, nm);
        if (sz[0]) text(x0 + pw - 8 - sw, y + 5, 13, C_DIM, sz);
    }
    /* scroll bar */
    if (p->n > rows) {
        int bh = (FOOT_Y - 2 - y0) * rows / p->n;
        if (bh < 12) bh = 12;
        int by = y0 + (FOOT_Y - 2 - y0 - bh) * p->top / (p->n - rows);
        fill(x0 + pw - 3, by, 3, bh, C_LINE);
    }
}

static void draw_footer(const char *hint)
{
    fill(0, FOOT_Y, W, H - FOOT_Y, C_PANEL);
    char h[300];
    text_fit(h, sizeof h, hint, 14, W - 20);
    text(10, FOOT_Y + 8, 14, C_DIM, h);
}

static void draw_files(void)
{
    fill(0, PANE_Y, W, FOOT_Y - PANE_Y, C_LINE);
    draw_pane(0);
    draw_pane(1);
    draw_footer("A open   B back   Y mark   X actions   L/R side   SELECT go to   START wireless (FTP)");
}

static void draw_ftp(void)
{
    fill(0, TOP_H, W, FOOT_Y - TOP_H, C_BG);
    int y = TOP_H + 14;
    text(24, y, 22, C_TEXT, "Wireless file transfer (FTP)");
    y += 40;
    if (!fs.running) {
        fill(24, y, 120, 28, C_LINE);
        text(36, y + 4, 18, C_DIM, "OFF");
        text(160, y + 5, 16, C_DIM, "Press A to start the server");
        y += 52;
        text(24, y, 16, C_TEXT, "Copy files between your PC (or phone) and this device");
        text(24, y + 24, 16, C_TEXT, "over Wi-Fi - no need to take the SD card out.");
        if (fs.error[0]) text(24, y + 60, 15, C_BAD, fs.error);
        draw_footer("A start server   B back to files   SELECT+B screen off (transfers continue)");
        return;
    }
    fill(24, y, 120, 28, C_GOOD);
    text(40, y + 4, 18, C_BG, "ON");
    char st[96];
    if (fs.clients) snprintf(st, sizeof st, "%d computer%s connected", fs.clients, fs.clients == 1 ? "" : "s");
    else snprintf(st, sizeof st, "Waiting for a connection");
    text(160, y + 5, 16, C_TEXT, st);
    y += 44;
    if (!fs.ip[0]) {
        text(24, y, 18, C_BAD, "No Wi-Fi connection - connect Wi-Fi first");
        y += 40;
    } else {
        text(24, y, 15, C_DIM, "On your PC open File Explorer and type in the address bar:");
        y += 22;
        char addr[96];
        snprintf(addr, sizeof addr, "ftp://%s:%d", fs.ip, fs.port);
        fill(24, y, W - 48, 44, C_PANEL2);
        text(40, y + 6, 30, C_FOLDER, addr);
        y += 54;
        char lg[96];
        snprintf(lg, sizeof lg, "User name:  %s        Password:  %s", cfg.ftp_user, cfg.ftp_pass);
        text(24, y, 20, C_TEXT, lg);
        y += 30;
        text(24, y, 14, C_DIM, "Folders you'll see: TF1 / TF2 (and USB). Also works with FileZilla and phone FTP apps.");
        y += 26;
    }
    /* transfer */
    fill(24, y, W - 48, 1, C_LINE);
    y += 10;
    if (fs.xfer_active) {
        char nm[200], line[260], d[32], sp[32];
        text_fit(nm, sizeof nm, fs.xfer_name, 16, W - 200);
        snprintf(line, sizeof line, "%s %s", fs.xfer_active == 1 ? "Receiving" : "Sending", nm);
        text(24, y, 16, C_WARN, line);
        y += 24;
        human(d, sizeof d, fs.xfer_done);
        human(sp, sizeof sp, (long long)fs.speed);
        if (fs.xfer_total > 0) {
            int bw = W - 48;
            fill(24, y, bw, 12, C_LINE);
            fill(24, y, (int)(bw * (double)fs.xfer_done / (double)fs.xfer_total), 12, C_WARN);
            y += 18;
            char tt[32];
            human(tt, sizeof tt, fs.xfer_total);
            snprintf(line, sizeof line, "%s of %s   %s/s", d, tt, sp);
        } else {
            snprintf(line, sizeof line, "%s so far   %s/s", d, sp);
        }
        text(24, y, 15, C_TEXT, line);
        y += 24;
    } else {
        char in[32], out[32], line[160];
        human(in, sizeof in, fs.bytes_in);
        human(out, sizeof out, fs.bytes_out);
        snprintf(line, sizeof line, "Received %d files (%s)    Sent %d files (%s)", fs.files_in, in, fs.files_out, out);
        text(24, y, 15, C_DIM, line);
        y += 26;
    }
    for (int i = 0; i < FTPD_LOG && fs.log[i][0] && y < FOOT_Y - 20; i++, y += 20) {
        char l[160];
        text_fit(l, sizeof l, fs.log[i], 14, W - 48);
        text(24, y, 14, i == 0 ? C_TEXT : C_DIM, l);
    }
    draw_footer("A stop server   X new password   B back to files   SELECT+B screen off (transfers continue)");
}

static void draw_popup(void)
{
    int n = pop.n, vis = n > 14 ? 14 : n;
    int pw = 440, ph = vis * 26 + 50, x = (W - pw) / 2, y = (H - ph) / 2;
    tint(0, 0, W, H, 0xFF000000u, 140);
    fill(x, y, pw, ph, C_PANEL);
    fill(x, y, pw, 2, C_ACCENT);
    text(x + 14, y + 10, 18, C_ACCENT, pop.title);
    int first = pop.sel - vis / 2;
    if (first > n - vis) first = n - vis;
    if (first < 0) first = 0;
    for (int k = 0; k < vis; k++) {
        int i = first + k, iy = y + 40 + k * 26;
        if (i == pop.sel) fill(x + 6, iy - 2, pw - 12, 26, C_SEL);
        char l[96];
        snprintf(l, sizeof l, "%s", pop.it[i].label);
        char *tab = strchr(l, '\t');
        if (tab) *tab++ = 0;
        text(x + 16, iy + 1, 17, pop.it[i].dim ? C_LINE : C_TEXT, l);
        if (tab) text(x + pw - 16 - text_w(14, tab), iy + 4, 14, C_DIM, tab);
    }
}

static void draw_confirm(void)
{
    int pw = 500, ph = 110, x = (W - pw) / 2, y = (H - ph) / 2;
    tint(0, 0, W, H, 0xFF000000u, 150);
    fill(x, y, pw, ph, C_PANEL);
    fill(x, y, pw, 2, cfm.action == ACT_DELETE ? C_BAD : C_ACCENT);
    char t[200];
    text_fit(t, sizeof t, cfm.text, 18, pw - 32);
    text(x + 16, y + 20, 18, C_TEXT, t);
    text(x + 16, y + 66, 16, C_DIM, "A = yes      B = no");
}

static void draw_osk(void)
{
    tint(0, 0, W, H, 0xFF000000u, 160);
    int x0 = 40, y0 = 90, kw = 56, kh = 44;
    fill(x0 - 10, y0 - 70, W - 2 * (x0 - 10), 6 * kh + 110, C_PANEL);
    fill(x0 - 10, y0 - 70, W - 2 * (x0 - 10), 2, C_ACCENT);
    text(x0, y0 - 62, 16, C_DIM, osk.purpose == 1 ? "Rename to:" : "Name of the new folder:");
    fill(x0, y0 - 40, W - 2 * x0, 32, C_BG);
    char t[300];
    snprintf(t, sizeof t, "%s|", osk.text);
    char tf[300];
    text_fit(tf, sizeof tf, t, 20, W - 2 * x0 - 16);
    text(x0 + 8, y0 - 36, 20, C_TEXT, tf);
    for (int r = 0; r < OSK_KEYROWS; r++)
        for (int c = 0; c < OSK_COLS; c++) {
            int x = x0 + c * kw, y = y0 + r * kh;
            int cur = osk.row == r && osk.col == c;
            fill(x + 2, y + 2, kw - 4, kh - 4, cur ? C_SEL : C_PANEL2);
            if (cur) frame(x + 2, y + 2, kw - 4, kh - 4, C_ACCENT);
            char k[2] = { osk_rows[osk.shift][r][c], 0 };
            text(x + kw / 2 - text_w(20, k) / 2, y + 10, 20, C_TEXT, k);
        }
    for (int s = 0; s < 5; s++) {
        int x = x0 + s * 2 * kw, y = y0 + OSK_KEYROWS * kh;
        int cur = osk.row == OSK_KEYROWS && osk.col / 2 == s;
        uint32_t bg = s == 4 ? 0xFF1E5A3Au : s == 3 ? 0xFF4A2228u : C_PANEL2;
        fill(x + 2, y + 2, 2 * kw - 4, kh - 4, cur ? C_SEL : bg);
        if (cur) frame(x + 2, y + 2, 2 * kw - 4, kh - 4, C_ACCENT);
        const char *l = s == 0 && osk.shift ? "SHIFT" : osk_special[s];
        text(x + kw - text_w(16, l) / 2, y + 12, 16, C_TEXT, l);
    }
    text(x0, y0 + 5 * kh + 8, 14, C_DIM, "A type   B delete   Y shift   X space   START done");
}

static void draw_viewer(void)
{
    fill(0, TOP_H, W, FOOT_Y - TOP_H, C_BG);
    char t[300];
    text_fit(t, sizeof t, view.title, 16, W - 120);
    text(10, TOP_H + 6, 16, C_ACCENT, t);
    char pos[40];
    snprintf(pos, sizeof pos, "%d/%d", view.n ? view.top + 1 : 0, view.n);
    text(W - 10 - text_w(14, pos), TOP_H + 8, 14, C_DIM, pos);
    for (int r = 0; r < 18 && view.top + r < view.n; r++) text(10, TOP_H + 30 + r * 21, 15, C_TEXT, view.lines[view.top + r]);
    draw_footer("UP/DOWN scroll   L1/R1 page   B close");
}

static void draw_job(const fo_state *j)
{
    int pw = 520, ph = 130, x = (W - pw) / 2, y = (H - ph) / 2;
    tint(0, 0, W, H, 0xFF000000u, 150);
    fill(x, y, pw, ph, C_PANEL);
    fill(x, y, pw, 2, C_WARN);
    const char *w = j->op == OP_COPY ? "Copying" : j->op == OP_MOVE ? "Moving" : "Deleting";
    char l[300], c[200];
    snprintf(l, sizeof l, "%s %d of %d files", w, j->done_files, j->total_files);
    text(x + 16, y + 14, 18, C_TEXT, l);
    text_fit(c, sizeof c, j->current, 15, pw - 32);
    text(x + 16, y + 42, 15, C_DIM, c);
    double frac = j->total_bytes > 0 ? (double)j->done_bytes / (double)j->total_bytes
                  : j->total_files ? (double)j->done_files / j->total_files : 0;
    if (frac > 1) frac = 1;
    fill(x + 16, y + 70, pw - 32, 12, C_LINE);
    fill(x + 16, y + 70, (int)((pw - 32) * frac), 12, C_WARN);
    char a[32], b[32];
    human(a, sizeof a, j->done_bytes);
    human(b, sizeof b, j->total_bytes);
    snprintf(l, sizeof l, "%s of %s      B = stop", a, b);
    text(x + 16, y + 94, 14, C_DIM, l);
}

static void draw_toast(void)
{
    if (!toast[0] || SDL_GetTicks() - toast_t > 3000) return;
    char t[300];
    text_fit(t, sizeof t, toast, 16, W - 60);
    int tw = text_w(16, t);
    fill(W / 2 - tw / 2 - 14, FOOT_Y - 44, tw + 28, 32, 0xFF22384Bu);
    text(W / 2 - tw / 2, FOOT_Y - 37, 16, C_TEXT, t);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *shot = NULL;
    int shot_secs = 0;
    if (argc >= 4 && !strcmp(argv[1], "--shot")) { shot = argv[2]; shot_secs = atoi(argv[3]); }

    char exe[512];
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (el > 0) {
        exe[el] = 0;
        char *sl = strrchr(exe, '/');
        if (sl) { *sl = 0; if (chdir(exe) != 0) perror("chdir"); }
    }
    if (!getcwd(app_dir, sizeof app_dir)) snprintf(app_dir, sizeof app_dir, ".");
    cfg_load();
    find_locations();
    for (int i = 0; i < 2; i++) {
        const char *start = cfg.path[i][0] ? cfg.path[i] : nlocs > i ? locs[i].path : nlocs ? locs[0].path : "/";
        struct stat st;
        if (stat(start, &st) != 0 || !S_ISDIR(st.st_mode)) start = nlocs ? locs[0].path : "/";
        pane_open(&P[i], start, NULL);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
#ifdef RG35XX
    Uint32 wf = SDL_WINDOW_FULLSCREEN;
#else
    Uint32 wf = 0;
#endif
    SDL_Window *win = SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H, wf);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++) SDL_JoystickOpen(i);
    font_init(app_dir);
    atexit(screen_restore);
    signal(SIGSEGV, fatal_sig);
    signal(SIGTERM, fatal_sig);
    signal(SIGINT, fatal_sig);

    if (cfg.ftp_auto) { cfg.ftp_auto = 0; ftp_toggle(); }

    Uint32 last_dir_check = 0;
    unsigned seen_log = 0;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) handle_event(&e);
        process_repeat();
        { /* dev aid: FB_SIM="down,a,x" presses one key per FB_SIM_MS (default 700 ms) from t=1.5 s */
            static const char *sim;
            static Uint32 next = 1500;
            if (!sim) sim = getenv("FB_SIM") ? getenv("FB_SIM") : "";
            if (*sim && SDL_GetTicks() >= next) {
                static const char *names[A_COUNT] = { "left", "right", "up", "down", "a", "b", "x", "y", "l1", "r1",
                                                      "l2", "r2", "select", "start", "menu" };
                size_t l = strcspn(sim, ",");
                int held[3], nh = 0;
                for (const char *t = sim; t < sim + l && nh < 3;) {
                    size_t tl = strcspn(t, "+,");
                    if (t + tl > sim + l) tl = (size_t)(sim + l - t);
                    for (int a = 0; a < A_COUNT; a++)
                        if (strlen(names[a]) == tl && !strncmp(t, names[a], tl)) { set_pressed(a, 1); held[nh++] = a; }
                    t += tl;
                    if (*t == '+') t++;
                }
                for (int i = 0; i < nh; i++) set_pressed(held[i], 0);
                sim += l;
                if (*sim == ',') sim++;
                next += getenv("FB_SIM_MS") ? (Uint32)atoi(getenv("FB_SIM_MS")) : 700;
            }
        }
        battery_poll();
        beep_poll();
        ftpd_get(&fs);
        fo_state js;
        fo_get(&js);
        if (script_done) {
            script_done = 0;
            show_toast("%s", script_done_msg);
            beep(strstr(script_done_msg, "(with errors)") != NULL);
            pane_load(&P[0], NULL);
            pane_load(&P[1], NULL);
        }
        if (js.finished) {
            fo_ack();
            show_toast("%s", js.result);
            pane_load(&P[0], NULL);
            pane_load(&P[1], NULL);
        }
        /* folders changed by FTP (or anything else): reload, keeping the cursor */
        Uint32 now = SDL_GetTicks();
        if (now - last_dir_check > 1500 || fs.log_seq != seen_log) {
            last_dir_check = now;
            seen_log = fs.log_seq;
            for (int i = 0; i < 2; i++) {
                struct stat st;
                if (stat(P[i].path, &st) == 0 && st.st_mtime != P[i].dir_mtime && !js.active) pane_load(&P[i], NULL);
            }
            static Uint32 last_ip;
            if (fs.running && now - last_ip > 5000) { last_ip = now; ftpd_refresh_ip(); }
        }

        if (screen_off && !shot) { SDL_Delay(50); continue; }
        fill(0, 0, W, H, C_BG);
        draw_top();
        if (view.open) draw_viewer();
        else if (mode == M_FTP) draw_ftp();
        else draw_files();
        if (pop.open) draw_popup();
        if (osk.open) draw_osk();
        if (cfm.open) draw_confirm();
        if (js.active) draw_job(&js);
        draw_toast();

        SDL_UpdateTexture(tex, NULL, fb, W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        Uint32 dt = SDL_GetTicks() - t0;
        if (dt < 33) SDL_Delay(33 - dt);
        if (shot && SDL_GetTicks() > (Uint32)shot_secs * 1000) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(fb, W, H, 32, W * 4, SDL_PIXELFORMAT_ARGB8888);
            SDL_SaveBMP(s, shot);
            SDL_FreeSurface(s);
            break;
        }
    }
    for (int i = 0; i < 2; i++) snprintf(cfg.path[i], sizeof cfg.path[i], "%s", P[i].path);
    ftpd_get(&fs);
    int auto_ftp = fs.running;
    ftpd_stop();
    cfg.ftp_auto = auto_ftp; /* running at exit -> starts again next time */
    if (!shot) cfg_save();
    sync();
    screen_restore();
    SDL_Quit();
    return 0;
}
