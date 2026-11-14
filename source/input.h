#ifndef INPUT_H
#define INPUT_H

#include <gctypes.h>

/* Boutons logiques (Wiimote ou manette GameCube) */
#define BTN_A      0x0001
#define BTN_B      0x0002
#define BTN_1      0x0004   /* GC : X */
#define BTN_2      0x0008   /* GC : Y */
#define BTN_PLUS   0x0010   /* GC : R */
#define BTN_MINUS  0x0020   /* GC : Z */
#define BTN_HOME   0x0040   /* GC : START */
#define BTN_UP     0x0100
#define BTN_DOWN   0x0200
#define BTN_LEFT   0x0400
#define BTN_RIGHT  0x0800
#define BTN_DIRS   0x0F00

void input_init(void);
/* down : appuis (+ repetition auto des fleches), held : maintenus,
 * held_ms : duree d'appui de la fleche en cours */
void input_scan(u32 *down, u32 *held, u32 *held_ms);

#endif
