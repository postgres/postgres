/*-------------------------------------------------------------------------
 *
 * hashscan.c
 *	  manage scans on hash tables
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/hash/hashscan.c,v 1.31 2003/09/04 22:06:27 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"


typedef struct HashScanListData
{
	IndexScanDesc hashsl_scan;
	struct HashScanListData *hashsl_next;
} HashScanListData;

typedef HashScanListData *HashScanList;

static HashScanList HashScans = (HashScanList) NULL;


/*
 * AtEOXact_hash() --- clean up hash subsystem at xact abort or commit.
 *
 * This is here because it needs to touch this module's static var HashScans.
 */
void
AtEOXact_hash(void)
{
	/*
	 * Note: these actions should only be necessary during xact abort; but
	 * they can't hurt during a commit.
	 */

	/*
	 * Reset the active-scans list to empty. We do not need to free the
	 * list elements, because they're all palloc()'d, so they'll go away
	 * at end of transaction anyway.
	 */
	HashScans = NULL;
}

/*
 *	_Hash_regscan() -- register a new scan.
 */
void
_hash_regscan(IndexScanDesc scan)
{
	HashScanList new_el;

	new_el = (HashScanList) palloc(sizeof(HashScanListData));
	new_el->hashsl_scan = scan;
	new_el->hashsl_next = HashScans;
	HashScans = new_el;
}

/*
 *	_hash_dropscan() -- drop a scan from the scan list
 */
void
_hash_dropscan(IndexScanDesc scan)
{
	HashScanList chk,
				last;

	last = (HashScanList) NULL;
	for (chk = HashScans;
		 chk != (HashScanList) NULL && chk->hashsl_scan != scan;
		 chk = chk->hashsl_next)
		last = chk;

	if (chk == (HashScanList) NULL)
		elog(ERROR, "hash scan list trashed; can't find 0x%p", (void *) scan);

	if (last == (HashScanList) NULL)
		HashScans = chk->hashsl_next;
	else
		last->hashsl_next = chk->hashsl_next;

	pfree(chk);
}

/*
 * Is there an active scan in this bucket?
 */
bool
_hash_has_active_scan(Relation rel, Bucket bucket)
{
	Oid			relid = RelationGetRelid(rel);
	HashScanList l;

	for (l = HashScans; l != NULL; l = l->hashsl_next)
	{
		if (relid == l->hashsl_scan->indexRelation->rd_id)
		{
			HashScanOpaque so = (HashScanOpaque) l->hashsl_scan->opaque;

			if (so->hashso_bucket_valid &&
				so->hashso_bucket == bucket)
				return true;
		}
	}

	return false;
}
