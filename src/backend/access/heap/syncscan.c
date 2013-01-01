/*-------------------------------------------------------------------------
 *
 * syncscan.c
 *	  heap scan synchronization support
 *
 * When multiple backends run a sequential scan on the same table, we try
 * to keep them synchronized to reduce the overall I/O needed.	The goal is
 * to read each page into shared buffer cache only once, and let all backends
 * that take part in the shared scan process the page before it falls out of
 * the cache.
 *
 * Since the "leader" in a pack of backends doing a seqscan will have to wait
 * for I/O, while the "followers" don't, there is a strong self-synchronizing
 * effect once we can get the backends examining approximately the same part
 * of the table at the same time.  Hence all that is really needed is to get
 * a new backend beginning a seqscan to begin it close to where other backends
 * are reading.  We can scan the table circularly, from block X up to the
 * end and then from block 0 to X-1, to ensure we visit all rows while still
 * participating in the common scan.
 *
 * To accomplish that, we keep track of the scan position of each table, and
 * start new scans close to where the previous scan(s) are.  We don't try to
 * do any extra synchronization to keep the scans together afterwards; some
 * scans might progress much more slowly than others, for example if the
 * results need to be transferred to the client over a slow network, and we
 * don't want such queries to slow down others.
 *
 * There can realistically only be a few large sequential scans on different
 * tables in progress at any time.	Therefore we just keep the scan positions
 * in a small LRU list which we scan every time we need to look up or update a
 * scan position.  The whole mechanism is only applied for tables exceeding
 * a threshold size (but that is not the concern of this module).
 *
 * INTERFACE ROUTINES
 *		ss_get_location		- return current scan location of a relation
 *		ss_report_location	- update current scan location
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/syncscan.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "miscadmin.h"
#include "utils/rel.h"


/* GUC variables */
#ifdef TRACE_SYNCSCAN
bool		trace_syncscan = false;
#endif


/*
 * Size of the LRU list.
 *
 * Note: the code assumes that SYNC_SCAN_NELEM > 1.
 *
 * XXX: What's a good value? It should be large enough to hold the
 * maximum number of large tables scanned simultaneously.  But a larger value
 * means more traversing of the LRU list when starting a new scan.
 */
#define SYNC_SCAN_NELEM 20

/*
 * Interval between reports of the location of the current scan, in pages.
 *
 * Note: This should be smaller than the ring size (see buffer/freelist.c)
 * we use for bulk reads.  Otherwise a scan joining other scans might start
 * from a page that's no longer in the buffer cache.  This is a bit fuzzy;
 * there's no guarantee that the new scan will read the page before it leaves
 * the buffer cache anyway, and on the other hand the page is most likely
 * still in the OS cache.
 */
#define SYNC_SCAN_REPORT_INTERVAL (128 * 1024 / BLCKSZ)


/*
 * The scan locations structure is essentially a doubly-linked LRU with head
 * and tail pointer, but designed to hold a fixed maximum number of elements in
 * fixed-size shared memory.
 */
typedef struct ss_scan_location_t
{
	RelFileNode relfilenode;	/* identity of a relation */
	BlockNumber location;		/* last-reported location in the relation */
} ss_scan_location_t;

typedef struct ss_lru_item_t
{
	struct ss_lru_item_t *prev;
	struct ss_lru_item_t *next;
	ss_scan_location_t location;
} ss_lru_item_t;

typedef struct ss_scan_locations_t
{
	ss_lru_item_t *head;
	ss_lru_item_t *tail;
	ss_lru_item_t items[1];		/* SYNC_SCAN_NELEM items */
} ss_scan_locations_t;

#define SizeOfScanLocations(N) offsetof(ss_scan_locations_t, items[N])

/* Pointer to struct in shared memory */
static ss_scan_locations_t *scan_locations;

/* prototypes for internal functions */
static BlockNumber ss_search(RelFileNode relfilenode,
		  BlockNumber location, bool set);


/*
 * SyncScanShmemSize --- report amount of shared memory space needed
 */
Size
SyncScanShmemSize(void)
{
	return SizeOfScanLocations(SYNC_SCAN_NELEM);
}

/*
 * SyncScanShmemInit --- initialize this module's shared memory
 */
void
SyncScanShmemInit(void)
{
	int			i;
	bool		found;

	scan_locations = (ss_scan_locations_t *)
		ShmemInitStruct("Sync Scan Locations List",
						SizeOfScanLocations(SYNC_SCAN_NELEM),
						&found);

	if (!IsUnderPostmaster)
	{
		/* Initialize shared memory area */
		Assert(!found);

		scan_locations->head = &scan_locations->items[0];
		scan_locations->tail = &scan_locations->items[SYNC_SCAN_NELEM - 1];

		for (i = 0; i < SYNC_SCAN_NELEM; i++)
		{
			ss_lru_item_t *item = &scan_locations->items[i];

			/*
			 * Initialize all slots with invalid values. As scans are started,
			 * these invalid entries will fall off the LRU list and get
			 * replaced with real entries.
			 */
			item->location.relfilenode.spcNode = InvalidOid;
			item->location.relfilenode.dbNode = InvalidOid;
			item->location.relfilenode.relNode = InvalidOid;
			item->location.location = InvalidBlockNumber;

			item->prev = (i > 0) ?
				(&scan_locations->items[i - 1]) : NULL;
			item->next = (i < SYNC_SCAN_NELEM - 1) ?
				(&scan_locations->items[i + 1]) : NULL;
		}
	}
	else
		Assert(found);
}

/*
 * ss_search --- search the scan_locations structure for an entry with the
 *		given relfilenode.
 *
 * If "set" is true, the location is updated to the given location.  If no
 * entry for the given relfilenode is found, it will be created at the head
 * of the list with the given location, even if "set" is false.
 *
 * In any case, the location after possible update is returned.
 *
 * Caller is responsible for having acquired suitable lock on the shared
 * data structure.
 */
static BlockNumber
ss_search(RelFileNode relfilenode, BlockNumber location, bool set)
{
	ss_lru_item_t *item;

	item = scan_locations->head;
	for (;;)
	{
		bool		match;

		match = RelFileNodeEquals(item->location.relfilenode, relfilenode);

		if (match || item->next == NULL)
		{
			/*
			 * If we reached the end of list and no match was found, take over
			 * the last entry
			 */
			if (!match)
			{
				item->location.relfilenode = relfilenode;
				item->location.location = location;
			}
			else if (set)
				item->location.location = location;

			/* Move the entry to the front of the LRU list */
			if (item != scan_locations->head)
			{
				/* unlink */
				if (item == scan_locations->tail)
					scan_locations->tail = item->prev;
				item->prev->next = item->next;
				if (item->next)
					item->next->prev = item->prev;

				/* link */
				item->prev = NULL;
				item->next = scan_locations->head;
				scan_locations->head->prev = item;
				scan_locations->head = item;
			}

			return item->location.location;
		}

		item = item->next;
	}

	/* not reached */
}

/*
 * ss_get_location --- get the optimal starting location for scan
 *
 * Returns the last-reported location of a sequential scan on the
 * relation, or 0 if no valid location is found.
 *
 * We expect the caller has just done RelationGetNumberOfBlocks(), and
 * so that number is passed in rather than computing it again.	The result
 * is guaranteed less than relnblocks (assuming that's > 0).
 */
BlockNumber
ss_get_location(Relation rel, BlockNumber relnblocks)
{
	BlockNumber startloc;

	LWLockAcquire(SyncScanLock, LW_EXCLUSIVE);
	startloc = ss_search(rel->rd_node, 0, false);
	LWLockRelease(SyncScanLock);

	/*
	 * If the location is not a valid block number for this scan, start at 0.
	 *
	 * This can happen if for instance a VACUUM truncated the table since the
	 * location was saved.
	 */
	if (startloc >= relnblocks)
		startloc = 0;

#ifdef TRACE_SYNCSCAN
	if (trace_syncscan)
		elog(LOG,
			 "SYNC_SCAN: start \"%s\" (size %u) at %u",
			 RelationGetRelationName(rel), relnblocks, startloc);
#endif

	return startloc;
}

/*
 * ss_report_location --- update the current scan location
 *
 * Writes an entry into the shared Sync Scan state of the form
 * (relfilenode, blocknumber), overwriting any existing entry for the
 * same relfilenode.
 */
void
ss_report_location(Relation rel, BlockNumber location)
{
#ifdef TRACE_SYNCSCAN
	if (trace_syncscan)
	{
		if ((location % 1024) == 0)
			elog(LOG,
				 "SYNC_SCAN: scanning \"%s\" at %u",
				 RelationGetRelationName(rel), location);
	}
#endif

	/*
	 * To reduce lock contention, only report scan progress every N pages. For
	 * the same reason, don't block if the lock isn't immediately available.
	 * Missing a few updates isn't critical, it just means that a new scan
	 * that wants to join the pack will start a little bit behind the head of
	 * the scan.  Hopefully the pages are still in OS cache and the scan
	 * catches up quickly.
	 */
	if ((location % SYNC_SCAN_REPORT_INTERVAL) == 0)
	{
		if (LWLockConditionalAcquire(SyncScanLock, LW_EXCLUSIVE))
		{
			(void) ss_search(rel->rd_node, location, true);
			LWLockRelease(SyncScanLock);
		}
#ifdef TRACE_SYNCSCAN
		else if (trace_syncscan)
			elog(LOG,
				 "SYNC_SCAN: missed update for \"%s\" at %u",
				 RelationGetRelationName(rel), location);
#endif
	}
}
