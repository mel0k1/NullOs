#ifndef TLSGLUE_H
#define TLSGLUE_H

/* mbedtls 4.2 HTTPS glue (kernel/tlsglue.c).
 * `https <host|ip> <path> [verify] [outfile]` — TLS1.2 client over the
 * kernel TCP stack; dev mode VERIFY_NONE, `verify` -> VERIFY_REQUIRED
 * against the compiled-in CA bundle. */
void cmd_https(int argc, char** argv);

#endif /* TLSGLUE_H */
