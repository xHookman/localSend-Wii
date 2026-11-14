#ifndef TLS_H
#define TLS_H

/* Identite TLS : cle EC P-256 + certificat auto-signe, conserves dans
 * sd:/localsend/tls_key.bin et tls_cert.der (crees au premier lancement). */
int         tls_init(void);              /* 0 = pret, 1 = nouvelle identite creee, <0 erreur */
int         tls_ready(void);
const char *tls_fingerprint(void);       /* SHA-256 du certificat, hex majuscules */

/* Connexion serveur (une a la fois). pre = octets deja lus sur le socket. */
int  tls_begin(int sock, const unsigned char *pre, int pre_len);
int  tls_read(void *buf, int len);       /* >0 octets, <=0 fin/erreur */
int  tls_write(const void *buf, int len);/* 0 ok, <0 erreur */
int  tls_flush(void);
void tls_end(void);
int  tls_last_error(void);               /* code d'erreur BearSSL */

#endif
