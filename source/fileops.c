/*
 * fileops.c - deplacement / copie verifiee de fichiers, cle USB
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fat.h>
#include "fileops.h"

#if defined(__has_include)
#if __has_include(<sys/statvfs.h>)
#include <sys/statvfs.h>
#define HAVE_STATVFS 1
#endif
#endif

#ifndef HOST_TEST
#include <ogc/usbstorage.h>
#endif

#define COPY_BUF (256 * 1024)

void fileop_ext(const char *name, char *out, int max) {
    const char *dot = strrchr(name, '.');
    out[0] = 0;
    if (!dot || dot == name || (int)strlen(dot) >= max) return;
    int i = 0;
    for (; dot[i]; i++) out[i] = (char)tolower((unsigned char)dot[i]);
    out[i] = 0;
}

int fileop_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int fileop_mkdirs(const char *dir) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    char *p = strchr(tmp, ':');
    p = p ? p + 1 : tmp;
    while (*p == '/') p++;
    for (;;) {
        char *s = strchr(p, '/');
        if (s) *s = 0;
        if (*p && !fileop_is_dir(tmp) && mkdir(tmp, 0777) != 0 && !fileop_is_dir(tmp))
            return -1;
        if (!s) break;
        *s = '/';
        p = s + 1;
    }
    return 0;
}

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

void fileop_unique(const char *dir, const char *name, char *out, int max) {
    snprintf(out, max, "%s/%s", dir, name);
    if (!exists(out)) return;
    char base[128], ext[32] = "";
    snprintf(base, sizeof(base), "%s", name);
    char *dot = strrchr(base, '.');
    if (dot && dot != base && strlen(dot) < sizeof(ext)) { strcpy(ext, dot); *dot = 0; }
    for (int i = 1; i < 1000; i++) {
        snprintf(out, max, "%s/%s (%d)%s", dir, base, i, ext);
        if (!exists(out)) return;
    }
}

static int same_device(const char *a, const char *b) {
    const char *ca = strchr(a, ':'), *cb = strchr(b, ':');
    if (!ca || !cb || ca - a != cb - b) return 0;
    return strncasecmp(a, b, ca - a) == 0;
}

/* ---------------------------------------------------------------- CRC32 */
static u32 crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static u32 crc_update(u32 crc, const u8 *p, size_t n) {
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

s64 fileop_free(const char *dev) {
#ifdef HAVE_STATVFS
    struct statvfs sv;
    if (statvfs(dev, &sv) == 0) return (s64)sv.f_bavail * (s64)sv.f_frsize;
#endif
    return -1;
}

const char *fileop_move(const char *src, const char *dir, char *out, int max, progress_fn cb) {
    const char *name = strrchr(src, '/');
    name = name ? name + 1 : src;

    if (fileop_mkdirs(dir) != 0) return "impossible de cr\xC3\xA9" "er le dossier";
    fileop_unique(dir, name, out, max);

    if (same_device(src, dir)) {
        if (rename(src, out) == 0) return NULL;
        /* sinon on tente une copie classique */
    }

    struct stat st;
    if (stat(src, &st) != 0) return "fichier source introuvable";
    s64 size = st.st_size;

    char dev[16];
    const char *colon = strchr(dir, ':');
    if (colon && colon - dir < 10) {
        memcpy(dev, dir, colon - dir + 1);
        strcpy(dev + (colon - dir + 1), "/");
        s64 fr = fileop_free(dev);
        if (fr >= 0 && fr < size + 64 * 1024) return "espace insuffisant sur la destination";
    }

    const char *err = fileop_copy_verify(src, out, size, cb);
    if (err) return err;
    if (remove(src) != 0) return "copi\xC3\xA9 mais original non supprim\xC3\xA9";
    return NULL;
}

const char *fileop_copy_verify(const char *src, const char *dst, s64 size, progress_fn cb) {
    crc_init();
    u8 *buf = (u8 *)memalign(32, COPY_BUF);
    if (!buf) return "m\xC3\xA9moire insuffisante";

    const char *err = NULL;
    FILE *in = fopen(src, "rb");
    FILE *o = in ? fopen(dst, "wb") : NULL;
    if (!in) err = "lecture impossible";
    else if (!o) err = "\xC3\xA9" "criture impossible sur la destination";

    u32 crc_src = 0;
    s64 done = 0;
    while (!err) {
        size_t r = fread(buf, 1, COPY_BUF, in);
        if (r == 0) { if (ferror(in)) err = "erreur de lecture"; break; }
        crc_src = crc_update(crc_src, buf, r);
        if (fwrite(buf, 1, r, o) != r) { err = "erreur d'\xC3\xA9" "criture (destination pleine ?)"; break; }
        done += r;
        if (cb) cb(done, size);
    }
    if (in) fclose(in);
    if (o && fclose(o) != 0 && !err) err = "erreur d'\xC3\xA9" "criture";
    if (!err && done != size) err = "taille copi\xC3\xA9" "e incorrecte";

    /* Relecture complete de la copie */
    if (!err) {
        FILE *v = fopen(dst, "rb");
        u32 crc_dst = 0;
        s64 vsz = 0;
        if (!v) err = "relecture impossible";
        while (!err) {
            size_t r = fread(buf, 1, COPY_BUF, v);
            if (r == 0) break;
            crc_dst = crc_update(crc_dst, buf, r);
            vsz += r;
        }
        if (v) fclose(v);
        if (!err && (vsz != size || crc_dst != crc_src)) err = "copie corrompue (CRC diff\xC3\xA9rent)";
    }
    free(buf);
    if (err && o) remove(dst);
    return err;
}

/* Cherche une chaine dans un fichier (lecture par blocs) ; copie la suite
 * jusqu'a stop dans out. 1 = trouvee. */
int fileop_find_marker(const char *path, const char *marker, char stop, char *out, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static char blk[64 * 1024 + 64];
    int ml = (int)strlen(marker), keep = 0, found = 0;
    for (;;) {
        int r = (int)fread(blk + keep, 1, 64 * 1024, f);
        if (r <= 0) break;
        int n = keep + r;
        for (int i = 0; i + ml <= n && !found; i++) {
            if (memcmp(blk + i, marker, ml) == 0) {
                int j = i + ml, k = 0;
                while (j < n && blk[j] != stop && k < max - 1) out[k++] = blk[j++];
                out[k] = 0;
                found = 1;
            }
        }
        if (found) break;
        keep = n < 48 ? n : 48;                /* chevauchement entre blocs */
        memmove(blk, blk + n - keep, keep);
    }
    fclose(f);
    return found;
}

/* -------------------------------------------------------------- cle USB */

int usb_mounted(void) {
    DIR *d = opendir("usb:/");
    if (!d) return 0;
    closedir(d);
    return 1;
}

int usb_remount(void) {
#ifndef HOST_TEST
    fatUnmount("usb:");
    fatMountSimple("usb", &__io_usbstorage);
#endif
    return usb_mounted();
}
