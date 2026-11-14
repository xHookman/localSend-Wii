#ifndef FILEOPS_H
#define FILEOPS_H

#include <gctypes.h>

typedef void (*progress_fn)(s64 done, s64 total);

/* Extension en minuscules avec le point (".gba"), "" si aucune */
void fileop_ext(const char *name, char *out, int max);

/* Cree tous les dossiers du chemin ("usb:/roms/gba"). 0 = ok */
int  fileop_mkdirs(const char *dir);

/* Chemin libre dans dir : "nom.ext", "nom (1).ext"... */
void fileop_unique(const char *dir, const char *name, char *out, int max);

/* Deplace src dans le dossier dir.
 * Meme peripherique : rename (instantane). Sinon : copie, verification
 * CRC32 complete de la copie, puis suppression de l'original.
 * out recoit le chemin final. Retourne 0 ou un message d'erreur (statique). */
const char *fileop_move(const char *src, const char *dir, char *out, int max, progress_fn cb);

/* Copie src -> dst, relit et compare le CRC32. Supprime dst en cas d'echec. */
const char *fileop_copy_verify(const char *src, const char *dst, s64 size, progress_fn cb);

/* Cherche marker dans le fichier, copie la suite jusqu'au caractere stop. */
int  fileop_find_marker(const char *path, const char *marker, char stop, char *out, int max);

int  fileop_is_dir(const char *path);
s64  fileop_free(const char *dev);      /* "sd:/" ou "usb:/" ; -1 si inconnu */

/* Cle USB */
int  usb_mounted(void);
int  usb_remount(void);                 /* 1 = montee */

#endif
