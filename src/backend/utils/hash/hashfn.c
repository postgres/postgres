/*-------------------------------------------------------------------------
 *
 * hashfn.c
 *		Hash functions for use in dynahash.c hashtables
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/hash/hashfn.c,v 1.29 2006/10/04 00:30:02 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "nodes/bitmapset.h"


/*
 * string_hash: hash function for keys that are null-terminated strings.
 *
 * NOTE: this is the default hash function if none is specified.
 */
uint32
string_hash(const void *key, Size keysize)
{
	/*
	 * If the string exceeds keysize-1 bytes, we want to hash only that many,
	 * because when it is copied into the hash table it will be truncated at
	 * that length.
	 */
	Size		s_len = strlen((const char *) key);

	s_len = Min(s_len, keysize - 1);
	return DatumGetUInt32(hash_any((const unsigned char *) key,
								   (int) s_len));
}

/*
 * tag_hash: hash function for fixed-size tag values
 */
uint32
tag_hash(const void *key, Size keysize)
{
	return DatumGetUInt32(hash_any((const unsigned char *) key,
								   (int) keysize));
}

/*
 * oid_hash: hash function for keys that are OIDs
 *
 * (tag_hash works for this case too, but is slower)
 */
uint32
oid_hash(const void *key, Size keysize)
{
	Assert(keysize == sizeof(Oid));
	/* We don't actually bother to do anything to the OID value ... */
	return (uint32) *((const Oid *) key);
}

/*
 * bitmap_hash: hash function for keys that are (pointers to) Bitmapsets
 *
 * Note: don't forget to specify bitmap_match as the match function!
 */
uint32
bitmap_hash(const void *key, Size keysize)
{
	Assert(keysize == sizeof(Bitmapset *));
	return bms_hash_value(*((const Bitmapset *const *) key));
}

/*
 * bitmap_match: match function to use with bitmap_hash
 */
int
bitmap_match(const void *key1, const void *key2, Size keysize)
{
	Assert(keysize == sizeof(Bitmapset *));
	return !bms_equal(*((const Bitmapset *const *) key1),
					  *((const Bitmapset *const *) key2));
}
