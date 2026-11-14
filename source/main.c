/*
 * LocalSend Wii - recepteur LocalSend v2 (HTTP) pour Wii
 * Fichiers enregistres dans sd:/localsend
 * IMPORTANT : desactiver le chiffrement (HTTPS) dans LocalSend sur le tel.
 *
 * v0.7 : - HTTPS (BearSSL, TLS 1.2) : plus besoin de desactiver le chiffrement
 *        - mise a jour du .dol par LocalSend, bouton YAWM ModMii Edition
 * v0.6 : - panneau apres reception (.wad -> sd:/wad, ROMs -> dossier choisi),
 *          copie verifiee vers la cle USB, reglages (port), lancement WiiXplorer
 * v0.5 : - interface graphique (ui.c / gfx.c / video.c)
 *        - prise en charge de "Transfer-Encoding: chunked" (LocalSend recent
 *          n'envoie plus de Content-Length -> fichiers tronques avant)
 *        - verification de la taille recue, fichier temporaire .part
 *        - tailles 64 bits, limite FAT32 4 Go, controle de l'espace libre
 *        - register_back non bloquant, abonnement multicast
 *        - session abandonnee liberee automatiquement
 *        - demontage propre de la SD (HOME, RESET, POWER)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <locale.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <network.h>
#include <fat.h>
#include <wiiuse/wpad.h>

#if defined(__has_include)
#if __has_include(<sys/statvfs.h>)
#include <sys/statvfs.h>
#define HAVE_STATVFS 1
#endif
#endif

#include "ui.h"
#include "video.h"
#include "config.h"
#include "input.h"
#include "actions.h"
#include "launcher.h"
#include "tls.h"

#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef IOS_O_NONBLOCK
#define IOS_O_NONBLOCK 4
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif
#ifndef SET_SIN_LEN
#define SET_SIN_LEN(a) ((a).sin_len = sizeof(a))
#endif

#define VERSION      "0.7"
#define MCAST_PORT   53317       /* decouverte : toujours 53317 */
#define MCAST_ADDR   "224.0.0.167"
#define SAVE_DIR     "sd:/localsend"
#define ALIAS        "Wii"
#define FINGERPRINT  "wii-homebrew-localsend"   /* si TLS indisponible */

#define HDR_SIZE     4096
#define JSON_MAX     32768
#define IO_SIZE      32768
#define RBUF_SIZE    16384
#define MAX_FILES    64
#define TIMEOUT_MS   10000
#define SESSION_IDLE_MS 30000
#define FAT32_MAX    4294967295LL

/* ====================================================================== */
/*                               ETAT GLOBAL                              */
/* ====================================================================== */

static char my_ip[16] = "0.0.0.0";

static char req[HDR_SIZE];
static char jbody[JSON_MAX];
static u8   iobuf[IO_SIZE] ATTRIBUTE_ALIGN(32);
static u8   rbuf[RBUF_SIZE] ATTRIBUTE_ALIGN(32);

typedef struct {
    char id[64];
    char name[128];
    char token[24];
    s64  size;
    int  done;
    int  ok;
} LSFile;

static LSFile files[MAX_FILES];
static int    nfiles = 0;
static char   session_id[32] = "";
static int    session_active = 0;
static u32    session_ip = 0;
static u64    session_last = 0;
static s64    session_bytes = 0;

static int    total_files = 0;
static s64    total_bytes = 0;

static volatile int g_quit = 0, g_poweroff = 0;

static u64 now_ms(void) { return ticks_to_millisecs(gettime()); }

static s64 sd_free(void) {
#ifdef HAVE_STATVFS
    struct statvfs sv;
    if (statvfs("sd:/", &sv) == 0) return (s64)sv.f_bavail * (s64)sv.f_frsize;
#endif
    return -1;
}

static void refresh_stats(void) { ui_set_stats(total_files, total_bytes, sd_free()); }

/* ====================================================================== */
/*                          RESEAU : utilitaires                          */
/* ====================================================================== */

static int is_again(int r) { return r == -EAGAIN || r == -EWOULDBLOCK; }

/* recv avec timeout sur socket non bloquant (0 = connexion fermee) */
static int recv_to(int s, void *buf, int len, int timeout_ms) {
    int waited = 0;
    for (;;) {
        int r = net_recv(s, buf, len, 0);
        if (r >= 0) return r;
        if (!is_again(r)) return r;
        if (waited >= timeout_ms) return -ETIMEDOUT;
        usleep(1000);
        waited++;
    }
}

static int send_all(int s, const void *data, int len) {
    const char *p = (const char *)data;
    int sent = 0, waited = 0;
    while (sent < len) {
        int r = net_send(s, p + sent, len - sent, 0);
        if (r > 0) { sent += r; waited = 0; continue; }
        if (is_again(r) && waited < TIMEOUT_MS) { usleep(1000); waited++; continue; }
        return -1;
    }
    return sent;
}

/* ---- Connexion courante : HTTP en clair ou HTTPS (meme port) ---- */
static struct {
    int s, tls;
    const u8 *pre;          /* octets deja lus (HTTP en clair) */
    int pre_len, pre_pos;
} conn;

static int c_recv(void *buf, int len) {
    if (conn.tls) {
        int r = tls_read(buf, len);
        return r > 0 ? r : -1;
    }
    if (conn.pre_pos < conn.pre_len) {
        int n = conn.pre_len - conn.pre_pos;
        if (n > len) n = len;
        memcpy(buf, conn.pre + conn.pre_pos, n);
        conn.pre_pos += n;
        return n;
    }
    int r = recv_to(conn.s, buf, len, TIMEOUT_MS);
    return r > 0 ? r : -1;
}

static int c_send(const void *buf, int len) {
    if (conn.tls) return tls_write(buf, len);
    return send_all(conn.s, buf, len) < 0 ? -1 : 0;
}

static void c_flush(void) { if (conn.tls) tls_flush(); }

static const char *reason(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 507: return "Insufficient Storage";
        default:  return "OK";
    }
}

static void http_reply(int c, int code, const char *body) {
    char hdr[256];
    int blen = body ? (int)strlen(body) : 0;
    int h = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        code, reason(code), blen);
    (void)c;
    c_send(hdr, h);
    if (blen) c_send(body, blen);
    c_flush();
}

/* strstr insensible a la casse */
static char *find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return (char *)hay;
    }
    return NULL;
}

/* ====================================================================== */
/*                 REQUETE HTTP : en-tetes + lecture du corps             */
/* ====================================================================== */

typedef struct {
    int  s;
    char method[8];
    char path[512];
    s64  clen;          /* -1 si absent */
    int  chunked;
    int  expect100;
    int  tls;
    int  pos, len;      /* octets en attente dans rbuf */
} Http;

/* Lit et analyse les en-tetes. 0 = ok, -1 = erreur. */
static int http_read_headers(Http *h) {
    int n = 0;
    char *end = NULL;
    int sock = h->s;
    memset(h, 0, sizeof(*h));
    h->s = sock;
    h->clen = -1;
    while (n < HDR_SIZE - 1) {
        int r = c_recv(req + n, HDR_SIZE - 1 - n);
        if (r <= 0) return -1;
        n += r;
        req[n] = 0;
        end = strstr(req, "\r\n\r\n");
        if (end) break;
    }
    if (!end) return -1;
    end[2] = 0;                                  /* coupe apres la derniere ligne */
    int hdr_len = (int)(end + 4 - req);

    sscanf(req, "%7s %511s", h->method, h->path);

    char *v = find_ci(req, "\r\ncontent-length:");
    if (v) h->clen = strtoll(v + 17, NULL, 10);
    v = find_ci(req, "\r\ntransfer-encoding:");
    if (v) {
        char *eol = strstr(v + 2, "\r\n");
        char save = 0;
        if (eol) { save = *eol; *eol = 0; }
        if (find_ci(v, "chunked")) h->chunked = 1;
        if (eol) *eol = save;
    }
    v = find_ci(req, "\r\nexpect:");
    if (v && find_ci(v, "100-continue")) h->expect100 = 1;

    /* octets du corps deja recus avec les en-tetes */
    int extra = n - hdr_len;
    if (extra > 0) memcpy(rbuf, req + hdr_len, extra);
    h->pos = 0;
    h->len = extra > 0 ? extra : 0;
    return 0;
}

static int rd_fill(Http *h) {
    int r = c_recv(rbuf, RBUF_SIZE);
    if (r <= 0) return -1;
    h->pos = 0;
    h->len = r;
    return r;
}

static int rd_byte(Http *h) {
    if (h->pos >= h->len && rd_fill(h) < 0) return -1;
    return rbuf[h->pos++];
}

/* Lit jusqu'a max octets (au moins 1). -1 = erreur/coupure. */
static int rd_some(Http *h, u8 *dst, int max) {
    if (h->pos < h->len) {
        int n = h->len - h->pos;
        if (n > max) n = max;
        memcpy(dst, rbuf + h->pos, n);
        h->pos += n;
        return n;
    }
    if (max >= 4096) {                 /* gros bloc : lecture directe */
        int r = c_recv(dst, max);
        return r > 0 ? r : -1;
    }
    if (rd_fill(h) < 0) return -1;
    return rd_some(h, dst, max);
}

/* Ligne terminee par CRLF (sans le CRLF). Retourne sa longueur ou -1. */
static int rd_line(Http *h, char *out, int max) {
    int i = 0;
    for (;;) {
        int c = rd_byte(h);
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c != '\r' && i < max - 1) out[i++] = (char)c;
    }
    out[i] = 0;
    return i;
}

#define BODY_NET    -1   /* coupure / delai depasse */
#define BODY_SINK   -2   /* erreur d'ecriture */
#define BODY_PROTO  -3   /* flux HTTP invalide */
#define BODY_BIG    -4   /* trop gros */

typedef int (*sink_fn)(void *ctx, const u8 *data, int len);

static s64 body_copy(Http *h, s64 n, sink_fn sink, void *ctx, s64 *total) {
    while (n > 0) {
        int want = n > IO_SIZE ? IO_SIZE : (int)n;
        int r = rd_some(h, iobuf, want);
        if (r <= 0) return BODY_NET;
        if (sink && sink(ctx, iobuf, r) < 0) return BODY_SINK;
        *total += r;
        n -= r;
    }
    return 0;
}

/* Lit le corps complet (Content-Length ou chunked). limit < 0 : pas de limite.
 * Retourne le nombre d'octets du corps ou un code BODY_* negatif. */
static s64 read_body(Http *h, sink_fn sink, void *ctx, s64 limit) {
    s64 total = 0, r;
    if (h->expect100) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        c_send(cont, sizeof(cont) - 1);
        c_flush();
        h->expect100 = 0;
    }
    if (h->chunked) {
        char line[80];
        for (;;) {
            if (rd_line(h, line, sizeof(line)) < 0) return BODY_NET;
            char *e;
            unsigned long long n = strtoull(line, &e, 16);
            if (e == line) return BODY_PROTO;
            if (n == 0) {                            /* dernier bloc + trailers */
                for (;;) {
                    int l = rd_line(h, line, sizeof(line));
                    if (l < 0) return BODY_NET;
                    if (l == 0) break;
                }
                return total;
            }
            if (limit >= 0 && total + (s64)n > limit) return BODY_BIG;
            if ((r = body_copy(h, (s64)n, sink, ctx, &total)) < 0) return r;
            if (rd_line(h, line, sizeof(line)) != 0) return BODY_PROTO;
        }
    }
    if (h->clen <= 0) return 0;                       /* pas de corps */
    if (limit >= 0 && h->clen > limit) return BODY_BIG;
    if ((r = body_copy(h, h->clen, sink, ctx, &total)) < 0) return r;
    return total;
}

/* Sink JSON : remplit jbody */
typedef struct { int len; } JsonCtx;
static int json_sink(void *ctx, const u8 *d, int n) {
    JsonCtx *j = (JsonCtx *)ctx;
    if (j->len + n > JSON_MAX - 1) return -1;
    memcpy(jbody + j->len, d, n);
    j->len += n;
    return 0;
}

/* ====================================================================== */
/*                          JSON / URL : utilitaires                      */
/* ====================================================================== */

/* Cherche "key": "valeur" a partir de start, copie la valeur (desechappee). */
static const char *json_str(const char *start, const char *key, char *out, int max) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(start, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u') {          /* \uXXXX -> '_' */
                out[i++] = '_';
                for (int k = 0; k < 4 && p[1]; k++) p++;
            } else if (*p == 'n' || *p == 't' || *p == 'r') {
                out[i++] = ' ';
            } else {
                out[i++] = *p;
            }
            p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = 0;
    return p;
}

static s64 json_num(const char *start, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(start, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return strtoll(p, NULL, 10);
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Recupere un parametre de query string (decode %XX et +) */
static int query_param(const char *path, const char *name, char *out, int max) {
    const char *q = strchr(path, '?');
    if (!q) return 0;
    q++;
    size_t nl = strlen(name);
    while (*q) {
        if (strncmp(q, name, nl) == 0 && q[nl] == '=') {
            q += nl + 1;
            int i = 0;
            while (*q && *q != '&' && i < max - 1) {
                if (*q == '%' && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) {
                    out[i++] = (char)(hexv(q[1]) * 16 + hexv(q[2]));
                    q += 3;
                } else if (*q == '+') {
                    out[i++] = ' '; q++;
                } else {
                    out[i++] = *q++;
                }
            }
            out[i] = 0;
            return 1;
        }
        q = strchr(q, '&');
        if (!q) break;
        q++;
    }
    return 0;
}

/* Nom de fichier valide pour FAT */
static void sanitize(char *s) {
    for (char *p = s; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 32 || strchr("/\\:*?\"<>|", ch)) *p = '_';
    }
    /* FAT refuse les points / espaces en fin de nom */
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == '.' || s[n - 1] == ' ')) s[--n] = 0;
    while (*s == ' ') memmove(s, s + 1, strlen(s));
    if (!*s) strcpy(s, "fichier");
}

/* Version ASCII stricte (secours si la SD refuse le nom) */
static void ascii_only(char *s) {
    for (; *s; s++) if ((unsigned char)*s >= 128) *s = '_';
}

/* Evite d'ecraser : "photo.jpg" -> "photo (1).jpg" */
static void unique_path(const char *name, char *out, int max) {
    struct stat st;
    char part[300];
    snprintf(out, max, "%s/%s", SAVE_DIR, name);
    snprintf(part, sizeof(part), "%s.part", out);
    if (stat(out, &st) != 0 && stat(part, &st) != 0) return;

    char base[128], ext[32] = "";
    strncpy(base, name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot && dot != base && strlen(dot) < sizeof(ext)) {
        strcpy(ext, dot);
        *dot = 0;
    }
    for (int i = 1; i < 1000; i++) {
        snprintf(out, max, "%s/%s (%d)%s", SAVE_DIR, base, i, ext);
        snprintf(part, sizeof(part), "%s.part", out);
        if (stat(out, &st) != 0 && stat(part, &st) != 0) return;
    }
}

/* ====================================================================== */
/*                              JSON d'identite                           */
/* ====================================================================== */

static const char *my_fingerprint(void) { return tls_ready() ? tls_fingerprint() : FINGERPRINT; }
static int use_https(void) { return tls_ready() && cfg.https; }

/* Marqueur pour reconnaitre un .dol de LocalSend pour Wii (mise a jour) */
__attribute__((used)) static const char dol_marker[] = "LSWII-DOL:" VERSION ":";

static int device_json(char *out, int max, int with_announce) {
    return snprintf(out, max,
        "{\"alias\":\"%s\",\"version\":\"2.0\",\"deviceModel\":\"Wii\","
        "\"deviceType\":\"headless\",\"fingerprint\":\"%s\","
        "\"port\":%d,\"protocol\":\"%s\",\"download\":false%s}",
        ALIAS, my_fingerprint(), cfg.port, use_https() ? "https" : "http", with_announce ? ",\"announce\":true" : "");
}

/* ====================================================================== */
/*                              ROUTES HTTP                               */
/* ====================================================================== */

static void end_session(void) {
    session_active = 0;
    nfiles = 0;
    session_ip = 0;
}

static void route_info(int c) {
    char body[512];
    device_json(body, sizeof(body), 0);
    http_reply(c, 200, body);
}

static void route_register(int c) {
    char who[64] = "?";
    json_str(jbody, "alias", who, sizeof(who));
    ui_log(LOG_INFO, "Appareil d\xC3\xA9tect\xC3\xA9 : %s", who);
    route_info(c);
}

static void route_prepare(int c, u32 client_ip) {
    if (session_active) {
        if (client_ip == session_ip) {
            ui_log(LOG_WARN, "Nouvelle demande : session pr\xC3\xA9" "c\xC3\xA9" "dente abandonn\xC3\xA9" "e");
            end_session();
        } else if (now_ms() - session_last > SESSION_IDLE_MS) {
            ui_log(LOG_WARN, "Session inactive lib\xC3\xA9r\xC3\xA9" "e");
            end_session();
        } else {
            http_reply(c, 409, "{\"message\":\"Session en cours\"}");
            ui_log(LOG_WARN, "Refus\xC3\xA9 : une session est d\xC3\xA9j\xC3\xA0 active");
            return;
        }
    }

    char sender[64] = "?";
    json_str(jbody, "alias", sender, sizeof(sender));

    nfiles = 0;
    const char *p = strstr(jbody, "\"files\"");
    if (!p) { http_reply(c, 400, "{}"); return; }

    /* Chaque fichier : {"id":"...","fileName":"...","size":N,...} */
    s64 sum = 0;
    while (nfiles < MAX_FILES) {
        const char *q = strstr(p, "\"id\"");
        if (!q) break;
        LSFile *f = &files[nfiles];
        memset(f, 0, sizeof(*f));
        if (!json_str(q, "id", f->id, sizeof(f->id))) break;

        const char *next = strstr(q + 4, "\"id\"");
        const char *fnp = strstr(q, "\"fileName\"");
        if (fnp && (!next || fnp < next))
            json_str(fnp, "fileName", f->name, sizeof(f->name));
        if (!f->name[0]) snprintf(f->name, sizeof(f->name), "fichier_%d.bin", nfiles);
        sanitize(f->name);

        const char *szp = strstr(q, "\"size\"");
        f->size = (szp && (!next || szp < next)) ? json_num(szp, "size") : 0;
        if (f->size < 0) f->size = 0;

        if (f->size > FAT32_MAX) {
            http_reply(c, 413, "{\"message\":\"Fichier > 4 Go (limite FAT32)\"}");
            ui_log(LOG_ERR, "Refus\xC3\xA9 : %s d\xC3\xA9passe 4 Go (limite FAT32)", f->name);
            nfiles = 0;
            return;
        }
        sum += f->size;

        snprintf(f->token, sizeof(f->token), "t%08lx%02d",
                 (unsigned long)(gettick() & 0xFFFFFFFF), nfiles);
        nfiles++;
        p = q + 4;
    }

    if (nfiles == 0) { http_reply(c, 400, "{}"); return; }

    s64 freeb = sd_free();
    if (freeb >= 0 && sum + 1024 * 1024 > freeb) {
        http_reply(c, 507, "{\"message\":\"Espace insuffisant sur la SD\"}");
        ui_log(LOG_ERR, "Refus\xC3\xA9 : espace insuffisant sur la SD");
        nfiles = 0;
        return;
    }

    snprintf(session_id, sizeof(session_id), "wii%08lx",
             (unsigned long)(gettick() & 0xFFFFFFFF));
    session_active = 1;
    session_ip = client_ip;
    session_last = now_ms();
    session_bytes = 0;

    static char resp[8192];
    int len = snprintf(resp, sizeof(resp),
                       "{\"sessionId\":\"%s\",\"files\":{", session_id);
    for (int i = 0; i < nfiles && len < (int)sizeof(resp) - 200; i++) {
        len += snprintf(resp + len, sizeof(resp) - len, "%s\"%s\":\"%s\"",
                        i ? "," : "", files[i].id, files[i].token);
    }
    snprintf(resp + len, sizeof(resp) - len, "}}");

    http_reply(c, 200, resp);
    ui_session(sender, nfiles, sum);
    ui_log(LOG_OK, "%s envoie %d fichier(s) - accept\xC3\xA9", sender, nfiles);
}

static void end_session_if_done(void) {
    int ok = 0;
    for (int i = 0; i < nfiles; i++) {
        if (!files[i].done) return;
        if (files[i].ok) ok++;
    }
    int count = nfiles;
    end_session();
    actions_session_end();               /* rangement automatique eventuel */
    refresh_stats();
    if (ok > 0) ui_done(ok, count, session_bytes);
}

typedef struct {
    FILE *fp;
    s64   got;
    s64   size;
    int   over;      /* plus d'octets que prevu */
    int   werr;      /* fwrite en echec */
} FileCtx;

static int file_sink(void *ctx, const u8 *d, int n) {
    FileCtx *f = (FileCtx *)ctx;
    if (f->got + n > f->size) { f->over = 1; return -1; }
    if (fwrite(d, 1, n, f->fp) != (size_t)n) { f->werr = 1; return -1; }
    f->got += n;
    session_last = now_ms();
    ui_progress(f->got);
    return 0;
}

static void route_upload(Http *h) {
    int c = h->s;
    char sid[48] = "", fid[64] = "", tok[32] = "";
    query_param(h->path, "sessionId", sid, sizeof(sid));
    query_param(h->path, "fileId", fid, sizeof(fid));
    query_param(h->path, "token", tok, sizeof(tok));

    LSFile *f = NULL;
    int idx = 0;
    if (session_active && strcmp(sid, session_id) == 0) {
        for (int i = 0; i < nfiles; i++)
            if (strcmp(files[i].id, fid) == 0 && strcmp(files[i].token, tok) == 0) {
                f = &files[i];
                idx = i;
            }
    }
    if (!f) {
        http_reply(c, 403, "{\"message\":\"Token invalide\"}");
        ui_log(LOG_ERR, "Upload refus\xC3\xA9 (session/token invalide)");
        return;
    }
    if (f->done) {
        http_reply(c, 409, "{\"message\":\"Deja recu\"}");
        return;
    }
    session_last = now_ms();

    mkdir(SAVE_DIR, 0777);
    char full[256], part[300];
    unique_path(f->name, full, sizeof(full));
    snprintf(part, sizeof(part), "%s.part", full);

    FILE *fp = fopen(part, "wb");
    if (!fp) {                                  /* nom refuse : version ASCII */
        ascii_only(f->name);
        unique_path(f->name, full, sizeof(full));
        snprintf(part, sizeof(part), "%s.part", full);
        fp = fopen(part, "wb");
    }
    if (!fp) {
        http_reply(c, 500, "{\"message\":\"Ecriture SD impossible\"}");
        ui_log(LOG_ERR, "Erreur \xC3\xA9" "criture : %s", f->name);
        ui_fail("Impossible de cr\xC3\xA9" "er le fichier sur la SD");
        f->done = 1;
        end_session_if_done();
        return;
    }

    ui_file_begin(f->name, idx + 1, nfiles, f->size);

    FileCtx fc = { fp, 0, f->size, 0, 0 };
    s64 r = read_body(h, file_sink, &fc, -1);
    int close_err = fclose(fp) != 0;

    char got_s[32], exp_s[32];
    snprintf(got_s, sizeof(got_s), "%lld", (long long)fc.got);
    snprintf(exp_s, sizeof(exp_s), "%lld", (long long)f->size);

    f->done = 1;
    if (r >= 0 && !close_err && fc.got == f->size) {
        int renamed = rename(part, full) == 0;
        if (!renamed)
            ui_log(LOG_WARN, "Renommage impossible, gard\xC3\xA9 en .part");
        f->ok = 1;
        total_files++;
        total_bytes += fc.got;
        session_bytes += fc.got;
        http_reply(c, 200, "{}");
        ui_log(LOG_OK, "Re\xC3\xA7u : %s (%.1f Ko, taille v\xC3\xA9rifi\xC3\xA9" "e)", f->name, fc.got / 1024.0);
        if (renamed) {
            const char *base = strrchr(full, '/');
            actions_received(full, base ? base + 1 : full, fc.got);
        }
    } else {
        remove(part);
        const char *why;
        if (r == BODY_NET)        why = "connexion coup\xC3\xA9" "e ou d\xC3\xA9lai d\xC3\xA9pass\xC3\xA9";
        else if (r == BODY_PROTO) why = "flux HTTP invalide";
        else if (fc.werr)         why = "erreur d'\xC3\xA9" "criture (SD pleine ?)";
        else if (fc.over)         why = "plus de donn\xC3\xA9" "es que pr\xC3\xA9vu";
        else if (close_err)       why = "erreur d'\xC3\xA9" "criture SD";
        else                      why = "taille incorrecte";
        http_reply(c, 500, "{\"message\":\"Transfert incomplet\"}");
        ui_log(LOG_ERR, "\xC3\x89" "chec : %s - %s (%s/%s o)", f->name, why, got_s, exp_s);
        char msg[200];
        snprintf(msg, sizeof(msg), "%.60s : %s", f->name, why);
        ui_fail(msg);
    }
    end_session_if_done();
}

static void route_cancel(int c) {
    if (session_active) {
        ui_log(LOG_WARN, "Transfert annul\xC3\xA9 par l'exp\xC3\xA9" "diteur");
        ui_idle();
    }
    end_session();
    http_reply(c, 200, "{}");
}

/* ====================================================================== */
/*                          GESTION D'UNE CONNEXION                       */
/* ====================================================================== */

static void handle_request(int c, u32 client_ip) {
    Http h;
    h.s = c;
    if (http_read_headers(&h) < 0) {
        if (conn.tls && tls_last_error())
            ui_log(LOG_ERR, "HTTPS : \xC3\xA9" "chec de la connexion s\xC3\xA9" "curis\xC3\xA9" "e (erreur %d)", tls_last_error());
        return;
    }

    if (strstr(h.path, "/api/localsend/v2/upload")) {
        route_upload(&h);
        return;
    }

    /* Autres routes : lire tout le corps JSON */
    JsonCtx jc = { 0 };
    s64 r = read_body(&h, json_sink, &jc, JSON_MAX - 1);
    jbody[jc.len] = 0;
    if (r == BODY_BIG) {
        http_reply(c, 413, "{\"message\":\"Requete trop grosse\"}");
        ui_log(LOG_ERR, "Requ\xC3\xAAte trop grosse (%s)", h.path);
        return;
    }

    if (strstr(h.path, "/api/localsend/v2/info"))                route_info(c);
    else if (strstr(h.path, "/api/localsend/v2/register"))       route_register(c);
    else if (strstr(h.path, "/api/localsend/v2/prepare-upload")) route_prepare(c, client_ip);
    else if (strstr(h.path, "/api/localsend/v2/cancel"))         route_cancel(c);
    else {
        http_reply(c, 404, "{}");
        ui_log(LOG_DIM, "%s %.50s -> 404", h.method, h.path);
    }
}

static void handle(int c, u32 client_ip) {
    static u8 sniff[HDR_SIZE];
    int n = recv_to(c, sniff, sizeof(sniff), TIMEOUT_MS);
    if (n <= 0) return;
    memset(&conn, 0, sizeof(conn));
    conn.s = c;
    if (sniff[0] == 0x16) {                       /* debut d'une poignee de main TLS */
        if (!tls_ready() || tls_begin(c, sniff, n) < 0) {
            ui_log(LOG_ERR, "Connexion HTTPS refus\xC3\xA9" "e : TLS indisponible");
            return;
        }
        conn.tls = 1;
    } else {
        conn.pre = sniff;
        conn.pre_len = n;
    }
    handle_request(c, client_ip);
    if (conn.tls) tls_end();
    conn.tls = 0;
}

/* ====================================================================== */
/*                       DECOUVERTE (multicast UDP)                       */
/* ====================================================================== */

static void announce(int udp) {
    char msg[512];
    int len = device_json(msg, sizeof(msg), 1);
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MCAST_PORT);
    SET_SIN_LEN(to);
    inet_aton(MCAST_ADDR, &to.sin_addr);
    net_sendto(udp, msg, len, 0, (struct sockaddr *)&to, sizeof(to));
}

/* Repond a une annonce en faisant POST /register chez l'expediteur.
 * Connexion non bloquante : au pire ~1,5 s d'attente, jamais de blocage. */
static void register_back(struct in_addr ip, int port) {
    int s = net_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    net_fcntl(s, F_SETFL, IOS_O_NONBLOCK);

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    SET_SIN_LEN(a);
    a.sin_addr = ip;

    int waited = 0, r;
    for (;;) {
        r = net_connect(s, (struct sockaddr *)&a, sizeof(a));
        if (r == 0 || r == -EISCONN) break;
        if ((r == -EINPROGRESS || r == -EALREADY || is_again(r)) && waited < 1500) {
            usleep(1000);
            waited++;
            continue;
        }
        net_close(s);
        return;
    }

    char body[512], hdr[256];
    int bl = device_json(body, sizeof(body), 0);
    int hl = snprintf(hdr, sizeof(hdr),
        "POST /api/localsend/v2/register HTTP/1.1\r\n"
        "Host: %s:%d\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        inet_ntoa(ip), port, bl);
    if (send_all(s, hdr, hl) > 0) send_all(s, body, bl);

    char tmp[256];
    recv_to(s, tmp, sizeof(tmp), 800);
    net_close(s);
}

static void poll_udp(int udp) {
    static char ub[1024];
    static u32 last_ip = 0;
    static u64 last_t = 0;

    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    int r = net_recvfrom(udp, ub, sizeof(ub) - 1, 0, (struct sockaddr *)&from, &fl);
    if (r <= 0) return;
    ub[r] = 0;

    if (strcmp(inet_ntoa(from.sin_addr), my_ip) == 0) return;   /* nous-memes */
    char fp[96] = "";
    json_str(ub, "fingerprint", fp, sizeof(fp));
    if (strcmp(fp, my_fingerprint()) == 0 || strcmp(fp, FINGERPRINT) == 0) return;
    if (!strstr(ub, "\"announce\":true") && !strstr(ub, "\"announcement\":true")) return;

    if (from.sin_addr.s_addr == last_ip && now_ms() - last_t < 5000) return;
    last_ip = from.sin_addr.s_addr;
    last_t = now_ms();

    char who[64] = "?", proto[16] = "http";
    json_str(ub, "alias", who, sizeof(who));
    json_str(ub, "protocol", proto, sizeof(proto));
    s64 port = json_num(ub, "port");
    if (port <= 0 || port > 65535) port = MCAST_PORT;

    ui_log(LOG_INFO, "Annonce de %s -> r\xC3\xA9ponse", who);
    /* Appareil en HTTP : on s'enregistre chez lui. En HTTPS il faudrait un
     * client TLS : la reponse multicast suffit pour qu'il nous voie. */
    if (strcmp(proto, "https") != 0) register_back(from.sin_addr, (int)port);
    announce(udp);
}

/* ====================================================================== */
/*                                  MAIN                                  */
/* ====================================================================== */

static void cb_reset(u32 irq, void *ctx) { (void)irq; (void)ctx; g_quit = 1; }
static void cb_power(void) { g_quit = 1; g_poweroff = 1; }
static void cb_wpad_power(s32 chan) { (void)chan; g_quit = 1; g_poweroff = 1; }

/* Serveur TCP (HTTP) */
static int tcp_sock = -1;

static int server_open(int port) {
    int s = net_socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    SET_SIN_LEN(addr);
    if (net_bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || net_listen(s, 5) < 0) {
        net_close(s);
        return -1;
    }
    net_fcntl(s, F_SETFL, IOS_O_NONBLOCK);
    return s;
}

static int udp_sock = -1;

/* Appele depuis les reglages : relance le serveur sur un autre port */
int app_set_port(int port) {
    int s = server_open(port);
    if (s < 0) return -1;
    if (tcp_sock >= 0) net_close(tcp_sock);
    tcp_sock = s;
    cfg.port = port;
    ui_set_info(ALIAS, my_ip, port);
    if (udp_sock >= 0) announce(udp_sock);   /* previent les telephones */
    return 0;
}

/* Appele depuis les reglages */
void app_https_changed(void) {
    ui_set_https(use_https());
    if (udp_sock >= 0) announce(udp_sock);
}

static void launch_app(void) {
    const char *path = actions_launch_path();
    char msg[96];
    snprintf(msg, sizeof(msg), "Lancement de %s...", actions_launch_label());
    ui_log(LOG_INFO, "Lancement de %s", path);
    ui_boot(msg);
    config_save();
    const char *err = launcher_run(path);   /* ne revient qu'en cas d'erreur */
    ui_log(LOG_ERR, "%s : %s", actions_launch_label(), err);
    ui_idle();
}

static void fatal(const char *msg) {
    ui_fatal(msg);
    sleep(5);
    fatUnmount("sd:");
    exit(1);
}

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "C.UTF-8");      /* noms de fichiers UTF-8 sur la SD */

    if (ui_init(VERSION) < 0) return 1;
    input_init();
    SYS_SetResetCallback(cb_reset);
    SYS_SetPowerCallback(cb_power);
    WPAD_SetPowerButtonCallback(cb_wpad_power);

    ui_boot("Initialisation de la carte SD...");
    if (!fatInitDefault()) fatal("Carte SD introuvable");
    mkdir(SAVE_DIR, 0777);
    config_load();
    actions_init(argc > 0 ? argv[0] : NULL);

    ui_boot("Connexion au r\xC3\xA9seau (DHCP)...");
    char gw[16], mask[16];
    if (if_config(my_ip, mask, gw, TRUE, 20) < 0)
        fatal("Erreur r\xC3\xA9seau : v\xC3\xA9rifie le profil de connexion de la Wii");
    /* Serveur TCP (port configurable, retour a 53317 si indisponible) */
    tcp_sock = server_open(cfg.port);
    if (tcp_sock < 0 && cfg.port != DEFAULT_PORT) {
        ui_log(LOG_WARN, "Port %d indisponible, utilisation de %d", cfg.port, DEFAULT_PORT);
        cfg.port = DEFAULT_PORT;
        tcp_sock = server_open(cfg.port);
    }
    if (tcp_sock < 0) fatal("Erreur : impossible d'ouvrir le serveur HTTP");
    ui_set_info(ALIAS, my_ip, cfg.port);

    /* Identite HTTPS (cle + certificat crees au premier lancement) */
    ui_boot("Pr\xC3\xA9paration du chiffrement HTTPS...");
    int tr = tls_init();
    if (tr == 1)      ui_log(LOG_INFO, "Nouvelle identit\xC3\xA9 HTTPS cr\xC3\xA9\xC3\xA9" "e (%.8s...)", tls_fingerprint());
    else if (tr < 0)  ui_log(LOG_WARN, "HTTPS indisponible (%d) : d\xC3\xA9sactive le chiffrement", tr);
    ui_set_https(use_https());

    /* Socket UDP (annonces + ecoute) */
    int udp = net_socket(AF_INET, SOCK_DGRAM, 0);
    udp_sock = udp;
    if (udp >= 0) {
        struct sockaddr_in ua;
        memset(&ua, 0, sizeof(ua));
        ua.sin_family = AF_INET;
        ua.sin_port = htons(MCAST_PORT);
        ua.sin_addr.s_addr = INADDR_ANY;
        SET_SIN_LEN(ua);
        net_bind(udp, (struct sockaddr *)&ua, sizeof(ua));
        net_fcntl(udp, F_SETFL, IOS_O_NONBLOCK);
#ifdef IP_ADD_MEMBERSHIP
        struct { struct in_addr multiaddr, iface; } mreq;
        inet_aton(MCAST_ADDR, &mreq.multiaddr);
        mreq.iface.s_addr = INADDR_ANY;
        int jr = net_setsockopt(udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        if (jr >= 0) ui_log(LOG_DIM, "Multicast : \xC3\xA9" "coute active");
        else         ui_log(LOG_DIM, "Multicast : \xC3\xA9" "coute indisponible (%d)", jr);
#endif
    }

    refresh_stats();
    if (use_https()) ui_log(LOG_OK, "Pr\xC3\xAAt (HTTPS actif, empreinte %.8s...).", tls_fingerprint());
    else             ui_log(LOG_OK, "Pr\xC3\xAAt (HTTP). D\xC3\xA9sactive le chiffrement dans LocalSend.");
    ui_idle();

    u64 last_announce = 0;
    while (!g_quit) {
        u32 down, held, held_ms;
        input_scan(&down, &held, &held_ms);
        if (down & BTN_HOME) break;
        if (actions_update(down, held_ms, session_active) == REQ_LAUNCH)
            launch_app();

        if (udp >= 0) {
            if (now_ms() - last_announce >= 5000) {
                announce(udp);
                last_announce = now_ms();
            }
            poll_udp(udp);
        }

        struct sockaddr_in client;
        socklen_t cl = sizeof(client);
        memset(&client, 0, sizeof(client));
        int c = net_accept(tcp_sock, (struct sockaddr *)&client, &cl);
        if (c >= 0) {
            net_fcntl(c, F_SETFL, IOS_O_NONBLOCK);   /* recv avec timeout */
            handle(c, client.sin_addr.s_addr);
            net_shutdown(c, 2);
            net_close(c);
        }

        ui_frame();
        video_wait();
    }

    if (udp >= 0) net_close(udp);
    net_close(tcp_sock);
    ui_boot(g_poweroff ? "Extinction..." : "Au revoir !");
    fatUnmount("sd:");
    if (g_poweroff) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    return 0;
}
