/*
 * Encryption / Decryption of functions for TDE
 */

#ifndef ENC_TDE_H
#define ENC_TDE_H

#define INTERNAL_KEY_LEN 16
#define INTERNAL_KEY_IV_LEN 16

typedef struct InternalKey
{
	uint8		key[INTERNAL_KEY_LEN];
	uint8		base_iv[INTERNAL_KEY_IV_LEN];
} InternalKey;

extern void pg_tde_generate_internal_key(InternalKey *int_key);
extern void pg_tde_stream_crypt(const char *iv_prefix,
								uint32 start_offset,
								const char *data,
								uint32 data_len,
								char *out,
								const uint8 *key,
								void **ctxPtr);

#endif							/* ENC_TDE_H */
