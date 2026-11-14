/*
 * gfx : petit moteur de rendu logiciel (tampon RGB 32 bits en RAM)
 * Aucune dependance : le tampon est converti vers le framebuffer Wii (video.c).
 */
#ifndef GFX_H
#define GFX_H

#include <gctypes.h>

typedef struct {
    u8  w, h;        /* taille du bitmap */
    s8  xo, yo;      /* decalage par rapport a la ligne de base */
    u16 adv;         /* avance horizontale en 1/8 de pixel */
    u32 off;         /* position dans le tableau de bits (alpha 4 bits) */
} Glyph;

typedef struct {
    u8 height, ascent;
    u8 first, last;
    const Glyph *g;
    const u8 *bits;
} Font;

extern u32 *gfx_buf;          /* pixels 0x00RRGGBB */
extern int  gfx_w, gfx_h;

int  gfx_init(int w, int h);
void gfx_vgrad(u32 top, u32 bottom);
void gfx_rect(int x, int y, int w, int h, u32 c, int a);
void gfx_rrect(int x, int y, int w, int h, int r, u32 c, int a);
void gfx_rrect_border(int x, int y, int w, int h, int r, u32 fill, u32 border);
void gfx_circle(float cx, float cy, float r, u32 c, int a);
void gfx_ring(float cx, float cy, float r, float thick, u32 c, int a);
void gfx_capsule(float x0, float y0, float x1, float y1, float rad, u32 c, int a);

/* Texte UTF-8 (Latin-1 affiche, autres caracteres -> '?'). y = haut de la ligne. */
int  gfx_text(const Font *f, int x, int y, const char *s, u32 c);
int  gfx_text_w(const Font *f, const char *s);
/* Texte tronque avec "..." s'il depasse maxw. align : 0 gauche, 1 centre, 2 droite */
void gfx_text_fit(const Font *f, int x, int y, int maxw, const char *s, u32 c, int align);

u32  gfx_mix(u32 a, u32 b, int t);   /* t 0..255 */

#endif
