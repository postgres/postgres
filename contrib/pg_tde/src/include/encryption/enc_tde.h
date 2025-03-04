/*-------------------------------------------------------------------------
 *
 * enc_tde.h
 *	  Encryption / Decryption of functions for TDE
 *
 * src/include/encryption/enc_tde.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENC_TDE_H
#define ENC_TDE_H

#include "utils/rel.h"
#include "storage/bufpage.h"
#include "executor/tuptable.h"
#include "executor/tuptable.h"
#include "access/pg_tde_tdemap.h"
#include "keyring/keyring_api.h"

extern void pg_tde_crypt(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, InternalKey *key, const char *context);

/* Function Macros over crypt */

#define PG_TDE_ENCRYPT_DATA(_iv_prefix, _start_offset, _data, _data_len, _out, _key) \
	pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, "ENCRYPT")

#define PG_TDE_DECRYPT_DATA(_iv_prefix, _start_offset, _data, _data_len, _out, _key) \
	pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, "DECRYPT")

extern void AesEncryptKey(const TDEPrincipalKey *principal_key, Oid dbOid, InternalKey *rel_key_data, InternalKey **p_enc_rel_key_data, size_t *enc_key_bytes);
extern void AesDecryptKey(const TDEPrincipalKey *principal_key, Oid dbOid, InternalKey **p_rel_key_data, InternalKey *enc_rel_key_data, size_t *key_bytes);

#endif							/* ENC_TDE_H */
