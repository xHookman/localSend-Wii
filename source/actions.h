#ifndef ACTIONS_H
#define ACTIONS_H

#include <gctypes.h>

enum { REQ_NONE = 0, REQ_LAUNCH = 1 };

void actions_init(const char *self_path);   /* argv[0] */

/* Un fichier vient d'etre recu et verifie (chemin sur la SD) */
void actions_received(const char *path, const char *name, s64 size);

/* Fin de session : rangement automatique si active. */
void actions_session_end(void);

/* A appeler a chaque image. busy = une session de transfert est en cours.
 * Retourne REQ_LAUNCH si l'utilisateur veut ouvrir WiiXplorer
 * (chemin dans actions_launch_path()). */
int  actions_update(u32 down, u32 held_ms, int busy);
const char *actions_launch_path(void);
const char *actions_launch_label(void);

/* Fourni par main.c : relance le serveur HTTP sur un autre port. 0 = ok */
int  app_set_port(int port);
void app_https_changed(void);

#endif
