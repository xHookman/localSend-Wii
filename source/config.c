/*
 * config.c - sd:/localsend/config.ini (modifiable depuis un ordinateur)
 *
 *   port = 53317
 *   auto = non
 *   wiixplorer =                  (vide = recherche automatique)
 *   .gba = sd:/vbagx/roms         (une ligne par extension)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "config.h"

Config cfg;

/* Types connus + destination proposee par defaut (emulateurs "GX") */
typedef struct { const char *ext, *kind, *def; } Known;
static const Known KNOWN[] = {
    { ".wad",  "WAD (cha\xC3\xAEne Wii)",        "sd:/wad" },
    { ".nes",  "ROM NES",                        "sd:/fceugx/roms" },
    { ".fds",  "ROM Famicom Disk System",        "sd:/fceugx/roms" },
    { ".sfc",  "ROM Super Nintendo",             "sd:/snes9xgx/roms" },
    { ".smc",  "ROM Super Nintendo",             "sd:/snes9xgx/roms" },
    { ".gb",   "ROM Game Boy",                   "sd:/vbagx/roms" },
    { ".gbc",  "ROM Game Boy Color",             "sd:/vbagx/roms" },
    { ".gba",  "ROM Game Boy Advance",           "sd:/vbagx/roms" },
    { ".md",   "ROM Mega Drive",                 "sd:/genplus/roms" },
    { ".gen",  "ROM Mega Drive",                 "sd:/genplus/roms" },
    { ".smd",  "ROM Mega Drive",                 "sd:/genplus/roms" },
    { ".sms",  "ROM Master System",              "sd:/genplus/roms" },
    { ".gg",   "ROM Game Gear",                  "sd:/genplus/roms" },
    { ".n64",  "ROM Nintendo 64",                NULL },
    { ".z64",  "ROM Nintendo 64",                NULL },
    { ".v64",  "ROM Nintendo 64",                NULL },
    { ".pce",  "ROM PC Engine",                  NULL },
    { ".iso",  "Image disque (ISO)",             NULL },
    { ".wbfs", "Jeu Wii (WBFS)",                 NULL },
    { ".zip",  "Archive ZIP",                    NULL },
    { ".7z",   "Archive 7z",                     NULL },
    { ".dol",  "Programme Wii (DOL)",            NULL },
    { NULL, NULL, NULL }
};

#define MAX_RULES 48
static struct { char ext[12]; char dir[128]; } rules[MAX_RULES];
static int nrules = 0;

static const Known *known(const char *ext) {
    for (const Known *k = KNOWN; k->ext; k++)
        if (strcasecmp(k->ext, ext) == 0) return k;
    return NULL;
}

const char *config_kind(const char *ext) {
    const Known *k = known(ext);
    if (k) return k->kind;
    for (int i = 0; i < nrules; i++)
        if (strcasecmp(rules[i].ext, ext) == 0) return "Fichier";
    return NULL;
}

const char *config_dest(const char *ext) {
    for (int i = 0; i < nrules; i++)
        if (strcasecmp(rules[i].ext, ext) == 0) return rules[i].dir[0] ? rules[i].dir : NULL;
    const Known *k = known(ext);
    return k ? k->def : NULL;
}

void config_set_dest(const char *ext, const char *dir) {
    int i;
    for (i = 0; i < nrules; i++)
        if (strcasecmp(rules[i].ext, ext) == 0) break;
    if (i == nrules) {
        if (nrules >= MAX_RULES) return;
        nrules++;
    }
    snprintf(rules[i].ext, sizeof(rules[i].ext), "%s", ext);
    for (char *p = rules[i].ext; *p; p++) *p = (char)tolower((unsigned char)*p);
    snprintf(rules[i].dir, sizeof(rules[i].dir), "%s", dir);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

void config_load(void) {
    cfg.port = DEFAULT_PORT;
    cfg.auto_move = 0;
    cfg.https = 1;
    cfg.wiixplorer[0] = 0;
    nrules = 0;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        char *c = strchr(v, ';');            /* commentaire en fin de ligne */
        if (c) { *c = 0; v = trim(v); }

        if (strcasecmp(k, "port") == 0) {
            int p = atoi(v);
            if (p >= 1024 && p <= 65535) cfg.port = p;
        } else if (strcasecmp(k, "auto") == 0) {
            cfg.auto_move = (strcasecmp(v, "oui") == 0 || strcasecmp(v, "yes") == 0 || strcmp(v, "1") == 0);
        } else if (strcasecmp(k, "https") == 0) {
            cfg.https = !(strcasecmp(v, "non") == 0 || strcasecmp(v, "no") == 0 || strcmp(v, "0") == 0);
        } else if (strcasecmp(k, "wiixplorer") == 0) {
            snprintf(cfg.wiixplorer, sizeof(cfg.wiixplorer), "%s", v);
        } else if (k[0] == '.' && strlen(k) < 12) {
            size_t n = strlen(v);
            while (n > 0 && v[n - 1] == '/') v[--n] = 0;   /* pas de / final */
            config_set_dest(k, v);
        }
    }
    fclose(f);
}

int config_save(void) {
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", CONFIG_PATH);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
        "# LocalSend pour Wii - configuration (modifiable depuis un ordinateur)\n"
        "# port : port HTTP de la Wii (defaut 53317). La decouverte reste sur 53317.\n"
        "port = %d\n"
        "# auto : oui = ranger les fichiers sans demander quand une regle existe\n"
        "auto = %s\n"
        "# https : oui = chiffrement actif (reglage par defaut de LocalSend)\n"
        "https = %s\n"
        "# wiixplorer : chemin du boot.dol (vide = recherche automatique)\n"
        "wiixplorer = %s\n"
        "\n# Destinations par extension (sd:/... ou usb:/...)\n",
        cfg.port, cfg.auto_move ? "oui" : "non", cfg.https ? "oui" : "non", cfg.wiixplorer);
    for (int i = 0; i < nrules; i++)
        fprintf(f, "%s = %s\n", rules[i].ext, rules[i].dir);
    int err = ferror(f);
    if (fclose(f) != 0 || err) { remove(tmp); return -1; }
    remove(CONFIG_PATH);
    return rename(tmp, CONFIG_PATH);
}
