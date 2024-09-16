/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog_encrypt_fe.h
 *	   Frontened definitions for encrypted XLog storage manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_XLOGENCRYPT_FE_H
#define PG_TDE_XLOGENCRYPT_FE_H

#ifdef PERCONA_EXT
#include "access/pg_tde_xlog_encrypt.h"
#include "catalog/tde_global_space.h"
#include "encryption/enc_aes.h"
#include "keyring/keyring_file.h"
#include "keyring/keyring_vault.h"

/* Frontend has to call it needs to read an encrypted XLog */
#define TDE_XLOG_INIT(kring_dir)	\
	AesInit();						\
	InstallFileKeyring();			\
	InstallVaultV2Keyring();		\
	TDEInitGlobalKeys(kring_dir);	\
	TDEXLogSmgrInit()

#endif							/* PERCONA_EXT */

#endif							/* PG_TDE_XLOGENCRYPT_FE_H */