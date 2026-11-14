/*
 * input.c - Wiimote + manette GameCube, avec repetition des fleches
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>
#include "input.h"

static u32 rep_btn = 0;
static u64 rep_start = 0, rep_next = 0;

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

void input_init(void) {
    WPAD_Init();
    PAD_Init();
}

static u32 map_wpad(u32 w) {
    u32 b = 0;
    if (w & WPAD_BUTTON_A)     b |= BTN_A;
    if (w & WPAD_BUTTON_B)     b |= BTN_B;
    if (w & WPAD_BUTTON_1)     b |= BTN_1;
    if (w & WPAD_BUTTON_2)     b |= BTN_2;
    if (w & WPAD_BUTTON_PLUS)  b |= BTN_PLUS;
    if (w & WPAD_BUTTON_MINUS) b |= BTN_MINUS;
    if (w & WPAD_BUTTON_HOME)  b |= BTN_HOME;
    if (w & WPAD_BUTTON_UP)    b |= BTN_UP;
    if (w & WPAD_BUTTON_DOWN)  b |= BTN_DOWN;
    if (w & WPAD_BUTTON_LEFT)  b |= BTN_LEFT;
    if (w & WPAD_BUTTON_RIGHT) b |= BTN_RIGHT;
    return b;
}

static u32 map_pad(u32 p) {
    u32 b = 0;
    if (p & PAD_BUTTON_A)     b |= BTN_A;
    if (p & PAD_BUTTON_B)     b |= BTN_B;
    if (p & PAD_BUTTON_X)     b |= BTN_1;
    if (p & PAD_BUTTON_Y)     b |= BTN_2;
    if (p & PAD_TRIGGER_R)    b |= BTN_PLUS;
    if (p & PAD_TRIGGER_Z)    b |= BTN_MINUS;
    if (p & PAD_BUTTON_START) b |= BTN_HOME;
    if (p & PAD_BUTTON_UP)    b |= BTN_UP;
    if (p & PAD_BUTTON_DOWN)  b |= BTN_DOWN;
    if (p & PAD_BUTTON_LEFT)  b |= BTN_LEFT;
    if (p & PAD_BUTTON_RIGHT) b |= BTN_RIGHT;
    return b;
}

void input_scan(u32 *down, u32 *held, u32 *held_ms) {
    WPAD_ScanPads();
    PAD_ScanPads();
    u32 d = 0, h = 0;
    for (int i = 0; i < 4; i++) {
        d |= map_wpad(WPAD_ButtonsDown(i)) | map_pad(PAD_ButtonsDown(i));
        h |= map_wpad(WPAD_ButtonsHeld(i)) | map_pad(PAD_ButtonsHeld(i));
    }

    /* repetition automatique des fleches : 400 ms puis toutes les 70 ms */
    u64 t = now_ms();
    u32 dir_down = d & BTN_DIRS;
    if (dir_down) {
        rep_btn = dir_down;
        rep_start = t;
        rep_next = t + 400;
    } else if (rep_btn && (h & rep_btn) == rep_btn) {
        if (t >= rep_next) {
            d |= rep_btn;
            rep_next = t + 70;
        }
    } else {
        rep_btn = 0;
    }
    *down = d;
    *held = h;
    *held_ms = rep_btn ? (u32)(t - rep_start) : 0;
}
