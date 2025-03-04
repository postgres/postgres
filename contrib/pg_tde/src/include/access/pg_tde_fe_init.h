/*-------------------------------------------------------------------------
 *
 * pg_tde_fe.h
 *	   Frontened definitions for encrypted XLog storage manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_FE_INIT_H
#define PG_TDE_FE_INIT_H

#include "common/pg_tde_utils.h"
#include "encryption/enc_aes.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"
#include "keyring/keyring_kmip.h"

/* Frontend has to call this to access keys */
static inline void
pg_tde_fe_init(const char *kring_dir)
{
	AesInit();
	InstallFileKeyring();
	InstallVaultV2Keyring();
	InstallKmipKeyring();
	pg_tde_set_data_dir(kring_dir);
}

#endif							/* PG_TDE_FE_INIT_H */
