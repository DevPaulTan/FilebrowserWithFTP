/* Drawing + TrueType text. */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

uint32_t fb[W * H];

void fill(int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        uint32_t *p = fb + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void tint(int x, int y, int w, int h, uint32_t c, int alpha)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    uint32_t cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
    for (int j = 0; j < h; j++) {
        uint32_t *p = fb + (y + j) * W + x;
        for (int i = 0; i < w; i++) {
            uint32_t v = p[i];
            uint32_t r = (((v >> 16) & 255) * (255 - alpha) + cr * alpha) / 255;
            uint32_t g = (((v >> 8) & 255) * (255 - alpha) + cg * alpha) / 255;
            uint32_t b = ((v & 255) * (255 - alpha) + cb * alpha) / 255;
            p[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

void frame(int x, int y, int w, int h, uint32_t c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

/* ---------------------------------------------------------------- fonts */

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    int ok;
} ttf;
static ttf fonts[2]; /* [0] main, [1] fallback for missing characters */

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 64L * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return b;
}

static int load_font(ttf *t, const char *path)
{
    unsigned char *d = load_file(path, NULL);
    if (!d) return 0;
    if (!stbtt_InitFont(&t->info, d, stbtt_GetFontOffsetForIndex(d, 0))) { free(d); return 0; }
    t->data = d;
    t->ok = 1;
    fprintf(stderr, "font: %s\n", path);
    return 1;
}

int font_init(const char *app_dir)
{
    char own[600];
    snprintf(own, sizeof own, "%s/font.ttf", app_dir);
    const char *mains[] = { own, "/mnt/vendor/bin/default.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", NULL };
    for (int i = 0; mains[i] && !fonts[0].ok; i++) load_font(&fonts[0], mains[i]);
    const char *falls[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                            "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", NULL };
    for (int i = 0; falls[i] && !fonts[1].ok; i++)
        if (!fonts[0].ok || strcmp(falls[i], mains[0])) load_font(&fonts[1], falls[i]);
    if (!fonts[0].ok) fprintf(stderr, "font: no TrueType font found, using the built-in one\n");
    return fonts[0].ok;
}

/* ---- glyph cache ---- */
typedef struct {
    uint32_t cp;
    uint8_t size, used;
    short w, h, xoff, yoff, adv;
    unsigned char *bmp;
} glyph;
#define GCACHE 4096
static glyph cache[GCACHE];

static glyph *get_glyph(uint32_t cp, int size)
{
    unsigned hsh = (cp * 2654435761u + (unsigned)size * 97u) & (GCACHE - 1);
    for (int probe = 0; probe < 16; probe++) {
        glyph *g = &cache[(hsh + (unsigned)probe) & (GCACHE - 1)];
        if (g->used && g->cp == cp && g->size == size) return g;
        if (!g->used) {
            ttf *t = &fonts[0];
            int gi = stbtt_FindGlyphIndex(&t->info, (int)cp);
            if (!gi && fonts[1].ok) {
                int g2 = stbtt_FindGlyphIndex(&fonts[1].info, (int)cp);
                if (g2) { t = &fonts[1]; gi = g2; }
            }
            float sc = stbtt_ScaleForPixelHeight(&t->info, (float)size);
            int adv, lsb, x0, y0, x1, y1;
            stbtt_GetGlyphHMetrics(&t->info, gi, &adv, &lsb);
            stbtt_GetGlyphBitmapBox(&t->info, gi, sc, sc, &x0, &y0, &x1, &y1);
            g->w = (short)(x1 - x0);
            g->h = (short)(y1 - y0);
            g->xoff = (short)x0;
            g->yoff = (short)y0;
            g->adv = (short)(adv * sc + 0.5f);
            g->bmp = NULL;
            if (g->w > 0 && g->h > 0) {
                g->bmp = malloc((size_t)g->w * (size_t)g->h);
                if (g->bmp) stbtt_MakeGlyphBitmap(&t->info, g->bmp, g->w, g->h, g->w, sc, sc, gi);
            }
            g->cp = cp;
            g->size = (uint8_t)size;
            g->used = 1;
            return g;
        }
    }
    return NULL; /* cache neighbourhood full: skip the glyph */
}

static int ascent_px(int size)
{
    int a, d, l;
    stbtt_GetFontVMetrics(&fonts[0].info, &a, &d, &l);
    float sc = stbtt_ScaleForPixelHeight(&fonts[0].info, (float)size);
    return (int)(a * sc + 0.5f);
}

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 7) << 18) | ((p[0] & 63u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u);
        p += 3;
    } else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u);
        p += 2;
    } else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80) {
        c = ((c & 31) << 6) | (p[0] & 63u);
        p += 1;
    } else if (c >= 0x80) {
        c = '?'; /* not valid UTF-8 (e.g. a Latin-1 name) */
    }
    *s = (const char *)p;
    return c;
}

/* ---- built-in fallback: 5x7, upper case only ---- */
#include "font5x7.h"

static int text_builtin(int x, int y, int size, uint32_t c, const char *s, int draw)
{
    int sc = size >= 24 ? 3 : size >= 15 ? 2 : 1;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        char ch = cp < 128 ? (char)cp : '?';
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        const uint8_t *g = font_glyph(ch);
        if (draw && g)
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (g[r] & (0x10 >> b)) fill(x + b * sc, y + r * sc + (size - 7 * sc) / 2, sc, sc, c);
        x += 6 * sc;
    }
    return x;
}

int text(int x, int y, int size, uint32_t c, const char *s)
{
    if (!fonts[0].ok) return text_builtin(x, y, size, c, s, 1);
    int base = y + ascent_px(size);
    uint32_t cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        glyph *g = get_glyph(cp, size);
        if (!g) continue;
        if (g->bmp) {
            int gx = x + g->xoff, gy = base + g->yoff;
            for (int j = 0; j < g->h; j++) {
                int py = gy + j;
                if (py < 0 || py >= H) continue;
                for (int i = 0; i < g->w; i++) {
                    int px = gx + i;
                    if (px < 0 || px >= W) continue;
                    unsigned a = g->bmp[j * g->w + i];
                    if (!a) continue;
                    uint32_t *p = &fb[py * W + px], v = *p;
                    uint32_t r = (((v >> 16) & 255) * (255 - a) + cr * a) / 255;
                    uint32_t gg = (((v >> 8) & 255) * (255 - a) + cg * a) / 255;
                    uint32_t b = ((v & 255) * (255 - a) + cb * a) / 255;
                    *p = 0xFF000000u | (r << 16) | (gg << 8) | b;
                }
            }
        }
        x += g->adv;
    }
    return x;
}

int text_w(int size, const char *s)
{
    if (!fonts[0].ok) return text_builtin(0, 0, size, 0, s, 0);
    int w = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        glyph *g = get_glyph(cp, size);
        if (g) w += g->adv;
    }
    return w;
}

void text_fit(char *dst, size_t n, const char *src, int size, int maxw)
{
    snprintf(dst, n, "%s", src);
    if (text_w(size, dst) <= maxw) return;
    /* keep the start and the end (file extension), cut the middle on UTF-8 boundaries */
    size_t len = strlen(src);
    size_t keep_end = 0, keep_start = 0;
    char buf[1024];
    for (;;) {
        /* grow alternately start / end while it fits */
        size_t ns = keep_start, ne = keep_end;
        if (keep_start <= keep_end + 4) {
            ns++;
            while (ns < len && ((unsigned char)src[ns] & 0xC0) == 0x80) ns++;
        } else {
            ne++;
            while (ne < len && ((unsigned char)src[len - ne] & 0xC0) == 0x80) ne++;
        }
        if (ns + ne >= len) break;
        snprintf(buf, sizeof buf, "%.*s...%s", (int)ns, src, src + len - ne);
        if (text_w(size, buf) > maxw) break;
        keep_start = ns;
        keep_end = ne;
    }
    snprintf(dst, n, "%.*s...%s", (int)keep_start, src, src + len - keep_end);
}
