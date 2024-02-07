/*-------------------------------------------------------------------------
 *
 * enc_tuple.h
 *	  Encryption / Decryption of tuples and item data
 *
 * src/include/encryption/enc_tuple.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENC_TUPLE_H
#define ENC_TUPLE_H

#include "utils/rel.h"
#include "storage/bufpage.h"
#include "executor/tuptable.h"
#include "executor/tuptable.h"
#include "access/pg_tde_tdemap.h"

extern void
pg_tde_crypt(const char* iv_prefix, uint32 start_offset, const char* data, uint32 data_len, char* out, RelKeysData* keys, const char* context);
extern void
pg_tde_crypt_tuple(HeapTuple tuple, HeapTuple out_tuple, RelKeysData* keys, const char* context);

/* A wrapper to encrypt a tuple before adding it to the buffer */
extern OffsetNumber
PGTdePageAddItemExtended(RelFileLocator rel, Oid oid, BlockNumber bn, Page page,
					Item item,
					Size size,
					OffsetNumber offsetNumber,
					int flags);

/* Wrapper functions for reading decrypted tuple into a given slot */
extern TupleTableSlot *
PGTdeExecStoreBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);
extern TupleTableSlot *
PGTdeExecStorePinnedBufferHeapTuple(Relation rel, HeapTuple tuple, TupleTableSlot *slot, Buffer buffer);

/* Function Macros over crypt */

#define PG_TDE_ENCRYPT_DATA(_iv_prefix, _iv_prefix_len, _data, _data_len, _out, _keys) \
	pg_tde_crypt(_iv_prefix, _iv_prefix_len, _data, _data_len, _out, _keys, "ENCRYPT")

#define PG_TDE_DECRYPT_DATA(_iv_prefix, _iv_prefix_len, _data, _data_len, _out, _keys) \
	pg_tde_crypt(_iv_prefix, _iv_prefix_len, _data, _data_len, _out, _keys, "DECRYPT")

#define PG_TDE_DECRYPT_TUPLE(_tuple, _out_tuple, _keys) \
	pg_tde_crypt_tuple(_tuple, _out_tuple, _keys, "DECRYPT-TUPLE")

#define PG_TDE_DECRYPT_TUPLE_EX(_tuple, _out_tuple, _keys, _context) \
	do { \
	const char* _msg_context = "DECRYPT-TUPLE-"_context ; \
	pg_tde_crypt_tuple(_tuple, _out_tuple, _keys, _msg_context); \
	} while(0)

#define PG_TDE_ENCRYPT_PAGE_ITEM(_iv_prefix, _iv_prefix_len, _data, _data_len, _out, _keys) \
	do { \
		pg_tde_crypt(_iv_prefix, _iv_prefix_len, _data, _data_len, _out, _keys, "ENCRYPT-PAGE-ITEM"); \
	} while(0)

#endif /*ENC_TUPLE_H*/
