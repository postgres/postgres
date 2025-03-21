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

#include "access/pg_tde_tdemap.h"

extern void pg_tde_crypt(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, InternalKey *key, void **ctxPtr, const char *context);

/* Function Macros over crypt */

#define PG_TDE_ENCRYPT_DATA(_iv_prefix, _start_offset, _data, _data_len, _out, _key, _ctxptr) \
	pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, _ctxptr, "ENCRYPT")

#define PG_TDE_DECRYPT_DATA(_iv_prefix, _start_offset, _data, _data_len, _out, _key, _ctxptr) \
	pg_tde_crypt(_iv_prefix, _start_offset, _data, _data_len, _out, _key, _ctxptr, "DECRYPT")

#endif							/* ENC_TDE_H */
