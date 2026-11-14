/*
 * ui.c - interface graphique de LocalSend pour Wii
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ogc/lwp_watchdog.h>
#include "gfx.h"
#include "video.h"
#include "ui.h"
#include "font_data.h"

/* ------------------------------------------------------------ palette */
#define C_BG_TOP   0x0E1726
#define C_BG_BOT   0x090E18
#define C_CARD     0x142033
#define C_BORDER   0x22324B
#define C_TRACK    0x0C1421
#define C_TEXT     0xE9EEF6
#define C_DIM      0x8D9DB6
#define C_FAINT    0x56667F
#define C_ACCENT   0x2FD3B5
#define C_ACCENT2  0x1E9BE0
#define C_OK       0x3DDC84
#define C_WARN     0xF5B942
#define C_ERR      0xF2555A
#define C_INFO     0x5AB8F0
#define C_WHITE    0xFFFFFF

#define MARGIN     32
#define LOG_N      5

static struct {
    char version[16];
    char alias[32], ip[20];
    int  port;
    int  state;
    char boot_msg[96], fail_msg[128];
    char sender[64], fname[128];
    int  index, count;
    s64  size, got;
    s64  sess_total, sess_done;    /* octets des fichiers termines */
    u64  t_file, t_state;
    int  done_ok, done_count;
    s64  done_bytes;
    int  files;
    s64  bytes, sd_free;
    char log_txt[LOG_N][100];
    int  log_kind[LOG_N];
    int  dirty;
    u64  last_present;
    unsigned frame;
    int  https;
    int  copy;                     /* RECV utilise pour une copie */
    char copy_dest[128];
    UiAction act;
    UiBrowser br;
    UiSettings st;
} U;

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

/* ----------------------------------------------------------- formatage */

static void fmt_size(s64 b, char *out, int max) {
    if (b < 0) b = 0;
    if (b < 1024)                    snprintf(out, max, "%d o", (int)b);
    else if (b < 1024 * 1024)        snprintf(out, max, "%.1f Ko", b / 1024.0);
    else if (b < 1024LL * 1024 * 1024) snprintf(out, max, "%.1f Mo", b / 1048576.0);
    else                             snprintf(out, max, "%.2f Go", b / 1073741824.0);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

static u32 kind_color(int k) {
    switch (k) {
        case LOG_OK:   return C_OK;
        case LOG_WARN: return C_WARN;
        case LOG_ERR:  return C_ERR;
        case LOG_INFO: return C_INFO;
        default:       return C_FAINT;
    }
}

/* ----------------------------------------------------------- dessins */

static void icon_arrow(float cx, float cy, float s, u32 c) {
    /* fleche vers le bas + socle (icone "reception") */
    gfx_capsule(cx, cy - 9 * s, cx, cy + 4 * s, 2.0f * s, c, 255);
    gfx_capsule(cx - 6.5f * s, cy - 2.5f * s, cx, cy + 4 * s, 2.0f * s, c, 255);
    gfx_capsule(cx + 6.5f * s, cy - 2.5f * s, cx, cy + 4 * s, 2.0f * s, c, 255);
    gfx_capsule(cx - 9 * s, cy + 10 * s, cx + 9 * s, cy + 10 * s, 1.8f * s, c, 255);
}

static void icon_check(float cx, float cy, u32 c) {
    gfx_capsule(cx - 10, cy + 1, cx - 3, cy + 8, 2.6f, c, 255);
    gfx_capsule(cx - 3, cy + 8, cx + 11, cy - 7, 2.6f, c, 255);
}

static void icon_cross(float cx, float cy, u32 c) {
    gfx_capsule(cx - 8, cy - 8, cx + 8, cy + 8, 2.6f, c, 255);
    gfx_capsule(cx + 8, cy - 8, cx - 8, cy + 8, 2.6f, c, 255);
}

static void draw_header(int oy) {
    int x = MARGIN, y = oy + 24;
    gfx_rrect(x, y, 44, 44, 12, C_ACCENT, 255);
    icon_arrow(x + 22, y + 20, 1.0f, C_WHITE);

    gfx_text(&font_lg, x + 58, y - 2, "LocalSend", C_TEXT);
    char sub[48];
    snprintf(sub, sizeof(sub), "pour Wii  \xC2\xB7  v%s", U.version);
    gfx_text(&font_sm, x + 60, y + 28, sub, C_DIM);

    /* pastille adresse */
    char addr[96];
    int online = strcmp(U.ip, "0.0.0.0") != 0;
    if (online) snprintf(addr, sizeof(addr), "%s  \xC2\xB7  %s:%d%s", U.alias, U.ip, U.port,
                         U.https ? "  \xC2\xB7  HTTPS" : "");
    else        snprintf(addr, sizeof(addr), "%s  \xC2\xB7  hors ligne", U.alias);
    int tw = gfx_text_w(&font_sm, addr);
    int pw = tw + 36, ph = 30;
    int px = gfx_w - MARGIN - pw, py = y + 7;
    gfx_rrect_border(px, py, pw, ph, 15, C_CARD, C_BORDER);
    gfx_circle(px + 16, py + ph / 2.0f, 4, online ? C_OK : C_WARN, 255);
    gfx_text(&font_sm, px + 26, py + 6, addr, C_TEXT);
}

static void draw_radar(float cx, float cy, u64 t) {
    for (int k = 0; k < 3; k++) {
        float ph = fmodf(t / 2400.0f + k / 3.0f, 1.0f);
        float r = 30 + ph * 44;
        gfx_ring(cx, cy, r, 2.0f, C_ACCENT, (int)((1 - ph) * 150));
    }
    gfx_circle(cx, cy, 29, C_ACCENT, 255);
    icon_arrow(cx, cy - 1, 1.15f, C_WHITE);
}

static void draw_bar(int x, int y, int w, int h, s64 got, s64 size, u32 c) {
    gfx_rrect_border(x, y, w, h, h / 2, C_TRACK, C_BORDER);
    if (size <= 0) return;
    if (got > size) got = size;
    int fw = (int)((double)(w - 4) * got / size);
    if (fw > 0) {
        if (fw < h - 4) fw = h - 4;
        gfx_rrect(x + 2, y + 2, fw, h - 4, (h - 4) / 2, c, 255);
        gfx_rect(x + 2 + (h - 4) / 2, y + 3, fw - (h - 4), (h - 4) / 2 - 1, C_WHITE, 40);
    }
}

static void draw_card(int oy, u64 t) {
    int cx = MARGIN, cy = oy + 86, cw = gfx_w - 2 * MARGIN, ch = 168;
    gfx_rrect_border(cx, cy, cw, ch, 14, C_CARD, C_BORDER);
    int tx = cx + 176;
    int tw = cw - 176 - 22;
    float rx = cx + 88, ry = cy + ch / 2.0f;
    char a[48], b[48], line[160];

    switch (U.state) {
    case UI_BOOT:
        draw_radar(rx, ry, t);
        gfx_text(&font_lg, tx, cy + 44, "D\xC3\xA9marrage...", C_TEXT);
        gfx_text_fit(&font_md, tx, cy + 88, tw, U.boot_msg, C_DIM, 0);
        break;

    case UI_IDLE:
        draw_radar(rx, ry, t);
        gfx_text(&font_lg, tx, cy + 28, "Pr\xC3\xAAt \xC3\xA0 recevoir", C_TEXT);
        gfx_text(&font_md, tx, cy + 70, "Ouvre LocalSend sur ton t\xC3\xA9l\xC3\xA9phone", C_DIM);
        gfx_text(&font_md, tx, cy + 92, "et choisis \xC2\xAB Wii \xC2\xBB comme destinataire.", C_DIM);
        if (U.https) {
            gfx_circle(tx + 4, cy + 133, 3.5f, C_OK, 255);
            gfx_text_fit(&font_sm, tx + 14, cy + 125, tw - 14,
                         "Chiffrement HTTPS actif (r\xC3\xA9glage par d\xC3\xA9" "faut de LocalSend)", C_OK, 0);
        } else {
            gfx_circle(tx + 4, cy + 133, 3.5f, C_WARN, 255);
            gfx_text_fit(&font_sm, tx + 14, cy + 125, tw - 14,
                         "Chiffrement (HTTPS) \xC3\xA0 d\xC3\xA9sactiver dans LocalSend", C_WARN, 0);
        }
        break;

    case UI_RECV: {
        int ix = cx + 24, iw = cw - 48;
        if (U.copy)
            snprintf(line, sizeof(line), "Copie vers %s", U.copy_dest);
        else
            snprintf(line, sizeof(line), "R\xC3\xA9" "ception depuis %s  \xC2\xB7  fichier %d / %d",
                     U.sender, U.index, U.count);
        gfx_text_fit(&font_sm, ix, cy + 18, iw - 150, line, C_DIM, 0);
        gfx_text_fit(&font_bd, ix, cy + 42, iw - 150, U.fname, C_TEXT, 0);

        int pct = U.size > 0 ? (int)(U.got * 100 / U.size) : 0;
        if (pct > 100) pct = 100;
        snprintf(a, sizeof(a), "%d%%", pct);
        gfx_text_fit(&font_xl, ix + iw - 150, cy + 14, 150, a, C_ACCENT, 2);

        draw_bar(ix, cy + 88, iw, 18, U.got, U.size, C_ACCENT);
        if (U.count > 1 && !U.copy)
            draw_bar(ix, cy + 114, iw, 8, U.sess_done + U.got, U.sess_total, C_ACCENT2);

        fmt_size(U.got, a, sizeof(a));
        fmt_size(U.size, b, sizeof(b));
        snprintf(line, sizeof(line), "%s / %s", a, b);
        gfx_text(&font_sm, ix, cy + 134, line, C_DIM);

        u64 el = t - U.t_file;
        if (el > 400 && U.got > 0) {
            double bps = U.got * 1000.0 / el;
            snprintf(a, sizeof(a), "%.1f Mo/s", bps / 1048576.0);
            for (char *p = a; *p; p++) if (*p == '.') *p = ',';
            gfx_text_fit(&font_sm, ix, cy + 134, iw, a, C_TEXT, 1);
            int rest = bps > 0 ? (int)ceil((U.size - U.got) / bps) : 0;
            if (rest < 0) rest = 0;
            snprintf(b, sizeof(b), "reste %d:%02d", rest / 60, rest % 60);
            gfx_text_fit(&font_sm, ix, cy + 134, iw, b, C_DIM, 2);
        }
        break;
    }

    case UI_DONE:
        gfx_circle(rx, ry, 29, U.done_ok == U.done_count ? C_OK : C_WARN, 255);
        icon_check(rx, ry, C_WHITE);
        gfx_text(&font_lg, tx, cy + 34, "Transfert termin\xC3\xA9", C_TEXT);
        fmt_size(U.done_bytes, a, sizeof(a));
        if (U.done_ok == U.done_count)
            snprintf(line, sizeof(line), "%d fichier%s re\xC3\xA7u%s  \xC2\xB7  %s",
                     U.done_ok, U.done_ok > 1 ? "s" : "", U.done_ok > 1 ? "s" : "", a);
        else
            snprintf(line, sizeof(line), "%d / %d fichiers re\xC3\xA7us  \xC2\xB7  %s",
                     U.done_ok, U.done_count, a);
        gfx_text_fit(&font_md, tx, cy + 78, tw, line, C_DIM, 0);
        gfx_text_fit(&font_sm, tx, cy + 106, tw, "D\xC3\xA9tails dans le journal ci-dessous", C_FAINT, 0);
        break;

    case UI_FAIL:
        gfx_circle(rx, ry, 29, C_ERR, 255);
        icon_cross(rx, ry, C_WHITE);
        gfx_text(&font_lg, tx, cy + 34, "\xC3\x89" "chec du transfert", C_TEXT);
        gfx_text_fit(&font_md, tx, cy + 78, tw, U.fail_msg, C_DIM, 0);
        break;
    }
}

/* ------------------------------------------------------ panneaux interactifs */

/* touche dessinee + legende ; retourne la position x suivante */
static int draw_key(int x, int y, const char *key, const char *label) {
    int kw = gfx_text_w(&font_sm, key);
    int bw = kw + 14 < 24 ? 24 : kw + 14;
    gfx_rrect(x, y, bw, 22, 6, C_TEXT, 255);
    gfx_text(&font_sm, x + (bw - kw) / 2, y + 3, key, C_CARD);
    return gfx_text(&font_sm, x + bw + 7, y + 3, label, C_DIM) + 18;
}

static void icon_folder(int x, int y, u32 c) {
    gfx_rrect(x, y, 8, 4, 1, c, 255);
    gfx_rrect(x, y + 2, 16, 11, 2, c, 255);
}

#define PANEL_Y(oy) ((oy) + 86)
#define PANEL_H     236

static void draw_action(int oy) {
    int x = MARGIN, y = PANEL_Y(oy), w = gfx_w - 2 * MARGIN;
    char a[48], line[200];
    gfx_rrect_border(x, y, w, PANEL_H, 14, C_CARD, C_BORDER);
    int ix = x + 24, iw = w - 48;

    snprintf(line, sizeof(line), "Fichier re\xC3\xA7u  \xC2\xB7  %d / %d", U.act.index, U.act.count);
    gfx_text(&font_sm, ix, y + 14, line, C_DIM);
    /* rappel des touches en haut a droite */
    int kx = x + w - 24;
    const char *keys[3][2] = { { "1", "R\xC3\xA9glages" }, { "B", "Laisser" }, { "A", "Valider" } };
    for (int k = 0; k < 3; k++) {
        int lw = gfx_text_w(&font_sm, keys[k][1]);
        int kw = gfx_text_w(&font_sm, keys[k][0]) + 14;
        if (kw < 24) kw = 24;
        kx -= lw + kw + 7;
        draw_key(kx, y + 11, keys[k][0], keys[k][1]);
        kx -= 14;
    }

    gfx_text_fit(&font_bd, ix, y + 36, iw, U.act.name ? U.act.name : "", C_TEXT, 0);
    fmt_size(U.act.size, a, sizeof(a));
    snprintf(line, sizeof(line), "%s  \xC2\xB7  %s%s", U.act.kind ? U.act.kind : "Fichier", a,
             U.act.usb_missing ? "  \xC2\xB7  cl\xC3\xA9 USB non d\xC3\xA9tect\xC3\xA9" "e" : "");
    gfx_text_fit(&font_sm, ix, y + 60, iw, line, U.act.usb_missing ? C_WARN : C_DIM, 0);

    int ly = y + 86, rh = 24;
    for (int i = 0; i < U.act.nopts && i < 6; i++) {
        int ry = ly + i * rh;
        int sel = (i == U.act.sel);
        if (sel) gfx_rrect(ix - 6, ry, iw + 12, rh - 2, 7, C_ACCENT, 55);
        gfx_circle(ix + 6, ry + 11, sel ? 4.0f : 2.5f, sel ? C_ACCENT : C_FAINT, 255);
        gfx_text_fit(&font_md, ix + 20, ry + 1, iw - 24, U.act.opts[i], sel ? C_TEXT : C_DIM, 0);
    }
}

static void draw_browser(int oy) {
    int x = MARGIN, y = PANEL_Y(oy), w = gfx_w - 2 * MARGIN;
    char line[200];
    gfx_rrect_border(x, y, w, PANEL_H, 14, C_CARD, C_BORDER);
    int ix = x + 24, iw = w - 48;

    snprintf(line, sizeof(line), "Ranger : %s", U.br.file ? U.br.file : "");
    gfx_text_fit(&font_sm, ix, y + 14, iw, line, C_DIM, 0);
    gfx_text_fit(&font_bd, ix, y + 34, iw,
                 U.br.path && U.br.path[0] ? U.br.path : "P\xC3\xA9riph\xC3\xA9riques", C_TEXT, 0);

    int ly = y + 62, rh = 22;
    if (U.br.n == 0)
        gfx_text(&font_sm, ix + 6, ly + 4, "(aucun sous-dossier)", C_FAINT);
    for (int r = 0; r < BROWSER_ROWS; r++) {
        int i = U.br.top + r;
        if (i >= U.br.n) break;
        int ry = ly + r * rh;
        int sel = (i == U.br.sel);
        if (sel) gfx_rrect(ix, ry, iw, rh, 6, C_ACCENT, 60);
        icon_folder(ix + 8, ry + 5, sel ? C_ACCENT : C_FAINT);
        gfx_text_fit(&font_sm, ix + 32, ry + 3, iw - 40, U.br.items[i], sel ? C_TEXT : C_DIM, 0);
    }
    if (U.br.n > BROWSER_ROWS) {           /* barre de defilement */
        int th = BROWSER_ROWS * rh;
        int bh = th * BROWSER_ROWS / U.br.n;
        int bt = th * U.br.top / U.br.n;
        gfx_rrect(ix + iw - 4, ly + bt, 4, bh, 2, C_FAINT, 255);
    }

    int kx = ix, ky = y + 202;
    kx = draw_key(kx, ky, "A", "Ouvrir");
    kx = draw_key(kx, ky, "B", "Remonter");
    if (U.br.path && U.br.path[0]) kx = draw_key(kx, ky, "1", "Choisir ce dossier");
    draw_key(kx, ky, "-", "Annuler");
}

static void draw_settings(int oy) {
    int x = MARGIN, y = PANEL_Y(oy), w = gfx_w - 2 * MARGIN;
    char val[96], fr[32];
    gfx_rrect_border(x, y, w, PANEL_H, 14, C_CARD, C_BORDER);
    int ix = x + 24, iw = w - 48;
    gfx_text(&font_bd, ix, y + 12, "R\xC3\xA9glages", C_TEXT);

    const char *labels[5] = { "Port HTTP", "Rangement automatique", "Chiffrement (HTTPS)",
                              "Cl\xC3\xA9 USB", "Ouvrir WiiXplorer" };
    for (int i = 0; i < 5; i++) {
        int ry = y + 40 + i * 29;
        int sel = (i == U.st.sel);
        if (sel) gfx_rrect(ix - 8, ry, iw + 16, 27, 8, C_ACCENT, 50);
        gfx_text(&font_md, ix, ry + 4, labels[i], sel ? C_TEXT : C_DIM);
        u32 vc = C_TEXT;
        switch (i) {
        case 0:
            snprintf(val, sizeof(val), sel ? "<  %d  >" : "%d", U.st.port);
            if (U.st.port_changed) vc = C_WARN;
            break;
        case 1:
            snprintf(val, sizeof(val), "%s", U.st.auto_move ? "Oui" : "Non");
            vc = U.st.auto_move ? C_OK : C_DIM;
            break;
        case 2:
            snprintf(val, sizeof(val), "%s", U.st.https ? "Activ\xC3\xA9" : "D\xC3\xA9sactiv\xC3\xA9");
            vc = U.st.https ? C_OK : C_WARN;
            break;
        case 3:
            if (U.st.usb) {
                if (U.st.usb_free >= 0) { fmt_size(U.st.usb_free, fr, sizeof(fr));
                    snprintf(val, sizeof(val), "D\xC3\xA9tect\xC3\xA9" "e (%s libres)", fr); }
                else snprintf(val, sizeof(val), "D\xC3\xA9tect\xC3\xA9" "e");
                vc = C_OK;
            } else { snprintf(val, sizeof(val), "Absente  (A : r\xC3\xA9" "essayer)"); vc = C_WARN; }
            break;
        case 4:
            snprintf(val, sizeof(val), "%s", U.st.wiix ? U.st.wiix : "introuvable");
            vc = U.st.wiix ? C_DIM : C_WARN;
            break;
        }
        gfx_text_fit(&font_sm, ix + 210, ry + 6, iw - 210, val, vc, 2);
    }
    const char *note = U.st.port_changed
        ? "Le nouveau port sera appliqu\xC3\xA9 en quittant les r\xC3\xA9glages."
        : "La d\xC3\xA9" "couverte LocalSend reste sur le port 53317.";
    gfx_text_fit(&font_sm, ix, y + 188, iw, note, U.st.port_changed ? C_WARN : C_FAINT, 0);

    int kx = ix, ky = y + 208;
    kx = draw_key(kx, ky, "A", "Valider");
    kx = draw_key(kx, ky, "B", "Retour");
    draw_key(kx, ky, "2", "Port par d\xC3\xA9" "faut");
}

static void draw_stats(int oy) {
    int y = oy + 266, h = 56, gap = 12;
    int w = (gfx_w - 2 * MARGIN - 2 * gap) / 3;
    const char *label[3] = { "Fichiers re\xC3\xA7us", "Total re\xC3\xA7u", "Espace libre SD" };
    char val[3][32];
    snprintf(val[0], sizeof(val[0]), "%d", U.files);
    fmt_size(U.bytes, val[1], sizeof(val[1]));
    if (U.sd_free >= 0) fmt_size(U.sd_free, val[2], sizeof(val[2]));
    else strcpy(val[2], "?");
    for (int i = 0; i < 3; i++) {
        int x = MARGIN + i * (w + gap);
        gfx_rrect_border(x, y, w, h, 12, C_CARD, C_BORDER);
        gfx_text(&font_sm, x + 14, y + 8, label[i], C_DIM);
        gfx_text_fit(&font_bd, x + 14, y + 28, w - 28, val[i], C_TEXT, 0);
    }
}

static void draw_log(int oy) {
    int y = oy + 334;
    gfx_text(&font_sm, MARGIN + 2, y, "JOURNAL", C_FAINT);
    gfx_text_fit(&font_sm, MARGIN, y, gfx_w - 2 * MARGIN, "1 : r\xC3\xA9glages   \xC2\xB7   HOME : quitter", C_FAINT, 2);
    gfx_rect(MARGIN, y + 20, gfx_w - 2 * MARGIN, 1, C_BORDER, 255);
    for (int i = 0; i < LOG_N; i++) {
        int idx = LOG_N - 1 - i;               /* le plus recent en haut */
        if (!U.log_txt[idx][0]) continue;
        int ly = y + 26 + i * 19;
        u32 col = kind_color(U.log_kind[idx]);
        u32 txt = gfx_mix(C_TEXT, C_FAINT, i * 50);
        gfx_circle(MARGIN + 8, ly + 9, 3.5f, col, 255 - i * 30);
        gfx_text_fit(&font_sm, MARGIN + 20, ly, gfx_w - 2 * MARGIN - 20, U.log_txt[idx], txt, 0);
    }
}

static void render(void) {
    u64 t = now_ms();
    int oy = (gfx_h - 480) / 2;
    if (oy < 0) oy = 0;
    gfx_vgrad(C_BG_TOP, C_BG_BOT);
    draw_header(oy);
    if (U.state == UI_ACTION)        draw_action(oy);
    else if (U.state == UI_BROWSE)   draw_browser(oy);
    else if (U.state == UI_SETTINGS) draw_settings(oy);
    else { draw_card(oy, t); draw_stats(oy); }
    draw_log(oy);
    video_present();
    U.last_present = t;
    U.dirty = 0;
}

/* ------------------------------------------------------------- API */

int ui_init(const char *version) {
    memset(&U, 0, sizeof(U));
    snprintf(U.version, sizeof(U.version), "%s", version);
    strcpy(U.alias, "Wii");
    strcpy(U.ip, "0.0.0.0");
    U.sd_free = -1;
    U.state = UI_BOOT;
    return video_init();
}

void ui_boot(const char *msg) {
    U.state = UI_BOOT;
    snprintf(U.boot_msg, sizeof(U.boot_msg), "%s", msg);
    render();
}

void ui_fatal(const char *msg) {
    U.state = UI_FAIL;
    snprintf(U.fail_msg, sizeof(U.fail_msg), "%s", msg);
    ui_log(LOG_ERR, "%s", msg);
    render();
}

void ui_set_info(const char *alias, const char *ip, int port) {
    snprintf(U.alias, sizeof(U.alias), "%s", alias);
    snprintf(U.ip, sizeof(U.ip), "%s", ip);
    U.port = port;
    U.dirty = 1;
}

void ui_set_https(int on) { U.https = on; U.dirty = 1; }

void ui_set_stats(int files, s64 bytes, s64 sd_free) {
    U.files = files;
    U.bytes = bytes;
    U.sd_free = sd_free;
    U.dirty = 1;
}

void ui_log(int kind, const char *fmt, ...) {
    for (int i = 0; i < LOG_N - 1; i++) {
        strcpy(U.log_txt[i], U.log_txt[i + 1]);
        U.log_kind[i] = U.log_kind[i + 1];
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.log_txt[LOG_N - 1], sizeof(U.log_txt[0]), fmt, ap);
    va_end(ap);
    U.log_kind[LOG_N - 1] = kind;
    U.dirty = 1;
}

void ui_idle(void) {
    U.state = UI_IDLE;
    U.dirty = 1;
}

void ui_session(const char *sender, int count, s64 total) {
    snprintf(U.sender, sizeof(U.sender), "%s", sender);
    U.count = count;
    U.sess_total = total;
    U.sess_done = 0;
    U.index = 0;
    U.done_ok = 0;
    U.done_bytes = 0;
    U.dirty = 1;
}

void ui_file_begin(const char *name, int index, int count, s64 size) {
    if (U.state == UI_RECV && U.index > 0) U.sess_done += U.size;
    U.state = UI_RECV;
    U.copy = 0;
    snprintf(U.fname, sizeof(U.fname), "%s", name);
    U.index = index;
    U.count = count;
    U.size = size;
    U.got = 0;
    U.t_file = now_ms();
    render();
}

void ui_progress(s64 got) {
    U.got = got;
    if (now_ms() - U.last_present >= 160) render();
}

void ui_done(int ok_files, int count, s64 bytes) {
    U.state = UI_DONE;
    U.done_ok = ok_files;
    U.done_count = count;
    U.done_bytes = bytes;
    U.t_state = now_ms();
    render();
}

void ui_fail(const char *msg) {
    U.state = UI_FAIL;
    snprintf(U.fail_msg, sizeof(U.fail_msg), "%s", msg);
    U.t_state = now_ms();
    render();
}

void ui_show_action(const UiAction *a)    { U.act = *a; U.state = UI_ACTION; U.dirty = 1; }
void ui_show_browser(const UiBrowser *b)  { U.br = *b;  U.state = UI_BROWSE; U.dirty = 1; }
void ui_show_settings(const UiSettings *s){ U.st = *s;  U.state = UI_SETTINGS; U.dirty = 1; }
int  ui_state(void) { return U.state; }

void ui_copy_begin(const char *name, const char *dest, s64 size) {
    U.state = UI_RECV;
    U.copy = 1;
    snprintf(U.copy_dest, sizeof(U.copy_dest), "%s", dest);
    snprintf(U.fname, sizeof(U.fname), "%s", name);
    U.index = U.count = 1;
    U.size = size;
    U.got = 0;
    U.t_file = now_ms();
    render();
}

void ui_frame(void) {
    U.frame++;
    u64 t = now_ms();
    if ((U.state == UI_DONE || U.state == UI_FAIL) && t - U.t_state > 15000) {
        U.state = UI_IDLE;
        U.dirty = 1;
    }
    int animated = (U.state == UI_IDLE || U.state == UI_BOOT);
    if (U.dirty || (animated && (U.frame & 1) == 0)) render();
}
