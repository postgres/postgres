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

extern void
			pg_tde_crypt(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, RelKeyData *key, const char *context);
extern void
			pg_tde_crypt_tuple(HeapTuple tuple, HeapTuple out_tuple, RelKeyData *key, const char *context);

/* A wrapper to encrypt a tuple before adding it to the buffer */
extern OffsetNumber
			PGTdePageAddItemExtended(RelFileLocator rel, Oid oid, BlockNumber bn, Page page,
									 Item item,
									 Size size,
									 OffsetNumber offsetNumber,
									 int flags);

/* Function Macros over crypt */

#define PG_TDE_ENCRYPT_DATA(_iv_prefix, _start_offset, _data, _data_len, _out, _key) \
	pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, "ENCRYPT")

#define PG_TDE_DECRYPT_DATA(_iv_prefix, _start_offset, _data, _data_len, _out, _key) \
	pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, "DECRYPT")

#define PG_TDE_DECRYPT_TUPLE(_tuple, _out_tuple, _key) \
	pg_tde_crypt_tuple(_tuple, _out_tuple, _key, "DECRYPT-TUPLE")

#define PG_TDE_DECRYPT_TUPLE_EX(_tuple, _out_tuple, _key, _context) \
	do { \
	const char* _msg_context = "DECRYPT-TUPLE-"_context ; \
	pg_tde_crypt_tuple(_tuple, _out_tuple, _key, _msg_context); \
	} while(0)

#define PG_TDE_ENCRYPT_PAGE_ITEM(_iv_prefix, _start_offset, _data, _data_len, _out, _key) \
	do { \
		pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, "ENCRYPT-PAGE-ITEM"); \
	} while(0)

extern void AesEncryptKey(const TDEPrincipalKey *principal_key, const RelFileLocator *rlocator, RelKeyData *rel_key_data, RelKeyData **p_enc_rel_key_data, size_t *enc_key_bytes);
extern void AesDecryptKey(const TDEPrincipalKey *principal_key, const RelFileLocator *rlocator, RelKeyData **p_rel_key_data, RelKeyData *enc_rel_key_data, size_t *key_bytes);

#endif /* ENC_TDE_H */
