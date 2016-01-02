/*-------------------------------------------------------------------------
 *
 * hashdesc.c
 *	  rmgr descriptor routines for access/hash/hash.c
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/hashdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"

void
hash_desc(StringInfo buf, XLogReaderState *record)
{
}

const char *
hash_identify(uint8 info)
{
	return NULL;
}
