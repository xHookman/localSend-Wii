#ifndef UI_H
#define UI_H

#include <gctypes.h>

enum { LOG_DIM, LOG_INFO, LOG_OK, LOG_WARN, LOG_ERR };
enum { UI_BOOT, UI_IDLE, UI_RECV, UI_DONE, UI_FAIL, UI_ACTION, UI_BROWSE, UI_SETTINGS };

/* Panneau "que faire de ce fichier ?" */
typedef struct {
    const char *name, *kind, *dest;   /* dest NULL = a choisir */
    s64 size;
    int index, count;                 /* position dans la file */
    int usb_missing;                  /* destination usb: sans cle */
    const char *const *opts;          /* actions proposees */
    int nopts, sel;
} UiAction;

/* Navigateur de dossiers */
typedef struct {
    const char *file;                 /* fichier a ranger */
    const char *path;                 /* "" = liste des peripheriques */
    const char *const *items;
    int n, sel, top;
} UiBrowser;

/* Reglages */
typedef struct {
    int sel;
    int port, port_changed;
    int auto_move;
    int https;
    int usb;                          /* 1 = cle montee */
    s64 usb_free;
    const char *wiix;                 /* chemin trouve ou NULL */
} UiSettings;

#define BROWSER_ROWS 6

int  ui_init(const char *version);
void ui_boot(const char *msg);                       /* ecran de demarrage */
void ui_fatal(const char *msg);                      /* erreur bloquante */
void ui_set_info(const char *alias, const char *ip, int port);
void ui_set_https(int on);
void ui_set_stats(int files, s64 bytes, s64 sd_free);
void ui_log(int kind, const char *fmt, ...);

void ui_idle(void);
void ui_session(const char *sender, int count, s64 total);
void ui_file_begin(const char *name, int index, int count, s64 size);
void ui_progress(s64 got);                           /* rafraichit au plus ~6 fois/s */
void ui_done(int ok_files, int count, s64 bytes);
void ui_fail(const char *msg);

void ui_show_action(const UiAction *a);
void ui_show_browser(const UiBrowser *b);
void ui_show_settings(const UiSettings *s);
void ui_copy_begin(const char *name, const char *dest, s64 size);  /* puis ui_progress */
int  ui_state(void);

void ui_frame(void);                                 /* a appeler a chaque image */

#endif
