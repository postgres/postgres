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
pg_tde_crypt(uint64 start_offset, const char* data, uint32 data_len, char* out, RelKeysData* keys, const char* context);
extern void
pg_tde_crypt_tuple(BlockNumber bn, Page page, HeapTuple tuple, HeapTuple out_tuple, RelKeysData* keys, const char* context);
extern void
pg_tde_move_encrypted_data(uint64 read_start_offset, const char* read_data,
				uint64 write_start_offset, char* write_data,
				uint32 data_len, RelKeysData* keys, const char* context);


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

#define PG_TDE_ENCRYPT_DATA(_start_offset, _data, _data_len, _out, _keys) \
	pg_tde_crypt(_start_offset, _data, _data_len, _out, _keys, "ENCRYPT")

#define PG_TDE_DECRYPT_DATA(_start_offset, _data, _data_len, _out, _keys) \
	pg_tde_crypt(_start_offset, _data, _data_len, _out, _keys, "DECRYPT")

#define PG_TDE_DECRYPT_TUPLE(_bn, _page, _tuple, _out_tuple, _keys) \
	pg_tde_crypt_tuple(_bn, _page, _tuple, _out_tuple, _keys, "DECRYPT-TUPLE")

#define PG_TDE_DECRYPT_TUPLE_EX(_bn, _page, _tuple, _out_tuple, _keys, _context) \
	do { \
	const char* _msg_context = "DECRYPT-TUPLE-"_context ; \
	pg_tde_crypt_tuple(_bn, _page, _tuple, _out_tuple, _keys, _msg_context); \
	} while(0)

#define PG_TDE_ENCRYPT_PAGE_ITEM(_bn, _offset_in_page, _data, _data_len, _out, _keys) \
	do { \
		uint64 offset_in_file = (_bn * BLCKSZ) + _offset_in_page; \
		pg_tde_crypt(offset_in_file, _data, _data_len, _out, _keys, "ENCRYPT-PAGE-ITEM"); \
	} while(0)

#define PG_TDE_RE_ENCRYPT_TUPLE_DATA(_read_bn, _read_offset_in_page, _read_data, \
				_write_bn, _write_offset_in_page, _write_data, _data_len, _keys) \
	do { \
		uint64 read_offset_in_file = (_read_bn * BLCKSZ) + _read_offset_in_page; \
		uint64 write_offset_in_file = (_write_bn * BLCKSZ) + _write_offset_in_page; \
	pg_tde_move_encrypted_data(read_offset_in_file, _read_data, \
				write_offset_in_file, _write_data, \
				_data_len, _keys, "RE_ENCRYPT_TUPLE_DATA"); \
	} while(0)

#endif /*ENC_TUPLE_H*/
