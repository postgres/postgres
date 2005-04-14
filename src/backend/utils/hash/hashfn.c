/*-------------------------------------------------------------------------
 *
 * hashfn.c
 *		Hash functions for use in dynahash.c hashtables
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/hash/hashfn.c,v 1.23 2005/04/14 20:32:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "utils/hsearch.h"


/*
 * string_hash: hash function for keys that are null-terminated strings.
 *
 * NOTE: this is the default hash function if none is specified.
 */
uint32
string_hash(const void *key, Size keysize)
{
	return DatumGetUInt32(hash_any((const unsigned char *) key,
								   (int) strlen((const char *) key)));
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
