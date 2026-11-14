#ifndef LAUNCHER_H
#define LAUNCHER_H

/* Cherche WiiXplorer (chemin force sinon emplacements habituels). 1 = trouve */
int launcher_find(const char *forced, char *out, int max);

/* Cherche YAWM ModMii Edition (sd:/apps/yawmME/boot.dol...). 1 = trouve */
int launcher_find_yawmme(char *out, int max);

/* Lance le .dol/.elf. Ne revient qu'en cas d'erreur (message). */
const char *launcher_run(const char *path);

#endif
