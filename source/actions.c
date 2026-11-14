/*
 * actions.c - que faire des fichiers recus : file d'attente, panneau,
 * navigateur de dossiers, reglages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include "actions.h"
#include "config.h"
#include "fileops.h"
#include "launcher.h"
#include "input.h"
#include "ui.h"

#define QMAX       32
#define MAX_DIRS   200

typedef struct {
    char path[256];
    char name[128];
    char ext[12];
    s64  size;
    int  is_update;          /* .dol de LocalSend pour Wii */
    char upd_ver[16];
} Item;

/* Actions proposees dans le panneau */
enum { ACT_INSTALL_YAWM, ACT_MOVE, ACT_BROWSE, ACT_LEAVE, ACT_ALL, ACT_WIIX, ACT_UPDATE };

enum { M_NONE, M_PANEL, M_BROWSE, M_SETTINGS };

static Item q[QMAX];
static int  qn = 0;
static int  batch_total = 0, batch_pos = 0;
static int  mode = M_NONE, back_mode = M_NONE;

/* navigateur */
static char cur[256];
static char names[MAX_DIRS][128];
static const char *items[MAX_DIRS];
static int  n_items = 0, sel = 0, top = 0;

/* reglages */
#define ST_ROWS 5   /* port, auto, https, usb, wiixplorer */
static int  st_sel = 0, st_port = 0, st_usb = 0;
static s64  st_usb_free = -1;
static char wiix_path[160];
static int  wiix_found = 0;

/* panneau */
static int  acts[8], nacts = 0, act_sel = 0;
static char act_txt[8][96];
static const char *act_ptr[8];

/* lancement demande a main.c */
static char launch_path[160], launch_label[48];

/* notre propre boot.dol (pour la mise a jour) */
static char self_path[160] = "sd:/apps/localsend/boot.dol";

/* ------------------------------------------------------------ utilitaires */

static int dev_is_usb(const char *p) { return strncasecmp(p, "usb:", 4) == 0; }

static int same_dev(const char *a, const char *b) {
    const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
    return ca && cb && ca - a == cb - b && strncasecmp(a, b, ca - a) == 0;
}

static void dequeue(void) {
    if (qn <= 0) return;
    memmove(&q[0], &q[1], sizeof(Item) * (qn - 1));
    qn--;
    batch_pos++;
    act_sel = 0;
    if (qn == 0) batch_total = batch_pos = 0;
}

static void copy_progress(s64 done, s64 total) { (void)total; ui_progress(done); }

/* Range l'element 0 dans dir. remember : memoriser pour l'extension. */
static int move_first(const char *dir, int remember) {
    Item *it = &q[0];
    if (dev_is_usb(dir) && !usb_mounted() && !usb_remount()) {
        ui_log(LOG_ERR, "Cl\xC3\xA9 USB absente : %s non rang\xC3\xA9", it->name);
        return -1;
    }
    char out[300];
    if (!same_dev(it->path, dir)) ui_copy_begin(it->name, dir, it->size);
    const char *err = fileop_move(it->path, dir, out, sizeof(out), copy_progress);
    if (err) {
        ui_log(LOG_ERR, "%s : %s", it->name, err);
        return -1;
    }
    const char *fname = strrchr(out, '/');
    ui_log(LOG_OK, "Rang\xC3\xA9 : %s dans %s", fname ? fname + 1 : out, dir);
    if (remember && it->ext[0]) {
        config_set_dest(it->ext, dir);
        config_save();
    }
    dequeue();
    return 0;
}

/* Range tous les elements qui ont une destination ; les autres restent. */
static void move_all(int skip_missing_usb) {
    int total = qn;
    for (int k = 0; k < total && qn > 0; k++) {
        const char *d = q[0].is_update ? NULL : config_dest(q[0].ext);
        int ok = d && !(skip_missing_usb && dev_is_usb(d) && !usb_mounted())
                 && move_first(d, 0) == 0;
        if (!ok && qn > 1) {                 /* on le met en fin de file */
            Item t = q[0];
            memmove(&q[0], &q[1], sizeof(Item) * (qn - 1));
            q[qn - 1] = t;
        }
    }
}

/* ------------------------------------------------------------- affichage */

static void add_act(int a, const char *fmt, const char *arg) {
    acts[nacts] = a;
    snprintf(act_txt[nacts], sizeof(act_txt[0]), fmt, arg ? arg : "");
    act_ptr[nacts] = act_txt[nacts];
    nacts++;
}

static void show_panel(void) {
    if (qn == 0) { mode = M_NONE; ui_idle(); return; }
    Item *it = &q[0];
    const char *dest = config_dest(it->ext);
    int same = (mode == M_PANEL);
    nacts = 0;
    if (it->is_update) {
        add_act(ACT_UPDATE, "Installer la v%s et red\xC3\xA9marrer", it->upd_ver);
        add_act(ACT_LEAVE, "Laisser dans sd:/localsend", NULL);
    } else {
        if (strcmp(it->ext, ".wad") == 0)
            add_act(ACT_INSTALL_YAWM, "Installer avec YAWM ModMii Edition", NULL);
        if (dest) add_act(ACT_MOVE, "Ranger dans %s", dest);
        add_act(ACT_BROWSE, "Choisir un autre dossier...", NULL);
        add_act(ACT_LEAVE, "Laisser dans sd:/localsend", NULL);
        if (qn > 1) {
            char n[16];
            snprintf(n, sizeof(n), "%d", qn);
            add_act(ACT_ALL, "Tout ranger (%s fichiers)", n);
        }
        add_act(ACT_WIIX, "Ouvrir WiiXplorer", NULL);
    }
    if (!same || act_sel >= nacts) act_sel = 0;

    UiAction a;
    memset(&a, 0, sizeof(a));
    a.name = it->name;
    a.kind = it->is_update ? "Mise \xC3\xA0 jour de LocalSend pour Wii" : config_kind(it->ext);
    a.dest = dest;
    a.size = it->size;
    a.index = batch_pos + 1;
    a.count = batch_total;
    a.usb_missing = dest && dev_is_usb(dest) && !usb_mounted();
    a.opts = act_ptr;
    a.nopts = nacts;
    a.sel = act_sel;
    mode = M_PANEL;
    ui_show_action(&a);
}

static void show_browser(void) {
    UiBrowser b;
    b.file = qn ? q[0].name : "";
    b.path = cur;
    b.items = items;
    b.n = n_items;
    b.sel = sel;
    b.top = top;
    mode = M_BROWSE;
    ui_show_browser(&b);
}

static void show_settings(void) {
    UiSettings s;
    s.sel = st_sel;
    s.port = st_port;
    s.port_changed = (st_port != cfg.port);
    s.auto_move = cfg.auto_move;
    s.https = cfg.https;
    s.usb = st_usb;
    s.usb_free = st_usb_free;
    s.wiix = wiix_found ? wiix_path : NULL;
    mode = M_SETTINGS;
    ui_show_settings(&s);
}

static void leave_to_base(void) {
    if (qn > 0) show_panel();
    else { mode = M_NONE; ui_idle(); }
}

/* ------------------------------------------------------------ navigateur */

static int cmp_names(const void *a, const void *b) {
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void load_dir(void) {
    n_items = 0;
    sel = top = 0;
    if (!cur[0]) {
        strcpy(names[n_items++], "sd:/");
        strcpy(names[n_items++], "usb:/");
    } else {
        DIR *d = opendir(cur);
        struct dirent *e;
        while (d && (e = readdir(d)) && n_items < MAX_DIRS) {
            if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(names[0])) continue;
            char full[600];
            size_t L = strlen(cur);
            snprintf(full, sizeof(full), "%s%s%.300s", cur, (L && cur[L - 1] == '/') ? "" : "/", e->d_name);
            if (!fileop_is_dir(full)) continue;
            snprintf(names[n_items], sizeof(names[0]), "%.127s", e->d_name);
            n_items++;
        }
        if (d) closedir(d);
    }
    for (int i = 0; i < n_items; i++) items[i] = names[i];
    if (cur[0]) qsort(items, n_items, sizeof(items[0]), cmp_names);
}

/* Ouvre le navigateur au plus pres de start */
static void open_browser(const char *start) {
    if (start && start[0]) {
        snprintf(cur, sizeof(cur), "%s", start);
        while (cur[0] && !fileop_is_dir(cur)) {
            char *sl = strrchr(cur, '/');
            if (!sl) { cur[0] = 0; break; }
            if (sl > cur && sl[-1] == ':') { sl[1] = 0; if (!fileop_is_dir(cur)) cur[0] = 0; break; }
            *sl = 0;
        }
        size_t L = strlen(cur);           /* "sd:" -> "sd:/" */
        if (L && cur[L - 1] == ':') strcat(cur, "/");
    } else {
        strcpy(cur, "sd:/");
    }
    load_dir();
    show_browser();
}

static void browser_up(void) {
    size_t L = strlen(cur);
    if (L && cur[L - 1] == '/') { cur[0] = 0; load_dir(); return; }   /* racine -> peripheriques */
    char *sl = strrchr(cur, '/');
    if (!sl) { cur[0] = 0; }
    else if (sl > cur && sl[-1] == ':') sl[1] = 0;
    else *sl = 0;
    load_dir();
}

static void browser_enter(void) {
    if (n_items == 0) return;
    const char *it = items[sel];
    if (!cur[0]) {
        if (strncasecmp(it, "usb:", 4) == 0 && !usb_mounted() && !usb_remount()) {
            ui_log(LOG_WARN, "Cl\xC3\xA9 USB non d\xC3\xA9tect\xC3\xA9" "e");
            return;
        }
        snprintf(cur, sizeof(cur), "%s", it);
    } else {
        char tmp[400];
        size_t L = strlen(cur);
        snprintf(tmp, sizeof(tmp), "%s%s%s", cur, cur[L - 1] == '/' ? "" : "/", it);
        if (strlen(tmp) >= sizeof(cur)) { ui_log(LOG_WARN, "Chemin trop long"); return; }
        strcpy(cur, tmp);
    }
    load_dir();
}

static void browser_input(u32 down) {
    if (down & BTN_UP)   { if (sel > 0) sel--; else sel = n_items ? n_items - 1 : 0; }
    if (down & BTN_DOWN) { if (sel < n_items - 1) sel++; else sel = 0; }
    if (down & BTN_LEFT)  { sel -= BROWSER_ROWS; if (sel < 0) sel = 0; }
    if (down & BTN_RIGHT) { sel += BROWSER_ROWS; if (sel > n_items - 1) sel = n_items > 0 ? n_items - 1 : 0; }
    if (sel < top) top = sel;
    if (sel >= top + BROWSER_ROWS) top = sel - BROWSER_ROWS + 1;

    if (down & BTN_A) browser_enter();
    else if (down & BTN_B) browser_up();
    else if ((down & BTN_1) && cur[0]) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", cur);
        size_t L = strlen(dir);
        if (L && dir[L - 1] == '/') dir[L - 1] = 0;       /* "sd:/" -> "sd:" */
        move_first(dir, 1);
        leave_to_base();
        return;
    } else if (down & BTN_MINUS) {
        show_panel();
        return;
    }
    if (down) show_browser();
}

/* ------------------------------------------------------------- reglages */

static void open_settings(void) {
    back_mode = mode;
    st_sel = 0;
    st_port = cfg.port;
    st_usb = usb_mounted();
    st_usb_free = st_usb ? fileop_free("usb:/") : -1;
    wiix_found = launcher_find(cfg.wiixplorer, wiix_path, sizeof(wiix_path));
    show_settings();
}

static int request_launch(const char *path, const char *label) {
    snprintf(launch_path, sizeof(launch_path), "%s", path);
    snprintf(launch_label, sizeof(launch_label), "%s", label);
    return REQ_LAUNCH;
}

static int settings_input(u32 down, u32 held_ms) {
    if (down & BTN_UP)   st_sel = (st_sel + ST_ROWS - 1) % ST_ROWS;
    if (down & BTN_DOWN) st_sel = (st_sel + 1) % ST_ROWS;
    if (st_sel == 0) {
        int step = held_ms > 1500 ? 10 : 1;
        if (down & BTN_LEFT)  st_port -= step;
        if (down & BTN_RIGHT) st_port += step;
        if (down & BTN_2)     st_port = DEFAULT_PORT;
        if (st_port < 1024)  st_port = 65535;
        if (st_port > 65535) st_port = 1024;
    }
    if (down & BTN_A) {
        if (st_sel == 1) {
            cfg.auto_move = !cfg.auto_move;
            config_save();
        } else if (st_sel == 2) {
            cfg.https = !cfg.https;
            config_save();
            app_https_changed();
            ui_log(LOG_INFO, cfg.https ? "HTTPS activ\xC3\xA9 (r\xC3\xA9glage par d\xC3\xA9" "faut de LocalSend)"
                                       : "HTTPS d\xC3\xA9sactiv\xC3\xA9 : d\xC3\xA9sactive aussi le chiffrement sur le t\xC3\xA9l\xC3\xA9phone");
        } else if (st_sel == 3) {
            st_usb = usb_remount();
            st_usb_free = st_usb ? fileop_free("usb:/") : -1;
            ui_log(st_usb ? LOG_OK : LOG_WARN, st_usb ? "Cl\xC3\xA9 USB mont\xC3\xA9" "e (usb:/)"
                                                      : "Aucune cl\xC3\xA9 USB d\xC3\xA9tect\xC3\xA9" "e");
        } else if (st_sel == 4) {
            if (wiix_found) return request_launch(wiix_path, "WiiXplorer");
            ui_log(LOG_WARN, "WiiXplorer introuvable (sd:/apps/WiiXplorer/boot.dol)");
        }
    }
    if (down & BTN_B) {
        if (st_port != cfg.port) {
            int old = cfg.port;
            if (app_set_port(st_port) == 0) {
                cfg.port = st_port;
                config_save();
                ui_log(LOG_OK, "Serveur relanc\xC3\xA9 sur le port %d", cfg.port);
            } else {
                ui_log(LOG_ERR, "Port %d indisponible, retour au port %d", st_port, old);
            }
        }
        if (back_mode == M_PANEL && qn > 0) show_panel();
        else leave_to_base();
        return REQ_NONE;
    }
    if (down) show_settings();
    return REQ_NONE;
}

/* --------------------------------------------------------- mise a jour */

static int install_update(void) {
    Item *it = &q[0];
    char dir[160], newp[180], bak[180];
    snprintf(dir, sizeof(dir), "%s", self_path);
    char *sl = strrchr(dir, '/');
    if (!sl) { ui_log(LOG_ERR, "Chemin de l'application inconnu"); return -1; }
    *sl = 0;
    if (!fileop_is_dir(dir)) { ui_log(LOG_ERR, "Dossier %s introuvable", dir); return -1; }
    snprintf(newp, sizeof(newp), "%s/boot.dol.new", dir);
    snprintf(bak, sizeof(bak), "%s/boot.dol.bak", dir);

    remove(newp);
    const char *err = NULL;
    if (!same_dev(it->path, newp) || rename(it->path, newp) != 0) {
        ui_copy_begin(it->name, dir, it->size);
        err = fileop_copy_verify(it->path, newp, it->size, copy_progress);
        if (!err) remove(it->path);
    }
    if (err) { ui_log(LOG_ERR, "Mise \xC3\xA0 jour : %s", err); return -1; }

    remove(bak);
    int had_old = rename(self_path, bak) == 0;
    if (rename(newp, self_path) != 0) {
        if (had_old) rename(bak, self_path);         /* on remet l'ancienne version */
        ui_log(LOG_ERR, "Mise \xC3\xA0 jour : remplacement impossible, ancienne version conserv\xC3\xA9" "e");
        return -1;
    }
    ui_log(LOG_OK, "Mise \xC3\xA0 jour v%s install\xC3\xA9" "e (sauvegarde : boot.dol.bak)", it->upd_ver);
    dequeue();
    return 0;
}

/* ---------------------------------------------------------------- panneau */

static int panel_input(u32 down) {
    if (qn == 0) { leave_to_base(); return REQ_NONE; }
    if (down & BTN_UP)   { act_sel = (act_sel + nacts - 1) % nacts; show_panel(); return REQ_NONE; }
    if (down & BTN_DOWN) { act_sel = (act_sel + 1) % nacts; show_panel(); return REQ_NONE; }
    if (down & BTN_1)    { open_settings(); return REQ_NONE; }

    int act;
    if (down & BTN_A) act = acts[act_sel];
    else if (down & BTN_B) act = ACT_LEAVE;
    else return REQ_NONE;

    const char *dest = config_dest(q[0].ext);
    switch (act) {
    case ACT_UPDATE: {
        char me[160];
        snprintf(me, sizeof(me), "%s", self_path);
        if (install_update() == 0) return request_launch(me, "LocalSend pour Wii");
        leave_to_base();
        break;
    }
    case ACT_INSTALL_YAWM: {
        char yp[160];
        if (!launcher_find_yawmme(yp, sizeof(yp))) {
            ui_log(LOG_WARN, "YAWM ModMii Edition introuvable (sd:/apps/yawmME/boot.dol)");
            break;
        }
        if (move_first(dest ? dest : "sd:/wad", 0) == 0) return request_launch(yp, "YAWM ModMii Edition");
        leave_to_base();
        break;
    }
    case ACT_MOVE:
        if (dest) move_first(dest, 0);
        leave_to_base();
        break;
    case ACT_BROWSE:
        open_browser(dest ? dest : "sd:/");
        break;
    case ACT_LEAVE:
        ui_log(LOG_DIM, "Laiss\xC3\xA9 dans sd:/localsend : %s", q[0].name);
        dequeue();
        leave_to_base();
        break;
    case ACT_ALL:
        move_all(0);
        leave_to_base();
        break;
    case ACT_WIIX:
        wiix_found = launcher_find(cfg.wiixplorer, wiix_path, sizeof(wiix_path));
        if (wiix_found) return request_launch(wiix_path, "WiiXplorer");
        ui_log(LOG_WARN, "WiiXplorer introuvable (sd:/apps/WiiXplorer/boot.dol)");
        break;
    }
    return REQ_NONE;
}

/* -------------------------------------------------------------------- API */

void actions_init(const char *self) {
    qn = 0;
    mode = M_NONE;
    /* chemin transmis par le Homebrew Channel, ex. "sd:/apps/localsend/boot.dol" */
    if (self && (strncasecmp(self, "sd:/", 4) == 0 || strncasecmp(self, "usb:/", 5) == 0))
        snprintf(self_path, sizeof(self_path), "%s", self);
}

void actions_received(const char *path, const char *name, s64 size) {
    char ext[12];
    fileop_ext(name, ext, sizeof(ext));
    if (!ext[0] || !config_kind(ext)) return;          /* type inconnu : reste la */
    if (qn >= QMAX) return;
    Item *it = &q[qn++];
    it->is_update = 0;
    if (strcmp(ext, ".dol") == 0 &&
        fileop_find_marker(path, "LSWII-DOL:", ':', it->upd_ver, sizeof(it->upd_ver)))
        it->is_update = 1;
    snprintf(it->path, sizeof(it->path), "%s", path);
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->ext, sizeof(it->ext), "%s", ext);
    it->size = size;
    batch_total++;
}

void actions_session_end(void) {
    if (!cfg.auto_move || qn == 0) return;
    move_all(1);
}

int actions_update(u32 down, u32 held_ms, int busy) {
    int st = ui_state();
    int ours = (st == UI_ACTION || st == UI_BROWSE || st == UI_SETTINGS);

    if (busy) return REQ_NONE;

    /* un transfert a interrompu l'ecran en cours : on le reaffiche */
    if (mode != M_NONE && !ours && st != UI_RECV) {
        if (mode == M_BROWSE) show_browser();
        else if (mode == M_SETTINGS) show_settings();
        else show_panel();
        return REQ_NONE;
    }
    if (mode == M_NONE) {
        if (qn > 0 && st != UI_RECV) { show_panel(); return REQ_NONE; }
        if (down & BTN_1) open_settings();
        return REQ_NONE;
    }
    if (!down) return REQ_NONE;
    switch (mode) {
        case M_PANEL:    return panel_input(down);
        case M_BROWSE:   browser_input(down); return REQ_NONE;
        case M_SETTINGS: return settings_input(down, held_ms);
    }
    return REQ_NONE;
}

const char *actions_launch_path(void)  { return launch_path; }
const char *actions_launch_label(void) { return launch_label; }
