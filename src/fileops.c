/* Copy / move / delete worker (see fileops.h). */
#define _GNU_SOURCE
#include "fileops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static fo_state S;
static volatile int cancel_req;
static char **job_src;
static int job_n;
static char job_dst[1024];

static void set_cur(const char *p)
{
    const char *b = strrchr(p, '/');
    pthread_mutex_lock(&m);
    snprintf(S.current, sizeof S.current, "%s", b ? b + 1 : p);
    pthread_mutex_unlock(&m);
}

static void add_done(long long bytes, int files)
{
    pthread_mutex_lock(&m);
    S.done_bytes += bytes;
    S.done_files += files;
    pthread_mutex_unlock(&m);
}

/* totals for the progress bar */
static void count(const char *p, long long *bytes, int *files)
{
    struct stat st;
    if (lstat(p, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            count(c, bytes, files);
        }
        if (d) closedir(d);
    } else {
        *bytes += st.st_size;
        (*files)++;
    }
}

static const char *base(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* dst/<name>, or "<name> (2).ext" ... if that exists */
static void free_name(const char *dstdir, const char *name, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dstdir, name);
    struct stat st;
    if (lstat(out, &st) != 0) return;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) dot = name + strlen(name);
    for (int i = 2; i < 1000; i++) {
        snprintf(out, n, "%s/%.*s (%d)%s", dstdir, (int)(dot - name), name, i, dot);
        if (lstat(out, &st) != 0) return;
    }
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    static char buf[256 * 1024];
    int ok = 1;
    for (;;) {
        ssize_t k = read(in, buf, sizeof buf);
        if (k < 0) { ok = 0; break; }
        if (k == 0) break;
        char *p = buf;
        ssize_t left = k;
        while (left > 0) {
            ssize_t w = write(out, p, (size_t)left);
            if (w <= 0) { ok = 0; break; }
            p += w;
            left -= w;
        }
        if (!ok) break;
        add_done(k, 0);
        if (cancel_req) { ok = 0; break; }
    }
    if (fsync(out) != 0) ok = 0;
    close(in);
    if (close(out) != 0) ok = 0;
    if (!ok) unlink(dst); /* no half-copied files left behind */
    struct stat st;
    if (ok && stat(src, &st) == 0) { /* keep the modification time */
        struct timeval tv[2] = { { st.st_atime, 0 }, { st.st_mtime, 0 } };
        utimes(dst, tv);
    }
    return ok ? 0 : -1;
}

static int copy_tree(const char *src, const char *dst)
{
    if (cancel_req) return -1;
    struct stat st;
    if (lstat(src, &st) != 0) return -1;
    set_cur(src);
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0755) != 0 && errno != EEXIST) return -1;
        int err = 0;
        DIR *d = opendir(src);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char s2[2048], d2[2048];
            snprintf(s2, sizeof s2, "%s/%s", src, e->d_name);
            snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
            if (copy_tree(s2, d2) != 0) err = -1;
            if (cancel_req) break;
        }
        if (d) closedir(d);
        return err;
    }
    int r = copy_file(src, dst);
    add_done(0, 1);
    return r;
}

static int delete_tree(const char *p)
{
    if (cancel_req) return -1;
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    set_cur(p);
    if (S_ISDIR(st.st_mode)) {
        int err = 0;
        DIR *d = opendir(p);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char c[2048];
            snprintf(c, sizeof c, "%s/%s", p, e->d_name);
            if (delete_tree(c) != 0) err = -1;
        }
        if (d) closedir(d);
        if (rmdir(p) != 0) err = -1;
        return err;
    }
    int r = unlink(p);
    add_done(st.st_size, 1);
    return r;
}

static void *worker(void *u)
{
    (void)u;
    int op = S.op, errors = 0;
    long long tb = 0;
    int tf = 0;
    for (int i = 0; i < job_n; i++) count(job_src[i], &tb, &tf);
    pthread_mutex_lock(&m);
    S.total_bytes = tb;
    S.total_files = tf;
    pthread_mutex_unlock(&m);
    for (int i = 0; i < job_n && !cancel_req; i++) {
        const char *src = job_src[i];
        if (op == OP_DELETE) {
            if (delete_tree(src) != 0) errors++;
            continue;
        }
        char dst[2048];
        size_t sl = strlen(src);
        if (!strncmp(job_dst, src, sl) && (job_dst[sl] == '/' || !job_dst[sl])) { errors++; continue; } /* into itself */
        free_name(job_dst, base(src), dst, sizeof dst);
        if (op == OP_MOVE && rename(src, dst) == 0) { /* same card: instant */
            long long b = 0;
            int f = 0;
            count(dst, &b, &f);
            add_done(b, f);
            continue;
        }
        if (copy_tree(src, dst) != 0) { errors++; continue; }
        if (op == OP_MOVE && !cancel_req && delete_tree(src) != 0) errors++;
    }
    sync();
    pthread_mutex_lock(&m);
    const char *what = op == OP_COPY ? "Copied" : op == OP_MOVE ? "Moved" : "Deleted";
    if (cancel_req) snprintf(S.result, sizeof S.result, "Stopped - %d of %d files done", S.done_files, S.total_files);
    else if (errors) snprintf(S.result, sizeof S.result, "%s with %d problem%s (card full or read-only?)", what, errors,
                              errors == 1 ? "" : "s");
    else snprintf(S.result, sizeof S.result, "%s %d file%s", what, S.done_files, S.done_files == 1 ? "" : "s");
    S.errors = errors;
    S.active = 0;
    S.finished = 1;
    pthread_mutex_unlock(&m);
    for (int i = 0; i < job_n; i++) free(job_src[i]);
    free(job_src);
    job_src = NULL;
    job_n = 0;
    return NULL;
}

int fo_start(int op, char **srcs, int n, const char *dstdir)
{
    if (S.active || n <= 0) return -1;
    job_src = calloc((size_t)n, sizeof *job_src);
    if (!job_src) return -1;
    for (int i = 0; i < n; i++) job_src[i] = strdup(srcs[i]);
    job_n = n;
    snprintf(job_dst, sizeof job_dst, "%s", dstdir ? dstdir : "");
    pthread_mutex_lock(&m);
    memset(&S, 0, sizeof S);
    S.active = 1;
    S.op = op;
    pthread_mutex_unlock(&m);
    cancel_req = 0;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) != 0) { S.active = 0; return -1; }
    pthread_detach(t);
    return 0;
}

void fo_cancel(void) { cancel_req = 1; }

void fo_get(fo_state *out)
{
    pthread_mutex_lock(&m);
    *out = S;
    pthread_mutex_unlock(&m);
}

void fo_ack(void)
{
    pthread_mutex_lock(&m);
    S.finished = 0;
    pthread_mutex_unlock(&m);
}
