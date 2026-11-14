/*
 * tls.c - HTTPS pour LocalSend (BearSSL, TLS 1.2)
 *
 * LocalSend (rustls) exige seulement : un certificat X.509 v3 auto-signe
 * valide, et que son SHA-256 soit le "fingerprint" annonce.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <network.h>
#include "bearssl/bearssl.h"
#include "tls.h"

#define KEY_PATH   "sd:/localsend/tls_key.bin"
#define CERT_PATH  "sd:/localsend/tls_cert.der"
#define CN         "LocalSend Wii"
#define IO_TIMEOUT 10000

static unsigned char sk_buf[BR_EC_KBUF_PRIV_MAX_SIZE];
static br_ec_private_key sk;
static unsigned char cert[600];
static int cert_len = 0;
static char fp_hex[65] = "";
static int ready = 0;

static br_hmac_drbg_context drbg;

static br_ssl_server_context sc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
static br_sslio_context ioc;
static br_x509_certificate chain[1];

static struct { int s; const unsigned char *pre; int pre_len, pre_pos; } io;

/* ---------------------------------------------------------------- hasard */

/* Melange de sources faibles mais variees : base de temps, gigue des appels
 * IOS, adresse MAC, heure. Suffisant pour un usage en reseau local. */
static void seed_rng(void) {
    br_sha256_context h;
    br_sha256_init(&h);
    u64 t = gettime();
    br_sha256_update(&h, &t, sizeof(t));
    time_t now = time(NULL);
    br_sha256_update(&h, &now, sizeof(now));
#ifndef HOST_TEST
    unsigned char mac[6];
    if (net_get_mac_address(mac) >= 0) br_sha256_update(&h, mac, sizeof(mac));
#endif
    for (int i = 0; i < 256; i++) {
        u64 a = gettime();
        usleep(17 + (i & 7));
        u64 b = gettime();
        u32 d = (u32)(b - a) ^ (u32)(a >> 7);
        br_sha256_update(&h, &d, sizeof(d));
    }
    unsigned char seed[32];
    br_sha256_out(&h, seed);
    br_hmac_drbg_init(&drbg, &br_sha256_vtable, seed, sizeof(seed));
}

static void rng_more(void) {           /* rafraichit avec la base de temps */
    u64 t = gettime();
    br_hmac_drbg_update(&drbg, &t, sizeof(t));
}

/* -------------------------------------------------------------- DER/X.509 */

static int der_hdr(unsigned char *o, int tag, int len) {
    o[0] = (unsigned char)tag;
    if (len < 128) { o[1] = (unsigned char)len; return 2; }
    if (len < 256) { o[1] = 0x81; o[2] = (unsigned char)len; return 3; }
    o[1] = 0x82; o[2] = (unsigned char)(len >> 8); o[3] = (unsigned char)len;
    return 4;
}

static int der_wrap(unsigned char *o, int tag, const unsigned char *v, int len) {
    int n = der_hdr(o, tag, len);
    memmove(o + n, v, len);
    return n + len;
}

static const unsigned char OID_ECDSA_SHA256[] = { 0x30, 0x0A, 0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02 };
static const unsigned char SPKI_PREFIX[] = {
    0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01,
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00 };

static int build_name(unsigned char *o) {
    unsigned char a[64], b[64], c[64];
    int n = 0;
    a[n++] = 0x06; a[n++] = 0x03; a[n++] = 0x55; a[n++] = 0x04; a[n++] = 0x03;
    n += der_wrap(a + n, 0x0C, (const unsigned char *)CN, (int)strlen(CN));
    int m = der_wrap(b, 0x30, a, n);
    m = der_wrap(c, 0x31, b, m);
    return der_wrap(o, 0x30, c, m);
}

static int make_cert(const br_ec_impl *ec) {
    unsigned char pub[BR_EC_KBUF_PUB_MAX_SIZE];
    br_ec_public_key pk;
    if (br_ec_compute_pub(ec, &pk, pub, &sk) != 65) return -1;

    unsigned char tbs_in[400], tbs[420];
    int n = 0;
    static const unsigned char ver[] = { 0xA0, 0x03, 0x02, 0x01, 0x02 };   /* v3 */
    memcpy(tbs_in + n, ver, sizeof(ver)); n += sizeof(ver);

    unsigned char serial[16];
    br_hmac_drbg_generate(&drbg, serial, sizeof(serial));
    serial[0] &= 0x7F;
    if (!serial[0]) serial[0] = 1;
    n += der_wrap(tbs_in + n, 0x02, serial, sizeof(serial));

    memcpy(tbs_in + n, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256)); n += sizeof(OID_ECDSA_SHA256);
    n += build_name(tbs_in + n);                                          /* issuer */
    static const char nb[] = "200101000000Z", na[] = "491231235959Z";
    unsigned char val[40];
    int v = der_wrap(val, 0x17, (const unsigned char *)nb, 13);
    v += der_wrap(val + v, 0x17, (const unsigned char *)na, 13);
    n += der_wrap(tbs_in + n, 0x30, val, v);                             /* validity */
    n += build_name(tbs_in + n);                                          /* subject */
    memcpy(tbs_in + n, SPKI_PREFIX, sizeof(SPKI_PREFIX)); n += sizeof(SPKI_PREFIX);
    memcpy(tbs_in + n, pk.q, 65); n += 65;
    int tlen = der_wrap(tbs, 0x30, tbs_in, n);

    unsigned char hash[32], sig[80];
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, tbs, tlen);
    br_sha256_out(&h, hash);
    size_t slen = br_ecdsa_sign_asn1_get_default()(ec, &br_sha256_vtable, hash, &sk, sig);
    if (!slen) return -1;

    unsigned char body[560], bits[90];
    int b = 0;
    memcpy(body + b, tbs, tlen); b += tlen;
    memcpy(body + b, OID_ECDSA_SHA256, sizeof(OID_ECDSA_SHA256)); b += sizeof(OID_ECDSA_SHA256);
    bits[0] = 0;
    memcpy(bits + 1, sig, slen);
    b += der_wrap(body + b, 0x03, bits, (int)slen + 1);
    cert_len = der_wrap(cert, 0x30, body, b);
    return 0;
}

/* --------------------------------------------------------------- fichiers */

static int load_file(const char *p, unsigned char *buf, int max) {
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, max, f);
    fclose(f);
    return n;
}

static int save_file(const char *p, const unsigned char *buf, int len) {
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int ok = (int)fwrite(buf, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static void compute_fp(void) {
    unsigned char h[32];
    br_sha256_context c;
    br_sha256_init(&c);
    br_sha256_update(&c, cert, cert_len);
    br_sha256_out(&c, h);
    for (int i = 0; i < 32; i++) sprintf(fp_hex + i * 2, "%02X", h[i]);
}

int tls_init(void) {
    const br_ec_impl *ec = br_ec_get_default();
    int created = 0;
    seed_rng();

    int kl = load_file(KEY_PATH, sk_buf, sizeof(sk_buf));
    int cl = load_file(CERT_PATH, cert, sizeof(cert));
    sk.curve = BR_EC_secp256r1;
    sk.x = sk_buf;
    sk.xlen = 32;
    if (kl == 32 && cl > 100 && cl < (int)sizeof(cert)) {
        cert_len = cl;
    } else {
        if (br_ec_keygen(&drbg.vtable, ec, &sk, sk_buf, BR_EC_secp256r1) != 32) return -1;
        if (make_cert(ec) < 0) return -2;
        save_file(KEY_PATH, sk_buf, 32);          /* echec non bloquant */
        save_file(CERT_PATH, cert, cert_len);
        created = 1;
    }
    compute_fp();

    chain[0].data = cert;
    chain[0].data_len = cert_len;
    br_ssl_server_init_full_ec(&sc, chain, 1, BR_KEYTYPE_EC, &sk);
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,   /* le plus rapide sans AES materiel */
        BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    };
    br_ssl_engine_set_suites(&sc.eng, suites, sizeof(suites) / sizeof(suites[0]));
    br_ssl_engine_set_versions(&sc.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_add_flags(&sc.eng, BR_OPT_ENFORCE_SERVER_PREFERENCES);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
    ready = 1;
    return created;
}

int tls_ready(void) { return ready; }
const char *tls_fingerprint(void) { return fp_hex; }

/* -------------------------------------------------------------- connexion */

static int low_read(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    if (io.pre_pos < io.pre_len) {
        int n = io.pre_len - io.pre_pos;
        if (n > (int)len) n = (int)len;
        memcpy(buf, io.pre + io.pre_pos, n);
        io.pre_pos += n;
        return n;
    }
    for (int waited = 0;; waited++) {
        int r = net_recv(io.s, buf, (int)len, 0);
        if (r > 0) return r;
        if (r == 0) return -1;
        if (r != -EAGAIN && r != -EWOULDBLOCK) return -1;
        if (waited >= IO_TIMEOUT) return -1;
        usleep(1000);
    }
}

static int low_write(void *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    for (int waited = 0;; waited++) {
        int r = net_send(io.s, buf, (int)len, 0);
        if (r > 0) return r;
        if (r != -EAGAIN && r != -EWOULDBLOCK) return -1;
        if (waited >= IO_TIMEOUT) return -1;
        usleep(1000);
    }
}

int tls_begin(int sock, const unsigned char *pre, int pre_len) {
    if (!ready) return -1;
    io.s = sock;
    io.pre = pre;
    io.pre_len = pre_len;
    io.pre_pos = 0;
    rng_more();
    unsigned char seed[32];
    br_hmac_drbg_generate(&drbg, seed, sizeof(seed));
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
    if (!br_ssl_server_reset(&sc)) return -1;
    br_sslio_init(&ioc, &sc.eng, low_read, NULL, low_write, NULL);
    return 0;
}

int tls_read(void *buf, int len) { return br_sslio_read(&ioc, buf, len); }

int tls_write(const void *buf, int len) { return br_sslio_write_all(&ioc, buf, len) < 0 ? -1 : 0; }

int tls_flush(void) { return br_sslio_flush(&ioc); }

void tls_end(void) {
    br_sslio_flush(&ioc);
    br_ssl_engine_close(&sc.eng);
    br_sslio_flush(&ioc);          /* envoie close_notify, sans attendre la reponse */
}

int tls_last_error(void) { return br_ssl_engine_last_error(&sc.eng); }
