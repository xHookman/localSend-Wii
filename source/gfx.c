/*
 * gfx.c - rendu logiciel anticrenele dans un tampon RGB
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"

u32 *gfx_buf = NULL;
int  gfx_w = 0, gfx_h = 0;

int gfx_init(int w, int h) {
    gfx_w = w;
    gfx_h = h;
    gfx_buf = (u32 *)malloc((size_t)w * h * 4);
    if (!gfx_buf) return -1;
    memset(gfx_buf, 0, (size_t)w * h * 4);
    return 0;
}

u32 gfx_mix(u32 d, u32 s, int a) {
    if (a <= 0) return d;
    if (a >= 255) return s;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
    dr += ((sr - dr) * a) / 255;
    dg += ((sg - dg) * a) / 255;
    db += ((sb - db) * a) / 255;
    return ((u32)dr << 16) | ((u32)dg << 8) | (u32)db;
}

static inline void plot(int x, int y, u32 c, int a) {
    if ((unsigned)x >= (unsigned)gfx_w || (unsigned)y >= (unsigned)gfx_h) return;
    u32 *p = &gfx_buf[y * gfx_w + x];
    *p = gfx_mix(*p, c, a);
}

void gfx_vgrad(u32 top, u32 bottom) {
    for (int y = 0; y < gfx_h; y++) {
        u32 c = gfx_mix(top, bottom, y * 255 / (gfx_h > 1 ? gfx_h - 1 : 1));
        u32 *p = &gfx_buf[y * gfx_w];
        for (int x = 0; x < gfx_w; x++) p[x] = c;
    }
}

void gfx_rect(int x, int y, int w, int h, u32 c, int a) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > gfx_w ? gfx_w : x + w, y1 = y + h > gfx_h ? gfx_h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        u32 *p = &gfx_buf[yy * gfx_w];
        if (a >= 255) for (int xx = x0; xx < x1; xx++) p[xx] = c;
        else          for (int xx = x0; xx < x1; xx++) p[xx] = gfx_mix(p[xx], c, a);
    }
}

static inline float clampf(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

void gfx_rrect(int x, int y, int w, int h, int r, u32 c, int a) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { gfx_rect(x, y, w, h, c, a); return; }
    /* bandes centrales pleines */
    gfx_rect(x, y + r, w, h - 2 * r, c, a);
    gfx_rect(x + r, y, w - 2 * r, r, c, a);
    gfx_rect(x + r, y + h - r, w - 2 * r, r, c, a);
    /* quatre coins anticrenelees */
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            float dx = r - (i + 0.5f), dy = r - (j + 0.5f);
            float cov = clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov <= 0) continue;
            int aa = (int)(cov * a);
            plot(x + i,         y + j,         c, aa);
            plot(x + w - 1 - i, y + j,         c, aa);
            plot(x + i,         y + h - 1 - j, c, aa);
            plot(x + w - 1 - i, y + h - 1 - j, c, aa);
        }
    }
}

void gfx_rrect_border(int x, int y, int w, int h, int r, u32 fill, u32 border) {
    gfx_rrect(x, y, w, h, r, border, 255);
    gfx_rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, fill, 255);
}

void gfx_circle(float cx, float cy, float r, u32 c, int a) {
    int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1);
    int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float cov = clampf(r - sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov > 0) plot(x, y, c, (int)(cov * a));
        }
}

void gfx_ring(float cx, float cy, float r, float thick, u32 c, int a) {
    float ro = r + thick / 2;
    int x0 = (int)floorf(cx - ro - 1), x1 = (int)ceilf(cx + ro + 1);
    int y0 = (int)floorf(cy - ro - 1), y1 = (int)ceilf(cy + ro + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - r);
            float cov = clampf(thick / 2 - d + 0.5f);
            if (cov > 0) plot(x, y, c, (int)(cov * a));
        }
}

void gfx_capsule(float x0, float y0, float x1, float y1, float rad, u32 c, int a) {
    int bx0 = (int)floorf((x0 < x1 ? x0 : x1) - rad - 1);
    int bx1 = (int)ceilf((x0 > x1 ? x0 : x1) + rad + 1);
    int by0 = (int)floorf((y0 < y1 ? y0 : y1) - rad - 1);
    int by1 = (int)ceilf((y0 > y1 ? y0 : y1) + rad + 1);
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            t = clampf(t);
            float dx = px - t * vx, dy = py - t * vy;
            float cov = clampf(rad - sqrtf(dx * dx + dy * dy) + 0.5f);
            if (cov > 0) plot(x, y, c, (int)(cov * a));
        }
}

/* ---------------------------------------------------------------- texte */

static int next_cp(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    int c = s[0];
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *ps += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) { *ps += 3; return '?'; }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) { *ps += 4; return '?'; }
    *ps += 1;
    return '?';
}

static const Glyph *glyph(const Font *f, int cp) {
    if (cp < f->first || cp > f->last) cp = '?';
    if (cp < f->first || cp > f->last) return NULL;
    const Glyph *g = &f->g[cp - f->first];
    if (g->adv == 0 && cp != '?') {
        if ('?' < f->first || '?' > f->last) return NULL;
        g = &f->g['?' - f->first];
    }
    return g;
}

static void draw_glyph(const Font *f, const Glyph *g, int x, int y, u32 c) {
    const u8 *b = f->bits + g->off;
    int rowb = (g->w + 1) / 2;
    for (int j = 0; j < g->h; j++) {
        for (int i = 0; i < g->w; i++) {
            u8 v = b[j * rowb + i / 2];
            int a = (i & 1) ? (v & 15) : (v >> 4);
            if (a) plot(x + g->xo + i, y + g->yo + j, c, a * 17);
        }
    }
}

int gfx_text(const Font *f, int x, int y, const char *s, u32 c) {
    int pen = x * 8;
    int base = y + f->ascent;
    while (*s) {
        const Glyph *g = glyph(f, next_cp(&s));
        if (!g) continue;
        if (g->w) draw_glyph(f, g, (pen + 4) / 8, base, c);
        pen += g->adv;
    }
    return (pen + 4) / 8;
}

int gfx_text_w(const Font *f, const char *s) {
    int pen = 0;
    while (*s) {
        const Glyph *g = glyph(f, next_cp(&s));
        if (g) pen += g->adv;
    }
    return (pen + 4) / 8;
}

void gfx_text_fit(const Font *f, int x, int y, int maxw, const char *s, u32 c, int align) {
    char tmp[256];
    int w = gfx_text_w(f, s);
    const char *out = s;
    if (w > maxw) {
        int ell = gfx_text_w(f, "...");
        int pen = 0, n = 0;
        const char *p = s;
        while (*p) {
            const char *q = p;
            const Glyph *g = glyph(f, next_cp(&q));
            int adv = g ? g->adv : 0;
            if ((pen + adv + 4) / 8 + ell > maxw) break;
            pen += adv;
            if ((q - s) >= (int)sizeof(tmp) - 4) break;
            p = q;
        }
        n = (int)(p - s);
        memcpy(tmp, s, n);
        strcpy(tmp + n, "...");
        out = tmp;
        w = gfx_text_w(f, out);
    }
    if (align == 1) x += (maxw - w) / 2;
    else if (align == 2) x += maxw - w;
    gfx_text(f, x, y, out, c);
}
