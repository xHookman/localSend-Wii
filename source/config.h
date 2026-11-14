#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_PATH   "sd:/localsend/config.ini"
#define DEFAULT_PORT  53317

typedef struct {
    int  port;            /* port HTTP (TCP) */
    int  auto_move;       /* deplacer sans demander quand une regle existe */
    int  https;           /* annoncer HTTPS (le serveur accepte toujours les deux) */
    char wiixplorer[128]; /* chemin force de WiiXplorer ("" = recherche auto) */
} Config;

extern Config cfg;

void        config_load(void);
int         config_save(void);

/* Destination memorisee/par defaut pour une extension (".gba"), ou NULL */
const char *config_dest(const char *ext);
void        config_set_dest(const char *ext, const char *dir);

/* Type de fichier connu ("ROM Game Boy Advance"), ou NULL si inconnu */
const char *config_kind(const char *ext);

#endif
