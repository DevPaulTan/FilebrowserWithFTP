/* Copy / move / delete on a background thread with progress. */
#ifndef FILEOPS_H
#define FILEOPS_H

enum { OP_COPY = 1, OP_MOVE, OP_DELETE };

typedef struct {
    int active;            /* a job is running */
    int op;
    long long done_bytes, total_bytes;
    int done_files, total_files;
    char current[256];     /* file being worked on */
    int finished;          /* set when a job ends (cleared by fo_ack) */
    int errors;
    char result[160];      /* summary for the user */
} fo_state;

/* srcs: full paths; dstdir: target folder (copy / move). Copies are never
 * overwritten: an existing name gets " (2)", " (3)", ... */
int fo_start(int op, char **srcs, int n, const char *dstdir);
void fo_cancel(void);
void fo_get(fo_state *out);
void fo_ack(void);

#endif
