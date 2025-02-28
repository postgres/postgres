/*-------------------------------------------------------------------------
 *
 * keyring_vault.h
 *      KMIP based keyring provider
 *
 * IDENTIFICATION
 * src/include/keyring/keyring_kmip.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef KEYRING_KMIP_H
#define KEYRING_KMIP_H

extern bool InstallKmipKeyring(void);

extern void kmip_ereport(bool throw_error, const char *msg, int errCode);

#endif							/* // KEYRING_KMIP_H */
