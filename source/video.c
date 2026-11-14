/*
 * video.c - initialisation video Wii et envoi du tampon gfx vers le XFB
 * (double framebuffer, conversion RGB -> YUY2)
 */
#include <gccore.h>
#include "gfx.h"
#include "video.h"

static GXRModeObj *rmode = NULL;
static void *xfb[2] = { NULL, NULL };
static int cur = 0;

int video_init(void) {
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);

    /* Ecran 16:9 : on elargit l'image pour garder des proportions correctes */
    if (CONF_GetAspectRatio() == CONF_ASPECT_16_9) {
        rmode->viWidth = 678;
        rmode->viXOrigin = (VI_MAX_WIDTH_NTSC - 678) / 2;
    }

    VIDEO_Configure(rmode);
    xfb[0] = SYS_AllocateFramebuffer(rmode);   /* adresses en cache (K0) */
    xfb[1] = SYS_AllocateFramebuffer(rmode);
    VIDEO_ClearFrameBuffer(rmode, MEM_K0_TO_K1(xfb[0]), COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, MEM_K0_TO_K1(xfb[1]), COLOR_BLACK);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

    return gfx_init(rmode->fbWidth, rmode->xfbHeight);
}

static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

void video_present(void) {
    cur ^= 1;
    u32 *dst = (u32 *)xfb[cur];
    const u32 *src = gfx_buf;
    int pairs = (gfx_w * gfx_h) / 2;

    for (int i = 0; i < pairs; i++) {
        u32 a = src[0], b = src[1];
        src += 2;
        int r1 = (a >> 16) & 255, g1 = (a >> 8) & 255, b1 = a & 255;
        int r2 = (b >> 16) & 255, g2 = (b >> 8) & 255, b2 = b & 255;
        int y1 = (66 * r1 + 129 * g1 + 25 * b1 + 128) / 256 + 16;
        int y2 = (66 * r2 + 129 * g2 + 25 * b2 + 128) / 256 + 16;
        int r = (r1 + r2) / 2, g = (g1 + g2) / 2, bb = (b1 + b2) / 2;
        int cb = (-38 * r - 74 * g + 112 * bb + 128) / 256 + 128;
        int cr = (112 * r - 94 * g - 18 * bb + 128) / 256 + 128;
        *dst++ = ((u32)clamp255(y1) << 24) | ((u32)clamp255(cb) << 16) |
                 ((u32)clamp255(y2) << 8)  |  (u32)clamp255(cr);
    }
    DCFlushRange(xfb[cur], gfx_w * gfx_h * 2);
    VIDEO_SetNextFramebuffer(xfb[cur]);
    VIDEO_Flush();
}

void video_wait(void) { VIDEO_WaitVSync(); }
