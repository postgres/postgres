/*-------------------------------------------------------------------------
 *
 * hashscan.c
 *	  manage scans on hash tables
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/hash/hashscan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/relscan.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/resowner.h"


/*
 * We track all of a backend's active scans on hash indexes using a list
 * of HashScanListData structs, which are allocated in TopMemoryContext.
 * It's okay to use a long-lived context because we rely on the ResourceOwner
 * mechanism to clean up unused entries after transaction or subtransaction
 * abort.  We can't safely keep the entries in the executor's per-query
 * context, because that might be already freed before we get a chance to
 * clean up the list.  (XXX seems like there should be a better way to
 * manage this...)
 */
typedef struct HashScanListData
{
	IndexScanDesc hashsl_scan;
	ResourceOwner hashsl_owner;
	struct HashScanListData *hashsl_next;
} HashScanListData;

typedef HashScanListData *HashScanList;

static HashScanList HashScans = NULL;


/*
 * ReleaseResources_hash() --- clean up hash subsystem resources.
 *
 * This is here because it needs to touch this module's static var HashScans.
 */
void
ReleaseResources_hash(void)
{
	HashScanList l;
	HashScanList prev;
	HashScanList next;

	/*
	 * Release all HashScanList items belonging to the current ResourceOwner.
	 * Note that we do not release the underlying IndexScanDesc; that's in
	 * executor memory and will go away on its own (in fact quite possibly has
	 * gone away already, so we mustn't try to touch it here).
	 *
	 * Note: this should be a no-op during normal query shutdown. However, in
	 * an abort situation ExecutorEnd is not called and so there may be open
	 * index scans to clean up.
	 */
	prev = NULL;

	for (l = HashScans; l != NULL; l = next)
	{
		next = l->hashsl_next;
		if (l->hashsl_owner == CurrentResourceOwner)
		{
			if (prev == NULL)
				HashScans = next;
			else
				prev->hashsl_next = next;

			pfree(l);
			/* prev does not change */
		}
		else
			prev = l;
	}
}

/*
 *	_hash_regscan() -- register a new scan.
 */
void
_hash_regscan(IndexScanDesc scan)
{
	HashScanList new_el;

	new_el = (HashScanList) MemoryContextAlloc(TopMemoryContext,
											   sizeof(HashScanListData));
	new_el->hashsl_scan = scan;
	new_el->hashsl_owner = CurrentResourceOwner;
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

	last = NULL;
	for (chk = HashScans;
		 chk != NULL && chk->hashsl_scan != scan;
		 chk = chk->hashsl_next)
		last = chk;

	if (chk == NULL)
		elog(ERROR, "hash scan list trashed; cannot find 0x%p", (void *) scan);

	if (last == NULL)
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
