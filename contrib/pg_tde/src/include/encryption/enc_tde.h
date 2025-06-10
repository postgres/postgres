/*
 * Encryption / Decryption of functions for TDE
 */

#ifndef ENC_TDE_H
#define ENC_TDE_H

#include "access/pg_tde_tdemap.h"

extern void pg_tde_generate_internal_key(InternalKey *int_key, TDEMapEntryType entry_type);
extern void pg_tde_stream_crypt(const char *iv_prefix, uint32 start_offset, const char *data, uint32 data_len, char *out, InternalKey *key, void **ctxPtr);

#endif							/* ENC_TDE_H */
