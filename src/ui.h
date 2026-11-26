/* Drawing into a 640x480 ARGB framebuffer + TrueType text (stb_truetype). */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include <stddef.h>

#define W 640
#define H 480

extern uint32_t fb[W * H];

/* colours */
#define C_BG      0xFF0B0F14u
#define C_PANEL   0xFF131B24u
#define C_PANEL2  0xFF1A2430u
#define C_LINE    0xFF233140u
#define C_TEXT    0xFFE6EDF3u
#define C_DIM     0xFF7D8B99u
#define C_ACCENT  0xFF35C2FFu
#define C_SEL     0xFF1F4460u
#define C_SELDIM  0xFF1A2C3Au
#define C_GOOD    0xFF4ADE80u
#define C_WARN    0xFFFFB020u
#define C_BAD     0xFFFF5A5Au
#define C_FOLDER  0xFFFFD23Fu
#define C_MARK    0xFFFF7AD9u

void fill(int x, int y, int w, int h, uint32_t c);
void tint(int x, int y, int w, int h, uint32_t c, int alpha);
void frame(int x, int y, int w, int h, uint32_t c); /* 1 px outline */

/* Loads a TrueType font: the app folder's font.ttf, else the firmware's, else a
 * system font. Returns 0 if none (then a small built-in font is used). */
int font_init(const char *app_dir);

/* Text at (x, y) = top of the line, pixel height size. Returns the x after it. */
int text(int x, int y, int size, uint32_t c, const char *utf8);
int text_w(int size, const char *utf8);
/* Copies src into dst shortened with "..." in the middle so it fits maxw px. */
void text_fit(char *dst, size_t n, const char *src, int size, int maxw);

#endif
