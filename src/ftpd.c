/* Small FTP server (see ftpd.h). One thread for accepting, one per client. */
#define _GNU_SOURCE
#include "ftpd.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_SESS 8
#define PATHMAX 1024

static pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
static ftpd_state S;
static volatile int g_stop;
static int g_lfd = -1;
static pthread_t g_thr;
static char g_user[64], g_pass[64];
static ftpd_root g_roots[FTPD_MAX_ROOTS];
static int g_nroots;
static int g_sess_fd[MAX_SESS];

static void logline(const char *fmt, ...)
{
    char b[110];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&gm);
    memmove(&S.log[1], &S.log[0], sizeof S.log[0] * (FTPD_LOG - 1));
    snprintf(S.log[0], sizeof S.log[0], "%s", b);
    S.log_seq++;
    pthread_mutex_unlock(&gm);
    fprintf(stderr, "[ftp] %s\n", b);
}

static void find_ip(char *out, size_t n)
{
    out[0] = 0;
    struct ifaddrs *ifa, *i;
    if (getifaddrs(&ifa) != 0) return;
    char best[64] = "";
    for (i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP)) continue;
        if (i->ifa_flags & IFF_LOOPBACK) continue;
        char a[64];
        inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, a, sizeof a);
        if (!best[0] || !strncmp(i->ifa_name, "wlan", 4)) snprintf(best, sizeof best, "%s", a);
        if (!strncmp(i->ifa_name, "wlan", 4)) break;
    }
    freeifaddrs(ifa);
    snprintf(out, n, "%s", best);
}

void ftpd_refresh_ip(void)
{
    char ip[64];
    find_ip(ip, sizeof ip);
    pthread_mutex_lock(&gm);
    snprintf(S.ip, sizeof S.ip, "%s", ip);
    pthread_mutex_unlock(&gm);
}

/* ---------------------------------------------------------------- session */

typedef struct {
    int fd, slot;
    char cwd[PATHMAX];      /* virtual path, "/" = the list of storage locations */
    int logged, user_ok;
    int pasv_fd;
    struct sockaddr_in port_addr;
    int use_port;
    long long rest;
    char rnfr[PATHMAX];     /* real path of RNFR */
    struct sockaddr_in local;
    char rbuf[4096];
    int rlen;
    char peer[64];
} sess;

static void reply(sess *s, const char *fmt, ...)
{
    char b[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof b - 3) n = (int)sizeof b - 3;
    b[n++] = '\r';
    b[n++] = '\n';
    send(s->fd, b, (size_t)n, MSG_NOSIGNAL);
}

/* Reads one command line (without CRLF). Returns -1 on disconnect / stop. */
static int read_line(sess *s, char *out, size_t n)
{
    for (;;) {
        for (int i = 0; i < s->rlen; i++) {
            if (s->rbuf[i] == '\n') {
                int l = i;
                if (l > 0 && s->rbuf[l - 1] == '\r') l--;
                if ((size_t)l >= n) l = (int)n - 1;
                memcpy(out, s->rbuf, (size_t)l);
                out[l] = 0;
                memmove(s->rbuf, s->rbuf + i + 1, (size_t)(s->rlen - i - 1));
                s->rlen -= i + 1;
                return 0;
            }
        }
        if (s->rlen >= (int)sizeof s->rbuf - 1) s->rlen = 0; /* absurdly long line: drop */
        struct pollfd pf = { s->fd, POLLIN, 0 };
        int r = poll(&pf, 1, 1000);
        if (g_stop) return -1;
        if (r == 0) continue;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        ssize_t k = recv(s->fd, s->rbuf + s->rlen, sizeof s->rbuf - 1 - (size_t)s->rlen, 0);
        if (k <= 0) return -1;
        s->rlen += (int)k;
    }
}

/* ---- paths ---- */

/* Builds a normalised virtual absolute path from cwd + arg (no "." / ".."). */
static void vpath(const sess *s, const char *arg, char *out, size_t n)
{
    char tmp[PATHMAX * 2];
    if (!arg || !*arg) snprintf(tmp, sizeof tmp, "%s", s->cwd);
    else if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", s->cwd, arg);
    char res[PATHMAX] = "";
    size_t rl = 0;
    char *save, *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".") || !*tok) {
        } else if (!strcmp(tok, "..")) {
            while (rl > 0 && res[rl - 1] != '/') rl--;
            if (rl > 0) rl--;
            res[rl] = 0;
        } else {
            size_t tl = strlen(tok);
            if (rl + 1 + tl < sizeof res) {
                res[rl++] = '/';
                memcpy(res + rl, tok, tl);
                rl += tl;
                res[rl] = 0;
            }
        }
        tok = strtok_r(NULL, "/", &save);
    }
    snprintf(out, n, "%s", rl ? res : "/");
}

/* Virtual -> real path. Returns 1 = real path, 0 = the virtual root, -1 = no such location. */
static int realpath_of(const char *v, char *real, size_t n)
{
    if (!strcmp(v, "/")) { real[0] = 0; return 0; }
    const char *p = v + 1;
    size_t cl = strcspn(p, "/");
    for (int i = 0; i < g_nroots; i++)
        if (strlen(g_roots[i].name) == cl && !strncmp(p, g_roots[i].name, cl)) {
            snprintf(real, n, "%s%s", g_roots[i].path, p + cl);
            return 1;
        }
    return -1;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* ---- data connection ---- */

static void close_pasv(sess *s)
{
    if (s->pasv_fd >= 0) close(s->pasv_fd);
    s->pasv_fd = -1;
}

static int open_data(sess *s)
{
    int fd = -1;
    if (s->pasv_fd >= 0) {
        struct pollfd pf = { s->pasv_fd, POLLIN, 0 };
        if (poll(&pf, 1, 15000) > 0) fd = accept(s->pasv_fd, NULL, NULL);
        close_pasv(s);
    } else if (s->use_port) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct timeval tv = { 10, 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            if (connect(fd, (struct sockaddr *)&s->port_addr, sizeof s->port_addr) != 0) { close(fd); fd = -1; }
        }
        s->use_port = 0;
    }
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        int big = 512 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    }
    return fd;
}

static int send_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
        if (k <= 0) { if (k < 0 && errno == EINTR) continue; return -1; }
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ---- listings ---- */

static void ls_line(char *out, size_t n, const char *name, const struct stat *st)
{
    char perm[11] = "----------";
    if (S_ISDIR(st->st_mode)) perm[0] = 'd';
    static const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        if (st->st_mode & (1 << (8 - i))) perm[i + 1] = rwx[i];
    struct tm tm;
    localtime_r(&st->st_mtime, &tm);
    char date[32];
    time_t now = time(NULL);
    if (now - st->st_mtime < 180L * 24 * 3600 && st->st_mtime <= now + 3600) strftime(date, sizeof date, "%b %e %H:%M", &tm);
    else strftime(date, sizeof date, "%b %e  %Y", &tm);
    snprintf(out, n, "%s 1 rg rg %13lld %s %s\r\n", perm, (long long)st->st_size, date, name);
}

static void mlsx_fact(char *out, size_t n, const char *name, const struct stat *st)
{
    struct tm tm;
    gmtime_r(&st->st_mtime, &tm);
    char mod[20];
    strftime(mod, sizeof mod, "%Y%m%d%H%M%S", &tm);
    if (S_ISDIR(st->st_mode))
        snprintf(out, n, "type=dir;modify=%s; %s", mod, name);
    else
        snprintf(out, n, "type=file;size=%lld;modify=%s; %s", (long long)st->st_size, mod, name);
}

static void do_list(sess *s, const char *arg, int mode /*0 LIST, 1 NLST, 2 MLSD*/)
{
    /* skip "ls" style options like -la */
    while (arg && *arg == '-') {
        arg = strchr(arg, ' ');
        if (arg) while (*arg == ' ') arg++;
    }
    char v[PATHMAX], real[PATHMAX];
    vpath(s, arg, v, sizeof v);
    int r = realpath_of(v, real, sizeof real);
    if (r < 0) { reply(s, "550 No such file or directory"); return; }
    struct stat st;
    if (r == 1 && stat(real, &st) != 0) { reply(s, "550 No such file or directory"); return; }
    reply(s, "150 Here comes the listing");
    int dfd = open_data(s);
    if (dfd < 0) { reply(s, "425 Can't open data connection"); return; }
    char line[PATHMAX + 128];
    if (r == 0) { /* virtual root: the storage locations */
        for (int i = 0; i < g_nroots; i++) {
            if (stat(g_roots[i].path, &st) != 0) continue;
            if (mode == 1) snprintf(line, sizeof line, "%s\r\n", g_roots[i].name);
            else if (mode == 2) { mlsx_fact(line, sizeof line - 2, g_roots[i].name, &st); strcat(line, "\r\n"); }
            else ls_line(line, sizeof line, g_roots[i].name, &st);
            send_all(dfd, line, strlen(line));
        }
    } else if (!S_ISDIR(st.st_mode)) {
        if (mode == 2) { mlsx_fact(line, sizeof line - 2, base_name(real), &st); strcat(line, "\r\n"); }
        else if (mode == 1) snprintf(line, sizeof line, "%s\r\n", base_name(real));
        else ls_line(line, sizeof line, base_name(real), &st);
        send_all(dfd, line, strlen(line));
    } else {
        DIR *d = opendir(real);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[PATHMAX * 2];
            snprintf(p, sizeof p, "%s/%s", real, e->d_name);
            if (stat(p, &st) != 0) continue;
            if (mode == 1) snprintf(line, sizeof line, "%s\r\n", e->d_name);
            else if (mode == 2) { mlsx_fact(line, sizeof line - 2, e->d_name, &st); strcat(line, "\r\n"); }
            else ls_line(line, sizeof line, e->d_name, &st);
            if (send_all(dfd, line, strlen(line)) < 0) break;
        }
        if (d) closedir(d);
    }
    close(dfd);
    reply(s, "226 Directory send OK");
}

/* ---- transfers ---- */

static void xfer_begin(int dir, const char *name, long long total, long long start)
{
    pthread_mutex_lock(&gm);
    S.xfer_active = dir;
    snprintf(S.xfer_name, sizeof S.xfer_name, "%s", name);
    S.xfer_total = total;
    S.xfer_done = start;
    S.speed = 0;
    pthread_mutex_unlock(&gm);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void xfer_progress(long long done, long long delta, int dir, double t0, long long start)
{
    pthread_mutex_lock(&gm);
    S.xfer_done = done;
    if (dir == 1) S.bytes_in += delta;
    else S.bytes_out += delta;
    double el = now_s() - t0;
    if (el > 0.3) S.speed = (done - start) / el;
    pthread_mutex_unlock(&gm);
}

static void xfer_end(int dir, int ok)
{
    pthread_mutex_lock(&gm);
    S.xfer_active = 0;
    if (ok) { if (dir == 1) S.files_in++; else S.files_out++; }
    pthread_mutex_unlock(&gm);
}

static void human(char *out, size_t n, long long v)
{
    if (v >= 1LL << 30) snprintf(out, n, "%.2f GB", v / 1073741824.0);
    else if (v >= 1LL << 20) snprintf(out, n, "%.1f MB", v / 1048576.0);
    else if (v >= 1024) snprintf(out, n, "%.0f KB", v / 1024.0);
    else snprintf(out, n, "%lld B", v);
}

static void do_retr(sess *s, const char *arg)
{
    char v[PATHMAX], real[PATHMAX];
    vpath(s, arg, v, sizeof v);
    if (realpath_of(v, real, sizeof real) != 1) { reply(s, "550 No such file"); s->rest = 0; return; }
    int f = open(real, O_RDONLY);
    struct stat st;
    if (f < 0 || fstat(f, &st) != 0 || S_ISDIR(st.st_mode)) {
        if (f >= 0) close(f);
        reply(s, "550 Can't open file");
        s->rest = 0;
        return;
    }
    long long start = s->rest > 0 && s->rest <= st.st_size ? s->rest : 0;
    s->rest = 0;
    if (start) lseek(f, start, SEEK_SET);
    reply(s, "150 Opening BINARY connection for %s (%lld bytes)", base_name(real), (long long)st.st_size);
    int dfd = open_data(s);
    if (dfd < 0) { close(f); reply(s, "425 Can't open data connection"); return; }
    xfer_begin(2, base_name(real), st.st_size, start);
    static __thread char *buf;
    if (!buf) buf = malloc(256 * 1024);
    long long done = start;
    double t0 = now_s();
    int ok = 1;
    for (;;) {
        ssize_t k = read(f, buf, 256 * 1024);
        if (k < 0) { ok = 0; break; }
        if (k == 0) break;
        if (send_all(dfd, buf, (size_t)k) < 0 || g_stop) { ok = 0; break; }
        done += k;
        xfer_progress(done, k, 2, t0, start);
    }
    close(f);
    close(dfd);
    xfer_end(2, ok);
    char hs[32];
    human(hs, sizeof hs, done - start);
    if (ok) { reply(s, "226 Transfer complete"); logline("Sent %s (%s)", base_name(real), hs); }
    else { reply(s, "426 Transfer aborted"); logline("Sending %s stopped", base_name(real)); }
}

static void do_stor(sess *s, const char *arg, int append)
{
    char v[PATHMAX], real[PATHMAX];
    vpath(s, arg, v, sizeof v);
    if (realpath_of(v, real, sizeof real) != 1) { reply(s, "553 Can't store here (open TF1 / TF2 first)"); s->rest = 0; return; }
    long long start = append ? -1 : s->rest;
    s->rest = 0;
    int fl = O_WRONLY | O_CREAT | (append ? O_APPEND : (start > 0 ? 0 : O_TRUNC));
    int f = open(real, fl, 0644);
    if (f < 0) { reply(s, "553 Can't create file: %s", strerror(errno)); return; }
    if (start > 0) lseek(f, start, SEEK_SET);
    if (start < 0) start = lseek(f, 0, SEEK_END);
    reply(s, "150 Ok to send data");
    int dfd = open_data(s);
    if (dfd < 0) { close(f); reply(s, "425 Can't open data connection"); return; }
    xfer_begin(1, base_name(real), 0, start);
    static __thread char *buf;
    if (!buf) buf = malloc(256 * 1024);
    long long done = start;
    double t0 = now_s();
    int ok = 1;
    for (;;) {
        ssize_t k = recv(dfd, buf, 256 * 1024, 0);
        if (k < 0) { if (errno == EINTR) continue; ok = 0; break; }
        if (k == 0) break;
        char *p = buf;
        ssize_t left = k;
        while (left > 0) {
            ssize_t w = write(f, p, (size_t)left);
            if (w <= 0) { ok = 0; break; }
            p += w;
            left -= w;
        }
        if (!ok || g_stop) { ok = 0; break; }
        done += k;
        xfer_progress(done, k, 1, t0, start);
    }
    fsync(f); /* on the card now, even if the device is switched off right after */
    close(f);
    close(dfd);
    xfer_end(1, ok);
    char hs[32];
    human(hs, sizeof hs, done - start);
    if (ok) { reply(s, "226 Transfer complete"); logline("Received %s (%s)", base_name(real), hs); }
    else { reply(s, "426 Transfer aborted"); logline("Receiving %s stopped", base_name(real)); }
}

/* ---- command loop ---- */

static void *sess_main(void *arg)
{
    sess *s = arg;
    char line[2048];
    reply(s, "220 RG35XX file transfer ready");
    while (!g_stop && read_line(s, line, sizeof line) == 0) {
        char *cmd = line, *a = strchr(line, ' ');
        if (a) { *a++ = 0; while (*a == ' ') a++; }
        else a = line + strlen(line);
        for (char *c = cmd; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;

        if (!strcmp(cmd, "USER")) {
            s->user_ok = !strcmp(a, g_user);
            s->logged = 0;
            reply(s, "331 Password please");
            continue;
        }
        if (!strcmp(cmd, "PASS")) {
            if (s->user_ok && !strcmp(a, g_pass)) {
                s->logged = 1;
                pthread_mutex_lock(&gm);
                S.clients++;
                pthread_mutex_unlock(&gm);
                reply(s, "230 Logged in");
                logline("%s connected", s->peer);
            } else {
                sleep(1); /* slow down guessing */
                reply(s, "530 Wrong user name or password (see the RG35XX screen)");
            }
            continue;
        }
        if (!strcmp(cmd, "QUIT")) { reply(s, "221 Bye"); break; }
        if (!strcmp(cmd, "SYST")) { reply(s, "215 UNIX Type: L8"); continue; }
        if (!strcmp(cmd, "FEAT")) {
            reply(s, "211-Features:\r\n UTF8\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n PASV\r\n EPSV\r\n"
                     " MLST type*;size*;modify*;\r\n211 End");
            continue;
        }
        if (!strcmp(cmd, "OPTS")) { reply(s, "200 OK"); continue; }
        if (!strcmp(cmd, "AUTH")) { reply(s, "502 TLS not supported, use plain FTP"); continue; }
        if (!strcmp(cmd, "NOOP")) { reply(s, "200 OK"); continue; }
        if (!s->logged) { reply(s, "530 Please log in"); continue; }

        if (!strcmp(cmd, "PWD") || !strcmp(cmd, "XPWD")) {
            reply(s, "257 \"%s\" is the current folder", s->cwd);
        } else if (!strcmp(cmd, "CWD") || !strcmp(cmd, "XCWD") || !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP")) {
            char v[PATHMAX], real[PATHMAX];
            vpath(s, !strcmp(cmd, "CDUP") || !strcmp(cmd, "XCUP") ? ".." : a, v, sizeof v);
            int r = realpath_of(v, real, sizeof real);
            struct stat st;
            if (r == 0 || (r == 1 && stat(real, &st) == 0 && S_ISDIR(st.st_mode))) {
                snprintf(s->cwd, sizeof s->cwd, "%s", v);
                reply(s, "250 OK");
            } else reply(s, "550 No such folder");
        } else if (!strcmp(cmd, "TYPE") || !strcmp(cmd, "MODE") || !strcmp(cmd, "STRU")) {
            reply(s, "200 OK");
        } else if (!strcmp(cmd, "PASV") || !strcmp(cmd, "EPSV")) {
            close_pasv(s);
            s->use_port = 0;
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in sa = s->local;
            sa.sin_port = 0;
            socklen_t sl = sizeof sa;
            if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 1) != 0 ||
                getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
                if (fd >= 0) close(fd);
                reply(s, "425 Can't open passive port");
                continue;
            }
            s->pasv_fd = fd;
            unsigned port = ntohs(sa.sin_port);
            if (!strcmp(cmd, "EPSV")) reply(s, "229 Entering Extended Passive Mode (|||%u|)", port);
            else {
                unsigned char *ip = (unsigned char *)&s->local.sin_addr;
                reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip[0], ip[1], ip[2], ip[3], port >> 8, port & 255);
            }
        } else if (!strcmp(cmd, "PORT")) {
            unsigned h[6];
            if (sscanf(a, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) == 6) {
                close_pasv(s);
                memset(&s->port_addr, 0, sizeof s->port_addr);
                s->port_addr.sin_family = AF_INET;
                s->port_addr.sin_addr.s_addr = htonl((h[0] << 24) | (h[1] << 16) | (h[2] << 8) | h[3]);
                s->port_addr.sin_port = htons((uint16_t)((h[4] << 8) | h[5]));
                s->use_port = 1;
                reply(s, "200 PORT OK");
            } else reply(s, "501 Bad PORT");
        } else if (!strcmp(cmd, "EPRT")) {
            char d = a[0], ipb[64];
            unsigned proto, port;
            char fmt[40];
            snprintf(fmt, sizeof fmt, "%c%%u%c%%63[^%c]%c%%u", d, d, d, d);
            if (d && sscanf(a, fmt, &proto, ipb, &port) == 3 && proto == 1) {
                close_pasv(s);
                memset(&s->port_addr, 0, sizeof s->port_addr);
                s->port_addr.sin_family = AF_INET;
                inet_pton(AF_INET, ipb, &s->port_addr.sin_addr);
                s->port_addr.sin_port = htons((uint16_t)port);
                s->use_port = 1;
                reply(s, "200 EPRT OK");
            } else reply(s, "522 Only IPv4 (1) supported");
        } else if (!strcmp(cmd, "LIST")) {
            do_list(s, a, 0);
        } else if (!strcmp(cmd, "NLST")) {
            do_list(s, a, 1);
        } else if (!strcmp(cmd, "MLSD")) {
            do_list(s, a, 2);
        } else if (!strcmp(cmd, "MLST")) {
            char v[PATHMAX], real[PATHMAX], f[PATHMAX + 128];
            vpath(s, a, v, sizeof v);
            int r = realpath_of(v, real, sizeof real);
            struct stat st;
            if (r == 0) { reply(s, "250-Listing\r\n type=dir; /\r\n250 End"); continue; }
            if (r < 0 || stat(real, &st) != 0) { reply(s, "550 No such file"); continue; }
            mlsx_fact(f, sizeof f, v, &st);
            reply(s, "250-Listing\r\n %s\r\n250 End", f);
        } else if (!strcmp(cmd, "RETR")) {
            do_retr(s, a);
        } else if (!strcmp(cmd, "STOR")) {
            do_stor(s, a, 0);
        } else if (!strcmp(cmd, "APPE")) {
            do_stor(s, a, 1);
        } else if (!strcmp(cmd, "REST")) {
            s->rest = atoll(a);
            reply(s, "350 Restarting at %lld", s->rest);
        } else if (!strcmp(cmd, "SIZE") || !strcmp(cmd, "MDTM")) {
            char v[PATHMAX], real[PATHMAX];
            vpath(s, a, v, sizeof v);
            struct stat st;
            if (realpath_of(v, real, sizeof real) != 1 || stat(real, &st) != 0) { reply(s, "550 No such file"); continue; }
            if (cmd[0] == 'S') reply(s, "213 %lld", (long long)st.st_size);
            else {
                struct tm tm;
                gmtime_r(&st.st_mtime, &tm);
                char b[20];
                strftime(b, sizeof b, "%Y%m%d%H%M%S", &tm);
                reply(s, "213 %s", b);
            }
        } else if (!strcmp(cmd, "DELE")) {
            char v[PATHMAX], real[PATHMAX];
            vpath(s, a, v, sizeof v);
            if (realpath_of(v, real, sizeof real) == 1 && unlink(real) == 0) {
                reply(s, "250 Deleted");
                logline("Deleted %s", base_name(real));
            } else reply(s, "550 Can't delete: %s", strerror(errno));
        } else if (!strcmp(cmd, "MKD") || !strcmp(cmd, "XMKD")) {
            char v[PATHMAX], real[PATHMAX];
            vpath(s, a, v, sizeof v);
            if (realpath_of(v, real, sizeof real) == 1 && mkdir(real, 0755) == 0) {
                reply(s, "257 \"%s\" created", v);
                logline("New folder %s", base_name(real));
            } else reply(s, "550 Can't create folder: %s", strerror(errno));
        } else if (!strcmp(cmd, "RMD") || !strcmp(cmd, "XRMD")) {
            char v[PATHMAX], real[PATHMAX];
            vpath(s, a, v, sizeof v);
            if (realpath_of(v, real, sizeof real) == 1 && rmdir(real) == 0) {
                reply(s, "250 Folder removed");
                logline("Removed folder %s", base_name(real));
            } else reply(s, "550 Can't remove folder: %s", strerror(errno));
        } else if (!strcmp(cmd, "RNFR")) {
            char v[PATHMAX];
            vpath(s, a, v, sizeof v);
            struct stat st;
            if (realpath_of(v, s->rnfr, sizeof s->rnfr) == 1 && stat(s->rnfr, &st) == 0) reply(s, "350 Ready for RNTO");
            else { s->rnfr[0] = 0; reply(s, "550 No such file"); }
        } else if (!strcmp(cmd, "RNTO")) {
            char v[PATHMAX], real[PATHMAX];
            vpath(s, a, v, sizeof v);
            if (s->rnfr[0] && realpath_of(v, real, sizeof real) == 1 && rename(s->rnfr, real) == 0) {
                reply(s, "250 Renamed");
                logline("Renamed to %s", base_name(real));
            } else reply(s, "553 Can't rename: %s", strerror(errno));
            s->rnfr[0] = 0;
        } else if (!strcmp(cmd, "ABOR")) {
            close_pasv(s);
            reply(s, "226 Aborted");
        } else if (!strcmp(cmd, "ALLO")) {
            reply(s, "202 OK");
        } else if (!strcmp(cmd, "SITE")) {
            reply(s, "200 OK"); /* CHMOD etc.: ignored (exFAT / FAT cards have no permissions) */
        } else if (!strcmp(cmd, "STAT")) {
            reply(s, "211 RG35XX FTP server");
        } else if (!strcmp(cmd, "HELP")) {
            reply(s, "214 Commands: USER PASS CWD CDUP PWD LIST NLST MLSD MLST RETR STOR APPE REST SIZE MDTM "
                     "DELE MKD RMD RNFR RNTO PASV EPSV PORT EPRT TYPE NOOP QUIT");
        } else {
            reply(s, "502 Command not implemented");
        }
    }
    close_pasv(s);
    if (s->logged) {
        pthread_mutex_lock(&gm);
        S.clients--;
        pthread_mutex_unlock(&gm);
        logline("%s disconnected", s->peer);
    }
    pthread_mutex_lock(&gm);
    g_sess_fd[s->slot] = -1;
    pthread_mutex_unlock(&gm);
    close(s->fd);
    free(s);
    return NULL;
}

static void *accept_main(void *u)
{
    (void)u;
    while (!g_stop) {
        struct pollfd pf = { g_lfd, POLLIN, 0 };
        int r = poll(&pf, 1, 500);
        if (r <= 0) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        int fd = accept(g_lfd, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        pthread_mutex_lock(&gm);
        int slot = -1;
        for (int i = 0; i < MAX_SESS; i++)
            if (g_sess_fd[i] < 0) { slot = i; g_sess_fd[i] = fd; break; }
        pthread_mutex_unlock(&gm);
        if (slot < 0) {
            const char *m = "421 Too many connections\r\n";
            send(fd, m, strlen(m), MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        sess *s = calloc(1, sizeof *s);
        if (!s) { close(fd); continue; }
        s->fd = fd;
        s->slot = slot;
        s->pasv_fd = -1;
        snprintf(s->cwd, sizeof s->cwd, "/");
        socklen_t ll = sizeof s->local;
        getsockname(fd, (struct sockaddr *)&s->local, &ll);
        inet_ntop(AF_INET, &peer.sin_addr, s->peer, sizeof s->peer);
        pthread_t t;
        if (pthread_create(&t, NULL, sess_main, s) == 0) pthread_detach(t);
        else { close(fd); free(s); pthread_mutex_lock(&gm); g_sess_fd[slot] = -1; pthread_mutex_unlock(&gm); }
    }
    return NULL;
}

int ftpd_start(int port, const char *user, const char *pass, const ftpd_root *roots, int nroots)
{
    if (S.running) return 0;
    snprintf(g_user, sizeof g_user, "%s", user);
    snprintf(g_pass, sizeof g_pass, "%s", pass);
    g_nroots = nroots > FTPD_MAX_ROOTS ? FTPD_MAX_ROOTS : nroots;
    memcpy(g_roots, roots, sizeof g_roots[0] * (size_t)g_nroots);
    for (int i = 0; i < MAX_SESS; i++) g_sess_fd[i] = -1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        pthread_mutex_lock(&gm);
        snprintf(S.error, sizeof S.error, "Can't open port %d: %s", port, strerror(errno));
        pthread_mutex_unlock(&gm);
        if (fd >= 0) close(fd);
        return -1;
    }
    g_lfd = fd;
    g_stop = 0;
    pthread_mutex_lock(&gm);
    S.running = 1;
    S.port = port;
    S.error[0] = 0;
    pthread_mutex_unlock(&gm);
    ftpd_refresh_ip();
    if (pthread_create(&g_thr, NULL, accept_main, NULL) != 0) {
        close(fd);
        g_lfd = -1;
        S.running = 0;
        return -1;
    }
    logline("Server started on port %d", port);
    return 0;
}

void ftpd_stop(void)
{
    if (!S.running) return;
    g_stop = 1;
    pthread_join(g_thr, NULL);
    close(g_lfd);
    g_lfd = -1;
    pthread_mutex_lock(&gm);
    for (int i = 0; i < MAX_SESS; i++)
        if (g_sess_fd[i] >= 0) shutdown(g_sess_fd[i], SHUT_RDWR);
    S.running = 0;
    S.clients = 0;
    S.xfer_active = 0;
    pthread_mutex_unlock(&gm);
    sync();
    logline("Server stopped");
}

void ftpd_get(ftpd_state *out)
{
    pthread_mutex_lock(&gm);
    *out = S;
    pthread_mutex_unlock(&gm);
}
