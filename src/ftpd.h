/* Small FTP server for wireless file transfer (runs while the app is open).
 * Works with Windows File Explorer (ftp://ip:port), FileZilla, WinSCP and phone
 * FTP apps: passive (PASV/EPSV) and active (PORT/EPRT) mode, LIST/MLSD, resume
 * (REST), UTF-8 names. One login (user + password). The client sees a virtual
 * root with one folder per storage location (e.g. TF1, TF2, USB-...). */
#ifndef FTPD_H
#define FTPD_H

#define FTPD_MAX_ROOTS 8
#define FTPD_LOG 8

typedef struct {
    char name[32];  /* folder name the client sees, e.g. "TF2" */
    char path[256]; /* real path, e.g. "/mnt/sdcard" */
} ftpd_root;

typedef struct {
    int running;
    int port;
    char ip[64];           /* this device's address ("" = no network) */
    int clients;           /* logged-in sessions */
    /* current / last transfer */
    int xfer_active;       /* 1 = receiving (upload to device), 2 = sending (download) */
    char xfer_name[160];
    long long xfer_done, xfer_total; /* total 0 = unknown (uploads) */
    double speed;          /* bytes/s */
    long long bytes_in, bytes_out;
    int files_in, files_out;
    char log[FTPD_LOG][110]; /* newest first */
    unsigned log_seq;
    char error[110];
} ftpd_state;

/* Starts the server; returns 0 or -1 (error text in the state). */
int ftpd_start(int port, const char *user, const char *pass, const ftpd_root *roots, int nroots);
void ftpd_stop(void);
void ftpd_get(ftpd_state *out);
/* Re-reads the address (Wi-Fi may connect later). */
void ftpd_refresh_ip(void);

#endif
