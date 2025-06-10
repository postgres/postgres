/*
 * Internals for the KMIP based keyring provider
 */

#ifndef KEYRING_KMIP_IMPL_H
#define KEYRING_KMIP_IMPL_H

extern int	pg_tde_kmip_set_by_name(BIO *bio, char *key_name, const unsigned char *key, unsigned int key_len);
extern int	pg_tde_kmip_locate_key(BIO *bio, const char *key_name, size_t *ids_found, char *id);
extern int	pg_tde_kmip_get_key(BIO *bio, char *id, char **key, int *key_len);

#endif							/* KEYRING_KMIP_IMPL_H */
