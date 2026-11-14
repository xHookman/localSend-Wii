#ifndef VIDEO_H
#define VIDEO_H

int  video_init(void);     /* initialise la video et le tampon gfx */
void video_present(void);  /* affiche le tampon gfx */
void video_wait(void);     /* attend la synchro verticale */

#endif
